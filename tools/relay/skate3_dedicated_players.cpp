// Who is connected, where they are, and the scope events that follow from it.
//
// Split out of the server's main translation unit. This is session state -
// identity, position, routing bucket, interest scope - which the server IS
// authoritative over. Player MOVEMENT is not: clients send their own pose
// and the server forwards it. Nothing here should start simulating skaters.

#include "skate3_dedicated_server.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace skate3::dedicated {

using skate3::multiplayer::routing::RelayPeer;

namespace {

// Set once in main before the loop starts; see SetScriptHost.
skate3::lua_host::LuaScriptHost *g_script_host = nullptr;

}  // namespace

void SetScriptHost(skate3::lua_host::LuaScriptHost *host) {
  g_script_host = host;
}

void PlayerRegistry::Publish(const std::vector<RelayPeer> &peers) {
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

std::vector<PlayerRegistry::Player> PlayerRegistry::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return players_;
  }

std::optional<PlayerRegistry::Player> PlayerRegistry::Find(
    std::uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &player : players_) {
      if (player.id == id) {
        return player;
      }
    }
    return std::nullopt;
  }

PlayerRegistry g_players;


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

// Recomputes every pair and raises the transitions. `radius` of 0 means
  // unlimited, in which case everyone on the same map is in scope.
void ScopeTracker::Update(
    const std::vector<PlayerRegistry::Player> &players, float radius) {
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
void ScopeTracker::Forget(std::uint32_t player_id) {
    for (auto it = in_scope_.begin(); it != in_scope_.end();) {
      it = (it->first == player_id || it->second == player_id)
               ? in_scope_.erase(it)
               : std::next(it);
    }
  }

void ScopeTracker::RaiseScopeEvent(const char *event, std::uint32_t observer,
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


}  // namespace skate3::dedicated
