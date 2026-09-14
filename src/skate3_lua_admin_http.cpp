#include "skate3_lua_admin_http.h"

#include "skate3_lua_script_host.h"

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace skate3::lua_host {

namespace {

std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        // Every other control character is illegal raw inside a JSON
        // string. Log lines do carry them in practice - a Lua traceback is
        // multi-line - and a single unescaped newline makes the whole
        // /api/console/log response unparseable, taking every other line
        // down with it rather than just the offending one.
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped.push_back(kHex[(c >> 4) & 0xF]);
          escaped.push_back(kHex[c & 0xF]);
        } else {
          escaped.push_back(c);
        }
    }
  }
  return escaped;
}

std::string EncodeResourceListJson(const std::vector<ResourceStatus>& resources) {
  std::string json = "[";
  bool first = true;
  for (const auto& resource : resources) {
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"name\":\"" + EscapeJson(resource.name) + "\",\"status\":\"" +
            (resource.state == ResourceState::kStarted ? "started" : "stopped") +
            "\"}";
  }
  json += "]";
  return json;
}

// Names/filenames are simple identifiers in practice (directory and file
// names), so this list is intentionally simple JSON, not general-purpose -
// the actual script source is served separately as raw text (see
// /api/scripts/<name>/source below) specifically to avoid needing to
// JSON-escape arbitrary Lua source code.
std::string EncodeClientScriptListJson(const std::vector<ClientScriptInfo>& scripts) {
  std::string json = "[";
  bool first = true;
  for (const auto& script : scripts) {
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"name\":\"" + EscapeJson(script.name) + "\",\"ui_page\":\"" +
            EscapeJson(script.ui_page) + "\",\"files\":[";
    bool first_file = true;
    for (const auto& file : script.files) {
      if (!first_file) {
        json += ",";
      }
      first_file = false;
      json += "\"" + EscapeJson(file) + "\"";
    }
    // The NUI assets, listed separately from the scripts: the client runs
    // one set through Lua and hands the other to the browser layer, and
    // conflating them would make it guess by file extension.
    json += "],\"ui_files\":[";
    bool first_ui_file = true;
    for (const auto& file : script.ui_files) {
      if (!first_ui_file) {
        json += ",";
      }
      first_ui_file = false;
      json += "\"" + EscapeJson(file) + "\"";
    }
    json += "]}";
  }
  json += "]";
  return json;
}

std::string EncodeLogLinesJson(const std::vector<LogLine>& lines, std::uint64_t cursor) {
  std::string json = "{\"cursor\":" + std::to_string(cursor) + ",\"lines\":[";
  bool first = true;
  for (const auto& line : lines) {
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"resource\":\"" + EscapeJson(line.resource) + "\",\"text\":\"" +
            EscapeJson(line.text) + "\",\"ts\":" + std::to_string(line.timestamp_ms) + "}";
  }
  json += "]}";
  return json;
}

// Pulls "event" and "args" out of {"event":"...","args":[...]} - the only
// shape this endpoint ever receives, so a full JSON parser is not needed.
// `args` is handed on as raw JSON text for the Lua side to decode.
void ParseEventJson(const std::string& body, std::string& event,
                    std::string& args) {
  const auto event_key = body.find("\"event\"");
  if (event_key != std::string::npos) {
    const auto open = body.find('"', body.find(':', event_key) + 1);
    if (open != std::string::npos) {
      auto close = open + 1;
      while (close < body.size() && body[close] != '"') {
        close += (body[close] == '\\') ? 2 : 1;
      }
      if (close < body.size()) {
        event = body.substr(open + 1, close - open - 1);
      }
    }
  }
  const auto args_key = body.find("\"args\"");
  if (args_key != std::string::npos) {
    const auto open = body.find('[', args_key);
    if (open != std::string::npos) {
      // Track nesting so a nested array does not end the scan early.
      int depth = 0;
      bool in_string = false;
      for (std::size_t i = open; i < body.size(); ++i) {
        const char c = body[i];
        if (in_string) {
          if (c == '\\') {
            ++i;
          } else if (c == '"') {
            in_string = false;
          }
          continue;
        }
        if (c == '"') {
          in_string = true;
        } else if (c == '[') {
          ++depth;
        } else if (c == ']' && --depth == 0) {
          args = body.substr(open, i - open + 1);
          break;
        }
      }
    }
  }
}

// Events queued for clients to collect, with a monotonic cursor so a client
// can poll from where it left off.
//
// Delivery is LONG-POLLED, not sampled on an interval: a client's request
// parks here until an event it should see actually exists, so a fired event
// reaches it on the next network round trip rather than on the next poll
// tick. Nothing is lost in the gap between one request returning and the
// next arriving, because the cursor makes that next request return
// immediately instead of parking.
class OutboundEventQueue {
 public:
  void Push(const std::string& event, const std::string& args,
            const std::string& target) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.push_back({next_index_++, event, args, target});
      if (entries_.size() > kMaxEntries) {
        entries_.pop_front();
      }
    }
    // Wakes every parked client; each re-checks whether this one was for it.
    ready_.notify_all();
  }

  // Blocks until this player has something to receive, or `timeout` passes
  // (returning an empty event list, which the client treats as "ask again").
  std::string WaitAndRead(std::uint64_t since, const std::string& player,
                          std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, timeout, [&] {
      return stopping_ || HasEventForLocked(since, player);
    });
    return ReadSinceLocked(since, player);
  }

  // Releases every parked request so the server can shut down promptly
  // instead of waiting out their timeouts.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
  }

  // `player` is the polling client's id. An entry addressed to a specific
  // player is withheld from everyone else; an entry with an empty target is
  // a broadcast. The cursor still advances past skipped entries, so a client
  // never re-polls events that were never for it.
  std::string ReadSince(std::uint64_t since, const std::string& player) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadSinceLocked(since, player);
  }

 private:
  bool HasEventForLocked(std::uint64_t since,
                         const std::string& player) const {
    for (const auto& entry : entries_) {
      if (entry.index > since &&
          (entry.target.empty() || entry.target == player)) {
        return true;
      }
    }
    return false;
  }

  std::string ReadSinceLocked(std::uint64_t since,
                              const std::string& player) const {
    std::string json = "{\"events\":[";
    std::uint64_t cursor = since;
    bool first = true;
    for (const auto& entry : entries_) {
      if (entry.index <= since) {
        continue;
      }
      if (!entry.target.empty() && entry.target != player) {
        cursor = entry.index;
        continue;
      }
      if (!first) {
        json += ",";
      }
      first = false;
      json += "{\"event\":\"" + EscapeJson(entry.event) + "\",\"target\":\"" +
              EscapeJson(entry.target) +
              "\",\"args\":" + (entry.args.empty() ? "[]" : entry.args) + "}";
      cursor = entry.index;
    }
    json += "],\"cursor\":" + std::to_string(cursor) + "}";
    return json;
  }

  struct Entry {
    std::uint64_t index = 0;
    std::string event;
    std::string args;
    std::string target;
  };
  static constexpr std::size_t kMaxEntries = 512;

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Entry> entries_;
  std::uint64_t next_index_ = 1;
  bool stopping_ = false;
};

}  // namespace

struct AdminHttpServer::Impl {
  explicit Impl(LuaScriptHost& host) : host(host) {}

  LuaScriptHost& host;
  httplib::Server server;
  std::thread thread;
  ConsoleExecHandler exec_handler;
  AppearanceStoreHandler appearance_store;
  AppearanceFetchHandler appearance_fetch;
  AppearanceRosterProvider appearance_roster;
  PropAddHandler prop_add;
  PropRemoveHandler prop_remove;
  PropQueryHandler prop_query;
  SettingsProvider settings_provider;
  MetricsProvider metrics_provider;
  PlayersProvider players_provider;
  PlayerNameHandler player_name_handler;
  NuiFileProvider nui_file_provider;
  OutboundEventQueue outbound;
};

AdminHttpServer::AdminHttpServer(LuaScriptHost& host,
                                 std::filesystem::path web_console_dir)
    : impl_(std::make_unique<Impl>(host)) {
  // Every parked long-poll holds a worker thread for as long as it waits,
  // and the console's own polling needs threads alongside them, so the
  // default pool (sized to core count) is too small to serve a full lobby.
  impl_->server.new_task_queue = [] { return new httplib::ThreadPool(64); };

  // The server's settings, for clients to apply directly. Deliberately served
  // to the ENGINE rather than pushed through a resource: rules like difficulty
  // must not depend on a script choosing to honour them.
  impl_->server.Get("/api/settings", [this](const httplib::Request&,
                                            httplib::Response& res) {
    res.set_content(impl_->settings_provider ? impl_->settings_provider() : "{}",
                    "application/json");
  });

  impl_->server.Get("/api/resources", [this](const httplib::Request&,
                                             httplib::Response& res) {
    res.set_content(EncodeResourceListJson(impl_->host.ListResources()),
                    "application/json");
  });

  // Bandwidth/hitches (relay-side, set only by skate3_dedicated) plus Lua
  // resmon (both sides, always available since LuaScriptHost lives here).
  // See SetMetricsProvider's own comment for why the caller builds the JSON.
  impl_->server.Get("/api/metrics", [this](const httplib::Request&,
                                           httplib::Response& res) {
    res.set_content(impl_->metrics_provider ? impl_->metrics_provider() : "{}",
                    "application/json");
  });

  impl_->server.Post(
      R"(/api/resources/([^/]+)/(ensure|restart|stop))",
      [this](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        const std::string action = req.matches[2];
        bool ok = false;
        if (action == "ensure") {
          ok = impl_->host.EnsureResource(name);
        } else if (action == "restart") {
          ok = impl_->host.RestartResource(name);
        } else if (action == "stop") {
          ok = impl_->host.StopResource(name);
        }
        res.set_content(ok ? "{\"ok\":true}" : "{\"ok\":false}",
                        "application/json");
      });

  // Consumed by a connecting client (skate3_lua_client_natives.cpp's
  // SyncResourcesFromServer) to fetch this server's client-side resources
  // - the "true remote script delivery" model: the client does not need its
  // own local resources/ files for anything served here.
  // NUI page serving - see SetNuiFileProvider for why these are HTTP and
  // not on the resources' own https origins.
  impl_->server.Get(R"(/nui/(.*))", [this](const httplib::Request& request,
                                           httplib::Response& response) {
    std::string data;
    std::string mime;
    if (!impl_->nui_file_provider ||
        !impl_->nui_file_provider(request.matches[1].str(), data, mime)) {
      response.status = 404;
      response.set_content("not found", "text/plain");
      return;
    }
    // NEVER cached. These are a resource's own UI files, served to CEF over
    // loopback, and they change whenever the resource is edited or a client
    // reconnects to a server carrying a newer copy. With no cache headers
    // CEF applies heuristic caching and keeps serving the version it first
    // saw, which shows up as one client running a visibly older UI than
    // another - a stale page with no way for the player to know, and no
    // reload to reach for. There is no versioning in these URLs to make
    // caching safe, and they come off local disk, so there is nothing to
    // gain by allowing it.
    response.set_header("Cache-Control", "no-store, must-revalidate");
    response.set_header("Pragma", "no-cache");
    response.set_content(data, mime.c_str());
  });

  // ------------------------------------------------------- appearances
  //
  // Binary in and out. The server never parses a blob: it is a client's
  // appearance recipe, addressed by a content hash the client computed, and
  // the only thing worth validating here is its size.
  impl_->server.Post(
      R"(/api/appearance/([0-9A-Fa-f]{1,16}))",
      [this](const httplib::Request& req, httplib::Response& res) {
        if (!impl_->appearance_store) {
          res.status = 501;
          res.set_content("no appearance store", "text/plain");
          return;
        }
        const std::uint64_t appearance_id =
            std::strtoull(req.matches[1].str().c_str(), nullptr, 16);
        std::uint32_t role = 0;
        if (req.has_param("role")) {
          role = static_cast<std::uint32_t>(
              std::strtoul(req.get_param_value("role").c_str(), nullptr, 10));
        }
        const std::vector<std::uint8_t> bytes(req.body.begin(), req.body.end());
        if (!impl_->appearance_store(appearance_id, role, bytes)) {
          res.status = 400;
          res.set_content("rejected", "text/plain");
          return;
        }
        res.set_content("{\"ok\":true}", "application/json");
      });

  impl_->server.Get(
      R"(/api/appearance/([0-9A-Fa-f]{1,16}))",
      [this](const httplib::Request& req, httplib::Response& res) {
        if (!impl_->appearance_fetch) {
          res.status = 501;
          res.set_content("no appearance store", "text/plain");
          return;
        }
        const std::uint64_t appearance_id =
            std::strtoull(req.matches[1].str().c_str(), nullptr, 16);
        const std::vector<std::uint8_t> bytes =
            impl_->appearance_fetch(appearance_id);
        if (bytes.empty()) {
          // A 404 is a real answer here, not an error: it means "that
          // appearance is not on this server", and the client falls back to
          // the peer-to-peer transfer rather than waiting.
          res.status = 404;
          res.set_content("unknown appearance", "text/plain");
          return;
        }
        res.set_content(std::string(bytes.begin(), bytes.end()),
                        "application/octet-stream");
      });

  impl_->server.Get("/api/appearance", [this](const httplib::Request& req,
                                              httplib::Response& res) {
    if (!impl_->appearance_roster) {
      res.set_content("{\"version\":0,\"peers\":[]}", "application/json");
      return;
    }
    std::uint32_t viewer_role = 0;
    if (req.has_param("role")) {
      viewer_role = static_cast<std::uint32_t>(
          std::strtoul(req.get_param_value("role").c_str(), nullptr, 10));
    }
    std::uint64_t since = 0;
    if (req.has_param("since")) {
      since = std::strtoull(req.get_param_value("since").c_str(), nullptr, 10);
    }
    const bool wait =
        req.has_param("wait") && req.get_param_value("wait") == "1";
    res.set_content(impl_->appearance_roster(viewer_role, since, wait),
                    "application/json");
  });

  // ------------------------------------------------------------- props
  //
  // The body is pipe-separated rather than JSON: it is five fields written
  // and read only here, and a hand-rolled JSON parser on the request path
  // would be more code and more ways to be wrong than the format deserves.
  impl_->server.Post("/api/props", [this](const httplib::Request& req,
                                          httplib::Response& res) {
    if (!impl_->prop_add) {
      res.status = 501;
      res.set_content("no prop store", "text/plain");
      return;
    }
    std::vector<std::string> fields;
    std::string current;
    for (const char c : req.body) {
      if (c == '|') {
        fields.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    fields.push_back(current);
    if (fields.size() < 4) {
      res.status = 400;
      res.set_content("expected path|x|y|z[|lod]", "text/plain");
      return;
    }
    const std::uint32_t id = impl_->prop_add(
        fields[0], std::strtof(fields[1].c_str(), nullptr),
        std::strtof(fields[2].c_str(), nullptr),
        std::strtof(fields[3].c_str(), nullptr),
        fields.size() > 4 ? std::strtof(fields[4].c_str(), nullptr) : 300.0f);
    if (id == 0) {
      res.status = 400;
      res.set_content("rejected", "text/plain");
      return;
    }
    res.set_content("{\"id\":" + std::to_string(id) + "}",
                    "application/json");
  });

  impl_->server.Post("/api/props/remove", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
    if (!impl_->prop_remove) {
      res.status = 501;
      res.set_content("no prop store", "text/plain");
      return;
    }
    const auto id =
        static_cast<std::uint32_t>(std::strtoul(req.body.c_str(), nullptr, 10));
    res.set_content(impl_->prop_remove(id) ? "{\"ok\":true}"
                                           : "{\"ok\":false}",
                    "application/json");
  });

  impl_->server.Get("/api/props", [this](const httplib::Request& req,
                                         httplib::Response& res) {
    if (!impl_->prop_query) {
      res.set_content("[]", "application/json");
      return;
    }
    // No position means "everything" - what a dashboard or a joining client
    // wants. A position means "what is near me", which is the streaming case.
    const bool all = !req.has_param("x");
    const auto number = [&req](const char* name) {
      return req.has_param(name)
                 ? std::strtof(req.get_param_value(name).c_str(), nullptr)
                 : 0.0f;
    };
    const bool wait = req.has_param("since");
    const auto since =
        wait ? std::strtoull(req.get_param_value("since").c_str(), nullptr, 10)
             : 0ull;
    res.set_content(
        impl_->prop_query(number("x"), number("y"), number("z"),
                          req.has_param("r") ? number("r") : 450.0f, all,
                          since, wait),
        "application/json");
  });

  impl_->server.Get("/api/scripts", [this](const httplib::Request&,
                                           httplib::Response& res) {
    res.set_content(EncodeClientScriptListJson(impl_->host.ListClientScripts()),
                    "application/json");
  });
  // One file out of a resource. `path` is relative to the resource
  // directory; ReadResourceFile is what enforces that it stays there.
  impl_->server.Get(
      R"(/api/scripts/([^/]+)/file)",
      [this](const httplib::Request& req, httplib::Response& res) {
        const std::string name = req.matches[1];
        if (!req.has_param("path")) {
          res.status = 400;
          return;
        }
        const auto source =
            impl_->host.ReadResourceFile(name, req.get_param_value("path"));
        if (!source) {
          res.status = 404;
          return;
        }
        res.set_content(*source, "text/plain; charset=utf-8");
      });

  // Script events. Turn-based game modes are the target here, so this rides
  // the existing HTTP channel (posted one way, polled the other) rather than
  // the pose-sync UDP protocol - the Lua-side API is transport-agnostic, so
  // moving it onto the relay later changes nothing that scripts can see.
  //
  // Client -> here: an event the client's TriggerServerEvent produced.
  impl_->server.Post("/api/events", [this](const httplib::Request& req,
                                           httplib::Response& res) {
    std::string event;
    std::string args;
    ParseEventJson(req.body, event, args);
    if (!event.empty()) {
      // The sender identifies itself with its relay-assigned player id;
      // handlers see it as `source`.
      int source = 0;
      if (req.has_param("player")) {
        source = std::atoi(req.get_param_value("player").c_str());
      }
      // State-bag replication is internal plumbing, not a script event.
      if (!impl_->host.TryApplyStateBagEvent(event, args)) {
        impl_->host.DispatchNetworkEvent(event, args, source);
      }
    }
    res.set_content("{\"ok\":true}", "application/json");
  });
  // Here -> client. Long-polled: this request parks until an event actually
  // exists for this client, so delivery costs one round trip rather than
  // waiting on a timer. The timeout only exists to keep connections from
  // living forever; expiring returns an empty list and the client re-asks.
  impl_->server.Get("/api/events", [this](const httplib::Request& req,
                                          httplib::Response& res) {
    std::uint64_t since = 0;
    if (req.has_param("since")) {
      since = std::strtoull(req.get_param_value("since").c_str(), nullptr, 10);
    }
    const std::string player =
        req.has_param("player") ? req.get_param_value("player") : std::string();
    res.set_content(
        impl_->outbound.WaitAndRead(since, player, std::chrono::seconds(20)),
        "application/json");
  });

  // Dev console (F7 in-game CEF overlay, or any browser): live print()/echo
  // feed via polling, and raw command dispatch. See SetConsoleExecHandler's
  // doc comment for why exec dispatch is injected rather than implemented
  // here.
  impl_->server.Get("/api/console/log", [this](const httplib::Request& req,
                                               httplib::Response& res) {
    std::uint64_t since = 0;
    if (req.has_param("since")) {
      since = std::strtoull(req.get_param_value("since").c_str(), nullptr, 10);
    }
    const auto lines = impl_->host.log_sink().ReadSince(since);
    res.set_content(EncodeLogLinesJson(lines, since), "application/json");
  });
  impl_->server.Post("/api/console/exec", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
    std::string_view line = req.body;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\n' ||
                             line.front() == '\r')) {
      line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n' ||
                             line.back() == '\r')) {
      line.remove_suffix(1);
    }
    if (!line.empty()) {
      impl_->host.log_sink().Append("", "[console] " + std::string(line));
      if (impl_->exec_handler) {
        impl_->exec_handler(line);
      }
    }
    res.set_content("{\"ok\":true}", "application/json");
  });

  // Dashboard's Active Players page. See SetPlayersProvider's own comment
  // for why this is injected rather than implemented here.
  impl_->server.Get("/api/players", [this](const httplib::Request&,
                                           httplib::Response& res) {
    res.set_content(impl_->players_provider ? impl_->players_provider() : "[]",
                    "application/json");
  });
  // A client posts its own display name once it has a player id (see
  // skate3_lua_client_natives.cpp's SyncResourcesFromServer). Kept as a
  // plain endpoint rather than routed through the script-event system: this
  // is core identity plumbing that must work with zero Lua resources
  // loaded, the same reasoning /api/players itself follows.
  impl_->server.Post("/api/players/name", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
    if (!req.has_param("player")) {
      res.status = 400;
      return;
    }
    const int player_id = std::atoi(req.get_param_value("player").c_str());
    std::string_view name = req.body;
    while (!name.empty() && (name.front() == ' ' || name.front() == '\n' ||
                             name.front() == '\r' || name.front() == '\t')) {
      name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\n' ||
                             name.back() == '\r' || name.back() == '\t')) {
      name.remove_suffix(1);
    }
    // A display name has no business being longer than this; capping here
    // means every downstream consumer (JSON encoding, in-game HUD text) can
    // assume a sane bound without re-checking.
    constexpr std::size_t kMaxNameLength = 32;
    if (name.size() > kMaxNameLength) {
      name = name.substr(0, kMaxNameLength);
    }
    if (!name.empty() && player_id != 0 && impl_->player_name_handler) {
      impl_->player_name_handler(player_id, name);
    }
    res.set_content("{\"ok\":true}", "application/json");
  });

  std::error_code ec;
  if (std::filesystem::exists(web_console_dir, ec)) {
    impl_->server.set_mount_point("/", web_console_dir.string());
  }
}

AdminHttpServer::~AdminHttpServer() { Stop(); }

void AdminHttpServer::SetAppearanceHandlers(AppearanceStoreHandler store,
                                            AppearanceFetchHandler fetch,
                                            AppearanceRosterProvider roster) {
  impl_->appearance_store = std::move(store);
  impl_->appearance_fetch = std::move(fetch);
  impl_->appearance_roster = std::move(roster);
}

void AdminHttpServer::SetPropHandlers(PropAddHandler add,
                                      PropRemoveHandler remove,
                                      PropQueryHandler query) {
  impl_->prop_add = std::move(add);
  impl_->prop_remove = std::move(remove);
  impl_->prop_query = std::move(query);
}

void AdminHttpServer::SetConsoleExecHandler(ConsoleExecHandler handler) {
  impl_->exec_handler = std::move(handler);
}

void AdminHttpServer::SetSettingsProvider(SettingsProvider provider) {
  impl_->settings_provider = std::move(provider);
}

void AdminHttpServer::SetMetricsProvider(MetricsProvider provider) {
  impl_->metrics_provider = std::move(provider);
}

void AdminHttpServer::SetPlayersProvider(PlayersProvider provider) {
  impl_->players_provider = std::move(provider);
}

void AdminHttpServer::SetPlayerNameHandler(PlayerNameHandler handler) {
  impl_->player_name_handler = std::move(handler);
}

void AdminHttpServer::SetNuiFileProvider(NuiFileProvider provider) {
  impl_->nui_file_provider = std::move(provider);
}

void AdminHttpServer::QueueClientEvent(const std::string& event,
                                       const std::string& json_args,
                                       const std::string& target) {
  impl_->outbound.Push(event, json_args, target);
}

bool AdminHttpServer::Start(int port) {
  // Refuse a port someone is already listening on, BEFORE trying to bind.
  //
  // bind_to_port cannot be trusted to report this on Windows. httplib's
  // default_socket_options sets SO_REUSEADDR *and* SO_EXCLUSIVEADDRUSE on
  // the same socket; those contradict each other, the second setsockopt
  // fails and its return value is discarded, and the socket is left merely
  // SO_REUSEADDR - which on Windows (unlike Linux) lets a SECOND process
  // bind a port that is already being listened on. Both processes then
  // believe they own it and connections land on whichever the OS picks.
  //
  // Measured, not assumed: with two clients running, both logged "dev
  // console listening on 127.0.0.1:27180" - so commands typed into one
  // client's console were executed by the other process.
  //
  // A connect probe is used rather than only fixing the socket options
  // because it answers the question that actually matters ("is anyone
  // serving here?") without depending on bind's platform semantics.
  {
    httplib::Client probe("127.0.0.1", port);
    probe.set_connection_timeout(0, 250000);  // 250ms; loopback or nothing
    probe.set_read_timeout(0, 250000);
    if (probe.Get("/api/resources")) {
      return false;
    }
  }

  // Belt and braces: ask for exclusive ownership WITHOUT SO_REUSEADDR, so a
  // second bind fails honestly even if the probe raced with another start.
  impl_->server.set_socket_options([](auto sock) {
    int opt = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    // On POSIX, SO_REUSEADDR does NOT permit stealing a live listener; it
    // only allows rebinding through TIME_WAIT, which a restart needs.
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
  });

  if (!impl_->server.bind_to_port("0.0.0.0", port)) {
    return false;
  }
  impl_->thread = std::thread([this]() { impl_->server.listen_after_bind(); });
  return true;
}

void AdminHttpServer::Stop() {
  // Release parked long-polls first, or shutdown waits out their timeouts.
  impl_->outbound.Stop();
  if (impl_->server.is_running()) {
    impl_->server.stop();
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
}

}  // namespace skate3::lua_host
