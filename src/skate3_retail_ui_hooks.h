#pragma once

// Hooks on retail's own front-end bindings.
//
// Skate 3's menus are DATA DRIVEN: the Flash movie only renders, and asks C++
// for its contents through bindings it calls by name. So retail's UI is
// changed by controlling what those bindings return - not by editing the
// movie, and not by driving the front end from outside (which does not work:
// see docs/frontend_bridge.md).
//
// Addresses here are for THIS build, taken from the binding registration table
// extracted in resources/Probe/bindings.lua. They are meaningless for any other
// build, including the sk3o decompile.

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/ppc/context.h>

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::retail_ui {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);

// Picks up cvar changes (the pause-menu block). Call once per frame.
void Tick();

// --- Game difficulty --------------------------------------------------
//
// The difficulty the game is actually using, not just what its menu shows.
// -1 before the setting has been located (it is resolved on a guest thread).
int CurrentDifficulty();
std::string DifficultyName(int index);
int DifficultyIndexForName(std::string_view name);

// Queues a change, applied on the next physics tick. Writing it needs a live
// guest context, which a script thread does not have.
void RequestDifficulty(int index);

// The game-mode global IsFreeSkateMode reads. Free skate is 0, 3 or 4.
// Applies the server's menu rules, or clears them when `active` is false.
// Not a setting: single player is left alone, and nothing persists between
// sessions.
void SetMenuPolicy(bool active, std::string tab_rows, int settings_rows,
                   int settings_skip);

// Fetches the server's rules (difficulty) and enforces them until applied.
// Called from the register acknowledgement; cleared on disconnect.
void ConfigureServerRules(const std::string& host, std::uint16_t port);
void ClearServerRules();

// True once the player is actually in gameplay - not merely spawned, which is
// true while the world is still loading.
bool PlayerInGame();

// Arms function coverage, or writes it out on the second call. Bound to a key
// so a capture is possible offline, where there is no console.
void ToggleCoverageCapture();

// Opens retail's own skater editor - the screen the Career menu's Edit Skaters
// row reaches. Queued; performed on the guest thread.
bool RequestSkaterEditor();
void ApplyPendingFrontEndState(PPCContext& ctx, std::uint8_t* base);

// True when the progression manager is actually carrying retail's own
// "everything is unlocked" flag - not merely that the setting asks for it.
// False until the manager has been constructed.
bool UnlockEverythingApplied();

// Whether the unlock is going to be on at all, and whether the session is what
// decides that. An online session forces it on and no script can take it away -
// everyone joining gets the same wardrobe, which is a rule of the session
// rather than a setting. Offline, the setting decides.
bool UnlockEverythingWanted();
bool UnlockEverythingForced();

int CurrentGameMode();
bool SetGameMode(int mode);
void ApplyPendingDifficulty(PPCContext& ctx, std::uint8_t* base);
void ApplyPendingGameMode(std::uint8_t* base);

}  // namespace skate3::retail_ui
