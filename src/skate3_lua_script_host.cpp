#include "skate3_lua_script_host.h"

#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace skate3::lua_host {

namespace {

// Wall-clock milliseconds, the same base the log sink stamps its lines
// with. Used here only for relative age (how long a NUI callback has gone
// unanswered), so a clock step would at worst sweep one request early.
// Milliseconds since the host started, from a monotonic clock. This is
// what the thread scheduler measures waits against, so it must not be the
// wall clock: an NTP correction or a DST step would otherwise make every
// pending Wait fire at once or hang for an hour.
std::int64_t GameTimerMilliseconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

int Lua_GameTimer(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(GameTimerMilliseconds()));
  return 1;
}

std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Moved up from its previous home beside kStateBagEvent: ResourceMetricsJson
// needs it and is defined earlier in the file than that block.
void AppendJsonString(std::string& out, std::string_view value) {
  out += '"';
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04x",
                        static_cast<unsigned char>(c));
          out += escape;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// OFF by default. A report of client frame time roughly doubling after this
// was added, on a build that turned out to still show the same cost with
// every individual measured cost in this file accounted for and none of
// them large enough on paper to explain it, is reason enough not to trust
// "it's only a couple of clock reads" and pay the cost unconditionally.
// Rather than keep guessing at the mechanism, this makes the entire
// per-resource CPU-time measurement opt-in - resmon's numbers are useful,
// not free, and asking for them (`resmon 1`) is now the moment that is
// decided, not process startup.
std::atomic<bool> g_metrics_enabled{false};

// resmon's stopwatch. Lives at every place a resource actually runs code -
// its thread scheduler tick, one event handler, one console command, one NUI
// callback - and folds the elapsed time straight into that resource's
// CURRENT-second accumulator on destruction, success or Lua error alike (a
// script that errors every frame still costs the time it took to fail).
//
// A no-op (no clock read at all, not even a call to steady_clock::now())
// while g_metrics_enabled is false, checked once per construction - the one
// branch this costs is not the thing that was under suspicion.
class ScopedResourceTimer {
 public:
  ScopedResourceTimer(double& accum_ms, std::uint64_t& accum_calls)
      : accum_ms_(accum_ms),
        accum_calls_(accum_calls),
        enabled_(g_metrics_enabled.load(std::memory_order_relaxed)),
        start_(enabled_ ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}) {}
  ~ScopedResourceTimer() {
    if (!enabled_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    accum_ms_ +=
        std::chrono::duration<double, std::milli>(elapsed).count();
    ++accum_calls_;
  }
  ScopedResourceTimer(const ScopedResourceTimer&) = delete;
  ScopedResourceTimer& operator=(const ScopedResourceTimer&) = delete;

 private:
  double& accum_ms_;
  std::uint64_t& accum_calls_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace

void ConsoleLogSink::Append(const std::string& resource, const std::string& text) {
  const std::int64_t now_ms = NowMilliseconds();
  std::lock_guard<std::mutex> lock(mutex_);
  lines_.push_back({next_index_++, resource, text, now_ms});
  while (lines_.size() > kMaxLines) {
    lines_.pop_front();
  }
}

std::vector<LogLine> ConsoleLogSink::ReadSince(std::uint64_t& since) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogLine> result;
  for (const LogLine& line : lines_) {
    if (line.index > since) {
      result.push_back(line);
    }
  }
  if (!result.empty()) {
    since = result.back().index;
  }
  return result;
}

namespace {

// Replaces the stock Lua print(): joins tostring() of each argument with
// tabs (matching stock print's own behavior) and appends the line to the
// host's ConsoleLogSink instead of stdout - skate3.exe is a WIN32-subsystem
// app, so stdout goes nowhere visible during normal play, and this is what
// actually makes print() show up in the dev console (and, dual-written,
// REXLOG). Upvalues: lightuserdata host, resource name string.
int Lua_Print(lua_State* L) {
  auto* host = static_cast<LuaScriptHost*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource = lua_tostring(L, lua_upvalueindex(2));
  const int argc = lua_gettop(L);
  std::string joined;
  for (int i = 1; i <= argc; ++i) {
    if (i > 1) {
      joined.push_back('\t');
    }
    luaL_tolstring(L, i, nullptr);
    joined += lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  host->log_sink().Append(resource, joined);
  return 0;
}

// Splits raw_args on whitespace into a vector of tokens, matching how
// FiveM's RegisterCommand args table is built from a typed console line.
std::vector<std::string> SplitArgs(std::string_view raw_args) {
  std::vector<std::string> tokens;
  std::size_t i = 0;
  while (i < raw_args.size()) {
    while (i < raw_args.size() && raw_args[i] == ' ') {
      ++i;
    }
    std::size_t start = i;
    while (i < raw_args.size() && raw_args[i] != ' ') {
      ++i;
    }
    if (i > start) {
      tokens.emplace_back(raw_args.substr(start, i - start));
    }
  }
  return tokens;
}

}  // namespace

LuaScriptHost::LuaScriptHost(std::filesystem::path resources_root, Side side)
    : resources_root_(std::move(resources_root)), side_(side) {}

LuaScriptHost::~LuaScriptHost() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [name, resource] : resources_) {
    if (resource.state == ResourceState::kStarted) {
      StopResourceLocked(name, resource);
    }
  }
}

void LuaScriptHost::RegisterResourceNative(const char* name,
                                           int (*fn)(lua_State*)) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Kept in its own list rather than flagged inside natives_: the two are
  // installed with different closure shapes (see CreateResourceLuaState).
  resource_natives_.push_back({name, fn});
}

void LuaScriptHost::RegisterNative(const char* name, int (*fn)(lua_State*)) {
  std::lock_guard<std::mutex> lock(mutex_);
  natives_.push_back({name, fn});
}

void LuaScriptHost::SetCommandHooks(CommandRegisteredFn on_registered,
                                   CommandUnregisteredFn on_unregistered) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_command_registered_ = std::move(on_registered);
  on_command_unregistered_ = std::move(on_unregistered);
}

void LuaScriptHost::DiscoverResources() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  if (!std::filesystem::exists(resources_root_, ec)) {
    return;
  }
  for (const auto& entry :
       std::filesystem::directory_iterator(resources_root_, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    const auto manifest = entry.path() / "sk8manifest.lua";
    if (!std::filesystem::exists(manifest, ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    resources_.emplace(name, Resource{});
  }
}

bool LuaScriptHost::EnsureResource(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return EnsureResourceLocked(name);
}

bool LuaScriptHost::RestartResource(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return EnsureResourceLocked(name);
}

bool LuaScriptHost::StopResource(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = resources_.find(name);
  if (it == resources_.end()) {
    return false;
  }
  StopResourceLocked(name, it->second);
  return true;
}

void LuaScriptHost::StopAllResources() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [name, resource] : resources_) {
    StopResourceLocked(name, resource);
  }
}

bool LuaScriptHost::EnsureResourceLocked(const std::string& name) {
  auto it = resources_.find(name);
  if (it == resources_.end()) {
    return false;
  }
  if (it->second.state == ResourceState::kStarted) {
    StopResourceLocked(name, it->second);
  }
  return StartResourceLocked(name, it->second);
}

namespace {

// Appends a manifest global that may be either a single string or an array
// of strings. Both spellings are read (e.g. "client_script" and
// "client_scripts") so a manifest can use whichever reads better.
void CollectManifestList(lua_State* L, const char* field,
                         std::vector<std::string>& out) {
  lua_getglobal(L, field);
  if (lua_isstring(L, -1)) {
    out.emplace_back(lua_tostring(L, -1));
  } else if (lua_istable(L, -1)) {
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(L, -1, i);
      if (lua_isstring(L, -1)) {
        out.emplace_back(lua_tostring(L, -1));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}

std::string ReadManifestString(lua_State* L, const char* field) {
  lua_getglobal(L, field);
  std::string value;
  if (lua_isstring(L, -1)) {
    value = lua_tostring(L, -1);
  }
  lua_pop(L, 1);
  return value;
}

}  // namespace

ResourceManifest LuaScriptHost::ReadManifest(const std::string& name) const {
  ResourceManifest manifest;
  const auto manifest_path = resources_root_ / name / "sk8manifest.lua";
  lua_State* manifest_L = luaL_newstate();
  if (manifest_L == nullptr) {
    return manifest;
  }
  // No libraries are opened: a manifest is data, and running it with the
  // standard library available would let it touch the filesystem or spawn
  // processes just by being loaded.
  if (luaL_dofile(manifest_L, manifest_path.string().c_str()) == LUA_OK) {
    CollectManifestList(manifest_L, "shared_script", manifest.shared_scripts);
    CollectManifestList(manifest_L, "shared_scripts", manifest.shared_scripts);
    CollectManifestList(manifest_L, "client_script", manifest.client_scripts);
    CollectManifestList(manifest_L, "client_scripts", manifest.client_scripts);
    CollectManifestList(manifest_L, "server_script", manifest.server_scripts);
    CollectManifestList(manifest_L, "server_scripts", manifest.server_scripts);
    CollectManifestList(manifest_L, "file", manifest.files);
    CollectManifestList(manifest_L, "files", manifest.files);
    manifest.ui_page = ReadManifestString(manifest_L, "ui_page");
  } else {
    std::fprintf(stderr, "[lua] %s: manifest error: %s\n", name.c_str(),
                lua_tostring(manifest_L, -1));
  }
  lua_close(manifest_L);
  return manifest;
}

std::vector<std::string> LuaScriptHost::ClientFacingScripts(
    const ResourceManifest& manifest) {
  std::vector<std::string> files = manifest.shared_scripts;
  files.insert(files.end(), manifest.client_scripts.begin(),
               manifest.client_scripts.end());
  return files;
}

lua_State* LuaScriptHost::CreateResourceLuaState(const std::string& name) {
  lua_State* L = luaL_newstate();
  if (L == nullptr) {
    return nullptr;
  }
  luaL_openlibs(L);

  // Built-in RegisterCommand(name, fn) - upvalues: lightuserdata host,
  // resource name string.
  lua_pushlightuserdata(L, this);
  lua_pushstring(L, name.c_str());
  lua_pushcclosure(L, &LuaScriptHost::RegisterCommand_Impl, 2);
  lua_setglobal(L, "RegisterCommand");

  // Built-in print() override, same upvalue shape - see Lua_Print's comment.
  lua_pushlightuserdata(L, this);
  lua_pushstring(L, name.c_str());
  lua_pushcclosure(L, &Lua_Print, 2);
  lua_setglobal(L, "print");

  // Event natives, all sharing RegisterCommand's upvalue shape.
  static constexpr struct {
    const char* name;
    lua_CFunction fn;
  } kEventNatives[] = {
      {"AddEventHandler", &LuaScriptHost::AddEventHandler_Impl},
      {"RegisterNetEvent", &LuaScriptHost::RegisterNetEvent_Impl},
      {"TriggerEvent", &LuaScriptHost::TriggerEvent_Impl},
      {"TriggerServerEvent", &LuaScriptHost::TriggerServerEvent_Impl},
      {"TriggerClientEvent", &LuaScriptHost::TriggerClientEvent_Impl},
  };
  for (const auto& native : kEventNatives) {
    lua_pushlightuserdata(L, this);
    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, native.fn, 2);
    lua_setglobal(L, native.name);
  }

  // NUI callbacks are a client-side concept: the dedicated server has no
  // browser to call back into, and registering the native there would only
  // let a server script silently collect refs nothing will ever invoke.
  if (side_ == Side::kClient) {
    lua_pushlightuserdata(L, this);
    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, &LuaScriptHost::RegisterNuiCallback_Impl, 2);
    lua_setglobal(L, "RegisterNUICallback");
  }

  // Threads and timing. The scheduler itself is Lua, because that is where
  // coroutines live and the whole thing is about them; C++ only supplies a
  // clock and calls __skate_tick once per frame (client) or loop iteration
  // (server) from LuaScriptHost::Tick.
  lua_pushcfunction(L, &Lua_GameTimer);
  lua_setglobal(L, "__skate_now");

  // resmon, as a built-in rather than a client-only native: it is exactly as
  // useful on the dedicated server, and every resource already gets its own
  // independent lua_State, so there is no per-side wiring to duplicate.
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaScriptHost::GetResourceMetrics_Impl, 1);
  lua_setglobal(L, "GetResourceMetrics");

  static constexpr char kThreadShim[] = R"lua(
    local now = __skate_now
    __skate_now = nil

    Skate = Skate or {}
    Skate.GetGameTimer = now

    -- Threads waiting to run, each { co = coroutine, wake = timer value }.
    local running = {}
    -- Threads created DURING a tick land here first: resuming a thread can
    -- create more, and appending to the list being iterated would either
    -- run them a frame early or skip one entirely.
    local pending = {}

    -- Resumes one thread and returns when it next wants to run, or nil if
    -- it finished or died. A thread that errors is dropped with its
    -- traceback logged: one bad thread must not take the resource with it.
    local function step(co)
      local ok, wait_ms = coroutine.resume(co)
      if not ok then
        print('thread error: ' .. tostring(wait_ms))
        print(debug.traceback(co))
        return nil
      end
      if coroutine.status(co) == 'dead' then
        return nil
      end
      return now() + (tonumber(wait_ms) or 0)
    end

    -- Skate.CreateThread(fn): runs fn as a coroutine. The body starts
    -- IMMEDIATELY and runs until its first Skate.Wait, matching FiveM -
    -- so a thread that never waits never returns control, exactly as there.
    function Skate.CreateThread(fn)
      if type(fn) ~= 'function' then
        error('Skate.CreateThread expects a function', 2)
      end
      local co = coroutine.create(fn)
      local wake = step(co)
      if wake then
        pending[#pending + 1] = { co = co, wake = wake }
      end
      return co
    end

    -- Skate.Wait(ms): yields the calling thread for at least ms (0 = next
    -- tick). Resolution is one tick, so a wait is never shorter than
    -- requested but can overshoot by a frame.
    function Skate.Wait(ms)
      if not coroutine.isyieldable() then
        error('Skate.Wait may only be called inside a Skate.CreateThread thread', 2)
      end
      return coroutine.yield(ms or 0)
    end

    -- Skate.TimeOut(ms, fn): runs fn once, ms from now.
    function Skate.TimeOut(ms, fn)
      if type(fn) ~= 'function' then
        error('Skate.TimeOut expects (milliseconds, function)', 2)
      end
      return Skate.CreateThread(function()
        Skate.Wait(ms)
        fn()
      end)
    end

    function __skate_tick()
      local t = now()
      local live = {}
      for i = 1, #running do
        local entry = running[i]
        if entry.wake <= t then
          local wake = step(entry.co)
          if wake then
            entry.wake = wake
            live[#live + 1] = entry
          end
        else
          live[#live + 1] = entry
        end
      end
      -- Threads spawned during this tick start on the NEXT one, so a
      -- thread that spawns in a loop cannot starve the frame.
      for i = 1, #pending do
        live[#live + 1] = pending[i]
      end
      pending = {}
      running = live
    end
  )lua";
  if (luaL_dostring(L, kThreadShim) != LUA_OK) {
    std::fprintf(stderr, "[lua] %s: thread shim failed: %s\n", name.c_str(),
                 lua_tostring(L, -1));
    lua_pop(L, 1);
  }

  // Export plumbing: two natives, then a Lua shim that gives them FiveM's
  // shape. Written as Lua rather than hand-built metatables in C because the
  // shape is entirely about __call/__index behaviour, which reads far more
  // clearly here.
  lua_pushlightuserdata(L, this);
  lua_pushstring(L, name.c_str());
  lua_pushcclosure(L, &LuaScriptHost::RegisterExport_Impl, 2);
  lua_setglobal(L, "__RegisterExport");
  lua_pushlightuserdata(L, this);
  lua_pushstring(L, name.c_str());
  lua_pushcclosure(L, &LuaScriptHost::CallExport_Impl, 2);
  lua_setglobal(L, "__CallExport");

  // State bags.
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaScriptHost::StateBagSet_Impl, 1);
  lua_setglobal(L, "__StateBagSet");
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaScriptHost::StateBagGet_Impl, 1);
  lua_setglobal(L, "__StateBagGet");
  lua_pushlightuserdata(L, this);
  lua_pushstring(L, name.c_str());
  lua_pushcclosure(L, &LuaScriptHost::AddStateBagChangeHandler_Impl, 2);
  lua_setglobal(L, "AddStateBagChangeHandler");

  static constexpr char kStateBagShim[] = R"lua(
    local set, get = __StateBagSet, __StateBagGet
    __StateBagSet, __StateBagGet = nil, nil

    -- A bag reads and writes like a plain table:
    --   GlobalState.round = 3
    --   Player(7).state.letters = 'SKA'
    -- and :set(key, value, replicated) exists for the explicit form.
    local function bag(name)
      return setmetatable({}, {
        __index = function(_, key)
          if key == 'set' then
            return function(_, k, v, replicated)
              set(name, k, v, replicated ~= false)
            end
          end
          return get(name, key)
        end,
        __newindex = function(_, key, value) set(name, key, value, true) end,
      })
    end

    GlobalState = bag('global')

    -- Player(id).state. Cached so repeated lookups return the same object.
    local player_bags = {}
    function Player(id)
      id = tonumber(id) or 0
      local cached = player_bags[id]
      if not cached then
        cached = { source = id, state = bag('player:' .. id) }
        player_bags[id] = cached
      end
      return cached
    end
  )lua";
  if (luaL_dostring(L, kStateBagShim) != LUA_OK) {
    std::fprintf(stderr, "[lua] %s: state bag shim failed: %s\n", name.c_str(),
                lua_tostring(L, -1));
    lua_pop(L, 1);
  }

  static constexpr char kExportsShim[] = R"lua(
    local register, call = __RegisterExport, __CallExport
    __RegisterExport, __CallExport = nil, nil
    -- exports('name', fn)            -> declare one
    -- exports.Other:name(a, b)       -> call another resource's
    exports = setmetatable({}, {
      __call = function(_, name, fn) register(name, fn) end,
      __index = function(_, resource)
        return setmetatable({}, {
          __index = function(_, method)
            -- Called with ':' so the proxy arrives as the first argument;
            -- it is dropped, since the target expects only real arguments.
            return function(_, ...) return call(resource, method, ...) end
          end,
        })
      end,
    })
  )lua";
  if (luaL_dostring(L, kExportsShim) != LUA_OK) {
    std::fprintf(stderr, "[lua] %s: exports shim failed: %s\n", name.c_str(),
                lua_tostring(L, -1));
    lua_pop(L, 1);
  }

  for (const auto& native : natives_) {
    lua_pushcfunction(L, native.fn);
    lua_setglobal(L, native.name.c_str());
  }
  // Resource-aware natives get the same two upvalues the built-ins above
  // use, so they can tell which resource is calling them - see
  // RegisterResourceNative's own comment for the contract.
  for (const auto& native : resource_natives_) {
    lua_pushlightuserdata(L, this);
    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, native.fn, 2);
    lua_setglobal(L, native.name.c_str());
  }
  return L;
}

bool LuaScriptHost::StartResourceLocked(const std::string& name,
                                       Resource& resource) {
  const ResourceManifest manifest = ReadManifest(name);
  std::vector<std::string> scripts = manifest.shared_scripts;
  const std::vector<std::string>& side_scripts =
      side_ == Side::kClient ? manifest.client_scripts : manifest.server_scripts;
  scripts.insert(scripts.end(), side_scripts.begin(), side_scripts.end());

  lua_State* L = CreateResourceLuaState(name);
  if (L == nullptr) {
    return false;
  }

  // One state shared by every script in the resource, run in manifest
  // order, so a later script sees what an earlier one defined.
  for (const std::string& script_file : scripts) {
    const auto script_path = resources_root_ / name / script_file;
    if (luaL_dofile(L, script_path.string().c_str()) != LUA_OK) {
      std::fprintf(stderr, "[lua] %s/%s: %s\n", name.c_str(),
                  script_file.c_str(), lua_tostring(L, -1));
      lua_close(L);
      return false;
    }
  }

  resource.L = L;
  resource.state = ResourceState::kStarted;
  // Announced after the state flips, so the resource that just started sees
  // its own start event alongside everyone else.
  DispatchEventLocked("onResourceStart", "[\"" + name + "\"]",
                      /*network_only=*/false, /*source=*/0);
  return true;
}

bool LuaScriptHost::EnsureResourceFromSources(
    const std::string& name, const std::vector<ScriptChunk>& chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  Resource& resource = resources_[name];
  if (resource.state == ResourceState::kStarted) {
    StopResourceLocked(name, resource);
  }

  lua_State* L = CreateResourceLuaState(name);
  if (L == nullptr) {
    return false;
  }
  for (const ScriptChunk& chunk : chunks) {
    const std::string chunk_name = "@" + name + "/" + chunk.chunk_name;
    if (luaL_loadbuffer(L, chunk.source.data(), chunk.source.size(),
                        chunk_name.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
      std::fprintf(stderr, "[lua] %s/%s: %s\n", name.c_str(),
                  chunk.chunk_name.c_str(), lua_tostring(L, -1));
      lua_close(L);
      return false;
    }
  }

  resource.L = L;
  resource.state = ResourceState::kStarted;
  DispatchEventLocked("onResourceStart", "[\"" + name + "\"]",
                      /*network_only=*/false, /*source=*/0);
  return true;
}

std::vector<ClientScriptInfo> LuaScriptHost::ListClientScripts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ClientScriptInfo> result;
  for (const auto& [name, resource] : resources_) {
    // STARTED resources only. This list is what a connecting client fetches
    // and runs, so including stopped ones made server.cfg's `ensure` list
    // apply to the server and be ignored by every client: a player joined
    // and started every resource sitting on disk, debug commands and
    // overlapping NUI pages included, no matter what the config said.
    if (resource.state != ResourceState::kStarted) {
      continue;
    }
    const ResourceManifest manifest = ReadManifest(name);
    std::vector<std::string> files = ClientFacingScripts(manifest);
    if (files.empty() && manifest.ui_page.empty()) {
      continue;
    }
    // ui_page is implicitly servable even when the manifest did not repeat
    // it in `files` - FiveM treats it that way and every real manifest
    // relies on it.
    std::vector<std::string> ui_files = manifest.files;
    if (!manifest.ui_page.empty() &&
        std::find(ui_files.begin(), ui_files.end(), manifest.ui_page) ==
            ui_files.end()) {
      ui_files.push_back(manifest.ui_page);
    }
    result.push_back(
        {name, std::move(files), manifest.ui_page, std::move(ui_files)});
  }
  return result;
}

std::optional<std::string> LuaScriptHost::ReadResourceFile(
    const std::string& name, const std::string& relative_path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (resources_.find(name) == resources_.end() || relative_path.empty()) {
    return std::nullopt;
  }
  // This backs an HTTP route, so the resolved path has to be proven to stay
  // inside the resource's own directory rather than merely look like it
  // does - "a/../../secret" passes any naive textual check.
  std::error_code ec;
  const auto resource_root =
      std::filesystem::weakly_canonical(resources_root_ / name, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto resolved =
      std::filesystem::weakly_canonical(resources_root_ / name / relative_path, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto relative = resolved.lexically_relative(resource_root);
  if (relative.empty() || *relative.begin() == "..") {
    return std::nullopt;
  }
  std::ifstream stream(resolved, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

void LuaScriptHost::StopResourceLocked(const std::string& name,
                                      Resource& resource) {
  if (resource.state != ResourceState::kStarted) {
    return;
  }
  // Announced while the resource is still running, so it can still clean up
  // in its own handler.
  DispatchEventLocked("onResourceStop", "[\"" + name + "\"]",
                      /*network_only=*/false, /*source=*/0);
  if (on_command_unregistered_) {
    for (const auto& command : resource.commands) {
      on_command_unregistered_(command);
    }
  }
  resource.commands.clear();
  // The refs live in the state that is about to be closed, so they need no
  // unref - but the tables must not outlive it and be reused on restart.
  resource.event_handlers.clear();
  resource.net_events.clear();
  resource.exports.clear();
  resource.nui_callbacks.clear();
  if (resource.L != nullptr) {
    lua_close(resource.L);
    resource.L = nullptr;
  }
  resource.state = ResourceState::kStopped;
}

std::vector<ResourceStatus> LuaScriptHost::ListResources() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ResourceStatus> result;
  result.reserve(resources_.size());
  for (const auto& [name, resource] : resources_) {
    result.push_back({name, resource.state});
  }
  return result;
}

void LuaScriptHost::Tick() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Drives every started resource's thread scheduler (see kThreadShim).
  // Runs with mutex_ held, the same as every other path that enters Lua -
  // a thread calling TriggerEvent or a state-bag setter relies on exactly
  // that, since those natives assume the lock is already taken.
  for (auto& [name, resource] : resources_) {
    if (resource.state != ResourceState::kStarted || resource.L == nullptr) {
      continue;
    }
    lua_State* L = resource.L;
    {
      ScopedResourceTimer timer(resource.metrics_accum_ms,
                                resource.metrics_accum_calls);
      lua_getglobal(L, "__skate_tick");
      if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);  // shim missing (it failed to load); nothing to drive.
        continue;
      }
      if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        // The scheduler itself already contains every thread's errors, so
        // reaching here means the scheduler broke rather than a script.
        log_sink_.Append(name, std::string("thread scheduler error: ") +
                                   lua_tostring(L, -1));
        std::fprintf(stderr, "[lua] %s: thread scheduler error: %s\n",
                     name.c_str(), lua_tostring(L, -1));
        lua_pop(L, 1);
      }
    }
  }
  // Was sampling lua_gc() here too, once per resource per call - measured
  // after a report that the client's frame cost went up post-resmon, and
  // fixed by moving it into the once-a-second rollover below instead. The
  // reasoning that put it here ("Tick already visits every resource on a
  // steady cadence") was true but beside the point: last_second_ms/
  // memory_kb are read once a second regardless of how often they are
  // WRITTEN, so sampling every frame was doing up to 60x the necessary
  // work (60fps x every resource x two lua_gc calls) for a number nothing
  // reads that often. One sample per resource per second is enough.
  RollResourceMetricsLocked();
}

void LuaScriptHost::RollResourceMetricsLocked() {
  if (!g_metrics_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  const std::int64_t now = NowMilliseconds();
  if (now - last_metrics_rollover_ms_ < 1000) {
    return;
  }
  last_metrics_rollover_ms_ = now;
  for (auto& [name, resource] : resources_) {
    (void)name;
    resource.metrics.last_second_ms = resource.metrics_accum_ms;
    resource.metrics.last_second_calls = resource.metrics_accum_calls;
    resource.metrics.lifetime_ms += resource.metrics_accum_ms;
    resource.metrics.lifetime_calls += resource.metrics_accum_calls;
    resource.metrics_accum_ms = 0.0;
    resource.metrics_accum_calls = 0;
    if (resource.state == ResourceState::kStarted && resource.L != nullptr) {
      resource.metrics.memory_kb = lua_gc(resource.L, LUA_GCCOUNT, 0) +
                                   lua_gc(resource.L, LUA_GCCOUNTB, 0) / 1024.0;
    }
  }
}

void LuaScriptHost::SetMetricsEnabled(bool enabled) {
  g_metrics_enabled.store(enabled, std::memory_order_relaxed);
}

bool LuaScriptHost::MetricsEnabled() {
  return g_metrics_enabled.load(std::memory_order_relaxed);
}

std::unordered_map<std::string, ResourceMetrics>
LuaScriptHost::ResourceMetricsSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_map<std::string, ResourceMetrics> result;
  result.reserve(resources_.size());
  for (const auto& [name, resource] : resources_) {
    result.emplace(name, resource.metrics);
  }
  return result;
}

std::string LuaScriptHost::ResourceMetricsJson() const {
  const auto snapshot = ResourceMetricsSnapshot();
  // Sorted by name for a stable, diffable dump - the map itself has no
  // useful order, and a resmon table that reshuffles every second is hard
  // to read.
  std::vector<std::string> names;
  names.reserve(snapshot.size());
  for (const auto& [name, metrics] : snapshot) {
    (void)metrics;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  std::string json = "[";
  bool first = true;
  for (const std::string& name : names) {
    const ResourceMetrics& metrics = snapshot.at(name);
    if (!first) {
      json += ",";
    }
    first = false;
    json += "{\"name\":";
    AppendJsonString(json, name);
    char numbers[192];
    std::snprintf(numbers, sizeof(numbers),
                 ",\"msLastSecond\":%.3f,\"callsLastSecond\":%llu,"
                 "\"msLifetime\":%.3f,\"callsLifetime\":%llu,"
                 "\"memoryKb\":%.1f}",
                 metrics.last_second_ms,
                 static_cast<unsigned long long>(metrics.last_second_calls),
                 metrics.lifetime_ms,
                 static_cast<unsigned long long>(metrics.lifetime_calls),
                 metrics.memory_kb);
    json += numbers;
  }
  json += "]";
  return json;
}

std::string LuaScriptHost::FormatResourceMetricsTable() const {
  const auto snapshot = ResourceMetricsSnapshot();
  std::vector<std::pair<std::string, ResourceMetrics>> rows(snapshot.begin(),
                                                            snapshot.end());
  // Worst first, like FiveM's own resmon - the point of the table is
  // spotting the resource actually worth investigating, not reading every
  // row to find it.
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.last_second_ms > b.second.last_second_ms;
  });
  double total_ms = 0.0;
  for (const auto& [name, metrics] : rows) {
    (void)name;
    total_ms += metrics.last_second_ms;
  }
  std::ostringstream out;
  out << "resource                        memory(KB)   ms/s   calls/s\n";
  char line[160];
  for (const auto& [name, metrics] : rows) {
    std::snprintf(line, sizeof(line), "%-30s  %10.1f  %6.3f  %8llu\n",
                 name.c_str(), metrics.memory_kb, metrics.last_second_ms,
                 static_cast<unsigned long long>(metrics.last_second_calls));
    out << line;
  }
  std::snprintf(line, sizeof(line), "%d resource(s), %.3f ms/s total",
               static_cast<int>(rows.size()), total_ms);
  out << line;
  return out.str();
}

namespace {

// Internal event name used to replicate state-bag changes. Not a script
// event: the receiving host applies it directly, so a resource never has to
// (and never should) RegisterNetEvent it.
constexpr char kStateBagEvent[] = "__stateBagChange";

// Serialises one Lua value. Sequences become JSON arrays and other tables
// become objects; values that cannot survive a network hop (functions,
// userdata, coroutines) become null rather than failing the whole send.
void EncodeLuaValue(lua_State* L, int index, std::string& out, int depth) {
  // A table that contains itself would otherwise recurse forever.
  if (depth > 16) {
    out += "null";
    return;
  }
  switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
      out += lua_toboolean(L, index) ? "true" : "false";
      break;
    case LUA_TNUMBER: {
      char text[40];
      if (lua_isinteger(L, index)) {
        std::snprintf(text, sizeof(text), "%lld",
                      static_cast<long long>(lua_tointeger(L, index)));
      } else {
        std::snprintf(text, sizeof(text), "%.14g",
                      static_cast<double>(lua_tonumber(L, index)));
      }
      out += text;
      break;
    }
    case LUA_TSTRING: {
      std::size_t length = 0;
      const char* text = lua_tolstring(L, index, &length);
      AppendJsonString(out, std::string_view(text, length));
      break;
    }
    case LUA_TTABLE: {
      const int table = lua_absindex(L, index);
      const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, table));
      if (count > 0) {
        out += '[';
        for (lua_Integer i = 1; i <= count; ++i) {
          if (i > 1) {
            out += ',';
          }
          lua_rawgeti(L, table, i);
          EncodeLuaValue(L, -1, out, depth + 1);
          lua_pop(L, 1);
        }
        out += ']';
      } else {
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, table) != 0) {
          // Only string keys are emitted, and the type is checked BEFORE
          // lua_tolstring: that call converts a number key in place, which
          // would corrupt the key lua_next uses to find the next entry.
          if (lua_type(L, -2) == LUA_TSTRING) {
            if (!first) {
              out += ',';
            }
            first = false;
            std::size_t length = 0;
            const char* key = lua_tolstring(L, -2, &length);
            AppendJsonString(out, std::string_view(key, length));
            out += ':';
            EncodeLuaValue(L, -1, out, depth + 1);
          }
          lua_pop(L, 1);
        }
        out += '}';
      }
      break;
    }
    default:
      out += "null";
      break;
  }
}

std::string EncodeLuaArgs(lua_State* L, int first_index) {
  std::string out = "[";
  const int top = lua_gettop(L);
  for (int i = first_index; i <= top; ++i) {
    if (i > first_index) {
      out += ',';
    }
    EncodeLuaValue(L, i, out, 0);
  }
  out += ']';
  return out;
}

void SkipJsonWhitespace(std::string_view text, std::size_t& i) {
  while (i < text.size() &&
         std::isspace(static_cast<unsigned char>(text[i]))) {
    ++i;
  }
}

bool ParseJsonValue(lua_State* L, std::string_view text, std::size_t& i,
                    int depth);

bool ParseJsonString(std::string_view text, std::size_t& i, std::string& out) {
  if (i >= text.size() || text[i] != '"') {
    return false;
  }
  ++i;
  while (i < text.size() && text[i] != '"') {
    if (text[i] == '\\' && i + 1 < text.size()) {
      ++i;
      switch (text[i]) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          // Only the BMP subset our own encoder emits (control chars).
          if (i + 4 >= text.size()) {
            return false;
          }
          const std::string hex(text.substr(i + 1, 4));
          out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
          i += 4;
          break;
        }
        default: out += text[i]; break;
      }
      ++i;
    } else {
      out += text[i++];
    }
  }
  if (i >= text.size()) {
    return false;
  }
  ++i;  // closing quote
  return true;
}

bool ParseJsonValue(lua_State* L, std::string_view text, std::size_t& i,
                    int depth) {
  if (depth > 16) {
    return false;
  }
  SkipJsonWhitespace(text, i);
  if (i >= text.size()) {
    return false;
  }
  const char c = text[i];
  if (c == '"') {
    std::string value;
    if (!ParseJsonString(text, i, value)) {
      return false;
    }
    lua_pushlstring(L, value.data(), value.size());
    return true;
  }
  if (c == '[' || c == '{') {
    const bool is_array = c == '[';
    const char close = is_array ? ']' : '}';
    ++i;
    lua_newtable(L);
    lua_Integer next_index = 1;
    while (true) {
      SkipJsonWhitespace(text, i);
      if (i >= text.size()) {
        return false;
      }
      if (text[i] == close) {
        ++i;
        return true;
      }
      if (text[i] == ',') {
        ++i;
        continue;
      }
      if (is_array) {
        if (!ParseJsonValue(L, text, i, depth + 1)) {
          return false;
        }
        lua_rawseti(L, -2, next_index++);
      } else {
        std::string key;
        if (!ParseJsonString(text, i, key)) {
          return false;
        }
        SkipJsonWhitespace(text, i);
        if (i >= text.size() || text[i] != ':') {
          return false;
        }
        ++i;
        if (!ParseJsonValue(L, text, i, depth + 1)) {
          return false;
        }
        lua_setfield(L, -2, key.c_str());
      }
    }
  }
  if (text.compare(i, 4, "true") == 0) {
    i += 4;
    lua_pushboolean(L, 1);
    return true;
  }
  if (text.compare(i, 5, "false") == 0) {
    i += 5;
    lua_pushboolean(L, 0);
    return true;
  }
  if (text.compare(i, 4, "null") == 0) {
    i += 4;
    lua_pushnil(L);
    return true;
  }
  // Number.
  const std::size_t start = i;
  while (i < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '-' ||
          text[i] == '+' || text[i] == '.' || text[i] == 'e' ||
          text[i] == 'E')) {
    ++i;
  }
  if (i == start) {
    return false;
  }
  const std::string number(text.substr(start, i - start));
  if (number.find_first_of(".eE") == std::string::npos) {
    lua_pushinteger(L, std::strtoll(number.c_str(), nullptr, 10));
  } else {
    lua_pushnumber(L, std::strtod(number.c_str(), nullptr));
  }
  return true;
}

// Pushes each element of a JSON array as a separate Lua value. Returns how
// many were pushed.
// Pushes one decoded JSON value (a NUI callback's POST body is an
// arbitrary value, not the argument ARRAY the event path uses). Pushes nil
// for empty or malformed input, so a handler always sees exactly one
// argument and never has to guard against a missing one.
void PushJsonValue(lua_State* L, const std::string& json) {
  std::size_t i = 0;
  SkipJsonWhitespace(json, i);
  if (i >= json.size() || !ParseJsonValue(L, json, i, 0)) {
    lua_pushnil(L);
  }
}

int PushJsonArgs(lua_State* L, const std::string& json) {
  std::size_t i = 0;
  SkipJsonWhitespace(json, i);
  if (i >= json.size() || json[i] != '[') {
    return 0;
  }
  ++i;
  int pushed = 0;
  while (true) {
    SkipJsonWhitespace(json, i);
    if (i >= json.size() || json[i] == ']') {
      break;
    }
    if (json[i] == ',') {
      ++i;
      continue;
    }
    if (!ParseJsonValue(L, json, i, 0)) {
      break;
    }
    ++pushed;
  }
  return pushed;
}

}  // namespace

std::string EncodeLuaValueToJson(lua_State* L, int index) {
  std::string out;
  EncodeLuaValue(L, index, out, 0);
  return out;
}

void LuaScriptHost::SetEventOutboundHandler(EventOutboundFn handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_event_outbound_ = std::move(handler);
}

void LuaScriptHost::DispatchNetworkEvent(const std::string& event,
                                         const std::string& json_args,
                                         int source) {
  std::lock_guard<std::mutex> lock(mutex_);
  DispatchEventLocked(event, json_args, /*network_only=*/true, source);
}

void LuaScriptHost::DispatchLocalEvent(const std::string& event,
                                       const std::string& json_args) {
  std::lock_guard<std::mutex> lock(mutex_);
  DispatchEventLocked(event, json_args, /*network_only=*/false, /*source=*/0);
}

// Splits the ["bag","key",value] payload a replicated bag change carries.
bool DecodeStateBagPayload(const std::string& json_args, std::string& bag,
                           std::string& key, std::string& json_value) {
  std::size_t i = 0;
  SkipJsonWhitespace(json_args, i);
  if (i >= json_args.size() || json_args[i] != '[') {
    return false;
  }
  ++i;
  SkipJsonWhitespace(json_args, i);
  if (!ParseJsonString(json_args, i, bag)) {
    return false;
  }
  SkipJsonWhitespace(json_args, i);
  if (i < json_args.size() && json_args[i] == ',') {
    ++i;
  }
  SkipJsonWhitespace(json_args, i);
  if (!ParseJsonString(json_args, i, key)) {
    return false;
  }
  SkipJsonWhitespace(json_args, i);
  if (i < json_args.size() && json_args[i] == ',') {
    ++i;
  }
  SkipJsonWhitespace(json_args, i);
  // The value is kept as raw JSON text; the store never decodes it until a
  // script actually reads or a handler fires.
  const std::size_t value_start = i;
  int depth = 0;
  bool in_string = false;
  for (; i < json_args.size(); ++i) {
    const char c = json_args[i];
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
    } else if (c == '[' || c == '{') {
      ++depth;
    } else if (c == '}' || (c == ']' && depth > 0)) {
      --depth;
    } else if (c == ']' && depth == 0) {
      break;
    }
  }
  json_value = json_args.substr(value_start, i - value_start);
  return !json_value.empty();
}

bool LuaScriptHost::TryApplyStateBagEvent(const std::string& event,
                                          const std::string& json_args) {
  if (event != kStateBagEvent) {
    return false;
  }
  std::string bag;
  std::string key;
  std::string json_value;
  if (DecodeStateBagPayload(json_args, bag, key, json_value)) {
    ApplyRemoteStateBagChange(bag, key, json_value);
  }
  return true;
}

void LuaScriptHost::DispatchEventLocked(const std::string& event,
                                        const std::string& json_args,
                                        bool network_only, int source) {
  // Iterate over a snapshot of the names rather than the map itself: a
  // handler is free to start or stop a resource, which would rehash
  // resources_ and invalidate an iterator held across the call.
  std::vector<std::string> names;
  names.reserve(resources_.size());
  for (const auto& [name, resource] : resources_) {
    (void)resource;
    names.push_back(name);
  }
  for (const std::string& name : names) {
    const auto entry = resources_.find(name);
    if (entry == resources_.end()) {
      continue;
    }
    Resource& resource = entry->second;
    if (resource.state != ResourceState::kStarted || resource.L == nullptr) {
      continue;
    }
    if (network_only && resource.net_events.find(event) ==
                            resource.net_events.end()) {
      continue;
    }
    const auto handlers = resource.event_handlers.find(event);
    if (handlers == resource.event_handlers.end()) {
      continue;
    }
    lua_State* L = resource.L;
    // `source` is a global for the duration of the dispatch, matching what
    // FiveM scripts expect, and cleared afterwards so it can never be read
    // as a stale value by an unrelated later call.
    if (source != 0) {
      lua_pushinteger(L, source);
      lua_setglobal(L, "source");
    }
    // Copied: a handler may add or remove handlers for this same event.
    const std::vector<int> refs = handlers->second;
    ScopedResourceTimer timer(resource.metrics_accum_ms,
                              resource.metrics_accum_calls);
    for (const int ref : refs) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
      if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      const int argument_count =
          json_args.empty() ? 0 : PushJsonArgs(L, json_args);
      if (lua_pcall(L, argument_count, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "[lua] event '%s': %s\n", event.c_str(),
                    lua_tostring(L, -1));
        lua_pop(L, 1);
      }
    }
    if (source != 0) {
      lua_pushnil(L);
      lua_setglobal(L, "source");
    }
  }
}

// The five event natives below all run with mutex_ ALREADY HELD: every path
// that enters Lua (StartResourceLocked, InvokeCommand, DispatchEventLocked)
// takes the lock first, and mutex_ is not recursive. They must therefore
// never lock, and must only call the *Locked helpers.
int LuaScriptHost::AddEventHandler_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource_name = lua_tostring(L, lua_upvalueindex(2));
  const char* event = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  auto it = host->resources_.find(resource_name);
  if (it != host->resources_.end()) {
    it->second.event_handlers[event].push_back(ref);
  }
  return 0;
}

int LuaScriptHost::RegisterNetEvent_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource_name = lua_tostring(L, lua_upvalueindex(2));
  const char* event = luaL_checkstring(L, 1);
  auto it = host->resources_.find(resource_name);
  if (it != host->resources_.end()) {
    it->second.net_events.insert(event);
    // The two-argument form registers the handler at the same time, which
    // is how most FiveM scripts are written.
    if (lua_isfunction(L, 2)) {
      lua_pushvalue(L, 2);
      it->second.event_handlers[event].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    }
  }
  return 0;
}

int LuaScriptHost::TriggerEvent_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string event = luaL_checkstring(L, 1);
  host->DispatchEventLocked(event, EncodeLuaArgs(L, 2), /*network_only=*/false,
                            /*source=*/0);
  return 0;
}

int LuaScriptHost::TriggerServerEvent_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string event = luaL_checkstring(L, 1);
  if (host->on_event_outbound_) {
    host->on_event_outbound_(event, EncodeLuaArgs(L, 2), std::string());
  }
  return 0;
}

int LuaScriptHost::TriggerClientEvent_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string event = luaL_checkstring(L, 1);
  // FiveM's signature is (event, target, ...): -1 means every client.
  std::string target;
  if (lua_isstring(L, 2) || lua_isnumber(L, 2)) {
    target = lua_tostring(L, 2);
    if (target == "-1") {
      target.clear();
    }
  }
  if (host->on_event_outbound_) {
    host->on_event_outbound_(event, EncodeLuaArgs(L, 3), target);
  }
  return 0;
}

void LuaScriptHost::SetStateBagLocked(const std::string& bag,
                                      const std::string& key,
                                      const std::string& json_value,
                                      bool replicate) {
  state_bags_[bag][key] = json_value;

  // Handlers see (bagName, key, value, reserved, replicated), matching the
  // argument order FiveM's own AddStateBagChangeHandler callbacks take.
  std::vector<std::string> names;
  names.reserve(resources_.size());
  for (const auto& [name, resource] : resources_) {
    (void)resource;
    names.push_back(name);
  }
  for (const std::string& name : names) {
    const auto entry = resources_.find(name);
    if (entry == resources_.end() ||
        entry->second.state != ResourceState::kStarted ||
        entry->second.L == nullptr) {
      continue;
    }
    lua_State* L = entry->second.L;
    const auto handlers = entry->second.state_bag_handlers;
    for (const auto& handler : handlers) {
      if (!handler.key_filter.empty() && handler.key_filter != key) {
        continue;
      }
      if (!handler.bag_filter.empty() && handler.bag_filter != bag) {
        continue;
      }
      lua_rawgeti(L, LUA_REGISTRYINDEX, handler.callback_ref);
      if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      lua_pushlstring(L, bag.data(), bag.size());
      lua_pushlstring(L, key.data(), key.size());
      std::size_t consumed = 0;
      if (!ParseJsonValue(L, json_value, consumed, 0)) {
        lua_pushnil(L);
      }
      lua_pushnil(L);  // reserved, for signature compatibility
      lua_pushboolean(L, replicate ? 1 : 0);
      if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "[lua] state bag '%s.%s': %s\n", bag.c_str(),
                    key.c_str(), lua_tostring(L, -1));
        lua_pop(L, 1);
      }
    }
  }

  // Replication rides the same channel as events; the receiving host applies
  // it through ApplyRemoteStateBagChange rather than as a script event.
  if (replicate && on_event_outbound_) {
    std::string payload = "[";
    AppendJsonString(payload, bag);
    payload += ",";
    AppendJsonString(payload, key);
    payload += "," + (json_value.empty() ? "null" : json_value) + "]";
    on_event_outbound_(kStateBagEvent, payload, std::string());
  }
}

void LuaScriptHost::ApplyRemoteStateBagChange(const std::string& bag,
                                              const std::string& key,
                                              const std::string& json_value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // replicate=false: this change came FROM the network, so echoing it back
  // would bounce between the two sides forever.
  SetStateBagLocked(bag, key, json_value, /*replicate=*/false);
}

int LuaScriptHost::StateBagSet_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string bag = luaL_checkstring(L, 1);
  const std::string key = luaL_checkstring(L, 2);
  std::string json_value;
  EncodeLuaValue(L, 3, json_value, 0);
  const bool replicate = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4) != 0;
  host->SetStateBagLocked(bag, key, json_value, replicate);
  return 0;
}

int LuaScriptHost::StateBagGet_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string bag = luaL_checkstring(L, 1);
  const std::string key = luaL_checkstring(L, 2);
  const auto found_bag = host->state_bags_.find(bag);
  if (found_bag == host->state_bags_.end()) {
    lua_pushnil(L);
    return 1;
  }
  const auto found_key = found_bag->second.find(key);
  if (found_key == found_bag->second.end()) {
    lua_pushnil(L);
    return 1;
  }
  std::size_t consumed = 0;
  if (!ParseJsonValue(L, found_key->second, consumed, 0)) {
    lua_pushnil(L);
  }
  return 1;
}

int LuaScriptHost::AddStateBagChangeHandler_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource_name = lua_tostring(L, lua_upvalueindex(2));
  // (keyFilter, bagFilter, handler) - either filter may be nil for "any".
  const std::string key_filter = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
  const std::string bag_filter = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  luaL_checktype(L, 3, LUA_TFUNCTION);
  lua_pushvalue(L, 3);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  auto it = host->resources_.find(resource_name);
  if (it != host->resources_.end()) {
    it->second.state_bag_handlers.push_back({key_filter, bag_filter, ref});
  }
  return 0;
}

int LuaScriptHost::RegisterExport_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource_name = lua_tostring(L, lua_upvalueindex(2));
  const char* export_name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  auto it = host->resources_.find(resource_name);
  if (it != host->resources_.end()) {
    // Re-registering the same name replaces the previous function rather
    // than leaking its reference.
    const auto existing = it->second.exports.find(export_name);
    if (existing != it->second.exports.end()) {
      luaL_unref(L, LUA_REGISTRYINDEX, existing->second);
    }
    lua_pushvalue(L, 2);
    it->second.exports[export_name] = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  return 0;
}

// Calls another resource's export. The two resources have SEPARATE lua_States,
// so arguments and results cannot be passed directly - they are marshalled
// through the same JSON encoding events use, which also means an export can
// only exchange values that survive that trip.
int LuaScriptHost::CallExport_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string target_resource = luaL_checkstring(L, 1);
  const std::string export_name = luaL_checkstring(L, 2);

  const auto target = host->resources_.find(target_resource);
  if (target == host->resources_.end() ||
      target->second.state != ResourceState::kStarted ||
      target->second.L == nullptr) {
    return luaL_error(L, "exports.%s: resource is not running",
                      target_resource.c_str());
  }
  const auto entry = target->second.exports.find(export_name);
  if (entry == target->second.exports.end()) {
    return luaL_error(L, "exports.%s: no export named '%s'",
                      target_resource.c_str(), export_name.c_str());
  }

  const std::string json_args = EncodeLuaArgs(L, 3);
  lua_State* target_L = target->second.L;
  const int base = lua_gettop(target_L);
  lua_rawgeti(target_L, LUA_REGISTRYINDEX, entry->second);
  if (!lua_isfunction(target_L, -1)) {
    lua_settop(target_L, base);
    return luaL_error(L, "exports.%s: '%s' is not callable",
                      target_resource.c_str(), export_name.c_str());
  }
  const int argument_count = PushJsonArgs(target_L, json_args);
  if (lua_pcall(target_L, argument_count, LUA_MULTRET, 0) != LUA_OK) {
    const std::string message = lua_tostring(target_L, -1)
                                    ? lua_tostring(target_L, -1)
                                    : "unknown error";
    lua_settop(target_L, base);
    return luaL_error(L, "exports.%s:%s: %s", target_resource.c_str(),
                      export_name.c_str(), message.c_str());
  }
  // Results come back the same way they went in.
  const std::string json_results = EncodeLuaArgs(target_L, base + 1);
  lua_settop(target_L, base);
  return PushJsonArgs(L, json_results);
}

// RegisterNUICallback(name, function(data, cb) ... end)
int LuaScriptHost::RegisterNuiCallback_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource = lua_tostring(L, lua_upvalueindex(2));
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  const int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  // Runs during the resource's own script load, which happens with mutex_
  // already held - the same invariant every other registration native here
  // relies on. See the note above the event natives.
  auto it = host->resources_.find(resource);
  if (it == host->resources_.end()) {
    luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
    return 0;
  }
  auto& callbacks = it->second.nui_callbacks;
  const auto existing = callbacks.find(name);
  if (existing != callbacks.end()) {
    // Re-registering the same name replaces the handler; the old ref would
    // otherwise leak for the life of the resource.
    luaL_unref(L, LUA_REGISTRYINDEX, existing->second);
    existing->second = callback_ref;
  } else {
    callbacks.emplace(name, callback_ref);
  }
  return 0;
}

// The `cb` argument handed to a NUI callback handler.
int LuaScriptHost::NuiRespond_Impl(lua_State* L) {
  auto* host =
      static_cast<LuaScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
  const auto id =
      static_cast<std::uint64_t>(lua_tointeger(L, lua_upvalueindex(2)));
  // A handler may answer with nothing at all (cb()), which the page sees as
  // an empty object.
  std::string json = "{}";
  if (lua_gettop(L) >= 1 && !lua_isnoneornil(L, 1)) {
    json.clear();
    EncodeLuaValue(L, 1, json, 0);
  }

  std::function<void(const std::string&)> respond;
  {
    std::lock_guard<std::mutex> lock(host->nui_mutex_);
    const auto it = host->nui_pending_.find(id);
    if (it == host->nui_pending_.end()) {
      return 0;  // already answered, or swept for taking too long.
    }
    respond = std::move(it->second.respond);
    host->nui_pending_.erase(it);
  }
  if (respond) {
    respond(json);
  }
  return 0;
}

bool LuaScriptHost::InvokeNuiCallback(
    const std::string& resource, const std::string& name,
    const std::string& json_body,
    std::function<void(const std::string&)> respond) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto resource_it = resources_.find(resource);
  if (resource_it == resources_.end() ||
      resource_it->second.state != ResourceState::kStarted ||
      resource_it->second.L == nullptr) {
    return false;
  }
  const auto callback_it = resource_it->second.nui_callbacks.find(name);
  if (callback_it == resource_it->second.nui_callbacks.end()) {
    return false;
  }

  std::uint64_t id = 0;
  {
    std::lock_guard<std::mutex> nui_lock(nui_mutex_);
    id = next_nui_response_id_++;
    nui_pending_.emplace(
        id, PendingNuiResponse{std::move(respond), NowMilliseconds()});
  }

  lua_State* L = resource_it->second.L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback_it->second);
  PushJsonValue(L, json_body);
  lua_pushlightuserdata(L, this);
  lua_pushinteger(L, static_cast<lua_Integer>(id));
  lua_pushcclosure(L, &LuaScriptHost::NuiRespond_Impl, 2);
  ScopedResourceTimer timer(resource_it->second.metrics_accum_ms,
                            resource_it->second.metrics_accum_calls);
  if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
    log_sink_.Append(resource, std::string("NUI callback '") + name +
                                   "' error: " + lua_tostring(L, -1));
    std::fprintf(stderr, "[lua] %s: NUI callback '%s' error: %s\n",
                 resource.c_str(), name.c_str(), lua_tostring(L, -1));
    lua_pop(L, 1);
    // The handler died before it could answer; drop the pending entry and
    // report failure so the caller answers the page itself rather than
    // leaving its fetch() hanging on a callback that will never come.
    std::lock_guard<std::mutex> nui_lock(nui_mutex_);
    nui_pending_.erase(id);
    return false;
  }
  return true;
}

void LuaScriptHost::SweepStaleNuiCallbacks(std::int64_t timeout_ms) {
  std::vector<std::function<void(const std::string&)>> expired;
  {
    std::lock_guard<std::mutex> lock(nui_mutex_);
    const std::int64_t now = NowMilliseconds();
    for (auto it = nui_pending_.begin(); it != nui_pending_.end();) {
      if (now - it->second.created_ms < timeout_ms) {
        ++it;
        continue;
      }
      expired.push_back(std::move(it->second.respond));
      it = nui_pending_.erase(it);
    }
  }
  // Answered outside the lock: respond() reaches into CEF, and holding a
  // lock across that would invite a deadlock with the invoke path.
  for (auto& respond : expired) {
    if (respond) {
      respond("{}");
    }
  }
}

void LuaScriptHost::InvokeCommand(const std::string& resource,
                                 int callback_ref, std::string_view raw_args) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = resources_.find(resource);
  if (it == resources_.end() || it->second.state != ResourceState::kStarted ||
      it->second.L == nullptr) {
    return;
  }
  lua_State* L = it->second.L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_pushinteger(L, -1);  // source: -1 == console, matching FiveM convention.
  lua_newtable(L);
  int index = 1;
  for (const auto& token : SplitArgs(raw_args)) {
    lua_pushlstring(L, token.data(), token.size());
    lua_rawseti(L, -2, index++);
  }
  lua_pushlstring(L, raw_args.data(), raw_args.size());  // rawCommand
  ScopedResourceTimer timer(it->second.metrics_accum_ms,
                            it->second.metrics_accum_calls);
  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    std::fprintf(stderr, "[lua] %s: %s\n", resource.c_str(),
                lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

int LuaScriptHost::GetResourceMetrics_Impl(lua_State* L) {
  auto* host = static_cast<LuaScriptHost*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  // ResourceMetricsSnapshot takes host->mutex_ itself - safe to call from
  // here even though this native runs WITH that lock already held (every
  // Lua entry point does), because it is a plain std::mutex and this is
  // still the thread that holds it: a second lock_guard on the same mutex,
  // from the same thread, deadlocks a script the first time it calls this.
  // So this reads the map directly instead, which is fine: the caller is
  // already inside the same critical section RollResourceMetricsLocked
  // itself runs in.
  lua_newtable(L);
  for (const auto& [name, resource] : host->resources_) {
    lua_newtable(L);
    lua_pushnumber(L, resource.metrics.last_second_ms);
    lua_setfield(L, -2, "msLastSecond");
    lua_pushinteger(L, static_cast<lua_Integer>(
                           resource.metrics.last_second_calls));
    lua_setfield(L, -2, "callsLastSecond");
    lua_pushnumber(L, resource.metrics.lifetime_ms);
    lua_setfield(L, -2, "msLifetime");
    lua_pushinteger(L,
                    static_cast<lua_Integer>(resource.metrics.lifetime_calls));
    lua_setfield(L, -2, "callsLifetime");
    lua_pushnumber(L, resource.metrics.memory_kb);
    lua_setfield(L, -2, "memoryKb");
    lua_setfield(L, -2, name.c_str());
  }
  return 1;
}

int LuaScriptHost::RegisterCommand_Impl(lua_State* L) {
  auto* host = static_cast<LuaScriptHost*>(
      lua_touserdata(L, lua_upvalueindex(1)));
  const std::string resource_name = lua_tostring(L, lua_upvalueindex(2));
  const char* command_name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  const int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  host->RecordCommandRegistrationLocked(resource_name, command_name,
                                       callback_ref, L);
  return 0;
}

void LuaScriptHost::RecordCommandRegistrationLocked(
    const std::string& resource, const std::string& command,
    int callback_ref, lua_State* L) {
  auto it = resources_.find(resource);
  if (it != resources_.end()) {
    it->second.commands.push_back(command);
  }
  if (on_command_registered_) {
    on_command_registered_(resource, command, callback_ref, L);
  }
}

}  // namespace skate3::lua_host
