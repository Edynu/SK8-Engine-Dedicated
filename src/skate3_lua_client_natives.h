#pragma once

// Client-only glue between the shared LuaScriptHost/AdminHttpServer (see
// skate3_lua_script_host.h / skate3_lua_admin_http.h) and rex::*: console
// command dispatch and guest-memory natives. Not linked into
// skate3_dedicated.

#include <cstdint>
#include <string>

namespace skate3::lua_client {

// FIRST port tried for the client's dev-console HTTP admin server - also
// where the CEF dev console (skate3_cef_console.h) and the NUI pages point
// their browsers.
//
// A BASE, not the port: two clients on one machine (the standard test
// setup, Launch-Two-Clients.bat) cannot both bind it, and the second one
// must not fall back to talking to the first. Use DevConsoleAdminPort() for
// the port actually bound - see the scan in Initialize().
constexpr int kDevConsoleAdminPortBase = 27180;

// How many consecutive ports the scan will try before giving up. Bounded so
// a machine with something squatting the whole range fails visibly instead
// of wandering into ports that mean something else.
constexpr int kDevConsoleAdminPortRange = 16;

// The port this client's admin server actually bound, or 0 before
// Initialize() has run or if every port in the range was taken. Every
// browser this client opens must be pointed here rather than at the base,
// or it would render ANOTHER client's console and execute commands there.
int DevConsoleAdminPort();

// Constructs the client's LuaScriptHost + AdminHttpServer and registers the
// client-only natives (SetEntityPosition, ...) and console-command hooks.
// Does NOT start any resource - the client has no scripting content of its
// own; resources only run once fetched from a connected server (see
// SyncResourcesFromServer). Call once from Skate3BaseApp::OnPostSetup.
void Initialize();

// Call once per rendered frame (there is no other per-frame hook on the
// client - see RenderScene in skate3_native_scene_gpu.cpp).
void Tick();

// Fetches every client-side resource the given server (a skate3_dedicated
// Direct Connect host, using the admin HTTP port learned from its
// RelayRegisterAck) has loaded, and runs each one locally. Call once the
// relay registration ack arrives (skate3_multiplayer.cpp). Non-blocking -
// does its network I/O on a background thread.
// `player_id` is the relay-assigned role from RelayRegisterAck - this
// client's identity for the rest of the session. Script events carry it so
// the server knows who sent them and can address replies back.
// `player_name` (from skate3_multiplayer_player_name) is posted to the
// server once, right after resources sync - see HandlePlayerNameChanged in
// skate3_dedicated_main.cpp for what the server does with it. Empty means
// "no name configured"; nothing is posted, and this client shows up
// name-less in the registry/dashboard until it sets one.
void SyncResourcesFromServer(const std::string& host, std::uint16_t admin_port,
                             std::uint16_t player_id,
                             const std::string& player_name);

// This client's relay-assigned player id, or 0 when not connected to a
// server. Exposed to Lua as GetPlayerId().
int LocalPlayerId();

// A player's display name - this client's own (id == LocalPlayerId(), set
// from skate3_multiplayer_player_name) or a remote peer's, learned from the
// server's playerNamed/playerRoster broadcasts (see
// HandlePlayerNamedEvent/HandlePlayerRosterEvent). Empty if unknown.
// Exposed to Lua as GetPlayerName(id).
std::string PlayerName(int id);

// Stops every resource obtained via SyncResourcesFromServer. Call when the
// client leaves/disconnects from its server (skate3_multiplayer.cpp /
// skate3_multiplayer_session.cpp).
void TeardownServerResources();

// Writes one line into the same log the F7 console and /api/console/log
// read. For engine-side messages that a person debugging from inside the
// game needs to see - REXLOG goes to a file nothing in-game surfaces.
// No-op before Initialize().
void AppendEngineLog(const std::string& text);

// Call from Skate3BaseApp::OnShutdown.
void Shutdown();

}  // namespace skate3::lua_client
