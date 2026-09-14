// Standalone, non-authoritative visual relay ("skate3_dedicated"). It never
// links the engine, rendering, or game logic - only the two header-only
// wire-protocol/routing definitions shared with skate3.exe. It authenticates
// each connected client's role/session and forwards realtime datagrams
// unchanged to every other client on the same map and (optionally) within a
// configured radius. It never decodes pose, animation, or appearance data.

#include "skate3_dedicated_server.h"

#include "skate3_appearance_store.h"
#include "skate3_prop_store.h"
#include "skate3_lua_admin_http.h"
#include "skate3_lua_script_host.h"
#include "skate3_multiplayer_protocol.h"
#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_routing.h"
#include "skate3_multiplayer_reliable_channel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <lua.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "Ws2_32.lib")
#else
#include <fcntl.h>
#endif

namespace {

// Bandwidth and loop-hitch counters. Atomic because the relay loop (the one
// writer) and the admin HTTP server's own thread (the reader, for the
// "netstat" console command and /api/metrics) are different threads, and
// these are read far more often than a full mutex would justify.
//
// Byte counts are OFFERED load - the size handed to sendto/recvfrom, not a
// confirmation the datagram actually left the NIC or reached a peer. UDP
// gives no such confirmation short of an application-level ack, and this is
// a bandwidth gauge, not a delivery guarantee; the distinction matters only
// if these numbers are ever compared against a router's own counters.
//
// A "hitch" is one loop iteration that took longer than kHitchThresholdUs.
// The relay loop is otherwise a tight non-blocking poll (recvfrom returns
// immediately when nothing is waiting, see the FIONBIO/O_NONBLOCK setup
// above), so an iteration this long means something inside it stalled -
// a script's __skate_tick, RefreshCrowdCounts's O(peers^2) pass, or a burst
// of packets big enough to process in one go. Every player on the relay
// feels a hitch as added latency, which is why it is worth surfacing
// separately from plain bandwidth.
using skate3::multiplayer::routing::RelayDisposition;
using skate3::multiplayer::routing::RelayPeer;
using skate3::multiplayer::routing::VisualRelayRouter;
namespace protocol_v12 = skate3::multiplayer::protocol_v12;

constexpr std::uint64_t kStaleTimeoutMicroseconds = 10'000'000;
constexpr std::uint64_t kSweepIntervalMicroseconds = 1'000'000;

// Defined below with the player registry; declared here because the
// connection handler above it raises playerConnecting.
void RaisePlayerEvent(const char *event, std::uint32_t player_id);

std::uint64_t NowMicroseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char value : text) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

// The relay's connection identity for a UDP peer: derived from the observed
// source address, never from anything the client sends, so it cannot be
// spoofed by packet contents.
std::uint64_t ConnectionIdFor(const sockaddr_in &address) {
  const std::uint64_t value =
      (static_cast<std::uint64_t>(address.sin_addr.s_addr) << 16) ^
      static_cast<std::uint64_t>(address.sin_port);
  return value == 0 ? 1 : value;
}

// Every v11 legacy packet (PosePacket, AnimationFragmentPacket,
// AppearanceFragmentPacket, ControlPacket - skate3_multiplayer_protocol.h)
// shares an identical 16-byte prefix: magic(4) version(2) byte_count(2)
// sender_role(4) sender_session(4).
//
// Realtime traffic now negotiates the compact v12 envelope on every
// transport, this one included, so most pose/animation datagrams arrive as
// v12 and are routed through DecodeEnvelope instead. v11 is still carried
// because appearance fragments and control packets use it, and because a
// peer that has not finished negotiating v12 keeps sending it - the format
// is per peer, not per server. Either way the relay only authenticates and
// forwards; it never interprets pose/animation/appearance payloads.
bool ExtractLegacyIdentity(std::span<const std::uint8_t> packet,
                          std::uint32_t &sender_role,
                          std::uint32_t &sender_session) {
  if (packet.size() < 16) {
    return false;
  }
  std::uint32_t magic = 0;
  std::memcpy(&magic, packet.data(), sizeof(magic));
  namespace protocol = skate3::multiplayer::protocol;
  if (magic != protocol::kPacketMagic &&
      magic != protocol::kAnimationPacketMagic &&
      magic != protocol::kAppearancePacketMagic &&
      magic != protocol::kControlPacketMagic) {
    return false;
  }
  std::memcpy(&sender_role, packet.data() + 8, sizeof(sender_role));
  std::memcpy(&sender_session, packet.data() + 12, sizeof(sender_session));
  return true;
}

std::string DescribeAddress(const sockaddr_in &address) {
  char text[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, const_cast<in_addr *>(&address.sin_addr), text,
            sizeof(text));
  return std::string(text) + ":" +
         std::to_string(ntohs(address.sin_port));
}

bool CloseSocket(SocketHandle socket_handle) {
#if defined(_WIN32)
  return closesocket(socket_handle) == 0;
#else
  return close(socket_handle) == 0;
#endif
}

class RoleAllocator {
public:
  static constexpr std::uint32_t kMaxRole = 100;

  RoleAllocator() {
    for (std::uint32_t role = 1; role <= kMaxRole; ++role) {
      free_roles_.push_back(role);
    }
  }

  std::uint32_t Assign(std::uint64_t connection_id) {
    const auto existing = role_by_connection_.find(connection_id);
    if (existing != role_by_connection_.end()) {
      return existing->second;
    }
    if (free_roles_.empty()) {
      return 0;
    }
    const std::uint32_t role = free_roles_.front();
    free_roles_.pop_front();
    role_by_connection_[connection_id] = role;
    return role;
  }

  void Release(std::uint64_t connection_id) {
    const auto existing = role_by_connection_.find(connection_id);
    if (existing == role_by_connection_.end()) {
      return;
    }
    // To the BACK of the queue: this is what makes the id wait its turn
    // rather than being reissued to the next joiner.
    free_roles_.push_back(existing->second);
    role_by_connection_.erase(existing);
  }

private:
  std::deque<std::uint32_t> free_roles_;
  std::unordered_map<std::uint64_t, std::uint32_t> role_by_connection_;
};

void SendRegisterAck(SocketHandle socket_handle, const sockaddr_in &sender,
                     const protocol_v12::RelayRegisterAck &ack) {
  std::array<std::uint8_t, protocol_v12::kEnvelopeBytes +
                               protocol_v12::kRelayRegisterAckPayloadBytes>
      datagram{};
  const bool ok = ack.status == protocol_v12::RelayRegisterStatus::kOk;
  const protocol_v12::Envelope envelope{
      .kind = protocol_v12::MessageKind::kRelayRegisterAck,
      .payload_bytes = protocol_v12::kRelayRegisterAckPayloadBytes,
      // A rejection has no real role/session to report; (1,1) is a
      // structurally-valid placeholder the client must ignore because it
      // reads payload.status first.
      .sender_role = ok ? ack.assigned_role : std::uint16_t{1},
      .sender_session = ok ? ack.assigned_session : std::uint32_t{1},
  };
  if (!protocol_v12::EncodeEnvelope(
          envelope, std::span(datagram).first(protocol_v12::kEnvelopeBytes)) ||
      !protocol_v12::EncodeRelayRegisterAck(
          ack, std::span(datagram).subspan(protocol_v12::kEnvelopeBytes))) {
    return;
  }
  sendto(socket_handle, reinterpret_cast<const char *>(datagram.data()),
         static_cast<int>(datagram.size()), 0,
         reinterpret_cast<const sockaddr *>(&sender), sizeof(sender));
  skate3::dedicated::metrics::RecordSend(datagram.size());
}

void HandleRegister(SocketHandle socket_handle, VisualRelayRouter &router,
                    RoleAllocator &roles, std::uint64_t connection_id,
                    const sockaddr_in &sender, std::uint64_t token_hash,
                    std::span<const std::uint8_t> payload,
                    std::uint64_t now_us, std::uint16_t admin_port) {
  protocol_v12::RelayRegister request;
  protocol_v12::RelayRegisterAck ack;
  ack.admin_http_port = admin_port;
  if (!protocol_v12::DecodeRelayRegister(payload, request)) {
    ack.status = protocol_v12::RelayRegisterStatus::kBadToken;
    SendRegisterAck(socket_handle, sender, ack);
    return;
  }
  if (token_hash != 0 && request.token_hash != token_hash) {
    ack.status = protocol_v12::RelayRegisterStatus::kBadToken;
    SendRegisterAck(socket_handle, sender, ack);
    std::printf("skate3-dedicated: rejected %s (bad token)\n",
                DescribeAddress(sender).c_str());
    return;
  }
  const std::uint32_t role = roles.Assign(connection_id);
  if (role == 0) {
    ack.status = protocol_v12::RelayRegisterStatus::kFull;
    SendRegisterAck(socket_handle, sender, ack);
    return;
  }
  std::uint32_t session =
      static_cast<std::uint32_t>(request.client_nonce ^
                                 (request.client_nonce >> 32));
  if (session == 0) {
    session = 1;
  }
  if (!router.Register({.connection_id = connection_id,
                        .role = role,
                        .session = session,
                        .last_seen_us = now_us})) {
    roles.Release(connection_id);
    ack.status = protocol_v12::RelayRegisterStatus::kFull;
    SendRegisterAck(socket_handle, sender, ack);
    return;
  }
  ack.status = protocol_v12::RelayRegisterStatus::kOk;
  ack.assigned_role = static_cast<std::uint16_t>(role);
  ack.assigned_session = session;
  SendRegisterAck(socket_handle, sender, ack);
  std::printf("skate3-dedicated: role %u connected from %s (map=0x%llx)\n",
              role, DescribeAddress(sender).c_str(),
              static_cast<unsigned long long>(request.requested_map_hash));
  // Accepted onto the server, but not yet in the world - scripts get a
  // chance to set up per-player state before the player can be seen.
  RaisePlayerEvent("playerConnecting", role);
}

// Server-side view of who is connected and where they are.
//
// The relay already learns every peer's position from their presence
// beacons; this republishes that as a snapshot scripts can read. It is a
// snapshot behind its own mutex rather than direct access to the router
// because the relay loop mutates peers on its own thread while a script
// may be reading them on an HTTP worker thread mid-event-dispatch.
//
// Keyed by role - the same id events carry as `source` and the client
// reports from GetPlayerId(), so an event handler can look up its sender.
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

  void Publish(const std::vector<RelayPeer> &peers) {
    std::vector<Player> next;
    next.reserve(peers.size());
    for (const auto &peer : peers) {
      next.push_back({.id = peer.role,
                      .x = peer.x,
                      .y = peer.y,
                      .z = peer.z,
                      .position_valid = peer.position_valid,
                      .map_hash = peer.map_hash,
                      .bucket = peer.bucket,
                      .name = peer.name,
                      .last_seen_us = peer.last_seen_us});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    players_ = std::move(next);
  }

  [[nodiscard]] std::vector<Player> All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return players_;
  }

  [[nodiscard]] std::optional<Player> Find(std::uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &player : players_) {
      if (player.id == id) {
        return player;
      }
    }
    return std::nullopt;
  }

private:
  mutable std::mutex mutex_;
  std::vector<Player> players_;
};

PlayerRegistry g_players;


// The script host, so relay-loop lifecycle can raise script events. Set once
// in main before the loop starts.
skate3::lua_host::LuaScriptHost *g_script_host = nullptr;

// Raises a server-side event with a single integer argument. Used for the
// player lifecycle, whose payload is always just an id.
void RaisePlayerEvent(const char *event, std::uint32_t player_id) {
  if (g_script_host == nullptr) {
    return;
  }
  // Dispatched as a network event so resources must opt in with
  // RegisterNetEvent, the same as any other event they do not raise
  // themselves. `source` carries the id as well, matching FiveM.
  g_script_host->DispatchNetworkEvent(
      event, "[" + std::to_string(player_id) + "]",
      static_cast<int>(player_id));
}

// Which players are currently within each other's interest radius. Scope
// transitions are edge-triggered off this, so a pair produces one
// playerEnteredScope and later one playerLeftScope, not an event per tick.
class ScopeTracker {
public:
  // Recomputes every pair and raises the transitions. `radius` of 0 means
  // unlimited, in which case everyone on the same map is in scope.
  void Update(const std::vector<PlayerRegistry::Player> &players,
              float radius) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> current;
    for (const auto &observer : players) {
      for (const auto &target : players) {
        if (observer.id == target.id || observer.map_hash != target.map_hash) {
          continue;
        }
        if (radius > 0.0f && observer.position_valid && target.position_valid) {
          const float dx = target.x - observer.x;
          const float dy = target.y - observer.y;
          const float dz = target.z - observer.z;
          if (dx * dx + dy * dy + dz * dz > radius * radius) {
            continue;
          }
        }
        current.insert({observer.id, target.id});
      }
    }
    for (const auto &pair : current) {
      if (!in_scope_.contains(pair)) {
        RaiseScopeEvent("playerEnteredScope", pair.first, pair.second);
      }
    }
    for (const auto &pair : in_scope_) {
      if (!current.contains(pair)) {
        RaiseScopeEvent("playerLeftScope", pair.first, pair.second);
      }
    }
    in_scope_ = std::move(current);
  }

  // A disconnect removes every pair the player was part of, without
  // reporting a scope exit for each - playerDropped already covers it, and
  // firing both would make handlers double-count.
  void Forget(std::uint32_t player_id) {
    for (auto it = in_scope_.begin(); it != in_scope_.end();) {
      it = (it->first == player_id || it->second == player_id)
               ? in_scope_.erase(it)
               : std::next(it);
    }
  }

private:
  static void RaiseScopeEvent(const char *event, std::uint32_t observer,
                              std::uint32_t target) {
    if (g_script_host == nullptr) {
      return;
    }
    // {forPlayer, player} - "player entered forPlayer's scope", matching
    // the shape of FiveM's own scope events.
    g_script_host->DispatchNetworkEvent(
        event, "[" + std::to_string(observer) + "," + std::to_string(target) + "]",
        static_cast<int>(observer));
  }

  std::set<std::pair<std::uint32_t, std::uint32_t>> in_scope_;
};

ScopeTracker g_scopes;

// Names are client-provided (unlike everything else this relay routes),
// so unlike the rest of this file's hand-rolled JSON encoding this one
// actually has to escape its input.
std::string EscapeJsonString(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

// Dashboard's Active Players page (GET /api/players via
// AdminHttpServer::SetPlayersProvider) - the same registry, encoded as JSON
// instead of pushed onto a Lua stack.
std::string EncodePlayersJson() {
  std::string json = "[";
  bool first = true;
  const std::uint64_t now = NowMicroseconds();
  for (const auto &player : g_players.All()) {
    if (!first) {
      json += ",";
    }
    first = false;
    char map_hash[24];
    std::snprintf(map_hash, sizeof(map_hash), "%016llx",
                 static_cast<unsigned long long>(player.map_hash));
    // pingMs is really "time since last datagram" - this relay never
    // measures round-trip latency, so it is the closest honest substitute
    // and at least tells the dashboard a connection is alive and recent.
    const double idle_ms =
        player.last_seen_us <= now
            ? static_cast<double>(now - player.last_seen_us) / 1000.0
            : 0.0;
    json += "{\"id\":" + std::to_string(player.id) +
           ",\"name\":\"" + EscapeJsonString(player.name) + "\"" +
           ",\"valid\":" + (player.position_valid ? "true" : "false") +
           ",\"x\":" + std::to_string(player.x) +
           ",\"y\":" + std::to_string(player.y) +
           ",\"z\":" + std::to_string(player.z) + ",\"mapHash\":\"" +
           map_hash + "\",\"bucket\":" + std::to_string(player.bucket) +
           ",\"idleMs\":" + std::to_string(idle_ms) + "}";
  }
  json += "]";
  return json;
}

void PushPlayerTable(lua_State *L, const PlayerRegistry::Player &player) {
  lua_newtable(L);
  lua_pushinteger(L, player.id);
  lua_setfield(L, -2, "id");
  lua_pushstring(L, player.name.c_str());
  lua_setfield(L, -2, "name");
  lua_pushboolean(L, player.position_valid ? 1 : 0);
  lua_setfield(L, -2, "valid");
  if (player.position_valid) {
    lua_pushnumber(L, player.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, player.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, player.z);
    lua_setfield(L, -2, "z");
  }
  // Lua numbers are doubles; a 64-bit hash is handed over as hex text so it
  // survives the trip intact and can be compared for equality.
  char map_hash[24];
  std::snprintf(map_hash, sizeof(map_hash), "%016llx",
                static_cast<unsigned long long>(player.map_hash));
  lua_pushstring(L, map_hash);
  lua_setfield(L, -2, "mapHash");
  lua_pushinteger(L, player.bucket);
  lua_setfield(L, -2, "bucket");
}

// The live router, so bucket natives can reach it. Only touched from the
// relay thread and from natives, which the queue below serialises.
VisualRelayRouter *g_router = nullptr;

// So the player-name HTTP handler can broadcast, from whatever thread
// Defined with the reliable-channel helpers below, but needed here.
void QueueClientScriptEvent(std::uint16_t role, const std::string &event,
                            const std::string &json_args);

// httplib calls it on - QueueClientEvent has its own internal locking and
// is safe to call from any thread, unlike the router.
skate3::lua_host::AdminHttpServer *g_admin_http = nullptr;
// Set once the store is constructed in main; the disconnect path below uses
// it to stop advertising an appearance nobody is wearing any more.
skate3::appearance_store::AppearanceStore *g_appearance_store = nullptr;

// Who `viewer_role` can currently see, and what each of them is wearing.
//
// This is the server deciding visibility rather than the client filtering a
// global list, and it deliberately applies the SAME rules the packet router
// applies in RouteRaw: same map, same routing bucket, and within sv_radius.
// A peer the relay would not forward packets from is a peer whose appearance
// this viewer has no business downloading.
//
// Reads the PlayerRegistry snapshot, not the router: the relay loop mutates
// the router on its own thread and this runs on an HTTP worker.
//
// Peers with no stored appearance are omitted - there is nothing to fetch,
// and leaving them out keeps them from perturbing the version hash.
struct VisiblePeer {
  std::uint32_t role = 0;
  std::uint64_t appearance_id = 0;
  std::string name;
};

[[nodiscard]] std::vector<VisiblePeer> VisibleAppearances(
    std::uint32_t viewer_role, float radius) {
  std::vector<VisiblePeer> visible;
  if (viewer_role == 0 || g_appearance_store == nullptr) {
    return visible;
  }
  const auto viewer = g_players.Find(viewer_role);
  if (!viewer.has_value()) {
    return visible;
  }
  std::unordered_map<std::uint32_t, std::uint64_t> worn;
  for (const auto &entry : g_appearance_store->Roster()) {
    worn[entry.role] = entry.appearance_id;
  }
  for (const auto &player : g_players.All()) {
    if (player.id == viewer_role || player.map_hash != viewer->map_hash ||
        player.bucket != viewer->bucket) {
      continue;
    }
    if (radius > 0.0f && player.position_valid && viewer->position_valid) {
      const float dx = player.x - viewer->x;
      const float dy = player.y - viewer->y;
      const float dz = player.z - viewer->z;
      if (dx * dx + dy * dy + dz * dz > radius * radius) {
        continue;
      }
    }
    const auto found = worn.find(player.id);
    const std::uint64_t appearance_id =
        found == worn.end() ? 0 : found->second;
    if (appearance_id == 0 && player.name.empty()) {
      continue;  // nothing to say about this peer yet.
    }
    // The NAME rides here too, not just the appearance. It used to reach
    // clients only as a one-shot "skate3:playerNamed" script event, so a
    // client that was not listening at that instant showed a role number
    // ("Player 3") for the rest of the session with nothing to correct it.
    // This answer is re-sent whenever it changes and is scoped to peers the
    // viewer can see, which is exactly the same guarantee appearances get.
    visible.push_back({.role = player.id,
                       .appearance_id = appearance_id,
                       .name = player.name});
  }
  // Sorted so the hash below depends on the CONTENT of the answer and not on
  // the order the registry happened to hand the players over.
  std::sort(visible.begin(), visible.end(),
            [](const auto &left, const auto &right) {
              return left.role < right.role;
            });
  return visible;
}

// FNV-1a over the (role, appearance) pairs. Serves as the long-poll version:
// equal hash means this viewer's answer has not changed, whatever else moved
// in the world.
[[nodiscard]] std::uint64_t HashAppearanceRoster(
    const std::vector<VisiblePeer> &roster) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= (value >> (byte * 8)) & 0xFFull;
      hash *= 1099511628211ull;
    }
  };
  for (const auto &entry : roster) {
    mix(entry.role);
    mix(entry.appearance_id);
    for (const char character : entry.name) {
      mix(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
    }
  }
  // Never collide with "caller has seen nothing yet".
  return hash == 0 ? 1 : hash;
}

// Bucket changes are requested from script threads but applied on the relay
// thread, since the router is not thread-safe.
std::mutex g_bucket_mutex;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_pending_buckets;

// Same reasoning, for names: SetName touches the router, which is only
// safe to mutate from the relay thread, but the HTTP handler that learns a
// new name runs on an httplib worker thread.
std::mutex g_name_mutex;
std::vector<std::pair<std::uint32_t, std::string>> g_pending_names;

// Wraps one JSON string field, e.g. NameField("Edynu") -> "\"Edynu\"".
std::string JsonString(std::string_view text) {
  return "\"" + EscapeJsonString(text) + "\"";
}

// POST /api/players/name's handler (AdminHttpServer::SetPlayerNameHandler).
// Runs on an httplib worker thread. Queues the actual router mutation for
// the relay thread, then does two things that need no such deferral:
//   1. tells every OTHER connected client this player's name, so anyone
//      already in the world learns it immediately ("first notice");
//   2. tells the NEWLY-named player everyone else's already-known names,
//      so they do not have to wait for each of those players to happen to
//      rename themselves again to learn who is already there.
void HandlePlayerNameChanged(int player_id, std::string_view name) {
  const auto role = static_cast<std::uint32_t>(player_id);

  // De-duplicated against every OTHER currently-connected name, so two
  // connections presenting the same identity - the common case testing
  // solo, two clients launched from the same .bat with the same
  // PLAYER_NAME - read as distinct people instead of two players both
  // silently named "Edynu" (which is what a nameplate fell back to
  // showing: a role number, because nothing else distinguished them).
  // Grows the suffix until unique rather than stopping at "(2)", so a
  // third or fourth identical connection still gets a real name instead
  // of colliding with the one "(2)" already claimed.
  std::string final_name(name);
  {
    int suffix = 2;
    bool collided;
    do {
      collided = false;
      for (const auto &player : g_players.All()) {
        if (player.id != role && player.name == final_name) {
          collided = true;
          break;
        }
      }
      if (collided) {
        final_name = std::string(name) + "(" + std::to_string(suffix) + ")";
        ++suffix;
      }
    } while (collided);
  }

  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    g_pending_names.push_back({role, final_name});
  }
  const std::string self_id = std::to_string(player_id);
  // On the reliable channel like every other script event - the HTTP event
  // feed these used to ride no longer has any subscribers.
  QueueClientScriptEvent(protocol_v12::kReliableBroadcastRole,
                         "skate3:playerNamed",
                         "[" + self_id + "," + JsonString(final_name) + "]");

  std::string roster = "[[";
  bool first = true;
  for (const auto &player : g_players.All()) {
    if (player.id == role || player.name.empty()) {
      continue;
    }
    if (!first) {
      roster += ",";
    }
    first = false;
    roster += "{\"id\":" + std::to_string(player.id) + ",\"name\":" +
             JsonString(player.name) + "}";
  }
  roster += "]]";
  if (!first) {
    QueueClientScriptEvent(static_cast<std::uint16_t>(role),
                           "skate3:playerRoster", roster);
  }
}

// SetPlayerRoutingBucket(id, bucket) -> queue a move into another instance.
int Lua_SetPlayerRoutingBucket(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto bucket = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
  std::lock_guard<std::mutex> lock(g_bucket_mutex);
  g_pending_buckets.push_back({id, bucket});
  return 0;
}

// GetPlayerRoutingBucket(id) -> the bucket that player is currently in.
int Lua_GetPlayerRoutingBucket(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto player = g_players.Find(id);
  lua_pushinteger(L, player ? player->bucket : 0);
  return 1;
}

// GetPlayerName(id) -> that player's self-reported name, or "" if they
// have not sent one yet (e.g. a client too old to know about
// POST /api/players/name, or one that has not connected long enough to).
int Lua_GetPlayerName(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto player = g_players.Find(id);
  lua_pushstring(L, player ? player->name.c_str() : "");
  return 1;
}

// GetSkaters() -> array of connected player ids.
// Set once the config is parsed, so the native below can answer without the
// options struct being threaded through every call site.
const std::unordered_map<std::string, std::string>* g_convars = nullptr;

// GetConvar(name [, default]) -> string
//
// Any "key value" line in server.cfg that is not one of the server's own
// directives. This is how a game mode reads its own settings - game_difficulty,
// for instance - without the server needing to know what they mean.
int Lua_GetConvar(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  const char* fallback = luaL_optstring(L, 2, "");
  if (g_convars != nullptr) {
    const auto found = g_convars->find(name);
    if (found != g_convars->end()) {
      lua_pushstring(L, found->second.c_str());
      return 1;
    }
  }
  lua_pushstring(L, fallback);
  return 1;
}

int Lua_GetSkaters(lua_State *L) {
  lua_newtable(L);
  int index = 1;
  for (const auto &player : g_players.All()) {
    lua_pushinteger(L, player.id);
    lua_rawseti(L, -2, index++);
  }
  return 1;
}

// GetSkater(id) -> table describing that player, or nil if not connected.
int Lua_GetSkater(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto player = g_players.Find(id);
  if (!player) {
    lua_pushnil(L);
    return 1;
  }
  PushPlayerTable(L, *player);
  return 1;
}

// ---------------------------------------------------------------------
// Script events over the reliable channel
//
// The relay forwards most message kinds between clients untouched, but NOT
// these. A script event addressed to the server is consumed here and handed
// to the Lua host; anything the host sends back is originated here. Events
// never travel client-to-client, because the server is the authority on who
// should receive what.
// ---------------------------------------------------------------------

struct ScriptEventPeer {
  skate3::multiplayer::ReliableSender sender;
  skate3::multiplayer::ReliableReceiver receiver;
  std::uint32_t sequence = 0;
  bool ack_due = false;
};

using ScriptEventPeers = std::unordered_map<std::uint64_t, ScriptEventPeer>;

// Queued by TriggerClientEvent on a script thread, drained by the relay
// thread that owns the channels. `role` is kReliableBroadcastRole for
// "every client".
struct PendingClientEvent {
  std::uint16_t role = 0;
  std::vector<std::uint8_t> message;
};

std::mutex g_client_event_mutex;
std::deque<PendingClientEvent> g_client_events;

void QueueClientScriptEvent(std::uint16_t role, const std::string &event,
                            const std::string &json_args) {
  std::vector<std::uint8_t> message;
  if (!protocol_v12::EncodeScriptEventMessage(event, json_args, message)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_client_event_mutex);
  g_client_events.push_back({role, std::move(message)});
}

// The server is not a peer and holds no role of its own, so outbound
// envelopes carry a placeholder that only has to satisfy EnvelopeShapeValid.
// Clients identify these purely by kind and by the fact that they arrived
// from the server's address.
constexpr std::uint16_t kServerSenderRole = 1;
constexpr std::uint32_t kServerSessionId = 1;

std::uint32_t RoleForConnection(const VisualRelayRouter &router,
                                std::uint64_t connection_id) {
  for (const auto &peer : router.Peers()) {
    if (peer.connection_id == connection_id) {
      return peer.role;
    }
  }
  return 0;
}

void SendScriptDatagram(SocketHandle relay_socket, const sockaddr_in &address,
                        const std::uint8_t *bytes, std::size_t byte_count) {
  sendto(relay_socket, reinterpret_cast<const char *>(bytes),
         static_cast<int>(byte_count), 0,
         reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  skate3::dedicated::metrics::RecordSend(byte_count);
}

void SendScriptEventAck(SocketHandle relay_socket, const sockaddr_in &address,
                        ScriptEventPeer &peer, std::uint64_t now_us) {
  const protocol_v12::ReliableAck ack =
      peer.receiver.BuildAck(protocol_v12::kReliableChannelScriptEvent);
  protocol_v12::Envelope envelope;
  envelope.kind = protocol_v12::MessageKind::kReliableAck;
  envelope.flags = protocol_v12::kFlagReliable;
  envelope.payload_bytes = protocol_v12::kReliableAckPayloadBytes;
  envelope.sender_role = kServerSenderRole;
  envelope.stream_id = protocol_v12::kReliableChannelScriptEvent;
  envelope.sender_session = kServerSessionId;
  envelope.sequence = ++peer.sequence;
  envelope.sender_time_us = now_us;
  std::array<std::uint8_t, protocol_v12::kEnvelopeBytes +
                               protocol_v12::kReliableAckPayloadBytes>
      packet{};
  if (protocol_v12::EncodeReliableAckDatagram(envelope, ack, packet)) {
    SendScriptDatagram(relay_socket, address, packet.data(), packet.size());
  }
}

void HandleScriptEventPacket(ScriptEventPeers &peers,
                             skate3::lua_host::LuaScriptHost &host,
                             const VisualRelayRouter &router,
                             std::uint64_t connection_id,
                             std::span<const std::uint8_t> packet,
                             const protocol_v12::Envelope &envelope) {
  ScriptEventPeer &peer = peers[connection_id];
  if (envelope.kind == protocol_v12::MessageKind::kReliableAck) {
    protocol_v12::Envelope ack_envelope;
    protocol_v12::ReliableAck ack;
    if (protocol_v12::DecodeReliableAckDatagram(packet, ack_envelope, ack) &&
        ack.channel == protocol_v12::kReliableChannelScriptEvent) {
      peer.sender.Acknowledge(ack);
    }
    return;
  }

  protocol_v12::Envelope stream_envelope;
  protocol_v12::ReliableHeader header;
  std::span<const std::uint8_t> fragment;
  if (!protocol_v12::DecodeReliableDatagram(packet, stream_envelope, header,
                                            fragment) ||
      stream_envelope.stream_id !=
          protocol_v12::kReliableChannelScriptEvent) {
    return;
  }
  std::vector<std::vector<std::uint8_t>> delivered;
  peer.receiver.Accept(header, fragment, delivered);
  // Acknowledged even for a duplicate: a duplicate means our previous ack
  // was what went missing, so silence would loop forever.
  peer.ack_due = true;

  // The sender's identity comes from the ROUTER, not the envelope: a client
  // may claim any role it likes in a datagram it wrote itself, and `source`
  // is what server scripts authorise on.
  const std::uint32_t role = RoleForConnection(router, connection_id);
  if (role == 0) {
    return;  // not a registered connection; nothing to attribute this to.
  }
  for (const std::vector<std::uint8_t> &message : delivered) {
    std::string event;
    std::string json_args;
    if (!protocol_v12::DecodeScriptEventMessage(message, event, json_args)) {
      continue;
    }
    if (host.TryApplyStateBagEvent(event, json_args)) {
      continue;
    }
    host.DispatchNetworkEvent(event, json_args, static_cast<int>(role));
  }
}

void DrainScriptEvents(SocketHandle relay_socket, ScriptEventPeers &peers,
                       const VisualRelayRouter &router,
                       const std::unordered_map<std::uint64_t, sockaddr_in>
                           &addresses,
                       std::uint64_t now_us) {
  // Hand anything scripts queued to the right per-connection sender.
  std::deque<PendingClientEvent> pending;
  {
    std::lock_guard<std::mutex> lock(g_client_event_mutex);
    pending.swap(g_client_events);
  }
  if (!pending.empty()) {
    const auto connected = router.Peers();
    for (PendingClientEvent &entry : pending) {
      for (const auto &peer : connected) {
        const bool addressed =
            entry.role == protocol_v12::kReliableBroadcastRole ||
            peer.role == entry.role;
        if (!addressed) {
          continue;
        }
        // Copied per recipient: each connection's channel owns its own
        // retransmit state and may still be resending long after another
        // has acknowledged.
        peers[peer.connection_id].sender.Queue(entry.message, peer.role);
      }
    }
  }

  for (auto entry = peers.begin(); entry != peers.end();) {
    const auto address = addresses.find(entry->first);
    if (address == addresses.end()) {
      entry = peers.erase(entry);  // connection gone; drop its channel.
      continue;
    }
    ScriptEventPeer &peer = entry->second;
    std::vector<skate3::multiplayer::ReliableOutboundFragment> fragments;
    peer.sender.Collect(now_us, fragments);
    for (const auto &fragment : fragments) {
      const std::uint16_t fragment_bytes =
          protocol_v12::ReliableFragmentByteCount(
              fragment.header.total_bytes, fragment.header.fragment_index);
      protocol_v12::Envelope envelope;
      envelope.kind = protocol_v12::MessageKind::kReliableStream;
      envelope.flags = protocol_v12::kFlagReliable;
      envelope.payload_bytes =
          protocol_v12::kReliableHeaderBytes + fragment_bytes;
      envelope.sender_role = kServerSenderRole;
      envelope.stream_id = protocol_v12::kReliableChannelScriptEvent;
      envelope.sender_session = kServerSessionId;
      envelope.sequence = ++peer.sequence;
      envelope.sender_time_us = now_us;
      std::array<std::uint8_t, protocol_v12::kMaximumDatagramBytes> packet{};
      const std::size_t packet_bytes =
          protocol_v12::kEnvelopeBytes + envelope.payload_bytes;
      if (protocol_v12::EncodeReliableDatagram(
              envelope, fragment.header, fragment.payload,
              std::span<std::uint8_t>(packet).first(packet_bytes))) {
        SendScriptDatagram(relay_socket, address->second, packet.data(),
                           packet_bytes);
      }
    }
    if (peer.ack_due) {
      SendScriptEventAck(relay_socket, address->second, peer, now_us);
      peer.ack_due = false;
    }
    ++entry;
  }
}

} // namespace

int main(int argc, char **argv) {
  // Line-buffer stdout even when redirected to a file/pipe (fully buffered
  // by default in that case), so log lines are visible immediately rather
  // than sitting in libc's buffer until process exit.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const skate3::dedicated::Options options = skate3::dedicated::ParseArgs(argc, argv);
  const std::uint64_t token_hash =
      options.token.empty() ? 0 : Fnv1a64(options.token);

#if defined(_WIN32)
  WSADATA wsa_data{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::fprintf(stderr, "skate3-dedicated: WSAStartup failed\n");
    return 1;
  }
#endif

  const SocketHandle relay_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (relay_socket == kInvalidSocket) {
    std::fprintf(stderr, "skate3-dedicated: could not create socket\n");
    return 1;
  }

  sockaddr_in bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(static_cast<std::uint16_t>(options.port));
  if (bind(relay_socket, reinterpret_cast<sockaddr *>(&bind_address),
           sizeof(bind_address)) != 0) {
    std::fprintf(stderr, "skate3-dedicated: could not bind UDP port %d\n",
                options.port);
    CloseSocket(relay_socket);
    return 1;
  }

#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(relay_socket, FIONBIO, &nonblocking);
#else
  const int flags = fcntl(relay_socket, F_GETFL, 0);
  fcntl(relay_socket, F_SETFL, flags | O_NONBLOCK);
#endif
  const int socket_buffer_bytes = 4 * 1024 * 1024;
  setsockopt(relay_socket, SOL_SOCKET, SO_RCVBUF,
             reinterpret_cast<const char *>(&socket_buffer_bytes),
             sizeof(socket_buffer_bytes));
  setsockopt(relay_socket, SOL_SOCKET, SO_SNDBUF,
             reinterpret_cast<const char *>(&socket_buffer_bytes),
             sizeof(socket_buffer_bytes));

  std::printf(
      "skate3-dedicated: \"%s\" listening on UDP port %d%s, max %d players, "
      "radius=%.1f (0=unlimited)\n",
      options.hostname.c_str(), options.port,
      token_hash != 0 ? ", token required" : "", options.max_players,
      options.radius);
  if (options.tiers.high_distance > 0.0f ||
      options.tiers.medium_distance > 0.0f ||
      options.tiers.low_distance > 0.0f) {
    std::printf(
        "skate3-dedicated: fidelity high=%.0f medium=%.0f low=%.0f\n",
        options.tiers.high_distance, options.tiers.medium_distance,
        options.tiers.low_distance);
  }

  // Hz per band expressed as divisors of the sender's rate, which is what
  // the thinning actually works in. Rounded to the nearest whole divisor and
  // never zero: a band can equal the send rate (no thinning) but cannot
  // exceed it, since the relay can only drop samples, not create them.
  const auto rate_divisor = [](std::int32_t high, std::int32_t band) {
    if (band <= 0 || high <= 0 || band >= high) {
      return std::uint32_t{1};
    }
    return static_cast<std::uint32_t>(std::max(1, (high + band / 2) / band));
  };
  const skate3::multiplayer::routing::FidelityRates fidelity_rates{
      .medium_divisor = rate_divisor(options.high_hz, options.medium_hz),
      .low_divisor = rate_divisor(options.high_hz, options.low_hz),
  };
  std::printf("skate3-dedicated: animation rates high=%dHz medium=%dHz "
              "low=%dHz (divisors %u/%u)\n",
              options.high_hz, options.medium_hz, options.low_hz,
              fidelity_rates.medium_divisor, fidelity_rates.low_divisor);

  VisualRelayRouter router;
  RoleAllocator roles;
  ScriptEventPeers script_peers;
  std::unordered_map<std::uint64_t, sockaddr_in> addresses;
  std::vector<std::uint8_t> receive_buffer(protocol_v12::kMaximumDatagramBytes);
  std::uint64_t last_sweep_us = NowMicroseconds();

  skate3::lua_host::LuaScriptHost script_host(options.resources_dir,
                                              skate3::lua_host::Side::kServer);
  script_host.DiscoverResources();
  g_script_host = &script_host;
  g_router = &router;
  // Connections that have sent at least one presence beacon, i.e. are in
  // the world rather than merely accepted.
  std::set<std::uint64_t> joined;
  g_convars = &options.convars;
  script_host.RegisterNative("GetConvar", &Lua_GetConvar);
  script_host.RegisterNative("GetSkaters", &Lua_GetSkaters);
  script_host.RegisterNative("GetSkater", &Lua_GetSkater);
  script_host.RegisterNative("SetPlayerRoutingBucket",
                             &Lua_SetPlayerRoutingBucket);
  script_host.RegisterNative("GetPlayerRoutingBucket",
                             &Lua_GetPlayerRoutingBucket);
  script_host.RegisterNative("GetPlayerName", &Lua_GetPlayerName);
  skate3::lua_host::AdminHttpServer admin_http(script_host, "web-console");
  admin_http.SetSettingsProvider([&options]() {
    // Only what a client needs in order to enforce a rule; the rest of
    // server.cfg is the server's own business.
    std::string json = "{\"game_difficulty\":\"";
    const auto found = options.convars.find("game_difficulty");
    json += found == options.convars.end() ? "" : found->second;
    // The rate clients should SEND their animation at. The relay can thin a
    // stream down for distant recipients but it cannot invent samples that
    // were never sent, so the ceiling has to be set at the source.
    json += "\",\"animation_hz\":" + std::to_string(options.high_hz);
    json += ",\"pose_hz\":" + std::to_string(options.pose_hz);
    json += ",\"hands_hz\":" + std::to_string(options.hands_hz);
    json += ",\"face_hz\":" + std::to_string(options.face_hz);
    json += "}";
    return json;
  });
  g_admin_http = &admin_http;
  admin_http.SetPlayersProvider(&EncodePlayersJson);
  admin_http.SetPlayerNameHandler(&HandlePlayerNameChanged);

  // Server-held player appearances. Clients upload once and peers fetch on
  // demand, replacing a peer-to-peer UDP fanout that cost O(peers) per
  // sender and, worse, transmitted on the SENDER's schedule - reliably
  // before a still-loading joiner could accept it.
  //
  // Capacity is generous relative to the player limit on purpose: changing
  // appearance mid-session leaves the previous blob behind, and the store
  // refuses to evict anything a connected player is still wearing (see
  // skate3_appearance_store.h), so a tight bound would only churn.
  static skate3::appearance_store::AppearanceStore appearance_store(
      static_cast<std::size_t>(std::max(options.max_players, 1)) * 4);
  constexpr std::size_t kMaximumAppearanceBytes = 4u * 1024u * 1024u;
  admin_http.SetAppearanceHandlers(
      [](std::uint64_t appearance_id, std::uint32_t role,
         std::vector<std::uint8_t> bytes) {
        const std::size_t size = bytes.size();
        if (!appearance_store.Put(appearance_id, std::move(bytes),
                                  kMaximumAppearanceBytes)) {
          return false;
        }
        // The role is optional so a client can pre-seed a blob without
        // claiming it. When present it is what tells peers to re-fetch.
        if (role != 0 &&
            appearance_store.SetRoleAppearance(role, appearance_id)) {
          std::printf("skate3-dedicated: role %u appearance %016llX (%zu "
                      "bytes, %zu stored)\n",
                      role,
                      static_cast<unsigned long long>(appearance_id), size,
                      appearance_store.StoredBlobs());
          RaisePlayerEvent("playerAppearanceChanged", role);
        }
        return true;
      },
      [](std::uint64_t appearance_id) {
        const auto blob = appearance_store.Get(appearance_id);
        return blob ? *blob : std::vector<std::uint8_t>{};
      },
      [radius = options.radius](std::uint32_t viewer_role,
                                std::uint64_t since, bool wait) {
        // Long-poll on THIS viewer's own answer rather than on a global
        // version counter. Position changes every tick, so a global counter
        // would wake every waiter constantly and the poll would degenerate
        // into a busy loop; hashing the answer instead means a wake happens
        // only when someone actually joins, leaves, changes outfit, or
        // crosses this viewer's visibility boundary.
        constexpr auto kWaitBudget = std::chrono::seconds(20);
        constexpr auto kReevaluate = std::chrono::milliseconds(200);
        const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
        std::vector<VisiblePeer> visible;
        std::uint64_t version = 0;
        for (;;) {
          visible = VisibleAppearances(viewer_role, radius);
          version = HashAppearanceRoster(visible);
          if (!wait || version != since ||
              std::chrono::steady_clock::now() >= deadline) {
            break;
          }
          std::this_thread::sleep_for(kReevaluate);
        }
        std::string json =
            "{\"version\":" + std::to_string(version) + ",\"peers\":[";
        bool first = true;
        for (const auto &entry : visible) {
          if (!first) {
            json += ",";
          }
          first = false;
          char id_text[19] = {};
          std::snprintf(id_text, sizeof(id_text), "%016llX",
                        static_cast<unsigned long long>(entry.appearance_id));
          json += "{\"role\":" + std::to_string(entry.role) +
                  ",\"id\":\"" + id_text + "\",\"name\":" +
                  JsonString(entry.name) + "}";
        }
        return json + "]}";
      });
  g_appearance_store = &appearance_store;

  // Networked world props. Placement is rare and small; the traffic that
  // matters is the streaming query, which every client makes about once a
  // second and which is answered from the server's own list rather than
  // from anything a client claims to know.
  static skate3::prop_store::PropStore prop_store(4096);
  admin_http.SetPropHandlers(
      [](const std::string &path, float x, float y, float z, float lod) {
        const std::uint32_t id = prop_store.Add(path, x, y, z, lod);
        if (id != 0) {
          std::printf("skate3-dedicated: prop %u '%s' at (%.1f, %.1f, %.1f) "
                      "lod %.0f (%zu stored)\n",
                      id, path.c_str(), x, y, z, lod, prop_store.Size());
        }
        return id;
      },
      [](std::uint32_t id) { return prop_store.Remove(id); },
      [](float x, float y, float z, float radius, bool all,
         std::uint64_t since, bool wait) {
        // Push, not poll: park here until the world actually changes. The
        // distance test then runs once per placement per waiting client,
        // rather than once per client per second forever.
        std::uint64_t version = prop_store.Version();
        if (wait && version <= since) {
          version = prop_store.WaitForChange(since, std::chrono::seconds(20));
        }
        const std::vector<skate3::prop_store::Prop> props =
            all ? prop_store.All() : prop_store.Near(x, y, z, radius);
        std::string json =
            wait ? "{\"version\":" + std::to_string(version) + ",\"props\":["
                 : "[";
        bool first = true;
        for (const auto &prop : props) {
          if (!first) {
            json += ",";
          }
          first = false;
          char numbers[160] = {};
          std::snprintf(numbers, sizeof(numbers),
                        ",\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"lod\":%.1f}",
                        prop.x, prop.y, prop.z, prop.lod);
          // The path is authored content from a local library, but it still
          // reaches a JSON reader, so the two characters that could break
          // the document are escaped rather than trusted.
          std::string path;
          for (const char c : prop.path) {
            if (c == '"' || c == '\\') {
              path.push_back('\\');
            }
            path.push_back(c);
          }
          json += "{\"id\":" + std::to_string(prop.id) + ",\"path\":\"" +
                  path + "\"" + numbers;
        }
        return json + (wait ? "]}" : "]");
      });
  // Without this, POST /api/console/exec echoed the line into the log and
  // did nothing: a server-side RegisterCommand was unreachable from the
  // dashboard or curl, which makes a game mode's own admin commands
  // untestable without a game client. The client has always had an
  // equivalent adapter; the server simply never grew one.
  //
  // Commands are tracked by name here rather than through SetCommandHooks
  // because the server has no console cvars to mirror them into - the
  // lookup below is the whole registry it needs. Runs on httplib's thread,
  // which is safe: InvokeCommand is internally locked, exactly as the
  // /api/resources routes already rely on.
  //
  // The registry is built from SetCommandHooks - the same mechanism the
  // client uses to mirror commands into console cvars - because the host
  // exposes no way to enumerate them, and a stale entry would call into a
  // closed lua_State after a resource restart. The unregister hook is what
  // keeps that from happening.
  struct ServerCommand {
    std::string resource;
    int callback_ref;
  };
  static std::mutex command_mutex;
  static std::map<std::string, ServerCommand> commands;
  script_host.SetCommandHooks(
      [](const std::string &resource, const std::string &command,
         int callback_ref, lua_State *) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands[command] = ServerCommand{resource, callback_ref};
      },
      [](const std::string &command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.erase(command);
      });
  // /api/metrics: relay bandwidth/hitches merged with Lua resmon under one
  // JSON object - see AdminHttpServer::SetMetricsProvider's own comment.
  admin_http.SetMetricsProvider([&script_host] {
    return "{\"relay\":" + skate3::dedicated::metrics::Json() +
          ",\"resources\":" + script_host.ResourceMetricsJson() + "}";
  });
  admin_http.SetConsoleExecHandler([&script_host](std::string_view line) {
    // resmon and netstat are built into the host/relay loop rather than
    // Lua commands, so they work even with zero resources ensured - the
    // same reasoning as the client's own "resmon" special-case.
    //
    // resmon 1 / resmon 0 toggles collection - OFF by default, since it has
    // a real, measured per-frame cost (see ScopedResourceTimer's comment in
    // skate3_lua_script_host.cpp) that a server relaying every player's
    // traffic should not pay unless someone is actually looking.
    if (line == "resmon" || line == "resmon 1" || line == "resmon 0") {
      if (line != "resmon") {
        skate3::lua_host::LuaScriptHost::SetMetricsEnabled(line == "resmon 1");
      }
      if (!skate3::lua_host::LuaScriptHost::MetricsEnabled()) {
        script_host.log_sink().Append(
            "", "[resmon] collection is off - \"resmon 1\" to enable, then "
                "\"resmon\" for a snapshot");
      } else {
        script_host.log_sink().Append("",
                                      script_host.FormatResourceMetricsTable());
      }
      return;
    }
    if (line == "netstat") {
      script_host.log_sink().Append("", skate3::dedicated::metrics::FormatTable());
      return;
    }
    const auto separator = line.find(' ');
    const std::string name(line.substr(0, separator));
    std::string args;
    if (separator != std::string_view::npos) {
      args = std::string(line.substr(separator + 1));
      while (!args.empty() && args.front() == ' ') {
        args.erase(args.begin());
      }
    }
    std::string resource;
    int callback_ref = 0;
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      const auto entry = commands.find(name);
      if (entry == commands.end()) {
        script_host.log_sink().Append("", "unknown command: " + name);
        return;
      }
      resource = entry->second.resource;
      callback_ref = entry->second.callback_ref;
    }
    script_host.InvokeCommand(resource, callback_ref, args);
  });
  // Server-side TriggerClientEvent queues onto the HTTP feed that connected
  // clients poll. Wired before any resource starts, so an event fired at
  // load time is not silently dropped.
  // TriggerClientEvent goes out on the game protocol's reliable channel.
  // `target` is a player id as a string, or empty for everyone - the same
  // contract the HTTP transport had, so no script changes.
  script_host.SetEventOutboundHandler(
      [](const std::string &event, const std::string &json_args,
         const std::string &target) {
        std::uint16_t role = protocol_v12::kReliableBroadcastRole;
        if (!target.empty()) {
          const long parsed = std::strtol(target.c_str(), nullptr, 10);
          if (parsed <= 0 || parsed > 0xFFFE) {
            return;  // addressed at nobody real; dropping beats broadcasting.
          }
          role = static_cast<std::uint16_t>(parsed);
        }
        QueueClientScriptEvent(role, event, json_args);
      });
  // server.cfg's ensure lines decide what runs. With none declared, every
  // discovered resource starts - convenient for local development, and the
  // behaviour this had before the config file existed.
  if (options.ensured.empty()) {
    for (const auto &resource : script_host.ListResources()) {
      script_host.EnsureResource(resource.name);
    }
  } else {
    for (const auto &name : options.ensured) {
      if (!script_host.EnsureResource(name)) {
        std::fprintf(stderr,
                     "skate3-dedicated: ensure '%s' failed (no such resource "
                     "in %s)\n",
                     name.c_str(), options.resources_dir.c_str());
      }
    }
  }
  if (admin_http.Start(options.admin_port)) {
    std::printf("skate3-dedicated: dev console listening on http://127.0.0.1:%d\n",
               options.admin_port);
  } else {
    std::fprintf(stderr,
                 "skate3-dedicated: dev console failed to bind port %d\n",
                 options.admin_port);
  }

  for (;;) {
    // Measures the WHOLE iteration, sleep excluded (see below) - that sleep
    // is deliberate idle throttling, not work, and including it would make
    // every quiet iteration read as a near-hitch and bury real ones under
    // noise.
    const std::uint64_t iteration_start_us = NowMicroseconds();
    sockaddr_in sender{};
#if defined(_WIN32)
    int sender_length = sizeof(sender);
#else
    socklen_t sender_length = sizeof(sender);
#endif
    const int received =
        recvfrom(relay_socket, reinterpret_cast<char *>(receive_buffer.data()),
                static_cast<int>(receive_buffer.size()), 0,
                reinterpret_cast<sockaddr *>(&sender), &sender_length);
    const std::uint64_t now_us = NowMicroseconds();

    if (received > 0) {
      skate3::dedicated::metrics::RecordReceive(static_cast<std::size_t>(received));
      const std::uint64_t connection_id = ConnectionIdFor(sender);
      addresses[connection_id] = sender;
      const std::span<const std::uint8_t> packet(
          receive_buffer.data(), static_cast<std::size_t>(received));

      protocol_v12::Envelope envelope;
      std::uint32_t legacy_sender_role = 0;
      std::uint32_t legacy_sender_session = 0;
      if (protocol_v12::DecodeEnvelope(packet, envelope)) {
        router.Touch(connection_id, now_us);
        const auto payload = packet.subspan(protocol_v12::kEnvelopeBytes);
        if (envelope.kind == protocol_v12::MessageKind::kRelayRegister) {
          HandleRegister(relay_socket, router, roles, connection_id, sender,
                        token_hash, payload, now_us,
                        static_cast<std::uint16_t>(options.admin_port));
        } else if (envelope.kind ==
                  protocol_v12::MessageKind::kPresenceBeacon) {
          protocol_v12::PresenceBeacon beacon;
          if (protocol_v12::DecodePresenceBeacon(payload, beacon)) {
            // The first beacon is the moment a connected client is actually
            // in the world with a position - that is what "joining" means
            // here, as opposed to "connecting" (accepted, not yet spawned).
            const bool first_beacon = !joined.contains(connection_id);
            (void)router.UpdatePresence(connection_id, beacon.map_hash,
                                        beacon.x, beacon.y, beacon.z, now_us);
            if (first_beacon) {
              joined.insert(connection_id);
              for (const auto &peer : router.Peers()) {
                if (peer.connection_id == connection_id) {
                  RaisePlayerEvent("playerJoining", peer.role);
                  break;
                }
              }
            }
          }
        } else if (envelope.kind ==
                       protocol_v12::MessageKind::kReliableStream ||
                   envelope.kind ==
                       protocol_v12::MessageKind::kReliableAck) {
          // Consumed here, never relayed: script events are a
          // client<->server conversation and the server decides who hears
          // what. Forwarding them would also bypass the interest management
          // applied below, which must never thin a game action.
          HandleScriptEventPacket(script_peers, script_host, router,
                                  connection_id, packet, envelope);
        } else {
          const auto route =
              router.Route(connection_id, packet, /*target_role=*/0,
                          options.radius, options.tiers, options.crowd,
                          fidelity_rates);
          if (route.disposition == RelayDisposition::kForward) {
            for (const std::uint64_t recipient :
                route.recipient_connections) {
              const auto address = addresses.find(recipient);
              if (address != addresses.end()) {
                sendto(relay_socket,
                      reinterpret_cast<const char *>(packet.data()),
                      static_cast<int>(packet.size()), 0,
                      reinterpret_cast<const sockaddr *>(&address->second),
                      sizeof(address->second));
                skate3::dedicated::metrics::RecordSend(packet.size());
              }
            }
          }
        }
      } else if (ExtractLegacyIdentity(packet, legacy_sender_role,
                                       legacy_sender_session)) {
        router.Touch(connection_id, now_us);
        const auto route = router.RouteRaw(
            connection_id, legacy_sender_role, legacy_sender_session,
            /*target_role=*/0, options.radius);
        if (route.disposition == RelayDisposition::kForward) {
          for (const std::uint64_t recipient : route.recipient_connections) {
            const auto address = addresses.find(recipient);
            if (address != addresses.end()) {
              sendto(relay_socket, reinterpret_cast<const char *>(packet.data()),
                    static_cast<int>(packet.size()), 0,
                    reinterpret_cast<const sockaddr *>(&address->second),
                    sizeof(address->second));
              skate3::dedicated::metrics::RecordSend(packet.size());
            }
          }
        }
      }
    }

    if (now_us - last_sweep_us > kSweepIntervalMicroseconds) {
      // Roles have to be read BEFORE RemoveStale erases the peers, or the
      // drop event has no id to report.
      std::unordered_map<std::uint64_t, std::uint32_t> role_by_connection;
      for (const auto &peer : router.Peers()) {
        role_by_connection[peer.connection_id] = peer.role;
      }
      const auto stale = router.RemoveStale(now_us, kStaleTimeoutMicroseconds);
      // After the stale peers are gone, so a departed player stops counting
      // toward a crowd immediately rather than a sweep later. Once per
      // sweep is deliberate: a crowd does not form inside a second, and
      // this is the only O(peers^2) work in the loop.
      router.RefreshCrowdCounts(options.tiers.high_distance);
      for (const std::uint64_t connection_id : stale) {
        addresses.erase(connection_id);
        roles.Release(connection_id);
        joined.erase(connection_id);
        const auto role = role_by_connection.find(connection_id);
        if (role != role_by_connection.end()) {
          g_scopes.Forget(role->second);
          if (g_appearance_store != nullptr) {
            // The blob itself is kept - a reconnecting player is the most
            // likely next uploader of exactly these bytes.
            g_appearance_store->ForgetRole(role->second);
          }
          RaisePlayerEvent("playerDropped", role->second);
        }
        std::printf("skate3-dedicated: connection %llu timed out\n",
                    static_cast<unsigned long long>(connection_id));
      }
      last_sweep_us = now_us;
    }

    // Republish who is connected and where, for GetSkaters/GetSkater. Done
    // on this thread so scripts never read the router directly while the
    // relay loop is mutating it.
    {
      // Applied here rather than in the native, because the router is only
      // safe to mutate from this thread.
      std::lock_guard<std::mutex> lock(g_bucket_mutex);
      for (const auto &[role, bucket] : g_pending_buckets) {
        (void)router.SetBucket(role, bucket);
      }
      g_pending_buckets.clear();
    }
    {
      std::lock_guard<std::mutex> lock(g_name_mutex);
      for (auto &[role, name] : g_pending_names) {
        (void)router.SetName(role, std::move(name));
      }
      g_pending_names.clear();
    }
    g_players.Publish(router.Peers());
    g_scopes.Update(g_players.All(), options.radius);
    script_host.Tick();
    DrainScriptEvents(relay_socket, script_peers, router, addresses, now_us);

    // Measured before the idle sleep below, deliberately - see RelayMetrics'
    // own comment for why the sleep must not count as loop work.
    skate3::dedicated::metrics::RecordIteration(
        now_us, NowMicroseconds() - iteration_start_us);

    if (received <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
}
