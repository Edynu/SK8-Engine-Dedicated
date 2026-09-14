#pragma once

// Dev-only, no-auth HTTP admin console for a LuaScriptHost: serves the
// built React/Tailwind/TS web console's static files plus a tiny JSON API
// to list/ensure/restart/stop resources. Shared between skate3.exe and
// skate3_dedicated.exe - rex-free like LuaScriptHost itself.

#include <filesystem>
#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <string_view>

namespace skate3::lua_host {

class LuaScriptHost;

class AdminHttpServer {
 public:
  // web_console_dir may not exist (e.g. the React app hasn't been built
  // yet) - the API still works, static file serving just 404s.
  AdminHttpServer(LuaScriptHost& host, std::filesystem::path web_console_dir);
  ~AdminHttpServer();

  AdminHttpServer(const AdminHttpServer&) = delete;
  AdminHttpServer& operator=(const AdminHttpServer&) = delete;

  // Where to accept connections from. Not a default argument: which of
  // these a process wants is a security decision, so both call sites have to
  // say it out loud.
  enum class Bind {
    // Loopback only. What a game CLIENT must use: its admin server exists
    // solely so the in-game CEF console and NUI pages can reach it over
    // 127.0.0.1, and it exposes POST /api/console/exec, which runs console
    // commands. Bound to every interface that is remote command execution
    // offered to anyone who can reach the player's machine.
    kLoopbackOnly,
    // Every interface. What the DEDICATED SERVER needs, because connecting
    // clients fetch resources, appearances and script events from it over
    // the network. Note this still has no authentication: see the security
    // notes in docs/.
    kAllInterfaces,
  };

  // How much a request has to prove before the admin-only routes will serve
  // it.
  enum class AdminAccess {
    // Loopback is trusted, or any caller presenting the token. What a
    // DEDICATED SERVER wants: the owner's dashboard on the server box works
    // with no configuration, and a remote admin sets sv_token.
    kLoopbackOrToken,
    // The token, always, even from loopback. What a game CLIENT must use,
    // and the reason this enum exists: a resource's NUI page is
    // server-supplied HTML and JavaScript served from the client's own
    // http://127.0.0.1:<admin port> origin. Loopback is therefore not
    // evidence of anything on a client - it is exactly where a hostile
    // server's script runs - and a same-origin fetch('/api/console/exec')
    // from a NUI page would otherwise be honoured. The client generates a
    // random token at startup and hands it only to its own dev-console
    // page, which no resource page can read.
    kTokenOnly,
  };

  // Shared secret for the admin-only routes (console exec, console log,
  // starting/stopping resources). Requests a connecting client legitimately
  // makes - appearances, script events, its own name, fetching resources -
  // are never gated, so this never stops anyone from playing. A caller sends
  // it as an X-Skate3-Token header, or as ?token= where a header cannot be
  // set. With no token set and kLoopbackOrToken, those routes answer to
  // loopback only; with no token set and kTokenOnly, they answer to nobody.
  void SetAdminToken(std::string token, AdminAccess access);

  // Starts listening on its own background thread. Returns false if the
  // port could not be bound.
  bool Start(int port, Bind bind);
  void Stop();

  // Dispatches a raw typed console line (e.g. "teleport 0 5 0"). This class
  // stays rex-free (shared with skate3_dedicated, which never links
  // rex::cvar), so the actual rex::cvar::GetFlagInfo/command_callback
  // dispatch - identical to what the backtick ConsoleDialog already does -
  // is injected from client-only code (skate3_lua_client_natives.cpp)
  // instead of implemented here. POST /api/console/exec calls this if set;
  // if unset (e.g. on skate3_dedicated, which has no console to dispatch
  // into), the route just echoes the line without acting on it.
  using ConsoleExecHandler = std::function<void(std::string_view line)>;
  void SetConsoleExecHandler(ConsoleExecHandler handler);

  // Supplies GET /api/players' body: a JSON array the caller has already
  // encoded. Same injection shape as the exec handler, and for the same
  // reason - the player registry (skate3_dedicated only; skate3.exe has no
  // such list to report) lives in relay code this class does not link.
  // Unset means "[]".
  // Supplies GET /api/settings: the server's own settings as JSON, so a
  // client can apply them without a script being involved. Unset means "{}".
  using SettingsProvider = std::function<std::string()>;
  void SetSettingsProvider(SettingsProvider provider);

  using PlayersProvider = std::function<std::string()>;
  void SetPlayersProvider(PlayersProvider provider);

  // Supplies GET /api/metrics: bandwidth, hitches and Lua resmon as one JSON
  // object, so a dashboard has one thing to poll rather than stitching a
  // relay-side gauge together with LuaScriptHost's own. Same injection
  // shape as the providers above - the caller builds the JSON (it has the
  // counters this class does not link) and this class only serves it.
  // Unset means "{}".
  using MetricsProvider = std::function<std::string()>;
  void SetMetricsProvider(MetricsProvider provider);

  // Serves the appearance store (skate3_appearance_store.h), which lives in
  // relay code this class does not link - hence the same injection shape as
  // the providers above.
  //
  //   POST /api/appearance/<id>?role=N   body is the raw blob
  //   GET  /api/appearance/<id>          the blob, or 404
  //   GET  /api/appearance               role -> id, as JSON
  //
  // HTTP rather than the UDP fanout it replaces: one upload instead of one
  // per peer, no chunking or bitmaps, and a joining client fetches when IT
  // is ready rather than hoping the sender transmits at the right moment.
  //
  // `store` returns false when the blob is unusable or the store refused it.
  // `fetch` returns an empty vector for an unknown id.
  using AppearanceStoreHandler =
      std::function<bool(std::uint64_t appearance_id, std::uint32_t role,
                         std::vector<std::uint8_t> bytes)>;
  using AppearanceFetchHandler =
      std::function<std::vector<std::uint8_t>(std::uint64_t appearance_id)>;
  // Answers "who can `viewer_role` currently see, and what are they
  // wearing" - the server decides visibility, not the client. `since` is the
  // version the caller last saw; with `wait` the provider parks until the
  // answer actually differs from it, so a join, an outfit change or a peer
  // skating into range reaches the client immediately instead of on a poll.
  using AppearanceRosterProvider = std::function<std::string(
      std::uint32_t viewer_role, std::uint64_t since, bool wait)>;
  void SetAppearanceHandlers(AppearanceStoreHandler store,
                             AppearanceFetchHandler fetch,
                             AppearanceRosterProvider roster);

  // Points one role at the appearance another role is already wearing, without
  // transferring any bytes - the store is content-hash keyed, so this is a
  // rebind, not a copy.
  //
  // Exists for the load-test harness: its virtual players replay a recorded
  // UDP stream and never upload an appearance, so without this they render as
  // nothing and a crowd test measures a crowd of invisible people. Pointing
  // them all at a real client's outfit makes them draw.
  //
  // Deliberately NOT how real clients get dressed - they upload their own.
  // Returns false if the source role has no appearance stored.
  using AppearanceCloneHandler =
      std::function<bool(std::uint32_t from_role, std::uint32_t to_role)>;
  void SetAppearanceCloneHandler(AppearanceCloneHandler clone);

  // Serves the networked prop store (skate3_prop_store.h), which lives in
  // relay code this class does not link.
  //
  //   POST /api/props            body: path|x|y|z|lod   -> {"id":N}
  //   GET  /api/props?x=&y=&z=&r=   props near a point, as JSON
  //   POST /api/props/remove     body: id
  //
  // Deciding which geometry a client loads is engine work - it runs against
  // every player's position, must survive a script restart, and has nothing
  // to do with the rules of a game mode. Hence C++ and not a Lua resource.
  using PropAddHandler = std::function<std::uint32_t(
      const std::string& path, float x, float y, float z, float lod)>;
  using PropRemoveHandler = std::function<bool(std::uint32_t id)>;
  // `since` is the store version the client already has. When it is present
  // the handler WAITS for the store to change past it before answering, so
  // a placement reaches every nearby client immediately instead of on each
  // one's next poll. Absent, it answers straight away - the debug view.
  using PropQueryHandler = std::function<std::string(
      float x, float y, float z, float radius, bool all,
      std::uint64_t since, bool wait)>;
  void SetPropHandlers(PropAddHandler add, PropRemoveHandler remove,
                       PropQueryHandler query);

  // Handles POST /api/players/name?player=<id> (raw text body = the name).
  // Same injection shape and reason as PlayersProvider - the player
  // registry this actually writes to lives in relay code this class does
  // not link. This class only trims whitespace and caps length before
  // calling the handler; anything past that (rejecting empty names,
  // notifying other clients) is the handler's job.
  using PlayerNameHandler =
      std::function<void(int player_id, std::string_view name)>;
  void SetPlayerNameHandler(PlayerNameHandler handler);

  // Serves GET /nui/<path>: the NUI layer's root document and every
  // resource's UI files. Returns false if the path is unknown (404).
  //
  // Served over plain HTTP rather than from the custom https:// scheme
  // handler that backs their callbacks, so the root document and every
  // resource page share one ordinary origin and no cross-origin rules
  // apply between them.
  using NuiFileProvider =
      std::function<bool(const std::string& path, std::string& out_data,
                         std::string& out_mime)>;
  void SetNuiFileProvider(NuiFileProvider provider);

  // Queues a script event for connected clients to collect on their next
  // poll of GET /api/events. Wired up by the owner rather than in the
  // constructor, because the client embeds this class too and routes its
  // own outbound events to the server instead of queueing them locally.
  // `target` empty means every client.
  void QueueClientEvent(const std::string& event, const std::string& json_args,
                        const std::string& target);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace skate3::lua_host
