#include "skate3_lua_client_natives.h"

#include "skate3_appearance_service.h"

#include "skate3_cef_nui.h"
#include "skate3_input_state.h"
#include "skate3_native_scene.h"
#include "skate3_multiplayer.h"
#include "skate3_clothing.h"
#include "skate3_demo_path.h"
#include "skate3_flash_bridge.h"
#include "skate3_guest_probe.h"
#include "skate3_retail_ui_hooks.h"
#include "skate3_function_coverage.h"
#include "skate3_lua_admin_http.h"
#include "skate3_lua_script_host.h"
#include "skate3_mechanics_sandbox.h"
#include <exception>
#include <filesystem>
#include <skate/world/skate_object_package.h>
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_prop_service.h"
#include "skate3_trick_pipeline.h"

#include <httplib.h>
#include <lua.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace skate3::lua_client {

namespace {

// How long a NUI callback may sit unanswered before it is answered with
// an empty object on the resource's behalf. Generous: a handler that goes
// to the server and back is legitimately slow, and the only cost of
// waiting is one pending fetch() in the page.
constexpr std::int64_t kNuiCallbackTimeoutMs = 15000;

std::unique_ptr<lua_host::LuaScriptHost> g_host;
std::unique_ptr<lua_host::AdminHttpServer> g_admin;
// The port g_admin actually bound; see DevConsoleAdminPort().
int g_admin_port = 0;

int Lua_SetEntityPosition(lua_State* L) {
  const lua_Integer entity = luaL_checkinteger(L, 1);
  if (entity != 0) {
    return luaL_error(
        L, "SetEntityPosition: only entity 0 (local player) is supported");
  }
  const float x = static_cast<float>(luaL_checknumber(L, 2));
  const float y = static_cast<float>(luaL_checknumber(L, 3));
  const float z = static_cast<float>(luaL_checknumber(L, 4));
  mechanics_sandbox::RequestLocalPlayerTeleport(x, y, z);
  return 0;
}

// Finds `role` in the cross-thread published remote-player list (see
// multiplayer::LatestRemotePlayers' own comment for why that list exists
// and where it is safe to read from). Shared by every native below that
// takes a role rather than just entity 0.
bool FindRemotePlayerByRole(uint32_t role, multiplayer::RemotePlayer& out) {
  const auto players = multiplayer::LatestRemotePlayers();
  if (!players) {
    return false;
  }
  for (const multiplayer::RemotePlayer& player : *players) {
    if (player.role == role) {
      out = player;
      return true;
    }
  }
  return false;
}

// GetEntityCoords(0) -> x, y, z                the local player
// GetEntityCoords(role) -> x, y, z              a remote player, by the same
//                                                role GetSkaters()/the
//                                                relay's own logging use
//
// Returns 3 values, matching FiveM's own natives.lua "returns multiple
// numbers rather than a vector3" convention for entity coords.
//
// For a remote player this is their replicated BOARD position - the same
// thing entity 0 means for the local player (see RemotePose's own comment:
// positions are the board/root transform, not a separate rider position -
// there is only one to ask for, same as locally).
int Lua_GetEntityCoords(lua_State* L) {
  const lua_Integer entity = luaL_checkinteger(L, 1);
  if (entity == 0) {
    float position[3];
    if (!trick_pipeline::CurrentLocalBoardPosition(position)) {
      return luaL_error(L, "GetEntityCoords: local player position not available");
    }
    lua_pushnumber(L, position[0]);
    lua_pushnumber(L, position[1]);
    lua_pushnumber(L, position[2]);
    return 3;
  }
  multiplayer::RemotePlayer remote;
  if (!FindRemotePlayerByRole(static_cast<uint32_t>(entity), remote)) {
    return luaL_error(L, "GetEntityCoords: no such entity %d", int(entity));
  }
  lua_pushnumber(L, remote.pose.position[0]);
  lua_pushnumber(L, remote.pose.position[1]);
  lua_pushnumber(L, remote.pose.position[2]);
  return 3;
}

// GetPlayerCoords(role) -> x, y, z
//
// The same lookup as GetEntityCoords(role), spelled for a script that is
// already thinking in player roles rather than a generic "entity" concept -
// GetSkaters()/GetPlayerId() and the console's own "role=" logging all use
// this same number.
int Lua_GetPlayerCoords(lua_State* L) {
  const lua_Integer role = luaL_checkinteger(L, 1);
  multiplayer::RemotePlayer remote;
  if (!FindRemotePlayerByRole(static_cast<uint32_t>(role), remote)) {
    return luaL_error(L, "GetPlayerCoords: no such player %d", int(role));
  }
  lua_pushnumber(L, remote.pose.position[0]);
  lua_pushnumber(L, remote.pose.position[1]);
  lua_pushnumber(L, remote.pose.position[2]);
  return 3;
}

// GetActivePlayers() -> table of roles
//
// Every remote player role this client currently has a published position
// for - the local player is never included (it has no role the way remote
// players do; use entity 0). A script that wants "everyone" iterates this
// and calls GetPlayerCoords on each.
int Lua_GetActivePlayers(lua_State* L) {
  const auto players = multiplayer::LatestRemotePlayers();
  lua_newtable(L);
  if (!players) {
    return 1;
  }
  int index = 1;
  for (const multiplayer::RemotePlayer& player : *players) {
    lua_pushinteger(L, static_cast<lua_Integer>(player.role));
    lua_rawseti(L, -2, index++);
  }
  return 1;
}

// GetSyncedProps() -> table of { id, path, x, y, z, lod }
//
// Not to be confused with ListProps(filter, limit), which lists prop ASSET
// FILES available on disk to spawn (objects/**) - this lists prop
// INSTANCES that actually exist in the shared world right now, placed by
// anyone (propplace, SpawnProp, or another client), by the network id the
// server assigned each one. This is prop_service's own record, filled as
// the client hears about each prop and never drained by this call - taking
// this list does not stop the frame loop from still spawning any of them.
int Lua_GetSyncedProps(lua_State* L) {
  const std::vector<prop_service::Spawn> props = prop_service::ListKnownProps();
  lua_newtable(L);
  int index = 1;
  for (const prop_service::Spawn& prop : props) {
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(prop.id));
    lua_setfield(L, -2, "id");
    lua_pushstring(L, prop.path.c_str());
    lua_setfield(L, -2, "path");
    lua_pushnumber(L, prop.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, prop.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, prop.z);
    lua_setfield(L, -2, "z");
    lua_pushnumber(L, prop.lod);
    lua_setfield(L, -2, "lod");
    lua_rawseti(L, -2, index++);
  }
  return 1;
}

// --- Scripted camera --------------------------------------------------
//
// A small FiveM-shaped camera API, backed by skate3_native_scene.h's
// ScriptCam* functions - see that header's own comment for how it takes
// over the guest's camera. This replaced the player-facing freecam/map
// -editor keybinds (G/E/End), which are gone now: this is the only way
// left to fly the camera, and it is Lua's to use, not a player's to press
// a key for.
//
// CreateCam() -> handle
//
// A handle is a small integer; 0 is never valid. Cheap - it is a slot in a
// small pool, not anything the renderer touches until SetCamActive(true).
int Lua_CreateCam(lua_State* L) {
  lua_pushinteger(L, native_scene::ScriptCamCreate());
  return 1;
}

// DestroyCam(cam)
//
// If `cam` is the active one, the guest camera is handed back next frame -
// there is no separate "deactivate before destroying" step needed.
int Lua_DestroyCam(lua_State* L) {
  const int handle = static_cast<int>(luaL_checkinteger(L, 1));
  native_scene::ScriptCamDestroy(handle);
  return 0;
}

// MoveCam(cam, x, y, z) -> boolean
//
// Instant, not animated - sets where the camera IS. A script wanting a
// smooth move calls this every frame (or every Skate.Wait) with the next
// point along its own path, the same way FiveM scripts drive SetCamCoord.
int Lua_MoveCam(lua_State* L) {
  const int handle = static_cast<int>(luaL_checkinteger(L, 1));
  const float x = static_cast<float>(luaL_checknumber(L, 2));
  const float y = static_cast<float>(luaL_checknumber(L, 3));
  const float z = static_cast<float>(luaL_checknumber(L, 4));
  lua_pushboolean(L, native_scene::ScriptCamSetCoord(handle, x, y, z));
  return 1;
}

// LookAtCam(cam, x, y, z) -> boolean          point at a world coordinate
// LookAtCam(cam, entity)  -> boolean          point at an entity's position
//
// Distinguished by argument count, the same way FiveM keeps
// PointCamAtCoord/PointCamAtEntity separate but this exposes them as one
// name. `entity` follows GetEntityCoords' own limit: only 0 (the local
// player) is supported today, because that is the only position this
// client can read outside of what the relay tells it about other players.
int Lua_LookAtCam(lua_State* L) {
  const int handle = static_cast<int>(luaL_checkinteger(L, 1));
  const int argument_count = lua_gettop(L);
  if (argument_count >= 4) {
    const float x = static_cast<float>(luaL_checknumber(L, 2));
    const float y = static_cast<float>(luaL_checknumber(L, 3));
    const float z = static_cast<float>(luaL_checknumber(L, 4));
    lua_pushboolean(L, native_scene::ScriptCamPointAtCoord(handle, x, y, z));
    return 1;
  }
  const lua_Integer entity = luaL_checkinteger(L, 2);
  if (entity != 0) {
    return luaL_error(
        L, "LookAtCam: only entity 0 (local player) is supported - pass "
           "x, y, z to look at a world coordinate instead");
  }
  float position[3];
  if (!trick_pipeline::CurrentLocalBoardPosition(position)) {
    return luaL_error(L, "LookAtCam: local player position not available");
  }
  lua_pushboolean(L, native_scene::ScriptCamPointAtCoord(
                         handle, position[0], position[1], position[2]));
  return 1;
}

// SetCamActive(cam, active) -> boolean
//
// active=true makes `cam` THE camera the game renders through, replacing
// whichever one was active before (there is only ever one). active=false
// hands control back to the guest's own camera. Returns false if `cam`
// does not exist (already destroyed, or never created).
int Lua_SetCamActive(lua_State* L) {
  const int handle = static_cast<int>(luaL_checkinteger(L, 1));
  const bool active = lua_toboolean(L, 2) != 0;
  lua_pushboolean(L, native_scene::ScriptCamSetActive(handle, active));
  return 1;
}

// IsCamActive(cam) -> boolean
int Lua_IsCamActive(lua_State* L) {
  const int handle = static_cast<int>(luaL_checkinteger(L, 1));
  lua_pushboolean(L, native_scene::ScriptCamIsActive(handle));
  return 1;
}

// IsPlayerSpawned() -> true once the local skater exists in the world with a
// real position.
//
// Worth having as its own native because teleporting before it is true does
// not fail in any visible way: the cMsgTeleport post succeeds, and then the
// spawn sequence runs afterwards and puts the player back where it wanted
// them. Measured directly - a teleport fired at connect logged "post sent"
// and the player still ended up at the default spawn. Anything that moves a
// player on join has to wait for this first.
int Lua_IsPlayerSpawned(lua_State* L) {
  float position[3];
  lua_pushboolean(L, trick_pipeline::CurrentLocalBoardPosition(position));
  return 1;
}

// --- Input ----------------------------------------------------------
//
// Controls are named, not numbered: "DPADUP", "A", "KEY_E". A numeric table
// like FiveM's would mean maintaining a mapping document nobody can read
// from the script, and the names cost nothing to look up once per call.
//
// An unknown name resolves to no control and therefore reads as never
// pressed, which is what makes a typo behave like a control the player
// simply is not touching rather than an error mid-frame.

int Lua_IsControlPressed(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_pushboolean(
      L, input_state::IsPressed(input_state::ControlFromName(name)));
  return 1;
}

int Lua_IsControlJustPressed(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_pushboolean(
      L, input_state::IsJustPressed(input_state::ControlFromName(name)));
  return 1;
}

int Lua_IsControlJustReleased(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_pushboolean(
      L, input_state::IsJustReleased(input_state::ControlFromName(name)));
  return 1;
}

// GetControlValue(axis) -> -1..1 for sticks, 0..1 for triggers.
int Lua_GetControlValue(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  lua_pushnumber(L, input_state::Value(input_state::AxisFromName(name)));
  return 1;
}

// IsPadConnected() -> whether any pad is present, so a script can offer
// keyboard fallbacks rather than silently doing nothing.
int Lua_IsPadConnected(lua_State* L) {
  lua_pushboolean(L, input_state::PadConnected());
  return 1;
}

// --- Player state ---------------------------------------------------
//
// These read retail's OWN state rather than guessing from position. The
// board-state provider's bytes were reverse-engineered and verified
// earlier: IsOffboard is byte 673 || byte 675, IsAirOffboard is byte 674,
// and retail hands its own ground predicate into the same sampling point.
//
// Each returns false/0 when there is no sample yet, which means "not in
// gameplay", NOT "on the ground and stationary" - a game mode must not
// treat the two as the same thing, so IsSkaterInAir deliberately reports
// false rather than guessing before the first sample.

// IsOffBoard() -> true when the skater is not on the board (bailed, walking).
int Lua_IsOffBoard(lua_State* L) {
  bool offboard = false, air_offboard = false, on_ground = false;
  const bool known =
      trick_pipeline::CurrentLocalBoardState(offboard, air_offboard, on_ground);
  lua_pushboolean(L, known && offboard);
  return 1;
}

// IsAirOffBoard() -> bailed AND airborne, which retail tracks separately
// from plain offboard because the recovery differs.
int Lua_IsAirOffBoard(lua_State* L) {
  bool offboard = false, air_offboard = false, on_ground = false;
  const bool known =
      trick_pipeline::CurrentLocalBoardState(offboard, air_offboard, on_ground);
  lua_pushboolean(L, known && air_offboard);
  return 1;
}

// IsSkaterInAir() -> the negation of retail's own ground predicate.
int Lua_IsSkaterInAir(lua_State* L) {
  bool offboard = false, air_offboard = false, on_ground = false;
  const bool known =
      trick_pipeline::CurrentLocalBoardState(offboard, air_offboard, on_ground);
  lua_pushboolean(L, known && !on_ground);
  return 1;
}

// IsSkaterOnGround() -> retail's ground predicate as-is. Not simply the
// inverse of IsSkaterInAir before the first sample, where both are false.
int Lua_IsSkaterOnGround(lua_State* L) {
  bool offboard = false, air_offboard = false, on_ground = false;
  const bool known =
      trick_pipeline::CurrentLocalBoardState(offboard, air_offboard, on_ground);
  lua_pushboolean(L, known && on_ground);
  return 1;
}

// WorldToScreen(x, y, z) -> sx, sy, depth
//
// Normalized screen fractions (0..1, origin top-left) for a world point,
// plus the distance in front of the camera. Returns nil when the point is
// BEHIND the camera or the projection is unavailable - a marker script must
// hide the marker on nil rather than draw it at 0,0, which would pin it to
// the top-left corner every time the player skates past it.
//
// x/y outside 0..1 is legitimate and means "in front of you but off to the
// side"; only nil means "do not draw".
//
// Answers only while the NATIVE renderer is engaged, because the
// view-projection this uses is published by the native scene build and that
// does not run in Emulated mode.
int Lua_WorldToScreen(lua_State* L) {
  const float world[3] = {static_cast<float>(luaL_checknumber(L, 1)),
                          static_cast<float>(luaL_checknumber(L, 2)),
                          static_cast<float>(luaL_checknumber(L, 3))};
  float sx = 0.0f, sy = 0.0f, depth = 0.0f;
  if (!native_scene::WorldToScreen(world, sx, sy, depth)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, sx);
  lua_pushnumber(L, sy);
  lua_pushnumber(L, depth);
  return 3;
}

// ListProps([filter][, limit]) -> { "objects/Cat/name.skateobj", ... }
//
// Enumerates the staged object library. This is the retail DMO set imported
// to .skateobj packages - 652 of them across category folders - so a caller
// needs a way to find one by name rather than guessing a path.
//
// Case-insensitive substring match on the whole relative path, so both
// "rail" and "DMO_Rails" work.
// --- Wardrobe (read-only) -------------------------------------------
//
// The shape a script wants is FiveM's: how many are there, give me the nth,
// what am I wearing. Nothing here CHANGES an outfit - see skate3_clothing.h
// for why applying is a separate problem.

// Parses "0x...." or a bare hex string into a 64-bit id. Hex only, and
// never a Lua number: an id is 64-bit and a double would round it, which
// silently names a different item.
bool ParseClothingId(const char* text, std::uint64_t& out) {
  if (text == nullptr) {
    return false;
  }
  std::string_view view(text);
  if (view.size() > 2 && view[0] == '0' && (view[1] == 'x' || view[1] == 'X')) {
    view.remove_prefix(2);
  }
  if (view.empty() || view.size() > 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : view) {
    const int digit = std::isxdigit(static_cast<unsigned char>(c))
                          ? (std::isdigit(static_cast<unsigned char>(c))
                                 ? c - '0'
                                 : std::tolower(c) - 'a' + 10)
                          : -1;
    if (digit < 0) {
      return false;
    }
    value = (value << 4) | static_cast<std::uint64_t>(digit);
  }
  out = value;
  return value != 0;
}

// SetClothingModel(category, modelId) -> ok, message
//
// Overrides a category's model in the live recipe. Returns a message as the
// second value rather than erroring, because the interesting failures
// ("that id is not in that category") are things a player typing a command
// should read, not a script crash.
int Lua_SetClothingModel(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  const char* id_text = luaL_checkstring(L, 2);
  std::uint64_t model_id = 0;
  if (!ParseClothingId(id_text, model_id)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "model id must be hex, e.g. 0x00000dca03e38811");
    return 2;
  }
  // Checked against the catalogue first: writing an id the archive does not
  // contain would leave a recipe naming a model nothing can load, and the
  // symptom (an invisible or default piece) would not point at the cause.
  char canonical[32];
  std::snprintf(canonical, sizeof(canonical), "0x%016llx",
                static_cast<unsigned long long>(model_id));
  if (clothing::Ids(category, canonical, 1).empty()) {
    lua_pushboolean(L, false);
    lua_pushfstring(L, "%s is not in category %s", canonical, category);
    return 2;
  }
  if (!clothing::SetOverride(category, model_id)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "override rejected");
    return 2;
  }
  lua_pushboolean(L, true);
  lua_pushstring(L, canonical);
  return 2;
}

// ClearClothing([category]) -> nothing. No argument clears every override.
int Lua_ClearClothing(lua_State* L) {
  clothing::ClearOverride(luaL_optstring(L, 1, ""));
  return 0;
}

// --- Retail front end ------------------------------------------------

// GetLastFrontEndState() -> number
//
// The id of the most recent screen retail's front end was asked to show.
// This is the DISCOVERY tool for OpenCharacterEditor: the state machine is
// numeric and the binary never names its screens, so the way to learn the
// character editor's id is to walk to it by hand and read this.
int Lua_GetLastFrontEndState(lua_State* L) {
  lua_pushinteger(
      L, static_cast<lua_Integer>(demo_path::LastRequestedFrontEndState()));
  return 1;
}

// GetFrontEndInfo() -> { state, manager, calls, history = { {state,mode,lr}, ... } }
//
// `calls` is the one that matters when a screen will not open: if it does not
// move while you navigate to that screen, the screen is not driven by the
// state machine and no id will ever reach it.
int Lua_GetFrontEndInfo(lua_State* L) {
  lua_newtable(L);
  lua_pushinteger(
      L, static_cast<lua_Integer>(demo_path::LastRequestedFrontEndState()));
  lua_setfield(L, -2, "state");
  char manager[16];
  std::snprintf(manager, sizeof(manager), "0x%08X",
                demo_path::LastFrontEndManager());
  lua_pushstring(L, manager);
  lua_setfield(L, -2, "manager");
  lua_pushinteger(
      L, static_cast<lua_Integer>(demo_path::FrontEndStateCallCount()));
  lua_setfield(L, -2, "calls");
  lua_newtable(L);
  int index = 0;
  for (const demo_path::FrontEndStateEvent& event :
       demo_path::RecentFrontEndStates()) {
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(event.state_id));
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, static_cast<lua_Integer>(event.mode));
    lua_setfield(L, -2, "mode");
    char lr[16];
    std::snprintf(lr, sizeof(lr), "0x%08X", event.caller_lr);
    lua_pushstring(L, lr);
    lua_setfield(L, -2, "lr");
    lua_rawseti(L, -2, ++index);
  }
  lua_setfield(L, -2, "history");
  return 1;
}

// CallFlashMethod(id, addr1, addr2, ...) -> ok, message
//
// Invokes an ActionScript method on retail's front end. `id` indexes the
// method table dumped by the Probe resource's "screentable"; each further
// argument is a GUEST ADDRESS passed straight through, which for a string
// argument is a pointer to NUL-terminated bytes already in guest memory.
//
// Runs at the next physics tick, on the guest thread. This is the first thing
// here that calls INTO retail rather than observing it, so treat a first run
// as an experiment.
int Lua_CallFlashMethod(lua_State* L) {
  const lua_Integer method_id = luaL_checkinteger(L, 1);
  if (method_id < 0 || method_id > 255) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "method id out of range");
    return 2;
  }
  std::uint32_t arguments[flash_bridge::kMaxArguments] = {};
  int count = 0;
  const int top = lua_gettop(L);
  for (int index = 2; index <= top && count < flash_bridge::kMaxArguments;
       ++index) {
    std::uint64_t address = 0;
    if (!ParseClothingId(luaL_checkstring(L, index), address)) {
      lua_pushboolean(L, false);
      lua_pushfstring(L, "argument %d is not a hex address", index - 1);
      return 2;
    }
    arguments[count++] = static_cast<std::uint32_t>(address);
  }
  if (!flash_bridge::RequestCall(static_cast<std::uint32_t>(method_id),
                                 arguments, count)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "a front-end call is already queued");
    return 2;
  }
  lua_pushboolean(L, true);
  lua_pushfstring(L, "queued method %d with %d argument(s)", (int)method_id,
                  count);
  return 2;
}

// --- Function coverage -----------------------------------------------
//
// Which retail functions ran. This is how an entry point is found when static
// search fails: arm, do the thing by hand, disarm, and compare against a
// baseline capture. Whatever is in one and not the other is the code for that
// action.
//
// Needed because searching the recompiled sources for a reference to a name
// string did not work - the compiler sets up one base register and reaches
// many strings by displacement, so there is no instruction pair to match.
// Watching the game do the thing sidesteps all of that.

// StartCoverage() -> true
int Lua_StartCoverage(lua_State* L) {
  function_coverage::ResetAndArm();
  lua_pushboolean(L, true);
  return 1;
}

// StopCoverage(label) -> count, path
//
// Writes the addresses to a file rather than returning them: a capture is
// tens of thousands of entries, which is a diff input, not something to read
// in a console.
int Lua_StopCoverage(lua_State* L) {
  const char* label = luaL_optstring(L, 1, "capture");
  const std::vector<std::uint32_t> addresses =
      function_coverage::SnapshotAndDisarm();
  std::string name = "coverage_";
  for (const char* c = label; *c != 0; ++c) {
    name.push_back(std::isalnum(static_cast<unsigned char>(*c)) ? *c : '_');
  }
  name += ".txt";
  // Next to the executable, not the working directory. A relative path lands
  // wherever the process happens to have been started from, which is not
  // somewhere the person who asked for the capture can be expected to find.
  const std::filesystem::path path =
      rex::filesystem::GetAppRootFolder() / name;
  std::FILE* file = std::fopen(path.string().c_str(), "w");
  if (file == nullptr) {
    lua_pushinteger(L, 0);
    lua_pushstring(L, "could not open the capture file");
    return 2;
  }
  for (const std::uint32_t address : addresses) {
    std::fprintf(file, "%08X\n", address);
  }
  std::fclose(file);
  lua_pushinteger(L, static_cast<lua_Integer>(addresses.size()));
  lua_pushstring(L, path.string().c_str());
  return 2;
}

// IsPlayerInGame() -> true once the player is actually PLAYING.
//
// Use this, not IsPlayerSpawned, to decide when it is safe to act on the
// player. IsPlayerSpawned is backed by the skateboard transform, which exists
// while the world is still loading - a script waiting on it runs during the
// load screen. This also requires retail's own gameplay presence context, so
// it does not become true until the front end has handed over.
int Lua_IsPlayerInGame(lua_State* L) {
  lua_pushboolean(L, retail_ui::PlayerInGame());
  return 1;
}

// GetGameMode() -> number   (the global IsFreeSkateMode reads; free skate is 0, 3 or 4)
int Lua_GetGameMode(lua_State* L) {
  lua_pushinteger(L, retail_ui::CurrentGameMode());
  return 1;
}

// SetGameMode(n) -> boolean
//
// Writes that global. EXPERIMENT, not a mode change: the game also loads and
// spawns for a mode, and this does none of that - it only changes what the
// game believes it is in.
int Lua_SetGameMode(lua_State* L) {
  lua_pushboolean(L, retail_ui::SetGameMode(
                         static_cast<int>(luaL_checkinteger(L, 1))));
  return 1;
}

// --- Unlocks ---------------------------------------------------------

// IsEverythingUnlocked() -> wanted, applied, forced
//
// Three answers because they can all differ, and each difference is a distinct
// thing worth knowing. `wanted` is whether it is going to be on at all;
// `applied` is whether the progression manager is actually carrying the flag,
// which it cannot be until the manager exists; `forced` is whether the SESSION
// decided, in which case the setting is not being consulted and cannot be.
int Lua_IsEverythingUnlocked(lua_State* L) {
  lua_pushboolean(L, retail_ui::UnlockEverythingWanted());
  lua_pushboolean(L, retail_ui::UnlockEverythingApplied());
  lua_pushboolean(L, retail_ui::UnlockEverythingForced());
  return 3;
}

// SetUnlockEverything(on) -> effective
//
// Returns what the answer will actually BE, which is not always what was asked
// for: an online session forces the unlock on, so turning it off there does
// nothing. Reporting that beats failing quietly - everyone in a session having
// the same wardrobe is a rule of the session, decided in the engine, and a
// resource is not allowed to opt out of it.
//
// Offline it does what it says, on the next frame, and is reversible: turning
// it off puts the flag back the way retail's own constructor left it.
int Lua_SetUnlockEverything(lua_State* L) {
  const bool on = lua_toboolean(L, 1) != 0;
  rex::cvar::SetFlagByName("skate3_unlock_everything", on ? "true" : "false");
  lua_pushboolean(L, on || retail_ui::UnlockEverythingForced());
  return 1;
}

// --- Game difficulty -------------------------------------------------

// GetGameDifficulty() -> name, index   (nil before it is known)
//
// Reads what the GAME is using, not what its menu displays - the two are the
// same field, which is why setting it actually changes play rather than just
// the label.
int Lua_GetGameDifficulty(lua_State* L) {
  const int index = retail_ui::CurrentDifficulty();
  if (index < 0) {
    lua_pushnil(L);
    return 1;
  }
  const std::string name = retail_ui::DifficultyName(index);
  lua_pushstring(L, name.empty() ? "?" : name.c_str());
  lua_pushinteger(L, index);
  return 2;
}

// SetGameDifficulty(nameOrIndex) -> ok, message
//
// Takes "easy"/"normal"/"hardcore" (case-insensitive, matched against the
// game's own names) or a raw index. Applied on the next physics tick, because
// writing it needs a guest context.
int Lua_SetGameDifficulty(lua_State* L) {
  int index = -1;
  if (lua_type(L, 1) == LUA_TNUMBER) {
    index = static_cast<int>(luaL_checkinteger(L, 1));
  } else {
    index = retail_ui::DifficultyIndexForName(luaL_checkstring(L, 1));
  }
  if (index < 0) {
    lua_pushboolean(L, false);
    lua_pushstring(L,
                   "unknown difficulty - the game's own names are what match, "
                   "and they are unreadable until it is running");
    return 2;
  }
  retail_ui::RequestDifficulty(index);
  lua_pushboolean(L, true);
  lua_pushfstring(L, "difficulty %d (%s)", index,
                  retail_ui::DifficultyName(index).c_str());
  return 2;
}

// --- Guest exploration -----------------------------------------------
//
// Read-only search of the running game's memory. This is how retail entry
// points are found in THIS build: the shipped image is encrypted so its
// strings cannot be grepped, and the sk3o decompile is a different build whose
// addresses do not carry over. See skate3_guest_probe.h.

// DumpGuestWords(address, count [, name]) -> count, path
//
// Writes a run of guest words to JSON next to the executable, each with the
// string it points at when it points at one. For reading a table: pasting
// hundreds of console lines back and forth is slow and loses entries, and a
// file can just be read.
int Lua_DumpGuestWords(lua_State* L) {
  std::uint64_t address = 0;
  if (!ParseClothingId(luaL_checkstring(L, 1), address)) {
    lua_pushinteger(L, 0);
    lua_pushstring(L, "address must be hex");
    return 2;
  }
  const lua_Integer count = luaL_checkinteger(L, 2);
  const char* label = luaL_optstring(L, 3, "words");
  if (count <= 0 || count > 65536) {
    lua_pushinteger(L, 0);
    lua_pushstring(L, "count out of range");
    return 2;
  }
  std::string name = "dump_";
  for (const char* c = label; *c != 0; ++c) {
    name.push_back(std::isalnum(static_cast<unsigned char>(*c)) ? *c : '_');
  }
  name += ".json";
  const std::filesystem::path path = rex::filesystem::GetAppRootFolder() / name;
  std::FILE* file = std::fopen(path.string().c_str(), "w");
  if (file == nullptr) {
    lua_pushinteger(L, 0);
    lua_pushstring(L, "could not open the dump file");
    return 2;
  }
  std::fprintf(file, "[\n");
  int written = 0;
  for (lua_Integer index = 0; index < count; ++index) {
    const std::uint32_t at = static_cast<std::uint32_t>(address) +
                             static_cast<std::uint32_t>(index) * 4;
    std::uint32_t value = 0;
    if (!guest_probe::ReadU32(at, value)) {
      continue;
    }
    const std::string text = guest_probe::ReadCString(value, 96);
    std::string escaped;
    for (const char c : text) {
      if (c == '"' || c == '\\') {
        escaped.push_back('\\');
      }
      escaped.push_back(c);
    }
    std::fprintf(file,
                 "%s  {\"at\":\"0x%08X\",\"value\":\"0x%08X\",\"text\":\"%s\"}",
                 written == 0 ? "" : ",\n", at, value, escaped.c_str());
    ++written;
  }
  std::fprintf(file, "\n]\n");
  std::fclose(file);
  lua_pushinteger(L, written);
  lua_pushstring(L, path.string().c_str());
  return 2;
}

// FindGuestString(text [, limit]) -> { "0x82......", ... }
int Lua_FindGuestString(lua_State* L) {
  const char* text = luaL_checkstring(L, 1);
  const lua_Integer limit = luaL_optinteger(L, 2, 16);
  const std::vector<std::uint32_t> found = guest_probe::FindString(
      text, limit > 0 ? static_cast<std::size_t>(limit) : 0);
  lua_newtable(L);
  int index = 0;
  for (const std::uint32_t address : found) {
    char text_address[16];
    std::snprintf(text_address, sizeof(text_address), "0x%08X", address);
    lua_pushstring(L, text_address);
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

// FindGuestRef(address [, limit]) -> { "0x82......", ... }
//
// Where a 32-bit value appears, word-aligned. Walking back from a name to the
// table that points at it is how a binding table is identified.
int Lua_FindGuestRef(lua_State* L) {
  std::uint64_t value = 0;
  if (!ParseClothingId(luaL_checkstring(L, 1), value)) {
    lua_pushnil(L);
    return 1;
  }
  const lua_Integer limit = luaL_optinteger(L, 2, 16);
  const std::vector<std::uint32_t> found = guest_probe::FindU32(
      static_cast<std::uint32_t>(value),
      limit > 0 ? static_cast<std::size_t>(limit) : 0);
  lua_newtable(L);
  int index = 0;
  for (const std::uint32_t address : found) {
    char text_address[16];
    std::snprintf(text_address, sizeof(text_address), "0x%08X", address);
    lua_pushstring(L, text_address);
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

// ReadGuestU32(address) -> "0x........" or nil
int Lua_ReadGuestU32(lua_State* L) {
  std::uint64_t address = 0;
  std::uint32_t value = 0;
  if (!ParseClothingId(luaL_checkstring(L, 1), address) ||
      !guest_probe::ReadU32(static_cast<std::uint32_t>(address), value)) {
    lua_pushnil(L);
    return 1;
  }
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", value);
  lua_pushstring(L, text);
  return 1;
}

// ReadGuestString(address [, max]) -> string or nil
int Lua_ReadGuestString(lua_State* L) {
  std::uint64_t address = 0;
  if (!ParseClothingId(luaL_checkstring(L, 1), address)) {
    lua_pushnil(L);
    return 1;
  }
  const lua_Integer max_length = luaL_optinteger(L, 2, 128);
  const std::string text = guest_probe::ReadCString(
      static_cast<std::uint32_t>(address),
      max_length > 0 ? static_cast<std::size_t>(max_length) : 0);
  if (text.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, text.c_str());
  return 1;
}

// SendPadInput(sequence) -> ok, message
//
// Replays pad presses, e.g. "start,down,down,a". Tokens: a b x y start back
// lb rb lt rt up down left right l3 r3, each optionally ":ms" to change the
// delay after it.
int Lua_SendPadInput(lua_State* L) {
  const char* sequence = luaL_checkstring(L, 1);
  std::string error;
  if (!demo_path::PlayInputSequence(sequence, error)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, error.c_str());
    return 2;
  }
  lua_pushboolean(L, true);
  lua_pushstring(L, "sequence started");
  return 2;
}

// OpenCharacterEditor() -> ok, message
//
// Opens retail's own skater editor by performing the transition the Career
// menu's Edit Skaters row performs - not by simulating a player reaching it.
//
// Traced from the menu: activating a row resolves to an ACTION id, and
// sub_8261BC30 switches on it. "MyCareerTeam" - the Edit Skaters screen - is
// action 25, whose case is three instructions:
//
//     r4 = 63 ; r5 = 0 ; r3 = menu manager ; bl sub_8261AF30
//
// and sub_8261AF30 ends in SetFrontEndState(manager, 63).
//
// So this needs no menu context, which matters: online the game is held in
// Free Play, whose menu has no Edit Skaters row to select at all.
int Lua_OpenCharacterEditor(lua_State* L) {
  retail_ui::RequestSkaterEditor();
  lua_pushboolean(L, true);
  lua_pushstring(L, "opening the skater editor (front-end state 63)");
  return 2;
}

// GetClothingStats() -> { examined, patched, rejected }
//
// Diagnostic: see clothing::OverrideStats for what the numbers mean. Exposed
// to Lua so the question can be answered from a console command instead of a
// debugger.
int Lua_GetClothingStats(lua_State* L) {
  const clothing::OverrideStats stats = clothing::Stats();
  lua_newtable(L);
  lua_pushinteger(L, static_cast<lua_Integer>(stats.examined));
  lua_setfield(L, -2, "examined");
  lua_pushinteger(L, static_cast<lua_Integer>(stats.patched));
  lua_setfield(L, -2, "patched");
  lua_pushinteger(L, static_cast<lua_Integer>(stats.rejected));
  lua_setfield(L, -2, "rejected");
  return 1;
}

// GetClothingOverrides() -> { { category, id }, ... }
int Lua_GetClothingOverrides(lua_State* L) {
  const std::vector<clothing::Override> overrides = clothing::Overrides();
  lua_newtable(L);
  int index = 0;
  for (const clothing::Override& entry : overrides) {
    lua_newtable(L);
    lua_pushstring(L, entry.category.c_str());
    lua_setfield(L, -2, "category");
    char text[32];
    std::snprintf(text, sizeof(text), "0x%016llx",
                  static_cast<unsigned long long>(entry.model_id));
    lua_pushstring(L, text);
    lua_setfield(L, -2, "id");
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

// GetClothingCategories() -> { { name = 'Hair', count = 112, custom = 0 }, ... }
int Lua_GetClothingCategories(lua_State* L) {
  const std::vector<clothing::CategoryInfo> categories = clothing::Categories();
  lua_newtable(L);
  int index = 0;
  for (const clothing::CategoryInfo& category : categories) {
    lua_newtable(L);
    lua_pushstring(L, category.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, static_cast<lua_Integer>(category.count));
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, static_cast<lua_Integer>(category.custom));
    lua_setfield(L, -2, "custom");
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

// GetClothingCount(category) -> number. Zero for an unknown category AND
// while the archive is still being extracted; the two are indistinguishable
// on purpose, because a script's response to both is to ask again.
int Lua_GetClothingCount(lua_State* L) {
  lua_pushinteger(
      L, static_cast<lua_Integer>(clothing::Count(luaL_checkstring(L, 1))));
  return 1;
}

// GetClothingId(category, index) -> id string, or nil. 1-based, Lua's
// convention, converted to the module's 0-based positions here so scripts
// never see the seam.
int Lua_GetClothingId(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  const lua_Integer index = luaL_checkinteger(L, 2);
  if (index < 1) {
    lua_pushnil(L);
    return 1;
  }
  const std::string id =
      clothing::IdAt(category, static_cast<std::size_t>(index - 1));
  if (id.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, id.c_str());
  return 1;
}

// ListClothing(category [, filter [, limit]]) -> { id, ... }
int Lua_ListClothing(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  const std::string filter = luaL_optstring(L, 2, "");
  const lua_Integer limit = luaL_optinteger(L, 3, 40);
  const std::vector<std::string> ids = clothing::Ids(
      category, filter, limit > 0 ? static_cast<std::size_t>(limit) : 0);
  lua_newtable(L);
  int index = 0;
  for (const std::string& id : ids) {
    lua_pushstring(L, id.c_str());
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

// AddClothingId(category, id) -> boolean. Makes an id VISIBLE to the
// listings above; it does not make the asset loadable. That split is
// deliberate, so custom items can be enumerated and named before the loader
// that can wear them exists.
int Lua_AddClothingId(lua_State* L) {
  const char* category = luaL_checkstring(L, 1);
  const char* id = luaL_checkstring(L, 2);
  lua_pushboolean(L, clothing::Register(category, id));
  return 1;
}

// GetSkaterOutfit() -> { { category = 'Hair', id = '0x...', model = '0x...',
//                          material = '0x...' }, ... } or nil.
//
// Ids come back as HEX STRINGS, not numbers: they are 64-bit, and a Lua
// number is a double - anything above 2^53 would silently round, which for
// an asset id means naming a different item.
int Lua_GetSkaterOutfit(lua_State* L) {
  std::vector<clothing::OutfitPiece> pieces;
  if (!clothing::LocalOutfit(pieces)) {
    lua_pushnil(L);
    return 1;
  }
  const auto push_hex = [L](std::uint64_t value, const char* field) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%016llx",
                  static_cast<unsigned long long>(value));
    lua_pushstring(L, text);
    lua_setfield(L, -2, field);
  };
  lua_newtable(L);
  int index = 0;
  for (const clothing::OutfitPiece& piece : pieces) {
    lua_newtable(L);
    lua_pushstring(L, piece.category.c_str());
    lua_setfield(L, -2, "category");
    push_hex(piece.asset_id, "id");
    push_hex(piece.model_id, "model");
    push_hex(piece.material_id, "material");
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

int Lua_ListProps(lua_State* L) {
  const std::string filter = luaL_optstring(L, 1, "");
  const int limit = static_cast<int>(luaL_optinteger(L, 2, 40));
  std::string needle = filter;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  lua_newtable(L);
  int count = 0;
  std::error_code error;
  const std::filesystem::path root("objects");
  if (!std::filesystem::is_directory(root, error)) {
    return 1;  // empty table: nothing staged next to the executable
  }
  for (std::filesystem::recursive_directory_iterator it(root, error), end;
       it != end && count < limit; it.increment(error)) {
    if (error) {
      break;
    }
    if (!it->is_regular_file(error) ||
        it->path().extension() != ".skateobj") {
      continue;
    }
    std::string relative = it->path().generic_string();
    std::string lowered = relative;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!needle.empty() && lowered.find(needle) == std::string::npos) {
      continue;
    }
    lua_pushinteger(L, ++count);
    lua_pushstring(L, relative.c_str());
    lua_settable(L, -3);
  }
  return 1;
}

// SpawnProp(path) -> ok, message
//
// A DIAGNOSTIC, not the eventual CreateObject. It loads a .skateobj package
// and appends it to the owned world at the player's feet, then reports what
// the engine thought happened.
//
// The open question it exists to answer: spawned props go into
// mechanics_sandbox::map's owned world (AppendSpawnedObject mutates that
// definition), and on the retail map the owned world is instantiated but is
// not the world being rendered. So whether a prop is VISIBLE on the retail
// map cannot be settled by reading the code - it needs one launch and one
// look. The message returned reports the sandbox state alongside the spawn
// result so a "nothing appeared" answer still says why.
int Lua_SpawnProp(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  float x = 0.0f, y = 0.0f, z = 0.0f;
  if (lua_gettop(L) >= 4) {
    x = static_cast<float>(luaL_checknumber(L, 2));
    y = static_cast<float>(luaL_checknumber(L, 3));
    z = static_cast<float>(luaL_checknumber(L, 4));
  } else {
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (!trick_pipeline::CurrentLocalBoardPosition(position)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "no player position yet - spawn in first");
      return 2;
    }
    x = position[0];
    y = position[1];
    z = position[2];
  }

  std::string report;
  bool ok = false;
  lua_Integer spawned_index = -1;
  try {
    skate::world::SkateObjectAsset asset =
        skate::world::LoadSkateObjectPackage(std::filesystem::path(path));
    const std::size_t index =
        mechanics_sandbox::map::AppendSpawnedObject(std::move(asset),
                                                    {x, y, z});
    // Render distance. Defaulted rather than unlimited because a world of
    // props drawn to the horizon costs draw calls for things too small to
    // see; 300 units is roughly where a ledge stops reading.
    const float lod = static_cast<float>(luaL_optnumber(L, 5, 300.0));
    mechanics_sandbox::map::SetRuntimePropLodDistance(index, lod);
    spawned_index = static_cast<lua_Integer>(index);
    ok = true;
    report = "appended object index " + std::to_string(index) +
             " at (" + std::to_string(x) + ", " + std::to_string(y) + ", " +
             std::to_string(z) + ") lod " + std::to_string(lod);
  } catch (const std::exception& error) {
    report = std::string("failed to load package: ") + error.what();
  }
  // The sandbox state is the other half of the answer: if the owned world is
  // only REQUESTED and never became active, a successful append still draws
  // nothing, and that is a different problem from a bad package.
  report += "  [sandbox requested=" +
            std::string(mechanics_sandbox::Requested() ? "yes" : "no") +
            " active=" +
            std::string(mechanics_sandbox::Active() ? "yes" : "no") + "]";
  // The index is the prop's handle - stable for the process lifetime, since
  // editable objects are only ever appended.
  if (ok) {
    lua_pushinteger(L, spawned_index);
  } else {
    lua_pushnil(L);
  }
  lua_pushstring(L, report.c_str());
  return 2;
}

// CreateProp(path, x, y, z[, lod][, networked]) -> handle|nil, message
//
// The one call that places a prop. `networked` decides who sees it:
//
//   true   the SERVER owns it - it gets an id, is streamed to every player
//          near it, and outlives this session. Returns nil, because the prop
//          does not exist locally yet: it arrives back through the normal
//          stream like any other.
//   false  local to this client, gone on disconnect. Returns the handle.
//
// Networked placement goes out on the prop service's own thread, so this
// never blocks the frame even if the server is slow to answer.
int Lua_CreateProp(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  const float x = static_cast<float>(luaL_checknumber(L, 2));
  const float y = static_cast<float>(luaL_checknumber(L, 3));
  const float z = static_cast<float>(luaL_checknumber(L, 4));
  const float lod = static_cast<float>(luaL_optnumber(L, 5, 300.0));
  const bool networked = lua_toboolean(L, 6) != 0;

  if (networked) {
    prop_service::Place(path, x, y, z, lod);
    lua_pushnil(L);
    lua_pushstring(L, "queued for the server");
    return 2;
  }
  lua_pushcfunction(L, &Lua_SpawnProp);
  lua_pushstring(L, path);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  lua_pushnumber(L, z);
  lua_pushnumber(L, lod);
  lua_call(L, 5, 2);
  return 2;
}

// SetPropLod(handle, distance) - change a prop's render distance after the
// fact. 0 draws it at any distance.
int Lua_SetPropLod(lua_State* L) {
  const auto handle = static_cast<std::size_t>(luaL_checkinteger(L, 1));
  mechanics_sandbox::map::SetRuntimePropLodDistance(
      handle, static_cast<float>(luaL_checknumber(L, 2)));
  return 0;
}

// GetCameraCoords() -> x, y, z, or nil when unavailable.
//
// Exposed because a rotating radar needs to know which way the player is
// FACING, and the chase camera gives that for free: the horizontal vector
// from the camera to the skater is the direction the player is looking.
// That is a much smaller thing to trust than unpicking a view matrix out of
// the frame's combined view-projection.
int Lua_GetCameraCoords(lua_State* L) {
  float position[3] = {0.0f, 0.0f, 0.0f};
  if (!native_scene::CameraPosition(position)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, position[0]);
  lua_pushnumber(L, position[1]);
  lua_pushnumber(L, position[2]);
  return 3;
}

// GetTrickName(id) -> name, trusted
//
// Looks an EScorable id up in retail's own fixed trick-metadata table. The
// second return is the honest part: it is false when the table entry did
// not corroborate the lookup, and a script must then treat the name as
// unknown rather than as the trick it appears to spell - an off-by-one in
// the table stride would otherwise read out a different, entirely plausible
// trick name. Empty name and false before any hook has run (i.e. before
// gameplay), because the lookup needs guest memory.
int Lua_GetTrickName(lua_State* L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  bool trusted = false;
  const std::string name = trick_pipeline::ScorableTrickName(id, trusted);
  lua_pushstring(L, name.c_str());
  lua_pushboolean(L, trusted);
  return 2;
}

// GetTrickPatternClass(id) -> number (TrickPatternClass: 2 is flip, 4 grab,
// 5 grind, ...). 0 for unknown.
int Lua_GetTrickPatternClass(lua_State* L) {
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
  lua_pushinteger(L, static_cast<lua_Integer>(
                         trick_pipeline::ScorableTrickPatternClass(id)));
  return 1;
}

// GetPlayerSpeed() -> world units per second, 0 before it can be measured.
// DERIVED by differentiating the sampled board position, not read from the
// game - see CurrentLocalBoardVelocity for why, and for the teleport
// rejection that stops a reposition reading as a huge speed.
int Lua_GetPlayerSpeed(lua_State* L) {
  float velocity[3] = {0.0f, 0.0f, 0.0f};
  float speed = 0.0f;
  trick_pipeline::CurrentLocalBoardVelocity(velocity, speed);
  lua_pushnumber(L, speed);
  return 1;
}

// GetPlayerVelocity() -> vx, vy, vz (three numbers, matching the
// GetEntityCoords convention). Y is the vertical axis in this engine, so
// vy is the one that tells you a drop from a jump.
int Lua_GetPlayerVelocity(lua_State* L) {
  float velocity[3] = {0.0f, 0.0f, 0.0f};
  float speed = 0.0f;
  trick_pipeline::CurrentLocalBoardVelocity(velocity, speed);
  lua_pushnumber(L, velocity[0]);
  lua_pushnumber(L, velocity[1]);
  lua_pushnumber(L, velocity[2]);
  return 3;
}

// This client's player id - the role the relay assigned at connect time,
// and the same value the server sees as `source` on events this client
// sends. 0 when playing offline, since nothing has assigned an identity.
int Lua_GetPlayerId(lua_State* L) {
  lua_pushinteger(L, LocalPlayerId());
  return 1;
}

// GetPlayerName(id) -> that player's display name, or "" if unknown. id 0
// (or the local player's own id) returns this client's own configured
// name (skate3_multiplayer_player_name); any other id is a remote peer,
// known once its playerNamed/playerRoster broadcast has arrived.
int Lua_GetPlayerName(lua_State* L) {
  const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
  const std::string name = PlayerName(id);
  lua_pushlstring(L, name.data(), name.size());
  return 1;
}

// Teleports the whole player to an arbitrary position using the engine's
// own cMsgTeleport message, instead of SetEntityPosition's direct physics
// patching. Orientation is inherited from wherever the player is facing.
int Lua_SetPlayerCoords(lua_State* L) {
  const float x = static_cast<float>(luaL_checknumber(L, 1));
  const float y = static_cast<float>(luaL_checknumber(L, 2));
  const float z = static_cast<float>(luaL_checknumber(L, 3));
  mechanics_sandbox::RequestMessageTeleport(x, y, z);
  return 0;
}

// Runs the retail game's own "put the player back at the session marker"
// reposition. Unlike SetEntityPosition (which only moves the skateboard's
// physics object), this is the engine's real code path, so the rider and
// camera come along - but the destination is whatever the session marker
// already is, not an arbitrary coordinate.
int Lua_TeleportToSessionMarker(lua_State* L) {
  (void)L;
  mechanics_sandbox::RequestSessionMarkerTeleport();
  return 0;
}

// SendNUIMessage(table) - delivers a message to THIS resource's own NUI
// frame, which receives it through window.addEventListener('message', ...).
// Resource-aware (see LuaScriptHost::RegisterResourceNative): the resource
// name is upvalue 2, and it is what addresses the right frame.
int Lua_SendNUIMessage(lua_State* L) {
  const char* resource = lua_tostring(L, lua_upvalueindex(2));
  if (resource == nullptr) {
    return 0;
  }
  luaL_checkany(L, 1);
  // Encoded through the same rules events use, so a table sent to a page
  // and a table sent over the network mean the same thing.
  cef_nui::PostMessageToResource(resource,
                                 lua_host::EncodeLuaValueToJson(L, 1));
  return 0;
}

// SetNuiFocus(hasFocus, hasCursor) - FiveM's own signature. hasCursor
// defaults to hasFocus, matching the overwhelmingly common call shape
// SetNuiFocus(true, true) / SetNuiFocus(false, false).
int Lua_SetNuiFocus(lua_State* L) {
  const char* resource = lua_tostring(L, lua_upvalueindex(2));
  const bool has_focus = lua_toboolean(L, 1) != 0;
  const bool has_cursor =
      lua_isnoneornil(L, 2) ? has_focus : (lua_toboolean(L, 2) != 0);
  // The calling resource is what decides which page receives the pointer -
  // see cef_nui::SetFocus.
  cef_nui::SetFocus(resource != nullptr ? resource : "", has_focus,
                    has_cursor);
  return 0;
}

// SetNuiFocusKeepInput(keepInput) - while set, the skater keeps receiving
// input even though NUI holds focus. The gameplay-input gate reads this
// (see Skate3BaseApp::OnPostSetup's SetActiveCallback).
int Lua_SetNuiFocusKeepInput(lua_State* L) {
  cef_nui::SetKeepInput(lua_toboolean(L, 1) != 0);
  return 0;
}

// GetNuiFocus() -> hasFocus, hasCursor, keepInput. Not a FiveM native, but
// the three-way state is otherwise unreadable from script, which makes a
// UI that toggles its own focus awkward to write correctly.
int Lua_GetNuiFocus(lua_State* L) {
  lua_pushboolean(L, cef_nui::HasFocus());
  lua_pushboolean(L, cef_nui::HasCursor());
  lua_pushboolean(L, cef_nui::KeepInput());
  return 3;
}

// GetCurrentResourceName() - the resource whose script is calling. Needed
// by anything that addresses itself (exports, NUI URLs) and free here,
// since it is exactly upvalue 2.
int Lua_GetCurrentResourceName(lua_State* L) {
  const char* resource = lua_tostring(L, lua_upvalueindex(2));
  lua_pushstring(L, resource != nullptr ? resource : "");
  return 1;
}

// Returns a table describing the local skater as the engine currently
// understands it. Deliberately a table of the real guest-side handles
// rather than an opaque FiveM-style integer: the retail game has no single
// "player" object we can hand back yet (see the note below), and while that
// is still being reverse-engineered, exposing every verified handle is what
// actually makes Lua useful for probing this from in-game.
//
//   local s = GetPlayerSkater()
//   -- s.valid, s.actor, s.physOut, s.controller, s.boardBody,
//   -- s.presEntity, s.x, s.y, s.z, s.frame
//
// NOTE on what these are: `controller` is the SkateboardController, whose
// +428 is the BOARD's physics object (what entity 0 teleports) and whose
// +432 is `skeleton` - the HUMAN skater's Physics::Skeleton, i.e. the body
// the visible character is actually drawn from. `actor` is the ActionGraph
// actor (a component container; it holds no position of its own), and
// `presEntity` is the render-side SkaterPresEntity.
int Lua_GetPlayerSkater(lua_State* L) {
  lua_newtable(L);
  auto set_integer = [L](const char* key, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
  };
  auto set_number = [L](const char* key, double value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
  };

  trick_pipeline::LiveSpatialSnapshot snapshot;
  const bool have_snapshot =
      trick_pipeline::CurrentLiveSpatialSnapshot(snapshot);

  lua_pushboolean(L, have_snapshot ? 1 : 0);
  lua_setfield(L, -2, "valid");
  set_integer("actor", trick_pipeline::LocalActionGraphActor());
  set_integer("skeleton", mechanics_sandbox::LocalSkaterSkeleton());
  set_integer("presEntity", mechanics_sandbox::LocalSkaterPresEntity());
  set_integer("physOut", have_snapshot ? snapshot.phys_out : 0);
  set_integer("controller", have_snapshot ? snapshot.board_controller : 0);
  set_integer("boardBody", have_snapshot ? snapshot.board_body : 0);
  set_integer("frame", have_snapshot ? lua_Integer(snapshot.frame) : 0);

  float position[3] = {0.0f, 0.0f, 0.0f};
  if (trick_pipeline::CurrentLocalBoardPosition(position)) {
    set_number("x", position[0]);
    set_number("y", position[1]);
    set_number("z", position[2]);
  }
  return 1;
}

void OnCommandRegistered(const std::string& resource, const std::string& command,
                         int callback_ref, lua_State* /*L*/) {
  lua_host::LuaScriptHost* host = g_host.get();
  rex::cvar::FlagEntry entry;
  entry.name = command;
  entry.type = rex::cvar::FlagType::Command;
  entry.category = "Lua";
  entry.description = "Lua command registered by resource '" + resource + "'";
  entry.setter = [](std::string_view) { return false; };
  entry.getter = []() { return std::string("<command>"); };
  entry.command_callback = [host, resource, callback_ref](std::string_view args) {
    host->InvokeCommand(resource, callback_ref, args);
  };
  entry.lifecycle = rex::cvar::Lifecycle::kHotReload;
  entry.default_value = "<command>";
  if (!rex::cvar::RegisterFlag(std::move(entry))) {
    REXLOG_WARN("lua: command '{}' from resource '{}' could not be registered "
                "(name already in use)",
                command, resource);
  }
}

void OnCommandUnregistered(const std::string& command) {
  rex::cvar::UnregisterFlag(command);
}

// Dispatches a raw console line the exact same way the backtick
// ConsoleDialog's ExecuteCommand does (third_party/rexglue-sdk/src/ui/
// overlay/console_overlay.cpp) - duplicated rather than shared because that
// dialog lives in the rex-dependent SDK while skate3_lua_admin_http.cpp
// (the caller, via AdminHttpServer::SetConsoleExecHandler) is deliberately
// rex-free. Result lines are echoed into the log sink so the CEF console
// sees the same feedback the backtick console shows inline.
void ExecuteConsoleLine(std::string_view line) {
  if (line.empty() || !g_host) {
    return;
  }
  auto echo = [](const std::string& text) { g_host->log_sink().Append("", text); };

  const auto sep = line.find(' ');

  // resmon: prints one snapshot; resmon 1 / resmon 0 turns collection on or
  // off. FiveM toggles a HUD overlay - there is no equivalent overlay here
  // yet, so this prints into the same log sink the backtick and CEF
  // consoles both read, and "resmon" again gets a fresh table.
  //
  // OFF by default: collection has a real, measured per-frame cost (see
  // ScopedResourceTimer's comment in skate3_lua_script_host.cpp), so it is
  // opt-in rather than always paid for a number nobody may be looking at.
  if (line == "resmon" || line == "resmon 1" || line == "resmon 0") {
    if (line != "resmon") {
      lua_host::LuaScriptHost::SetMetricsEnabled(line == "resmon 1");
    }
    if (!lua_host::LuaScriptHost::MetricsEnabled()) {
      echo("[resmon] collection is off - \"resmon 1\" to enable, then "
           "\"resmon\" for a snapshot");
    } else {
      echo(g_host->FormatResourceMetricsTable());
    }
    return;
  }
  // netstat: this client's own upload/download rate. The dedicated server
  // has the equivalent for relay bandwidth and loop hitches; this is the
  // client-side half, which is per-connection transport telemetry rather
  // than anything the relay measures.
  if (line == "netstat") {
    echo(multiplayer::FormatNetworkTelemetryLine());
    return;
  }

  // Resource management (restart/ensure/stop <name>): previously only
  // reachable via a raw POST to /api/resources/{name}/{action} (the
  // React dashboard's own Resources panel) - there was no way to trigger
  // this from a typed console line at all, which is why typing "restart
  // Teleport" here silently did nothing (it fell through to the generic
  // cvar dispatch below and echoed "unknown cvar: restart").
  if (sep != std::string_view::npos) {
    const std::string_view verb = line.substr(0, sep);
    if (verb == "restart" || verb == "ensure" || verb == "stop") {
      std::string name(line.substr(sep + 1));
      while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
      }
      bool ok = false;
      const char* past_tense = "";
      if (verb == "restart") {
        ok = g_host->RestartResource(name);
        past_tense = "restarted";
      } else if (verb == "ensure") {
        ok = g_host->EnsureResource(name);
        past_tense = "ensured";
      } else {
        ok = g_host->StopResource(name);
        past_tense = "stopped";
      }
      echo(ok ? ("resource " + std::string(past_tense) + ": " + name)
              : ("resource FAILED to " + std::string(verb) + ": " + name +
                 " (not discovered - check sk8manifest.lua exists)"));
      return;
    }
  }

  if (sep == std::string_view::npos) {
    const auto* info = rex::cvar::GetFlagInfo(line);
    if (info && info->type == rex::cvar::FlagType::Command) {
      info->command_callback("");
      return;
    }
    const std::string value = rex::cvar::GetFlagByName(line);
    if (value.empty() && !info) {
      echo("unknown cvar: " + std::string(line));
    } else {
      echo(std::string(line) + " = " + value);
    }
    return;
  }

  const std::string name(line.substr(0, sep));
  std::string value(line.substr(sep + 1));
  while (!value.empty() && value.front() == ' ') {
    value.erase(value.begin());
  }

  const auto* command_info = rex::cvar::GetFlagInfo(name);
  if (command_info && command_info->type == rex::cvar::FlagType::Command) {
    command_info->command_callback(value);
    return;
  }
  if (!rex::cvar::SetFlagByName(name, value)) {
    echo("unknown cvar: " + name);
  }
}

struct ScriptListEntry {
  std::string name;
  std::string ui_page;
  std::vector<std::string> files;
  // NUI assets (html/css/js/images). Fetched into memory and handed to the
  // browser layer rather than to Lua - see SyncResourcesFromServer.
  std::vector<std::string> ui_files;
};

// One event pulled from the server's feed. `args` stays raw JSON - the
// script host decodes it into Lua values.
struct IncomingEvent {
  std::string event;
  std::string args;
};

std::atomic<bool> g_event_poll_running{false};
// This client's relay-assigned role. 0 until a server assigns one.
std::atomic<int> g_local_player_id{0};

// This client's own name and every remote peer's name this client has
// learned of, kept in memory for the life of the connection (not persisted
// to disk - see SyncResourcesFromServer's own comment on what "saved"
// means here). Guarded by its own mutex since the inbound event thread
// writes it and Lua natives read it from the guest thread.
std::mutex g_player_names_mutex;
std::string g_local_player_name;
std::unordered_map<int, std::string> g_remote_player_names;

// Pulls one JSON string literal starting at text[i] (which must be '"'),
// unescaping \", \\, \n, \r, \t. Advances i past the closing quote. Matches
// the same small escape set this file's other hand-rolled parsers use.
std::string ParseJsonStringLiteral(const std::string& text, std::size_t& i) {
  std::string value;
  if (i >= text.size() || text[i] != '"') {
    return value;
  }
  ++i;
  while (i < text.size() && text[i] != '"') {
    if (text[i] == '\\' && i + 1 < text.size()) {
      ++i;
      switch (text[i]) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(text[i]); break;
      }
    } else {
      value.push_back(text[i]);
    }
    ++i;
  }
  if (i < text.size()) {
    ++i;  // closing quote
  }
  return value;
}

// Handles the server's "skate3:playerNamed" broadcast: args = [id,"name"].
// Intercepted before generic event dispatch (same reasoning as
// TryApplyStateBagEvent) so player names are known even with zero Lua
// resources loaded - GetPlayerName() must work on its own.
bool HandlePlayerNamedEvent(const std::string& event, const std::string& json_args) {
  if (event != "skate3:playerNamed") {
    return false;
  }
  std::size_t i = json_args.find('[');
  if (i == std::string::npos) {
    return true;
  }
  ++i;
  const int id = std::atoi(json_args.c_str() + i);
  const auto comma = json_args.find(',', i);
  if (comma == std::string::npos) {
    return true;
  }
  i = comma + 1;
  while (i < json_args.size() && json_args[i] == ' ') {
    ++i;
  }
  const std::string name = ParseJsonStringLiteral(json_args, i);
  if (!name.empty()) {
    std::lock_guard<std::mutex> lock(g_player_names_mutex);
    g_remote_player_names[id] = name;
  }
  return true;
}

// Handles "skate3:playerRoster": args = [[{"id":N,"name":"..."},...]], sent
// once to a newly-named client so it learns everyone already connected
// without waiting on each of them to rename themselves again.
bool HandlePlayerRosterEvent(const std::string& event, const std::string& json_args) {
  if (event != "skate3:playerRoster") {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_player_names_mutex);
  std::size_t i = 0;
  while ((i = json_args.find("\"id\"", i)) != std::string::npos) {
    const auto id_colon = json_args.find(':', i);
    if (id_colon == std::string::npos) {
      break;
    }
    const int id = std::atoi(json_args.c_str() + id_colon + 1);
    const auto name_key = json_args.find("\"name\"", id_colon);
    if (name_key == std::string::npos) {
      break;
    }
    auto j = json_args.find(':', name_key) + 1;
    while (j < json_args.size() && json_args[j] == ' ') {
      ++j;
    }
    const std::string name = ParseJsonStringLiteral(json_args, j);
    if (!name.empty()) {
      g_remote_player_names[id] = name;
    }
    i = j;
  }
  return true;
}

// Outbound events waiting to be sent. A single worker drains this rather
// than each TriggerServerEvent spawning its own thread: two events fired
// back to back must arrive in the order they were fired, and independent
// threads racing to POST cannot promise that.
struct OutboundEvent {
  std::string event;
  std::string args;
};
std::mutex g_outbound_mutex;
std::condition_variable g_outbound_ready;
std::deque<OutboundEvent> g_outbound_queue;

void QueueOutboundEvent(OutboundEvent outbound) {
  {
    std::lock_guard<std::mutex> lock(g_outbound_mutex);
    g_outbound_queue.push_back(std::move(outbound));
  }
  g_outbound_ready.notify_one();
}

// Reads {"events":[{"event":"...","args":[...]},...],"cursor":N}, advancing
// `cursor`. Hand-rolled to match this one shape, like the other parsers in
// this file - the args array is extracted as text, brace-matched so nested
// structures survive intact.
std::vector<IncomingEvent> ParseEventFeedJson(const std::string& json,
                                              std::uint64_t& cursor) {
  std::vector<IncomingEvent> result;
  std::size_t i = 0;
  while ((i = json.find("\"event\"", i)) != std::string::npos) {
    const auto name_open = json.find('"', json.find(':', i) + 1);
    if (name_open == std::string::npos) {
      break;
    }
    auto name_close = name_open + 1;
    while (name_close < json.size() && json[name_close] != '"') {
      name_close += (json[name_close] == '\\') ? 2 : 1;
    }
    if (name_close >= json.size()) {
      break;
    }
    IncomingEvent incoming;
    incoming.event = json.substr(name_open + 1, name_close - name_open - 1);

    const auto args_key = json.find("\"args\"", name_close);
    if (args_key != std::string::npos) {
      const auto open = json.find('[', args_key);
      if (open != std::string::npos) {
        int depth = 0;
        bool in_string = false;
        for (std::size_t k = open; k < json.size(); ++k) {
          const char c = json[k];
          if (in_string) {
            if (c == '\\') {
              ++k;
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
            incoming.args = json.substr(open, k - open + 1);
            break;
          }
        }
      }
    }
    result.push_back(std::move(incoming));
    i = name_close;
  }
  const auto cursor_key = json.find("\"cursor\"");
  if (cursor_key != std::string::npos) {
    cursor = std::strtoull(json.c_str() + json.find(':', cursor_key) + 1,
                           nullptr, 10);
  }
  return result;
}

// Parses exactly the shape EncodeClientScriptListJson (skate3_lua_admin_http.cpp)
// produces: [{"name":"...","file":"..."},...]. Not a general-purpose JSON
// parser - the two fields are simple identifiers, so only basic escapes are
// handled.
std::vector<ScriptListEntry> ParseScriptListJson(const std::string& json) {
  std::vector<ScriptListEntry> result;
  std::size_t i = 0;
  auto skip_ws = [&]() {
    while (i < json.size() &&
           std::isspace(static_cast<unsigned char>(json[i]))) {
      ++i;
    }
  };
  auto parse_string = [&]() -> std::string {
    if (i >= json.size() || json[i] != '"') {
      return {};
    }
    ++i;
    std::string value;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) {
        ++i;
        switch (json[i]) {
          case 'n': value.push_back('\n'); break;
          case 't': value.push_back('\t'); break;
          case 'r': value.push_back('\r'); break;
          default: value.push_back(json[i]); break;
        }
        ++i;
      } else {
        value.push_back(json[i]);
        ++i;
      }
    }
    if (i < json.size()) {
      ++i;  // closing quote
    }
    return value;
  };

  skip_ws();
  if (i < json.size() && json[i] == '[') {
    ++i;
  }
  while (true) {
    skip_ws();
    if (i >= json.size() || json[i] == ']') {
      break;
    }
    if (json[i] == ',') {
      ++i;
      continue;
    }
    if (json[i] != '{') {
      ++i;
      continue;
    }
    ++i;
    ScriptListEntry entry;
    while (true) {
      skip_ws();
      if (i < json.size() && json[i] == '}') {
        ++i;
        break;
      }
      if (i < json.size() && json[i] == ',') {
        ++i;
        continue;
      }
      skip_ws();
      const std::string key = parse_string();
      skip_ws();
      if (i < json.size() && json[i] == ':') {
        ++i;
      }
      skip_ws();
      // "files" and "ui_files" are arrays; every other field is a
      // plain string.
      if (i < json.size() && json[i] == '[') {
        ++i;
        while (true) {
          skip_ws();
          if (i >= json.size() || json[i] == ']') {
            ++i;
            break;
          }
          if (json[i] == ',') {
            ++i;
            continue;
          }
          std::string element = parse_string();
          if (element.empty()) {
            continue;
          }
          if (key == "files") {
            entry.files.push_back(std::move(element));
          } else if (key == "ui_files") {
            entry.ui_files.push_back(std::move(element));
          }
        }
        continue;
      }
      const std::string value = parse_string();
      if (key == "name") {
        entry.name = value;
      } else if (key == "ui_page") {
        entry.ui_page = value;
      }
    }
    result.push_back(std::move(entry));
  }
  return result;
}

}  // namespace

int DevConsoleAdminPort() { return g_admin_port; }

void Initialize() {
  // Local resources/ are not auto-STARTED (a connected server's
  // SyncResourcesFromServer push still wins - see its own comment), but
  // they must be DISCOVERED so the admin console's ensure/restart/stop
  // (and the F7 console's own "restart <name>" line - see
  // ExecuteConsoleLine) have a resources_ entry to find at all: without
  // this, EnsureResourceLocked's very first lookup fails silently for a
  // name nothing has ever pushed via EnsureResourceFromSource, which is
  // exactly why local dev-loop testing (edit resources/X/client.lua,
  // restart) previously required a prior EnsureResourceFromSource call to
  // have implicitly created the map entry.
  const auto resources_root = rex::filesystem::GetAppRootFolder() / "resources";
  g_host = std::make_unique<lua_host::LuaScriptHost>(resources_root,
                                                      lua_host::Side::kClient);
  g_host->DiscoverResources();
  g_host->RegisterNative("SetEntityPosition", &Lua_SetEntityPosition);
  g_host->RegisterNative("GetEntityCoords", &Lua_GetEntityCoords);
  g_host->RegisterNative("GetPlayerCoords", &Lua_GetPlayerCoords);
  g_host->RegisterNative("GetActivePlayers", &Lua_GetActivePlayers);
  g_host->RegisterNative("GetSyncedProps", &Lua_GetSyncedProps);
  g_host->RegisterNative("CreateCam", &Lua_CreateCam);
  g_host->RegisterNative("DestroyCam", &Lua_DestroyCam);
  g_host->RegisterNative("MoveCam", &Lua_MoveCam);
  g_host->RegisterNative("LookAtCam", &Lua_LookAtCam);
  g_host->RegisterNative("SetCamActive", &Lua_SetCamActive);
  g_host->RegisterNative("IsCamActive", &Lua_IsCamActive);
  g_host->RegisterNative("GetPlayerSkater", &Lua_GetPlayerSkater);
  g_host->RegisterNative("TeleportToSessionMarker",
                         &Lua_TeleportToSessionMarker);
  g_host->RegisterNative("SetPlayerCoords", &Lua_SetPlayerCoords);
  g_host->RegisterNative("GetPlayerId", &Lua_GetPlayerId);
  g_host->RegisterNative("IsPlayerSpawned", &Lua_IsPlayerSpawned);
  g_host->RegisterNative("IsControlPressed", &Lua_IsControlPressed);
  g_host->RegisterNative("IsControlJustPressed", &Lua_IsControlJustPressed);
  g_host->RegisterNative("IsControlJustReleased", &Lua_IsControlJustReleased);
  g_host->RegisterNative("GetControlValue", &Lua_GetControlValue);
  g_host->RegisterNative("IsPadConnected", &Lua_IsPadConnected);
  g_host->RegisterNative("IsOffBoard", &Lua_IsOffBoard);
  g_host->RegisterNative("IsAirOffBoard", &Lua_IsAirOffBoard);
  g_host->RegisterNative("IsSkaterInAir", &Lua_IsSkaterInAir);
  g_host->RegisterNative("IsSkaterOnGround", &Lua_IsSkaterOnGround);
  g_host->RegisterNative("GetTrickName", &Lua_GetTrickName);
  g_host->RegisterNative("GetTrickPatternClass", &Lua_GetTrickPatternClass);
  g_host->RegisterNative("WorldToScreen", &Lua_WorldToScreen);
  g_host->RegisterNative("GetCameraCoords", &Lua_GetCameraCoords);
  g_host->RegisterNative("SpawnProp", &Lua_SpawnProp);
  g_host->RegisterNative("ListProps", &Lua_ListProps);
  g_host->RegisterNative("GetClothingCategories", &Lua_GetClothingCategories);
  g_host->RegisterNative("GetClothingCount", &Lua_GetClothingCount);
  g_host->RegisterNative("GetClothingId", &Lua_GetClothingId);
  g_host->RegisterNative("ListClothing", &Lua_ListClothing);
  g_host->RegisterNative("AddClothingId", &Lua_AddClothingId);
  g_host->RegisterNative("GetSkaterOutfit", &Lua_GetSkaterOutfit);
  g_host->RegisterNative("SetClothingModel", &Lua_SetClothingModel);
  g_host->RegisterNative("ClearClothing", &Lua_ClearClothing);
  g_host->RegisterNative("GetClothingOverrides", &Lua_GetClothingOverrides);
  g_host->RegisterNative("GetClothingStats", &Lua_GetClothingStats);
  g_host->RegisterNative("GetLastFrontEndState", &Lua_GetLastFrontEndState);
  g_host->RegisterNative("OpenCharacterEditor", &Lua_OpenCharacterEditor);
  g_host->RegisterNative("SendPadInput", &Lua_SendPadInput);
  g_host->RegisterNative("IsPlayerInGame", &Lua_IsPlayerInGame);
  g_host->RegisterNative("GetGameMode", &Lua_GetGameMode);
  g_host->RegisterNative("SetGameMode", &Lua_SetGameMode);
  g_host->RegisterNative("IsEverythingUnlocked", &Lua_IsEverythingUnlocked);
  g_host->RegisterNative("SetUnlockEverything", &Lua_SetUnlockEverything);
  g_host->RegisterNative("GetGameDifficulty", &Lua_GetGameDifficulty);
  g_host->RegisterNative("SetGameDifficulty", &Lua_SetGameDifficulty);
  g_host->RegisterNative("CallFlashMethod", &Lua_CallFlashMethod);
  g_host->RegisterNative("StartCoverage", &Lua_StartCoverage);
  g_host->RegisterNative("StopCoverage", &Lua_StopCoverage);
  g_host->RegisterNative("DumpGuestWords", &Lua_DumpGuestWords);
  g_host->RegisterNative("FindGuestString", &Lua_FindGuestString);
  g_host->RegisterNative("FindGuestRef", &Lua_FindGuestRef);
  g_host->RegisterNative("ReadGuestU32", &Lua_ReadGuestU32);
  g_host->RegisterNative("ReadGuestString", &Lua_ReadGuestString);
  g_host->RegisterNative("GetFrontEndInfo", &Lua_GetFrontEndInfo);
  g_host->RegisterNative("SetPropLod", &Lua_SetPropLod);
  g_host->RegisterNative("CreateProp", &Lua_CreateProp);
  g_host->RegisterNative("GetPlayerSpeed", &Lua_GetPlayerSpeed);
  g_host->RegisterNative("GetPlayerVelocity", &Lua_GetPlayerVelocity);
  g_host->RegisterNative("GetPlayerName", &Lua_GetPlayerName);
  // NUI natives need to know which resource is calling (SendNUIMessage
  // addresses that resource's own frame), so they are registered as
  // resource-aware closures rather than plain globals.
  g_host->RegisterResourceNative("SendNUIMessage", &Lua_SendNUIMessage);
  g_host->RegisterResourceNative("SetNuiFocus", &Lua_SetNuiFocus);
  g_host->RegisterResourceNative("SetNuiFocusKeepInput",
                                 &Lua_SetNuiFocusKeepInput);
  g_host->RegisterResourceNative("GetNuiFocus", &Lua_GetNuiFocus);
  g_host->RegisterResourceNative("GetCurrentResourceName",
                                 &Lua_GetCurrentResourceName);
  g_host->SetCommandHooks(&OnCommandRegistered, &OnCommandUnregistered);

  const auto web_console_dir =
      rex::filesystem::GetAppRootFolder() / "web-console";
  g_admin = std::make_unique<lua_host::AdminHttpServer>(*g_host, web_console_dir);
  g_admin->SetConsoleExecHandler(&ExecuteConsoleLine);
  // /api/metrics: this client's own upload/download rate merged with Lua
  // resmon - the client-side twin of the dedicated server's provider (which
  // additionally has relay bandwidth/hitches, since only the relay sees
  // those). See AdminHttpServer::SetMetricsProvider's own comment.
  g_admin->SetMetricsProvider([] {
    return "{\"network\":" + multiplayer::NetworkTelemetryJson() +
          ",\"resources\":" + g_host->ResourceMetricsJson() + "}";
  });
  // Walk up from the base until a port binds.
  //
  // A FIXED port would be a correctness bug, not just an inconvenience, when
  // two clients run on one machine: the second client's server fails to
  // bind, but its CEF pages still resolve 127.0.0.1:<base> - which is the
  // FIRST client's admin server. Every command typed into client 2's console
  // would then execute on client 1 (a propplace landing at client 1's
  // position), and client 2's scrollback would show client 1's log, because
  // both were reading and writing one server. Binding a distinct port per
  // client is what keeps the two consoles separate.
  for (int offset = 0; offset < kDevConsoleAdminPortRange; ++offset) {
    const int port = kDevConsoleAdminPortBase + offset;
    if (g_admin->Start(port)) {
      g_admin_port = port;
      REXLOG_INFO("lua: dev console listening on http://127.0.0.1:{}", port);
      break;
    }
  }
  if (g_admin_port == 0) {
    REXLOG_WARN(
        "lua: dev console failed to bind any port in {}..{}; the in-game "
        "console and NUI will not load",
        kDevConsoleAdminPortBase,
        kDevConsoleAdminPortBase + kDevConsoleAdminPortRange - 1);
  }

  cef_nui::Initialize(g_admin_port);
  // NUI's own pages ride on the dev-console admin server - see
  // AdminHttpServer::SetNuiFileProvider for why they are not served from
  // the resources' https origins.
  g_admin->SetNuiFileProvider(&cef_nui::LookupFile);
  // Runs on a CEF thread. LuaScriptHost is internally locked, so entering
  // it from here is safe; a handler that answers later keeps `respond`
  // alive on its own.
  cef_nui::SetCallbackInvoker([](const std::string& resource,
                                 const std::string& name,
                                 const std::string& json_body,
                                 cef_nui::RespondFn respond) {
    if (!g_host || !g_host->InvokeNuiCallback(resource, name, json_body,
                                              respond)) {
      // No such callback (or the handler errored): answer anyway, or the
      // page's fetch() never settles and its await hangs forever.
      respond("{}");
    }
  });
}

// Turns the tricks the game recorded this frame into script events.
//
// Two events rather than one with a flag, because a game mode almost always
// wants exactly one of them: S.K.A.T.E. cares about a landed trick, a
// bail-punishing mode cares about a cancelled one, and making each resource
// re-test a boolean invites the bug where it silently handles both.
//
// A trick whose name could not be corroborated against the metadata table
// is still published - dropping it would make a round hang waiting for a
// trick that did happen - but `nameTrusted` is false and `name` may be
// empty, so an adjudicating script can refuse to score it rather than score
// the wrong trick.
void PublishTrickEvents() {
  static std::uint64_t reported_drops = 0;
  std::uint64_t dropped = 0;
  const std::vector<trick_pipeline::TrickEventRecord> events =
      trick_pipeline::DrainTrickEvents(dropped);
  if (dropped > reported_drops) {
    AppendEngineLog("trick events: dropped " +
                    std::to_string(dropped - reported_drops) +
                    " record(s) - the queue filled between frames");
    reported_drops = dropped;
  }
  for (const trick_pipeline::TrickEventRecord& record : events) {
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), "%.6g",
                  static_cast<double>(record.value));
    std::ostringstream args;
    args << "[{\"name\":\"" << record.name << "\",\"id\":"
         << record.scorable_id << ",\"patternClass\":"
         << record.pattern_class << ",\"value\":" << value_text
         << ",\"frame\":" << record.frame << ",\"nameTrusted\":"
         << (record.name_trusted ? "true" : "false") << "}]";
    g_host->DispatchLocalEvent(
        record.landed ? "skate3:trickLanded" : "skate3:trickCancelled",
        args.str());
  }
}

// Streams world props: tells the service where the player is, then spawns
// whatever the server said is nearby.
//
// Spawning happens HERE rather than on the service thread because
// AppendSpawnedObject mutates the world definition the renderer reads, and
// this is the one per-frame hook the client has. The service only ever does
// network I/O.
void ServiceStreamedProps() {
  float position[3] = {0.0f, 0.0f, 0.0f};
  if (trick_pipeline::CurrentLocalBoardPosition(position)) {
    prop_service::SetLocalPosition(position[0], position[1], position[2]);
  }
  for (const prop_service::Spawn& prop : prop_service::TakeSpawns()) {
    try {
      skate::world::SkateObjectAsset asset =
          skate::world::LoadSkateObjectPackage(
              std::filesystem::path(prop.path));
      const std::size_t index = mechanics_sandbox::map::AppendSpawnedObject(
          std::move(asset), {prop.x, prop.y, prop.z});
      mechanics_sandbox::map::SetRuntimePropLodDistance(index, prop.lod);
    } catch (const std::exception& error) {
      // A package this client cannot read is the server's content, not a
      // reason to stop streaming: log it once and carry on with the rest.
      REXLOG_WARN("prop-service: prop {} ('{}') failed to load: {}", prop.id,
                  prop.path, error.what());
    }
  }
}

void Tick() {
  if (g_host) {
    // Before anything runs, so every script in this tick sees the same
    // press/release edges - see skate3_input_state.h.
    input_state::BeginScriptFrame();
    ServiceStreamedProps();
    PublishTrickEvents();
    g_host->Tick();
    // A handler that took `cb` and never called it would otherwise leave
    // the page's fetch() pending forever. Swept here rather than on a timer
    // thread because this is already the one per-frame hook the client has.
    g_host->SweepStaleNuiCallbacks(kNuiCallbackTimeoutMs);
  }
}

void SyncResourcesFromServer(const std::string& host, std::uint16_t admin_port,
                             std::uint16_t player_id,
                             const std::string& player_name) {
  if (admin_port == 0) {
    return;
  }
  g_local_player_id.store(player_id, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(g_player_names_mutex);
    g_local_player_name = player_name;
  }
  // Runs on its own thread: this is a blocking network round-trip (or two -
  // list, then one GET per script) and must not stall the render thread
  // that drives relay registration. LuaScriptHost is internally
  // mutex-guarded, so calling EnsureResourceFromSource off-thread is safe.
  std::thread([host, admin_port, player_id, player_name]() {
    httplib::Client client(host, admin_port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(3, 0);
    // Told to the server once, up front: HandlePlayerNamedEvent on the
    // OTHER end broadcasts it to everyone already connected and replies
    // with their names in turn (see skate3_dedicated_main.cpp).
    if (!player_name.empty()) {
      client.Post("/api/players/name?player=" + std::to_string(player_id),
                  player_name, "text/plain");
    }
    auto list_response = client.Get("/api/scripts");
    if (!list_response || list_response->status != 200) {
      REXLOG_WARN("lua: could not fetch server resource list from {}:{}", host,
                  admin_port);
      return;
    }
    for (const auto& script : ParseScriptListJson(list_response->body)) {
      // Every file the manifest declared for the client side, fetched in
      // the order the server listed them - shared scripts first, so a
      // client script can rely on what they defined.
      std::vector<lua_host::ScriptChunk> chunks;
      bool complete = true;
      for (const std::string& file : script.files) {
        auto source_response =
            client.Get("/api/scripts/" + script.name + "/file?path=" + file);
        if (!source_response || source_response->status != 200) {
          REXLOG_WARN("lua: could not fetch '{}/{}' from server", script.name,
                      file);
          complete = false;
          break;
        }
        chunks.push_back({file, source_response->body});
      }
      // A resource is started whole or not at all: running half its scripts
      // would leave it in a state no author ever wrote.
      if (!complete || chunks.empty()) {
        continue;
      }
      if (g_host && g_host->EnsureResourceFromSources(script.name, chunks)) {
        REXLOG_INFO("lua: resource '{}' loaded from server ({} script(s))",
                    script.name, chunks.size());
      } else {
        continue;
      }

      // NUI assets, fetched into memory and handed straight to the browser
      // layer - client-side resources never touch the disk, so the scheme
      // handler serving https://<resource>/ has nowhere else to read them
      // from. Published AFTER the scripts run, so a resource's own
      // RegisterNUICallback handlers already exist by the time its page
      // loads and starts calling them.
      if (!script.ui_files.empty()) {
        std::unordered_map<std::string, std::string> ui_files;
        for (const std::string& file : script.ui_files) {
          auto file_response =
              client.Get("/api/scripts/" + script.name + "/file?path=" + file);
          if (!file_response || file_response->status != 200) {
            REXLOG_WARN("lua: could not fetch NUI asset '{}/{}' from server",
                        script.name, file);
            continue;  // a missing asset degrades the page, not the resource.
          }
          ui_files.emplace(file, file_response->body);
        }
        cef_nui::SetResourcePage(script.name, script.ui_page,
                                 std::move(ui_files));
      }
    }

    // Now that this client is bound to a server, script events can flow.
    // Both directions ride the game protocol's reliable channel (see
    // skate3_multiplayer_protocol_v12_reliable.h) rather than the admin HTTP
    // server: same immediacy and ordering, but on the transport the rest of
    // the session already uses, so events are subject to the same
    // connection lifetime as everything else.
    if (g_host) {
      // Runs on the guest thread inside a Lua handler, so it must not block:
      // SendScriptEvent only encodes and enqueues, and the network worker
      // does the sending.
      g_host->SetEventOutboundHandler(
          [](const std::string& event, const std::string& json_args,
             const std::string& /*target*/) {
            // A client can only ever address the server; the server decides
            // who else hears about it.
            if (!multiplayer::SendScriptEvent(event, json_args)) {
              REXLOG_WARN(
                  "lua: script event '{}' was not queued (no server "
                  "connection, or the channel is backed up)",
                  event);
            }
          });
    }

    // Inbound. Called on the network worker thread; LuaScriptHost is
    // internally locked, so entering it from there is safe.
    multiplayer::SetScriptEventHandler(
        [](const std::string& event, const std::string& json_args) {
          if (HandlePlayerNamedEvent(event, json_args) ||
              HandlePlayerRosterEvent(event, json_args)) {
            return;
          }
          if (g_host && !g_host->TryApplyStateBagEvent(event, json_args)) {
            // Events arriving on a client always come from the server.
            g_host->DispatchNetworkEvent(event, json_args, /*source=*/0);
          }
        });
  }).detach();
}

int LocalPlayerId() { return g_local_player_id.load(std::memory_order_acquire); }

std::string PlayerName(int id) {
  {
    std::lock_guard<std::mutex> lock(g_player_names_mutex);
    if (id == 0 || id == g_local_player_id.load(std::memory_order_relaxed)) {
      return g_local_player_name;
    }
    const auto it = g_remote_player_names.find(id);
    if (it != g_remote_player_names.end()) {
      return it->second;
    }
  }
  // Falls back to what the SERVER last reported for this role. The
  // "skate3:playerNamed" broadcast above is a one-shot: a client that was
  // not listening at that instant - still loading, or joining later than
  // the event - never learned the name and showed "Player <role>" for the
  // rest of the session. The server's roster is re-sent whenever it
  // changes, so it recovers from exactly that.
  return skate3::appearance_service::PeerName(static_cast<std::uint32_t>(id));
}

void TeardownServerResources() {
  g_event_poll_running.store(false, std::memory_order_release);
  // Wakes the sender thread so it observes the stop flag and exits.
  g_outbound_ready.notify_all();
  g_local_player_id.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(g_player_names_mutex);
    g_local_player_name.clear();
    g_remote_player_names.clear();
  }
  // Every resource (and therefore every NUI page) came from the server
  // being left, so the UI layer is emptied along with them.
  cef_nui::SetFocus(std::string(), false, false);
  cef_nui::SetKeepInput(false);
  cef_nui::ClearResourcePages();
  if (g_host) {
    g_host->StopAllResources();
  }
}

void AppendEngineLog(const std::string& text) {
  if (g_host) {
    g_host->log_sink().Append("engine", text);
  }
}

void Shutdown() {
  cef_nui::Shutdown();
  g_admin.reset();
  g_host.reset();
}

}  // namespace skate3::lua_client
