// The server's Lua native surface, and the player-name HTTP handler that
// feeds it.
//
// This is the file to extend when adding server natives: register them in
// main alongside the existing RegisterNative calls and implement them here.
//
// THREADING: natives run on whichever thread the script host is ticking, and
// the HTTP handlers run on httplib workers - neither is the relay thread,
// and the router is not thread-safe. Anything that has to mutate the router
// is therefore queued (see the pending-bucket and pending-name vectors) and
// applied by the relay loop. Read-only views come from the PlayerRegistry
// snapshot instead, which has its own lock.

#include "skate3_dedicated_server.h"

#include "skate3_lua_admin_http.h"
#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_protocol_v12_reliable.h"

#include <cstdlib>
#include <lua.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skate3::dedicated {

namespace protocol_v12 = skate3::multiplayer::protocol_v12;
using skate3::multiplayer::routing::VisualRelayRouter;

void PushPlayerTable(lua_State *L, const skate3::dedicated::PlayerRegistry::Player &player) {
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
  return "\"" + skate3::dedicated::EscapeJsonString(text) + "\"";
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
      for (const auto &player : skate3::dedicated::g_players.All()) {
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
  for (const auto &player : skate3::dedicated::g_players.All()) {
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
  const auto player = skate3::dedicated::g_players.Find(id);
  lua_pushinteger(L, player ? player->bucket : 0);
  return 1;
}

// GetPlayerName(id) -> that player's self-reported name, or "" if they
// have not sent one yet (e.g. a client too old to know about
// POST /api/players/name, or one that has not connected long enough to).
int Lua_GetPlayerName(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto player = skate3::dedicated::g_players.Find(id);
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
  for (const auto &player : skate3::dedicated::g_players.All()) {
    lua_pushinteger(L, player.id);
    lua_rawseti(L, -2, index++);
  }
  return 1;
}

// GetSkater(id) -> table describing that player, or nil if not connected.
int Lua_GetSkater(lua_State *L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  const auto player = skate3::dedicated::g_players.Find(id);
  if (!player) {
    lua_pushnil(L);
    return 1;
  }
  PushPlayerTable(L, *player);
  return 1;
}


}  // namespace skate3::dedicated
