#pragma once

// Shared Lua 5.4 resource host, embedded by both skate3.exe and
// skate3_dedicated.exe. Deliberately independent of rex::*/rendering: the
// dedicated server links this without pulling in the rexglue-sdk build
// graph. Client-only native bindings (console-command wiring, guest-memory
// natives) live in skate3_lua_client_natives.cpp instead.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace skate3::lua_host {

// One line captured from a resource's print() (or "" resource for
// console-echoed/engine-sourced lines). Consumed by the CEF dev console's
// polling API (skate3_lua_admin_http.cpp's /api/console/log).
struct LogLine {
  std::uint64_t index = 0;
  std::string resource;
  std::string text;
  std::int64_t timestamp_ms = 0;
};

// Small mutex-guarded ring buffer shared by every resource's print()
// override and the dev-console HTTP polling route. Rex-free like the rest
// of this file - shared by both skate3.exe and skate3_dedicated.exe.
class ConsoleLogSink {
 public:
  void Append(const std::string& resource, const std::string& text);
  // Returns every line with index > since, and advances since to the
  // returned lines' highest index (or leaves it unchanged if there were no
  // new lines) - callers pass the same variable back in on the next poll.
  std::vector<LogLine> ReadSince(std::uint64_t& since) const;

 private:
  static constexpr std::size_t kMaxLines = 2000;

  mutable std::mutex mutex_;
  std::deque<LogLine> lines_;
  std::uint64_t next_index_ = 1;
};

enum class Side { kClient, kServer };

enum class ResourceState { kStopped, kStarted };

struct ResourceStatus {
  std::string name;
  ResourceState state = ResourceState::kStopped;
};

// A parsed sk8manifest.lua.
//
// Named sk8manifest rather than FiveM's fxmanifest on purpose: the file
// format is close enough that FiveM's own tooling would happily rewrite one
// of these, and the syntax is NOT the same - fields here are assignments
// (client_scripts = { "client.lua" }), not FiveM's call form
// (client_script "client.lua"). A manifest runs with no libraries loaded,
// so the call form invokes a nil global and fails the whole manifest.
//
// Manifests are executed as real Lua rather than
// pattern-matched, so every field accepts either the singular string form
// or the plural table form (client_script = "a.lua" and
// client_scripts = {"a.lua", "b.lua"} are both valid, and both land here).
struct ResourceManifest {
  // Load order within a side is shared first, then side-specific - shared
  // scripts exist to define things the others depend on.
  std::vector<std::string> shared_scripts;
  std::vector<std::string> client_scripts;
  std::vector<std::string> server_scripts;
  // NUI entry point, relative to the resource directory.
  std::string ui_page;
  // Extra assets (html/css/js/images) the resource wants served alongside
  // ui_page. Declaring a file here is what makes it fetchable.
  std::vector<std::string> files;
};

// A resource's client-visible payload, for the server's dev-console HTTP
// API to advertise to connecting clients (see skate3_lua_admin_http.cpp and
// skate3_lua_client_natives.cpp's SyncResourcesFromServer).
struct ClientScriptInfo {
  std::string name;
  // shared_scripts followed by client_scripts, in the order they must run.
  std::vector<std::string> files;
  std::string ui_page;
  // The manifest's own `files` list (plus ui_page, which is implicitly
  // servable). These are the NUI assets - html/css/js/images - the client
  // must fetch alongside the scripts before it can show the page, since
  // client-side resources live only in memory and never touch its disk.
  std::vector<std::string> ui_files;
};

// One loaded script chunk: its display name (used as the Lua chunk name, so
// it shows up in error messages) and its source text.
struct ScriptChunk {
  std::string chunk_name;
  std::string source;
};

// A plain C function exposed as a Lua global on every resource VM. Used for
// natives that need no per-resource context (e.g. SetEntityPosition).
struct NativeBinding {
  std::string name;
  int (*fn)(lua_State*) = nullptr;
};

// Called when a script sends an event to the other side of the connection
// (TriggerServerEvent from a client, TriggerClientEvent from a server).
// `target` is empty for "the server" / "every client", otherwise it names a
// specific client. `json_args` is the argument list encoded as a JSON array.
//
// Injected rather than implemented here because this file is shared by the
// client and the dedicated server and must stay free of either one's
// networking; the two sides plug in different transports.
using EventOutboundFn =
    std::function<void(const std::string& event, const std::string& json_args,
                       const std::string& target)>;

// Invoked (still holding the host's internal lock - see .cpp) whenever a
// resource script calls RegisterCommand(name, fn). Client-only: this is
// where the console-command cvar gets wired up. `callback_ref` is a
// LUA_REGISTRYINDEX reference in the resource's own lua_State, valid until
// that resource is stopped/restarted.
using CommandRegisteredFn = std::function<void(
    const std::string& resource, const std::string& command,
    int callback_ref, lua_State* L)>;
// Invoked once per command a resource had registered, right before that
// resource's lua_State is closed (stop/restart). Client-only: unregisters
// the corresponding console cvar.
using CommandUnregisteredFn = std::function<void(const std::string& command)>;

// Encodes the Lua value at `index` as JSON, using exactly the rules the
// event path uses (sequences become arrays, other tables become objects,
// unencodable values become null). Exposed because client-only natives -
// SendNUIMessage in particular - need to hand a script's table to
// something outside Lua and must agree with the rest of the host on what
// that means.
std::string EncodeLuaValueToJson(lua_State* L, int index);

// Per-resource cost, in the FiveM 'resmon' sense: how much CPU each
// resource used, and how much Lua memory it is holding. Measured, not
// estimated - see the ScopedResourceTimer call sites in the .cpp for the
// four places a resource actually runs (its thread scheduler tick, an
// event handler, a console command, a NUI callback).
//
// `last_second_ms` is a ROLLED-OVER value: it is the total from the
// previous full second, not a live accumulator, so it never reads as a
// partial number that happens to catch the reader between two updates.
struct ResourceMetrics {
  double last_second_ms = 0.0;
  std::uint64_t last_second_calls = 0;
  double lifetime_ms = 0.0;
  std::uint64_t lifetime_calls = 0;
  // KB, straight from lua_gc(L, LUA_GCCOUNT) - the resource's own state,
  // since every resource owns an independent lua_State. No attribution
  // problem: this number IS that resource's memory, not a share of a pool.
  double memory_kb = 0.0;
};

class LuaScriptHost {
 public:
  LuaScriptHost(std::filesystem::path resources_root, Side side);
  ~LuaScriptHost();

  LuaScriptHost(const LuaScriptHost&) = delete;
  LuaScriptHost& operator=(const LuaScriptHost&) = delete;

  // Must be called before any resource is started - applied to every
  // resource's lua_State at creation time.
  void RegisterNative(const char* name, int (*fn)(lua_State*));

  // Same, for a native that needs to know WHICH resource called it -
  // SendNUIMessage has to address that resource's own page, for instance.
  // The function is installed as a closure with the two upvalues every
  // built-in here uses:
  //   lua_upvalueindex(1)  lightuserdata, this LuaScriptHost
  //   lua_upvalueindex(2)  string, the resource name
  // Callers that only need the name can read upvalue 2 and ignore 1.
  void RegisterResourceNative(const char* name, int (*fn)(lua_State*));

  void SetCommandHooks(CommandRegisteredFn on_registered,
                       CommandUnregisteredFn on_unregistered);

  // Where TriggerServerEvent / TriggerClientEvent send to. Without one set,
  // those natives are silently local-only.
  void SetEventOutboundHandler(EventOutboundFn handler);

  // Applies a state-bag change that arrived from the network. Kept separate
  // from DispatchNetworkEvent because bag replication is internal plumbing:
  // it must NOT require a resource to opt in with RegisterNetEvent the way
  // a script-authored event does.
  void ApplyRemoteStateBagChange(const std::string& bag, const std::string& key,
                                 const std::string& json_value);

  // If `event` is the internal state-bag replication event, applies it and
  // returns true. Transports call this before DispatchNetworkEvent so bag
  // traffic never reaches script event handlers.
  bool TryApplyStateBagEvent(const std::string& event,
                             const std::string& json_args);

  // Delivers an event that arrived from the network to every resource that
  // both declared it with RegisterNetEvent and has a handler for it. The
  // RegisterNetEvent gate is what stops a peer from invoking arbitrary
  // internal events. `json_args` is a JSON array, or empty for no args.
  //
  // `source` is the sender's player id (the relay-assigned role), exposed to
  // the handler as a global named `source` for the duration of the call -
  // the FiveM convention. 0 means "not from a specific player".
  void DispatchNetworkEvent(const std::string& event,
                            const std::string& json_args, int source);

  // Fires an event the ENGINE raised, as though a local script had called
  // TriggerEvent. Unlike DispatchNetworkEvent there is no RegisterNetEvent
  // gate: that gate exists to stop a remote peer reaching internal events,
  // and nothing remote is involved here. `json_args` is a JSON array.
  void DispatchLocalEvent(const std::string& event,
                          const std::string& json_args);

  // Scans resources_root for immediate subdirectories containing a
  // sk8manifest.lua. Populates the resource table (all initially stopped).
  // Call once at startup before Ensure/Restart/Stop/ListResources are used.
  void DiscoverResources();

  // FiveM "ensure" semantics: start if stopped, or stop+start if running.
  bool EnsureResource(const std::string& name);
  bool RestartResource(const std::string& name);
  bool StopResource(const std::string& name);
  void StopAllResources();
  std::vector<ResourceStatus> ListResources() const;

  // Parses a resource's sk8manifest.lua. Re-read from disk on every call so
  // an edit takes effect on the next restart with no caching to invalidate.
  ResourceManifest ReadManifest(const std::string& name) const;

  // Server-side only in practice: every discovered resource's client-visible
  // payload, for advertising to connecting clients.
  std::vector<ClientScriptInfo> ListClientScripts() const;

  // Reads one file belonging to a resource. `relative_path` is resolved
  // inside the resource's own directory and REFUSED if it escapes it, since
  // this backs an HTTP route that hands files to whoever asks.
  std::optional<std::string> ReadResourceFile(
      const std::string& name, const std::string& relative_path) const;

  // Client-side only in practice: starts (or restarts) a resource directly
  // from in-memory Lua source received over the network, bypassing the
  // on-disk sk8manifest/script-file lookup that EnsureResource/StartResource
  // use. Chunks run in the order given. Used when resources are fetched
  // from a connected server rather than discovered locally (see
  // SyncResourcesFromServer).
  bool EnsureResourceFromSources(const std::string& name,
                                 const std::vector<ScriptChunk>& chunks);

  // Called once per client render frame / once per server loop iteration.
  void Tick();

  // Called by the client's console-command adapter when a user types a
  // registered command's name in the backtick console.
  void InvokeCommand(const std::string& resource, const std::string& command,
                     int callback_ref,
                     std::string_view raw_args);

  // Runs a RegisterNUICallback handler in `resource`. `json_body` is the
  // page's POST body (an arbitrary JSON value, decoded and handed to the
  // handler as its first argument); `respond` is the second - the FiveM
  // `cb` - and may be called from inside the handler or kept and called
  // much later. Returns false if no such handler exists, in which case
  // `respond` is never called and the caller must answer the page itself.
  //
  // Safe to call from any thread (it takes the host's own lock), which
  // matters because this is driven from a CEF thread.
  bool InvokeNuiCallback(const std::string& resource, const std::string& name,
                         const std::string& json_body,
                         std::function<void(const std::string&)> respond);

  // Answers any NUI callback whose `cb` has gone unclaimed for longer than
  // `timeout_ms`, with an empty object. Without this a handler that forgets
  // to call cb leaves the page's fetch() pending forever - and holds the
  // browser-side request object alive with it. Call periodically from
  // Tick's owner.
  void SweepStaleNuiCallbacks(std::int64_t timeout_ms);

  // Calls a Lua function a resource registered earlier, by its registry ref.
  //
  // The general form of what InvokeCommand and InvokeNuiCallback each do for
  // their own case, for natives that take a callback and call it back later -
  // world markers being the first. `json_args` is encoded the same way event
  // arguments are, so a handler receives real Lua values; `context` names the
  // call in an error report ("marker 3").
  //
  // Runs the callback as a scheduler thread, so it may Skate.Wait - which for
  // a marker matters: "teleport, wait, start a countdown" is the obvious thing
  // to write in one. Returns false if the resource or ref is gone.
  bool InvokeResourceCallback(const std::string& resource, int callback_ref,
                              const std::string& json_args,
                              const std::string& context);

  // Frees a ref a native took with luaL_ref. Callers that hand out callbacks
  // have to release them when whatever owned them goes away, or the ref (and
  // the closure behind it) lives as long as the resource.
  void ReleaseResourceCallback(const std::string& resource, int callback_ref);

  // Shared by every resource's print() override (see CreateResourceLuaState
  // in the .cpp) and the dev-console HTTP polling route.
  // Reports a script failure to the dev console (and stderr) in one entry:
  // what failed, where it was called from, and the traceback. `context` names
  // the entry point, e.g. "command 'skategame'" or "event 'x' handler"; it is
  // what turns "attempt to index a nil value" into something a script author
  // can act on without guessing which of their handlers ran.
  //
  // Public because the scheduler's own error native reports through it.
  void ReportScriptError(const std::string& resource, const std::string& context,
                         const std::string& detail);

  ConsoleLogSink& log_sink() { return log_sink_; }

  // Per-resource CPU time and memory, rolled over once a second inside
  // Tick(). Safe to call from any thread; takes the same lock everything
  // else here does.
  std::unordered_map<std::string, ResourceMetrics> ResourceMetricsSnapshot() const;
  // The same data as a JSON array, for the admin HTTP /api/metrics route
  // and the 'resmon' console command's Lua-facing twin.
  std::string ResourceMetricsJson() const;
  // A plain-text table for the 'resmon' console command: name, memory,
  // ms/last second, calls/last second, sorted by cost (worst first, like
  // FiveM's own resmon) so the resource actually worth investigating is
  // always at the top rather than found by scanning.
  std::string FormatResourceMetricsTable() const;

  // OFF at process start. Collection has a real, measured cost (a client
  // frame-time regression was traced to it), so it is opt-in: `resmon 1`
  // turns it on for as long as someone wants to look, `resmon 0` turns it
  // back off. Process-wide rather than per-instance because there is one
  // LuaScriptHost per process; static so the (free-function) timer helper
  // in the .cpp can check it without threading a host pointer through
  // every one of its call sites.
  static void SetMetricsEnabled(bool enabled);
  static bool MetricsEnabled();

 private:
  struct Resource {
    ResourceState state = ResourceState::kStopped;
    lua_State* L = nullptr;
    std::vector<std::string> commands;
    // event name -> LUA_REGISTRYINDEX refs of its handlers, and the subset
    // of names this resource is willing to accept from the network.
    std::unordered_map<std::string, std::vector<int>> event_handlers;
    std::unordered_set<std::string> net_events;
    // export name -> LUA_REGISTRYINDEX ref, for exports('name', fn).
    std::unordered_map<std::string, int> exports;
    // NUI callback name -> LUA_REGISTRYINDEX ref, for
    // RegisterNUICallback('name', fn). Client-side only in practice.
    std::unordered_map<std::string, int> nui_callbacks;
    // State-bag change handlers. Empty filter means "any".
    struct StateBagHandler {
      std::string key_filter;
      std::string bag_filter;
      int callback_ref = 0;
    };
    std::vector<StateBagHandler> state_bag_handlers;

    // resmon. accum_ms/accum_calls are the CURRENT, not-yet-rolled second;
    // Tick() moves them into metrics.last_second_* and zeroes them once a
    // second has elapsed. Kept separate from ResourceMetrics itself so a
    // reader (ResourceMetricsSnapshot, called from another thread) only
    // ever sees a value that a roll-over has already finished computing.
    ResourceMetrics metrics;
    double metrics_accum_ms = 0.0;
    std::uint64_t metrics_accum_calls = 0;
  };

  bool EnsureResourceLocked(const std::string& name);
  bool StartResourceLocked(const std::string& name, Resource& resource);
  void StopResourceLocked(const std::string& name, Resource& resource);
  // Creates a fresh lua_State with RegisterCommand + every registered
  // native already wired in. Caller loads/runs the actual resource code.
  lua_State* CreateResourceLuaState(const std::string& name);
  // The client-visible script list (shared then client) for a manifest.
  static std::vector<std::string> ClientFacingScripts(
      const ResourceManifest& manifest);

  static int RegisterCommand_Impl(lua_State* L);
  // GetResourceMetrics() -> table keyed by resource name, each
  // {msLastSecond, callsLastSecond, msLifetime, callsLifetime, memoryKb}.
  // A built-in global (wired in CreateResourceLuaState) rather than a
  // client-only native, since resmon is exactly as useful server-side.
  static int GetResourceMetrics_Impl(lua_State* L);
  static int AddEventHandler_Impl(lua_State* L);
  static int RegisterNetEvent_Impl(lua_State* L);
  static int TriggerEvent_Impl(lua_State* L);
  static int TriggerServerEvent_Impl(lua_State* L);
  static int TriggerClientEvent_Impl(lua_State* L);
  static int RegisterExport_Impl(lua_State* L);
  static int CallExport_Impl(lua_State* L);
  static int RegisterNuiCallback_Impl(lua_State* L);
  // The `cb` handed to a NUI callback handler. Upvalues: lightuserdata
  // host, integer response id. Deliberately does NOT take mutex_ - it can
  // be called from inside InvokeNuiCallback, which already holds it.
  static int NuiRespond_Impl(lua_State* L);
  static int StateBagSet_Impl(lua_State* L);
  static int StateBagGet_Impl(lua_State* L);
  static int AddStateBagChangeHandler_Impl(lua_State* L);
  // Stores the value and runs every matching change handler. Assumes mutex_
  // is held.
  void SetStateBagLocked(const std::string& bag, const std::string& key,
                         const std::string& json_value, bool replicate);
  // Runs `event`'s handlers in every started resource. When `network_only`
  // is set, a resource is skipped unless it declared the event via
  // RegisterNetEvent. Assumes mutex_ is held.
  void DispatchEventLocked(const std::string& event,
                           const std::string& json_args, bool network_only,
                           int source);
  // Assumes mutex_ is already held by the calling thread (only ever called
  // synchronously from within StartResourceLocked's script load).
  void RecordCommandRegistrationLocked(const std::string& resource,
                                      const std::string& command,
                                      int callback_ref, lua_State* L);

  mutable std::mutex mutex_;
  std::filesystem::path resources_root_;
  Side side_;
  std::vector<NativeBinding> natives_;
  // Registered separately from natives_ because they are installed as
  // closures carrying the host/resource upvalues rather than as bare C
  // functions - see RegisterResourceNative.
  std::vector<NativeBinding> resource_natives_;
  std::unordered_map<std::string, Resource> resources_;
  CommandRegisteredFn on_command_registered_;
  CommandUnregisteredFn on_command_unregistered_;
  EventOutboundFn on_event_outbound_;
  // bag name -> key -> JSON-encoded value. One store per host, so every
  // resource observes the same bags.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      state_bags_;
  ConsoleLogSink log_sink_;

  // In-flight NUI callbacks, keyed by the id NuiRespond_Impl carries as an
  // upvalue. Guarded by its OWN mutex rather than mutex_, precisely so a
  // handler can call cb() synchronously while mutex_ is still held.
  struct PendingNuiResponse {
    std::function<void(const std::string&)> respond;
    std::int64_t created_ms = 0;
  };
  mutable std::mutex nui_mutex_;
  std::unordered_map<std::uint64_t, PendingNuiResponse> nui_pending_;
  std::uint64_t next_nui_response_id_ = 1;

  // resmon's rollover clock. A host-wide window (not one per resource)
  // because Tick() already visits every resource together - one timestamp
  // is enough to decide "has a second passed" for all of them at once.
  std::int64_t last_metrics_rollover_ms_ = 0;
  void RollResourceMetricsLocked();
};

}  // namespace skate3::lua_host
