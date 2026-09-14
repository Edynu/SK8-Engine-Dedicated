#pragma once

// Shared surface of the dedicated server.
//
// The server started as one translation unit with everything in an anonymous
// namespace. That was fine while it was a packet relay; it is not fine now
// that it owns appearances, player identity, routing buckets, script events
// and a Lua native surface that is expected to grow. This header is what
// lets those live in separate files: the pieces that genuinely have to be
// shared, and nothing else.
//
// WHAT STAYS CLIENT-AUTHORITATIVE: player movement. Clients send their own
// position and pose and the server forwards them. The server is
// authoritative over everything ABOUT a session - who is connected, what
// role they hold, who can see whom, what they are wearing, which routing
// bucket they are in - but it does not simulate skaters and is not trying
// to. Anything added here should respect that line.

#include "skate3_appearance_store.h"
#include "skate3_lua_admin_http.h"
#include "skate3_lua_script_host.h"
#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_reliable_channel.h"
#include "skate3_multiplayer_routing.h"

#include <chrono>
#include <span>
#include <lua.hpp>
#include <cstdint>
#include <string>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace skate3::dedicated {

// Monotonic microseconds. Shared because the relay loop, the metrics window
// and the player registry all stamp times against the same clock.
[[nodiscard]] inline std::uint64_t NowMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Everything server.cfg and the command line can set. Parsed once at
// startup and then read-only, so it is passed by const reference rather
// than reached for through a global.
struct Options {
  int port = 27200;
  std::string token;
  float radius = 0.0f;
  std::string resources_dir = "resources";
  int admin_port = 27280;
  std::string config_path = "server.cfg";
  std::string hostname = "Skate 3 Server";
  int max_players = 16;
  skate3::multiplayer::routing::FidelityTiers tiers;
  // Hot zones: neighbour counts past which a crowded area is thinned.
  skate3::multiplayer::routing::CrowdPolicy crowd;
  // Root-snapshot rate in Hz, separate from the animation pose rate.
  std::int32_t pose_hz = 30;
  // Refresh rates for the detail bone groups. Fingers matter - a board grab
  // or a handplant is all hands - so they keep a real rate; facial
  // expression does not survive the distance, so it rides the head and
  // refreshes on keyframes only.
  std::int32_t hands_hz = 3;
  std::int32_t face_hz = 0;
  // Per-band animation rates in Hz. high_hz is what connected clients are
  // told to SEND at; the other two are what the relay thins that down to
  // for more distant recipients.
  std::int32_t high_hz = 20;
  std::int32_t medium_hz = 10;
  std::int32_t low_hz = 5;
  // Resources named by server.cfg's `ensure` lines. Empty means start
  // everything that was discovered, which is what this did before the
  // config file existed.
  std::vector<std::string> ensured;
  // Anything else in server.cfg, verbatim, for scripts to read through
  // GetConvar.
  std::unordered_map<std::string, std::string> convars;
};

// server.cfg first, then the command line, so a flag can override a file.
void ApplyConfigFile(const std::string &path, Options &options);
[[nodiscard]] Options ParseArgs(int argc, char **argv);

// Server-side view of who is connected and where they are. A snapshot behind
// its own mutex rather than direct access to the router, because the relay
// loop mutates peers on its own thread while scripts and HTTP workers read
// them. Keyed by role - the id events carry as `source`.
class PlayerRegistry {
public:
  struct Player {
    std::uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool position_valid = false;
    std::uint64_t map_hash = 0;
    std::uint32_t bucket = 0;
    std::string name;
    std::uint64_t last_seen_us = 0;
  };

  void Publish(
      const std::vector<skate3::multiplayer::routing::RelayPeer> &peers);
  [[nodiscard]] std::vector<Player> All() const;
  [[nodiscard]] std::optional<Player> Find(std::uint32_t id) const;

private:
  mutable std::mutex mutex_;
  std::vector<Player> players_;
};

// Which players are inside each other's interest radius. Edge-triggered, so
// a pair yields one playerEnteredScope and later one playerLeftScope rather
// than an event per tick.
class ScopeTracker {
public:
  void Update(const std::vector<PlayerRegistry::Player> &players, float radius);
  void Forget(std::uint32_t player_id);

private:
  static void RaiseScopeEvent(const char *event, std::uint32_t observer,
                              std::uint32_t target);
  std::set<std::pair<std::uint32_t, std::uint32_t>> in_scope_;
};

extern PlayerRegistry g_players;
extern ScopeTracker g_scopes;

// Raises a server-side event carrying one player id, dispatched as a network
// event so resources must opt in with RegisterNetEvent.
void RaisePlayerEvent(const char *event, std::uint32_t player_id);
// Set once in main, before the relay loop starts.
void SetScriptHost(skate3::lua_host::LuaScriptHost *host);

[[nodiscard]] std::string EscapeJsonString(std::string_view text);
[[nodiscard]] std::string EncodePlayersJson();

// Server-held player appearances. Set once in main when the store is
// constructed; the disconnect path uses it to stop advertising an appearance
// nobody is wearing any more.
extern skate3::appearance_store::AppearanceStore *g_appearance_store;

// Who `viewer_role` can currently see and what they are wearing, plus a hash
// of that answer used as the long-poll version. Computed per requesting
// client from the same interest rules the packet router applies.
struct VisiblePeer {
  std::uint32_t role = 0;
  std::uint64_t appearance_id = 0;
  std::string name;
};
[[nodiscard]] std::vector<VisiblePeer> VisibleAppearances(
    std::uint32_t viewer_role, float radius);
[[nodiscard]] std::uint64_t HashAppearanceRoster(
    const std::vector<VisiblePeer> &roster);

// The live router and admin server, set once in main.
extern skate3::multiplayer::routing::VisualRelayRouter *g_router;
extern skate3::lua_host::AdminHttpServer *g_admin_http;

// Queues an event for delivery to a client over the reliable script channel.
// Implemented with the script-event transport; declared here because the
// name handler and player lifecycle both broadcast.
void QueueClientScriptEvent(std::uint16_t role, const std::string &event,
                            const std::string &json_args);

// Per-connection reliable script-event channel state, owned by the relay
// loop and passed to the handlers below.
struct ScriptEventPeer {
  skate3::multiplayer::ReliableSender sender;
  skate3::multiplayer::ReliableReceiver receiver;
  std::uint32_t sequence = 0;
  bool ack_due = false;
};
using ScriptEventPeers = std::unordered_map<std::uint64_t, ScriptEventPeer>;

// `source` comes from the ROUTER, never from the datagram: a client may
// claim any role it likes in bytes it wrote itself.
[[nodiscard]] std::uint32_t RoleForConnection(
    const skate3::multiplayer::routing::VisualRelayRouter &router,
    std::uint64_t connection_id);

void HandleScriptEventPacket(
    ScriptEventPeers &peers, skate3::lua_host::LuaScriptHost &host,
    const skate3::multiplayer::routing::VisualRelayRouter &router,
    std::uint64_t connection_id, std::span<const std::uint8_t> packet,
    const skate3::multiplayer::protocol_v12::Envelope &envelope);

void DrainScriptEvents(
    SocketHandle relay_socket, ScriptEventPeers &peers,
    const skate3::multiplayer::routing::VisualRelayRouter &router,
    const std::unordered_map<std::uint64_t, sockaddr_in> &addresses,
    std::uint64_t now_us);

// Router mutations requested from script or HTTP threads, applied by the
// relay loop because the router is not thread-safe.
extern std::mutex g_bucket_mutex;
extern std::vector<std::pair<std::uint32_t, std::uint32_t>> g_pending_buckets;
extern std::mutex g_name_mutex;
extern std::vector<std::pair<std::uint32_t, std::string>> g_pending_names;

// server.cfg values scripts can read through GetConvar. Set once in main.
extern const std::unordered_map<std::string, std::string> *g_convars;

// Lua natives. Register these in main; implement them in the lua module.
int Lua_SetPlayerRoutingBucket(lua_State *L);
int Lua_GetPlayerRoutingBucket(lua_State *L);
int Lua_GetPlayerName(lua_State *L);
int Lua_GetConvar(lua_State *L);
int Lua_GetSkaters(lua_State *L);
int Lua_GetSkater(lua_State *L);

// Wraps one JSON string field, e.g. JsonString("Edynu") -> "\"Edynu\"".
[[nodiscard]] std::string JsonString(std::string_view text);

// POST /api/players/name's handler, run on an httplib worker.
void HandlePlayerNameChanged(int player_id, std::string_view name);

// Bandwidth and loop-hitch counters, published through /api/metrics and the
// netstat console command. Safe to call from any thread.
namespace metrics {

void RecordReceive(std::size_t bytes);
void RecordSend(std::size_t bytes);
// Called once per relay-loop iteration with how long that iteration took;
// iterations past the hitch threshold are counted separately.
void RecordIteration(std::uint64_t now_us, std::uint64_t iteration_us);

[[nodiscard]] std::string FormatTable();
[[nodiscard]] std::string Json();

}  // namespace metrics

}  // namespace skate3::dedicated
