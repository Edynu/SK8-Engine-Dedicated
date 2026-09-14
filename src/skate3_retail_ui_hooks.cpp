#include "skate3_retail_ui_hooks.h"

#include "skate3_multiplayer.h"

#include "generated/skate3_init.h"
#include "skate3_function_coverage.h"
#include "skate3_guest_probe.h"
#include "skate3_native_scene_state.h"
#include "skate3_lua_client_natives.h"
#include "skate3_trick_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <httplib.h>

#include <rex/kernel/guest_presence.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

#include <rex/filesystem.h>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(skate3_unlock_everything, false, "Skate 3",
                    "Treat every progression unlock as earned - clothing, boards, "
                    "graphics, locations. This is RETAIL'S OWN SWITCH, not a display "
                    "hook: the progression manager carries a flag that makes every "
                    "unlock query answer yes, so the padlock, the item's blurb and "
                    "the code that decides whether you may WEAR it all agree. OFF, "
                    "and this setting governs OFFLINE play only: a career is the "
                    "player's own and keeps its progression. An online session forces "
                    "it on regardless, because everyone joining has to have the same "
                    "wardrobe. Runtime only either way: the flag lives in a heap "
                    "object rebuilt each launch and cannot reach a save file");

REXCVAR_DEFINE_BOOL(skate3_online_rules, true, "Skate 3",
                    "Master switch for the engine-side online rules: forced Free "
                    "Play, the trimmed menu, forced unlock-everything, and the "
                    "suppressed board-sales milestone. ON. Clearing it makes all of "
                    "them inert WITHOUT disconnecting, which is the point - it "
                    "exists so this whole layer can be A/B tested against a frame "
                    "-time readout in one step, instead of relaunching offline and "
                    "changing the relay traffic at the same time. Note that the two "
                    "rules that WRITE guest state (Free Play, the unlock flag) are "
                    "not undone by clearing this, only stopped from being "
                    "re-asserted - a relaunch is what resets those");

namespace skate3::retail_ui {

namespace {

// The unlock id of an item, as CAC_GetItemUnlockHALID reports it.
//
// Retail's implementation (0x825A74A0) reads the item index from argument 0,
// asks the CAC manager for that item's unlock id, and wraps the result as a
// script string. The editor screen draws a padlock when an item HAS an unlock
// id, so reporting none reads as "no unlock required".
//
// EMPTY IS NOT INVENTED. The returned pointer is the terminator of an existing
// string in the image - the NUL after "Receipt" at 0x8220EF34, which is
// immediately followed by "cas/createskater.swf" at 0x8220EF3C. It is only ever
// READ, so borrowing it costs nothing and needs no guest allocation.
constexpr uint32_t kEmptyString = 0x8220EF3Bu;

// The item states CAC_GetOptionType reports, as string literals in the image:
// "none" (not an item), "owned", "locked", "dlc". The editor draws a padlock
// on "locked", which is why clearing the unlock id alone removed the
// requirement TEXT but left the lock.
constexpr uint32_t kOptionTypeOwned = 0x82202420u;
constexpr uint32_t kOptionTypeLocked = 0x82202428u;

// The FOURTH answer, and the reason this needs a constant of its own.
//
// sub_825FF5E8 ends with a fallthrough that also reads "none" - but from a
// DIFFERENT copy of the word (0x82202434, not the 0x82202418 that sub_825FF4C8
// returns for a row that is not an item at all). Two identical strings at two
// addresses is what makes them separable: that fallthrough means "this IS an
// item, and it is neither owned, nor locked, nor DLC", which is exactly what an
// unlocked-by-flag item looks like once the lock predicate stops claiming it.
// Mapping it is therefore precise; mapping "none" by TEXT would also swallow
// the empty rows.
constexpr uint32_t kOptionTypeUnclaimed = 0x82202434u;

// --- Why these are cached and not queried per call ----------------------
//
// rex::cvar::Query is NOT a cheap read. Every call takes the global cvar
// registry MUTEX, builds a std::string from the name (every name here is
// past the 15-char small-string limit, so that is a real malloc/free),
// looks it up in the registry map, then calls the entry's std::function
// getter which returns another std::string by value.
//
// That is fine once a frame and ruinous per guest call. The hooks below sit
// on retail functions with hundreds of call sites - sub_825DFAF0 alone has
// 372 - so a trace flag read inside one of them runs thousands of times a
// frame. Measured as a client frame-time regression from ~6ms to ~15ms,
// found by counting call sites of every hooked symbol after the cheaper
// suspects (resmon, the Steam removal) were each ruled out.
//
// So: one Query per flag per frame, from Tick(), into a relaxed atomic the
// hooks read instead. Relaxed is right - these are diagnostics, and a flag
// flipped in the console taking one extra frame to be observed changes
// nothing.
std::atomic<bool> g_trace_cac_bindings{false};
std::atomic<bool> g_trace_menu{false};
std::atomic<bool> g_unlock_everything{false};
std::atomic<bool> g_online_session{false};

// The ONE place that actually pays for a cvar read. Everything else in this
// file goes through the accessors below.
void RefreshCachedFlags() {
  g_trace_cac_bindings.store(
      rex::cvar::Query<bool>("skate3_trace_cac_bindings"),
      std::memory_order_relaxed);
  g_trace_menu.store(rex::cvar::Query<bool>("skate3_trace_menu"),
                     std::memory_order_relaxed);
  g_unlock_everything.store(rex::cvar::Query<bool>("skate3_unlock_everything"),
                            std::memory_order_relaxed);
  // The online engine rules as a single switch, so the whole layer can be
  // A/B tested against a frame-time readout without relaunching offline
  // (which would also remove the relay traffic and confound the result).
  // Everything online-gated in this file reads OnlineSessionFlag(), so
  // clearing skate3_online_rules makes all of it inert in one step.
  g_online_session.store(
      rex::cvar::Query<bool>("skate3_multiplayer_relay_active") &&
          rex::cvar::Query<bool>("skate3_online_rules"),
      std::memory_order_relaxed);
}

bool TraceCacBindings() {
  return g_trace_cac_bindings.load(std::memory_order_relaxed);
}
bool TraceMenu() { return g_trace_menu.load(std::memory_order_relaxed); }
bool UnlockEverythingFlag() {
  return g_unlock_everything.load(std::memory_order_relaxed);
}
bool OnlineSessionFlag() {
  return g_online_session.load(std::memory_order_relaxed);
}


}  // namespace

// CAC_GetItemUnlockHALID - report no unlock requirement.
//
// A STRONG SYMBOL, like the others: dispatcher->SetFunction reaches only calls
// routed through the dispatcher, and defining the symbol is what actually
// replaces a directly-called function here.
extern "C" REX_FUNC(sub_825A74A0) {
  if (!UnlockEverythingFlag()) {
    __imp__sub_825A74A0(ctx, base);
    return;
  }
  // The same wrapper the original ends with, so the caller receives an
  // ordinary script string and cannot tell the difference.
  ctx.r3.u32 = kEmptyString;
  sub_82E864F8(ctx, base);
}

// --- Tracing the Create-a-Skater item database ------------------------
//
// The bindings themselves are the wrong layer to watch: their argument is the
// script context and their result is a script value object on the heap, so a
// trace there shows pointers and nothing else. One level down, the item
// database is called with a plain item INDEX and answers with raw values -
// which is where "locked" and "owned" actually appear.
//
// Coverage cannot substitute for this: it only records at internal branch
// labels, so small functions never appear in a capture at all.
REXCVAR_DEFINE_BOOL(skate3_trace_cac_bindings, false, "Skate 3",
                    "Log the Create-a-Skater item database queries - item index in, "
                    "raw answer out. For finding which value carries an item's lock "
                    "state");

namespace {

void TraceItem(const char* name, uint32_t index, uint32_t result,
               uint8_t* base) {
  if (!TraceCacBindings()) {
    return;
  }
  char text[40] = {};
  if (result >= 0x82000000u && result < 0x83000000u) {
    const char* guest = reinterpret_cast<const char*>(base + result);
    for (int i = 0; i < 32; ++i) {
      const char c = guest[i];
      if (c == 0) break;
      text[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
  }
  REXLOG_INFO("cac-item: {} index={} -> 0x{:08X} {}", name, index, result,
              text);
}

}  // namespace

// STRONG SYMBOL OVERRIDES, not dispatcher hooks.
//
// DEFINE_REX_FUNC in the generated sources emits a WEAK `sub_X` aliasing
// `__imp__sub_X`, so defining `sub_X` here makes the linker pick ours and
// `__imp__sub_X` still reaches the original. dispatcher->SetFunction only
// intercepts calls that go THROUGH the dispatcher - true of the script
// bindings (which is why the unlock-id hook works) and not of these, which
// their bindings reach by a direct call. A dispatcher hook on them installs
// cleanly and never fires.
//
// r3 is the item database, r4 the item index.
#define SKATE3_CAC_ITEM_TRACE(symbol, label)                    extern "C" REX_FUNC(symbol) {                                   const uint32_t index = ctx.r4.u32;                            __imp__##symbol(ctx, base);                                   TraceItem(label, index, ctx.r3.u32, base);                  }

// OptionType is the padlock.
//
// Measured: browsing Button Shirts, this reports "locked" for exactly the
// items the editor draws a padlock on, and "none" for the rows that are not
// items. Reporting "owned" instead is the whole unlock.
//
// "dlc" and the not-an-item "none" are left alone, so downloadable content and
// empty rows keep their own meaning.
//
// This survives the real unlock rather than being replaced by it. With the
// progression flag set the lock predicate answers "not locked", so an item that
// used to report "locked" now reaches sub_825FF5E8's fallthrough instead - and
// a grid of rows reporting "none" renders as a grid of nothing. Both answers
// therefore map to "owned", and either alone would leave a hole.
extern "C" REX_FUNC(sub_825FF4C8) {
  const uint32_t index = ctx.r4.u32;
  __imp__sub_825FF4C8(ctx, base);
  if (UnlockEverythingFlag() &&
      (ctx.r3.u32 == kOptionTypeLocked ||
       ctx.r3.u32 == kOptionTypeUnclaimed)) {
    ctx.r3.u32 = kOptionTypeOwned;
  }
  TraceItem("OptionType", index, ctx.r3.u32, base);
}

SKATE3_CAC_ITEM_TRACE(sub_825FF3E0, "UnlockHALID")
SKATE3_CAC_ITEM_TRACE(sub_825FF018, "ItemText")
SKATE3_CAC_ITEM_TRACE(sub_825FFB18, "IntegerValue")
SKATE3_CAC_ITEM_TRACE(sub_825FFE08, "StringValue")

#undef SKATE3_CAC_ITEM_TRACE

// The query BOTH the padlock and the selection path consult.
//
// sub_825FF5E8 (which computes "locked"/"owned") and sub_825FD9F0 (which
// handles picking an item) both call this, so whatever it answers is the
// shared truth about an item. It has 68 call sites across 30 functions
// though, so it is NOT safe to blanket-override - hence tracing first, and
// filtering to calls that come from the CAC code so the log stays readable.
extern "C" REX_FUNC(sub_82DDB710) {
  const uint32_t caller = ctx.lr;
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;
  const uint32_t a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__sub_82DDB710(ctx, base);
  if (!TraceCacBindings()) {
    return;
  }
  // Only the Create-a-Skater item code; everything else uses this too.
  if (caller < 0x825FA000u || caller > 0x82601000u) {
    return;
  }
  REXLOG_INFO(
      "cac-query: from=0x{:08X} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X} "
      "r6=0x{:08X} -> 0x{:08X}",
      caller, a3, a4, a5, a6, ctx.r3.u32);
}

// Does pressing A even reach C++?
//
// The trace showed sub_825FD9F0 never running while trying to pick a locked
// item, which would mean the movie refuses before asking. These three say
// where the refusal is: the binding the movie calls on a pick, the code behind
// it, and the generic "is this row selectable" query.
#define SKATE3_CAC_CALL_TRACE(symbol, label)                          extern "C" REX_FUNC(symbol) {                                         const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;                    __imp__##symbol(ctx, base);                                         if (TraceCacBindings()) {            REXLOG_INFO("cac-pick: {} r3=0x{:08X} r4=0x{:08X} -> 0x{:08X}",                   label, a3, a4, ctx.r3.u32);                           }                                                                 }

SKATE3_CAC_CALL_TRACE(sub_825A70B0, "CAC_OnThumbnailSelect")
SKATE3_CAC_CALL_TRACE(sub_825FD9F0, "SelectItem")
SKATE3_CAC_CALL_TRACE(sub_825A5078, "OnMenuGetItemEnabled")
SKATE3_CAC_CALL_TRACE(sub_825A7600, "CAC_IsPartNew")
SKATE3_CAC_CALL_TRACE(sub_825A74F0, "CAC_GetInfoPanelType")

#undef SKATE3_CAC_CALL_TRACE

// Why SelectItem refuses.
//
// It returns 0 for a previously-locked item and 1 for one you own, and it does
// so BEFORE consulting the item lookup - so the gate is one of the checks it
// makes first. These two are the calls in that stretch; the value compared
// against 26 immediately after the second is the likeliest gate.
//
// Filtered by return address so only calls made from inside SelectItem are
// logged - both are used widely elsewhere.
#define SKATE3_GATE_TRACE(symbol, label)                                extern "C" REX_FUNC(symbol) {                                           const uint32_t caller = ctx.lr;                                       const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;                      __imp__##symbol(ctx, base);                                           if (TraceCacBindings() &&                caller >= 0x825FD9F0u && caller <= 0x825FE100u) {                   REXLOG_INFO("cac-gate: {} from=0x{:08X} r3=0x{:08X} r4=0x{:08X}"                   " -> 0x{:08X}",                                                       label, caller, a3, a4, ctx.r3.u32);                     }                                                                   }

SKATE3_GATE_TRACE(sub_82600820, "Query820")
SKATE3_GATE_TRACE(sub_825DFAF0, "QueryFAF0")

#undef SKATE3_GATE_TRACE

// --- The pause menu, which IS trimmable -------------------------------
//
// Unlike the wardrobe, nothing under these is guarded by a career-progress
// collection: the menu renders exactly what C++ hands it. Tracing first, to
// see how the list is built before changing it.
REXCVAR_DEFINE_BOOL(skate3_trace_menu, false, "Skate 3",
                    "Log the in-game menu's content queries - how many rows, what "
                    "each is, and which are selectable");

#define SKATE3_MENU_TRACE(symbol, label)                               extern "C" REX_FUNC(symbol) {                                          const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;                     __imp__##symbol(ctx, base);                                          if (TraceMenu()) {                     REXLOG_INFO("menu: {} r3=0x{:08X} r4=0x{:08X} -> 0x{:08X}",                      label, a3, a4, ctx.r3.u32);                            }                                                                  }

SKATE3_MENU_TRACE(sub_825A52A8, "GetMenuItems")
SKATE3_MENU_TRACE(sub_825B6C30, "GetNumMenus")
SKATE3_MENU_TRACE(sub_825B6C90, "GetMenuName")
SKATE3_MENU_TRACE(sub_825A4F00, "OnMenuSelect")
SKATE3_MENU_TRACE(sub_825A5240, "OnMenuRefresh")

#undef SKATE3_MENU_TRACE

// --- Suppressing retail's pause menu ----------------------------------
//
// The menu opens on START. Hiding that button from the GUEST stops the menu
// existing at all, without reverse engineering the front end and without the
// button going dead for anything else - the host still sees it, so our own
// console and NUI keep working.
//
// This is the blunt option and it is deliberately separate from trimming the
// menu's contents: it is reversible with one cvar, and it does not depend on
// anything we have had to infer.
REXCVAR_DEFINE_BOOL(skate3_block_pause_menu, false, "Skate 3",
                    "Stop START reaching the game, so retail's pause menu never "
                    "opens. The button still works for this engine's own UI");

namespace {

// X_INPUT_GAMEPAD_START, as the guest sees it.
constexpr uint16_t kGuestStartButton = 0x0010;

void RefreshPauseMenuBlock() {
  static bool applied = false;
  const bool block = rex::cvar::Query<bool>("skate3_block_pause_menu");
  if (block == applied) {
    return;
  }
  applied = block;
  rex::kernel::xam::SetBlockedGuestButtons(block ? kGuestStartButton : 0);
  REXLOG_INFO("retail-ui: pause menu {}", block ? "blocked" : "allowed");
}

}  // namespace

// --- Game difficulty ---------------------------------------------------
//
// Where it lives, read out of GetDifficulty (0x825B9768):
//
//   manager = sub_824AD240()
//   settings = [[manager + 8] + 156]
//   index    = [settings + 88]            <- the difficulty, 0..n
//   name     = [0x83038280 + index * 4]   <- its display string
//
// Setting it writes that index. The FE getter then reports the new value, and
// so does everything else that reads the same field - which is the point:
// forcing the MENU's answer would leave the actual setting untouched.
//
// The write happens on a guest thread, not from script: resolving the manager
// means calling retail code, which needs a live context.
namespace {

constexpr uint32_t kDifficultyNameTable = 0x83038280u;
constexpr uint32_t kDifficultyCount = 8;  // generous; entries are validated

std::atomic<int> g_pending_difficulty{-1};
std::atomic<int> g_pending_mode{-1};
std::atomic<uint32_t> g_difficulty_field{0};

// Resolves &settings.difficulty, caching it: the manager is stable once the
// game is running, and this runs every physics tick.
uint32_t ResolveDifficultyField(PPCContext& ctx, uint8_t* base) {
  const uint32_t cached = g_difficulty_field.load(std::memory_order_relaxed);
  if (cached != 0) {
    return cached;
  }
  PPCContext call = ctx;
  sub_824AD240(call, base);
  const uint32_t manager = call.r3.u32;
  uint32_t object = 0, settings = 0;
  if (manager == 0 || !guest_probe::ReadU32(manager + 8, object) ||
      object == 0 || !guest_probe::ReadU32(object + 156, settings) ||
      settings == 0) {
    return 0;
  }
  const uint32_t field = settings + 88;
  g_difficulty_field.store(field, std::memory_order_relaxed);
  return field;
}

}  // namespace

int CurrentDifficulty() {
  const uint32_t field = g_difficulty_field.load(std::memory_order_relaxed);
  uint32_t value = 0;
  if (field == 0 || !guest_probe::ReadU32(field, value)) {
    return -1;
  }
  return static_cast<int>(value);
}

std::string DifficultyName(int index) {
  if (index < 0 || static_cast<uint32_t>(index) >= kDifficultyCount) {
    return {};
  }
  uint32_t pointer = 0;
  if (!guest_probe::ReadU32(kDifficultyNameTable + index * 4, pointer) ||
      pointer == 0) {
    return {};
  }
  // The table holds LOCALISATION KEYS, not display text:
  // "ID_GAMESETTINGS_DIFFICULTY_NORMAL". The trailing word is the only part
  // anyone can reasonably type, so that is what this reports and what
  // DifficultyIndexForName matches - "easy" should mean easy without a script
  // having to know retail's string-table naming.
  std::string key = guest_probe::ReadCString(pointer, 96);
  const std::size_t underscore = key.rfind('_');
  if (underscore != std::string::npos && underscore + 1 < key.size()) {
    key = key.substr(underscore + 1);
  }
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return key;
}

int DifficultyIndexForName(std::string_view name) {
  for (uint32_t index = 0; index < kDifficultyCount; ++index) {
    const std::string candidate = DifficultyName(static_cast<int>(index));
    if (candidate.empty()) {
      continue;
    }
    if (candidate.size() == name.size() &&
        std::equal(candidate.begin(), candidate.end(), name.begin(),
                   [](char a, char b) {
                     return std::tolower(static_cast<unsigned char>(a)) ==
                            std::tolower(static_cast<unsigned char>(b));
                   })) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

void RequestDifficulty(int index) {
  g_pending_difficulty.store(index, std::memory_order_relaxed);
}

void ApplyPendingDifficulty(PPCContext& ctx, uint8_t* base) {
  const uint32_t field = ResolveDifficultyField(ctx, base);
  if (field == 0) {
    return;
  }
  const int wanted = g_pending_difficulty.exchange(-1, std::memory_order_acq_rel);
  if (wanted < 0) {
    return;
  }
  uint32_t current = 0;
  if (guest_probe::ReadU32(field, current) &&
      current == static_cast<uint32_t>(wanted)) {
    return;
  }
  // Big-endian, like everything the guest stores.
  const uint32_t value = static_cast<uint32_t>(wanted);
  uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24),
                      static_cast<uint8_t>(value >> 16),
                      static_cast<uint8_t>(value >> 8),
                      static_cast<uint8_t>(value)};
  std::memcpy(base + field, bytes, sizeof(bytes));
  REXLOG_INFO("retail-ui: difficulty set to {} ({})", wanted,
              DifficultyName(wanted));
}

constexpr uint32_t kGameModeGlobal = 0x830B7AE8u;

void ApplyPendingGameMode(uint8_t* base) {
  const int wanted = g_pending_mode.exchange(-1, std::memory_order_acq_rel);
  if (wanted < 0 || base == nullptr) {
    return;
  }
  const uint32_t value = static_cast<uint32_t>(wanted);
  const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24),
                            static_cast<uint8_t>(value >> 16),
                            static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value)};
  std::memcpy(base + kGameModeGlobal, bytes, sizeof(bytes));
  // Not logged per write: while the game is arguing about the mode this runs
  // repeatedly, and ReportGameModeChanges already records each transition.
}

// --- Game mode ---------------------------------------------------------
//
// IsFreeSkateMode (0x825A4A40) reads one global and answers true for 0, 3 or 4:
//
//   lis r11,-31989 ; lwz r11,31464(r11)   ->  [0x830B7AE8]
//
// Exposed rather than forced, because writing a mode global is not the same as
// performing a mode CHANGE - the game also loads and spawns for a mode. Read it
// in each mode first and we will know what the numbers mean; forcing one is a
// second step, and an experiment rather than a fix.
int CurrentGameMode() {
  uint32_t value = 0;
  if (!guest_probe::ReadU32(kGameModeGlobal, value)) {
    return -1;
  }
  return static_cast<int>(value);
}

bool SetGameMode(int mode) {
  if (mode < 0) {
    return false;
  }
  // Queued and written on a guest thread, alongside the difficulty write -
  // the same boundary, and it keeps the guest probe read-only as documented.
  g_pending_mode.store(mode, std::memory_order_relaxed);
  return true;
}

// What an online session changes about retail's own UI.
//
// IN THE ENGINE, not a script: these are the rules of fair play, so a resource
// must not be able to grant itself the whole menu back by not calling a native.
// A player's single-player career keeps retail whole - the rules apply only
// while connected, and are lifted the moment the session ends.
//
// Tabs are the icons along the top of Career, numbered from 0:
//   0 Main, 1 Xbox LIVE, 2 Create, 3 Graduation, 4 Options
//
// Rows are kept from the START of a tab, which is why none of this needs index
// remapping. Game Settings is the exception - Difficulty is its row 0 - so that
// one is skipped rather than trimmed.
//
//   Xbox LIVE     a second, unrelated online session
//   Free Play     the server decides the mode, not each player
//   Create tools  skate.Park / skate.Reel / Share Pack edit the world
//   Difficulty    the server sets it (see the Difficulty resource)
//   Online        would fight the server's own networking
// Free Play, identified by watching the mode global change while selecting it
// offline: career is 1, the world load is 0, and Free Play is 3.
constexpr int kGameModeFreePlay = 3;

// FREE PLAY's tab numbering, because an online session is always Free Play.
// It has four tabs, not Career's five - there is no Xbox LIVE - so Options is
// 3 here and 4 there. Using Career's numbers left Extras and Sign in to EA
// Nation on screen.
//
//   0 Main  1 Create  2 Graduation  3 Options
//
// Main keeps only Challenge Map: Free Play's list has no Edit Skaters to keep
// (it is Challenge Map, Resume Career, Free Play, Call Skater and
// skate.Profile), and the rest either leave the session or duplicate it.
constexpr const char* kOnlineTabRows = "0:1,1:1,3:2";
constexpr int kOnlineSettingsRows = 3;  // Audio, Video, Control
constexpr int kOnlineSettingsSkip = 1;  // hides Difficulty Settings

namespace {

void RefreshOnlineMenuPolicy() {
  static bool applied = false;
  // Non-zero exactly while connected to a server: set from the register
  // acknowledgement and cleared on disconnect.
  const bool online = lua_client::LocalPlayerId() != 0;
  if (online == applied) {
    return;
  }
  applied = online;
  SetMenuPolicy(online, online ? kOnlineTabRows : "",
                online ? kOnlineSettingsRows : 0,
                online ? kOnlineSettingsSkip : 0);
  if (!online) {
    ClearServerRules();
  }
}

}  // namespace

// --- Rules the server sets, enforced by the engine ---------------------
//
// Difficulty and game mode are fair-play rules, so they are fetched and
// applied HERE rather than by a resource. A script can still change
// difficulty afterwards for a scenario (SetGameDifficulty) - what it cannot do
// is decline to apply the server's rule in the first place.
namespace {

std::mutex g_server_rules_mutex;
std::string g_wanted_difficulty;   // name from the server, empty when none
bool g_difficulty_applied = false;

void FetchServerRules(std::string host, std::uint16_t port) {
  httplib::Client client(host, port);
  client.set_connection_timeout(3, 0);
  client.set_read_timeout(5, 0);
  const auto response = client.Get("/api/settings");
  if (!response || response->status / 100 != 2) {
    return;
  }
  // {"game_difficulty":"hardcore"} - one fixed shape produced a few lines
  // away, so a JSON dependency would be for nothing.
  const std::string& body = response->body;
  // Animation send rate, if this server sets one. Applied before the
  // difficulty parse below so a server that sets only the rate still gets it.
  const auto rate_key = body.find("\"animation_hz\"");
  if (rate_key != std::string::npos) {
    const auto colon = body.find(':', rate_key);
    if (colon != std::string::npos) {
      skate3::multiplayer::SetServerAnimationRate(
          std::atoi(body.c_str() + colon + 1));
    }
  }
  const auto pose_key = body.find("\"pose_hz\"");
  if (pose_key != std::string::npos) {
    const auto colon = body.find(':', pose_key);
    if (colon != std::string::npos) {
      skate3::multiplayer::SetServerPoseRate(
          std::atoi(body.c_str() + colon + 1));
    }
  }
  const auto read_int = [&body](const char* name, int fallback) {
    const auto at = body.find(name);
    if (at == std::string::npos) {
      return fallback;
    }
    const auto colon = body.find(':', at);
    return colon == std::string::npos ? fallback
                                      : std::atoi(body.c_str() + colon + 1);
  };
  const int hands_hz = read_int("\"hands_hz\"", -1);
  const int face_hz = read_int("\"face_hz\"", -1);
  if (hands_hz >= 0 || face_hz >= 0) {
    skate3::multiplayer::SetServerDetailRates(hands_hz, face_hz);
  }
  const auto key = body.find("\"game_difficulty\"");
  if (key == std::string::npos) {
    return;
  }
  const auto open = body.find('"', body.find(':', key) + 1);
  const auto close = open == std::string::npos
                         ? std::string::npos
                         : body.find('"', open + 1);
  if (open == std::string::npos || close == std::string::npos) {
    return;
  }
  std::string value = body.substr(open + 1, close - open - 1);
  if (value.empty()) {
    return;
  }
  REXLOG_INFO("retail-ui: server difficulty is {}", value);
  std::lock_guard<std::mutex> lock(g_server_rules_mutex);
  g_wanted_difficulty = std::move(value);
  g_difficulty_applied = false;
}

// Retried until it takes: the difficulty field lives in game memory that does
// not exist until the player is in the world, so the first attempts after
// connecting will fail and that is expected.
void EnforceServerRules() {
  std::string wanted;
  {
    std::lock_guard<std::mutex> lock(g_server_rules_mutex);
    if (g_difficulty_applied || g_wanted_difficulty.empty()) {
      return;
    }
    wanted = g_wanted_difficulty;
  }
  if (!PlayerInGame()) {
    return;
  }
  const int index = DifficultyIndexForName(wanted);
  if (index < 0 || CurrentDifficulty() < 0) {
    return;
  }
  RequestDifficulty(index);
  std::lock_guard<std::mutex> lock(g_server_rules_mutex);
  g_difficulty_applied = true;
}

}  // namespace

void ConfigureServerRules(const std::string& host, std::uint16_t port) {
  if (host.empty() || port == 0) {
    return;
  }
  // Off the calling thread: this runs from the register acknowledgement, and a
  // server that answers slowly must not hold up joining.
  std::thread(&FetchServerRules, host, port).detach();
}

void ClearServerRules() {
  std::lock_guard<std::mutex> lock(g_server_rules_mutex);
  g_wanted_difficulty.clear();
  g_difficulty_applied = false;
}

// Is the player actually PLAYING, as opposed to merely existing?
//
// IsPlayerSpawned is backed by the skateboard transform, which exists while the
// world is still loading - measured: a script waiting on it printed during the
// load, not on arrival. Acting then is why the forced game mode did not stick
// and why the difficulty write raced the spawn sequence.
//
// Retail publishes presence context 0x8001 - 1 in gameplay, 0 in the front end,
// pause menu or a load - and that is the honest signal. The transform check
// stays as well, so "in gameplay" also means there is a skater to act on.
bool PlayerInGame() {
  if (rex::kernel::guest_presence::GameplayContextValue() != 1) {
    return false;
  }
  float position[3];
  return trick_pipeline::CurrentLocalBoardPosition(position);
}

namespace {

// Holds an online session in Free Play.
//
// Written once on connect it did not stick: the game's own spawn sequence runs
// afterwards and set career back about three seconds later. So it is asserted
// for as long as the session lasts, the same way the server's difficulty is -
// a rule that only applies until the game disagrees is not a rule.
//
// Only ever writes when the value is actually wrong, so a game that leaves it
// alone costs one comparison a frame.
void EnforceOnlineGameMode() {
  // Set from the FIRST FRAME of an online launch, not on connect.
  //
  // Waiting until the player was in the world was the mistake: by then the
  // career challenges are already loaded, and changing the mode afterwards does
  // not unload them - which is why denying their data instead crashed, leaving
  // references to content that was never meant to be there in the first place.
  //
  // The launcher says this is an online session before anything loads
  // (skate3_multiplayer_relay_active), so the game can be told what it is
  // early enough for the answer to matter.
  if (!OnlineSessionFlag()) {
    return;
  }
  const int mode = CurrentGameMode();
  if (mode < 0 || mode == kGameModeFreePlay) {
    return;
  }
  SetGameMode(kGameModeFreePlay);
}

}  // namespace

namespace {

// Logs the game-mode global whenever it changes.
//
// There is no console offline - resources come from a server - so this is how
// the mode numbers get identified: play single player, switch to Free Play,
// and the log names both values. IsFreeSkateMode treats 0, 3 and 4 as free
// skate, but which is which, and what career is, has to be observed.
void ReportGameModeChanges() {
  static int last = -2;
  const int mode = CurrentGameMode();
  if (mode == last) {
    return;
  }
  last = mode;
  REXLOG_INFO("retail-ui: game mode is now {} (free skate: {})", mode,
              (mode == 0 || mode == 3 || mode == 4) ? "yes" : "no");
}

}  // namespace

// --- Everything unlocked ------------------------------------------------
//
// Retail already has this switch. Finding it beat every display hook that came
// before, and the difference is the whole point: a display hook tells the menu
// an item is available while the code that applies it still says no. This tells
// the one system BOTH of them ask.
//
// THE CHAIN, read out of the image rather than guessed:
//
//   sub_825F46C8 is the predicate behind "is this item locked". It is reached
//   as a functor - the 24-byte record at 0x823046E0 holds it at +8 - which is
//   why no call site names it, and why a search for callers found nothing. Both
//   users matter: sub_825FF5E8 asks it to choose "locked" over "owned" for the
//   padlock, and SelectItem (sub_825FD9F0) asks it at 0x825FDB40 before it will
//   apply an item. One predicate, two questions, one answer.
//
//   It reads two 16-bit unlock ids off the item (+240 and +220). 0xFFFF in both
//   means "nothing to unlock" and it returns false immediately. Otherwise it
//   FNV-1 hashes the unlock's name - the 0x811C9DC5 basis and 0x01000193 prime
//   are both literal in the function - and asks sub_82503AE0.
//
//   sub_82503AE0 begins: lbz r11,36(r3) ... if nonzero, return true. Every
//   query, every category, short-circuited by one byte. sub_82503B60 next door
//   opens the same way. The constructor (sub_825039F0) zeroes that byte and
//   never writes it again, and nothing else in the image does either - so it is
//   a switch with no owner, which is what a leftover development flag looks
//   like.
//
// So: one byte, in the game's own words. The padlock goes, the blurb goes, and
// SelectItem accepts - not because three hooks were talked into agreeing, but
// because there was only ever one question.
//
// Runtime only. The flag lives in a heap object the constructor rebuilds every
// launch, so nothing here can reach a save file - which rules out the other
// route to the same place (writing career counters until the milestones are
// genuinely met) on the grounds that it would edit the player's career for
// real.
namespace {

// [0x83067060] is the service table; slot +20 is the progression manager.
// Taken from sub_825F46C8 itself, which builds 0x83067060 at 0x825F46E8 and
// then loads 20(r6).
constexpr uint32_t kProgressionManagerSlot = 0x83067074u;
constexpr uint32_t kUnlockAllFlagOffset = 36u;

// What the flag was last seen to be, so a script can ask whether the request
// has landed rather than only whether it was made.
std::atomic<bool> g_unlock_applied{false};

// ONLINE FORCES IT, and no script is consulted.
//
// Everyone joining a session gets the same wardrobe - which is the fair state,
// not a favour - and that is a rule of the session rather than a setting, so it
// belongs here for the same reason the menu trimming and the forced Free Play
// do: a resource must not be able to hand itself a different game by calling,
// or not calling, a native. It follows that SetUnlockEverything(false) cannot
// take an online session's unlocks away; the native reports that rather than
// failing quietly.
//
// Launch-scoped, like the Free Play forcing: the launcher says this is an
// online session before anything loads, so the flag is on from the first frame
// and there is no window in which a menu could be drawn with padlocks on it.
bool UnlockForcedByOnlineSession() {
  return OnlineSessionFlag();
}

void EnforceUnlockEverything(uint8_t* base) {
  if (base == nullptr) {
    return;
  }
  uint32_t manager = 0;
  if (!guest_probe::ReadU32(kProgressionManagerSlot, manager) ||
      manager == 0) {
    return;  // not constructed yet; nothing to set
  }
  // The flag is the HIGH byte of this word, big-endian. Reading it through the
  // guarded copy first also proves the page is mapped before anything is
  // written to it.
  uint32_t word = 0;
  if (!guest_probe::ReadU32(manager + kUnlockAllFlagOffset, word)) {
    return;
  }
  const uint8_t desired =
      (UnlockForcedByOnlineSession() ||
       UnlockEverythingFlag())
          ? 1
          : 0;
  if (static_cast<uint8_t>(word >> 24) == desired) {
    g_unlock_applied.store(desired != 0, std::memory_order_relaxed);
    return;  // already right - the steady state, and it costs one compare
  }
  base[manager + kUnlockAllFlagOffset] = desired;
  g_unlock_applied.store(desired != 0, std::memory_order_relaxed);
  REXLOG_INFO("retail-ui: progression unlock-all = {} (manager 0x{:08X})",
              desired != 0, manager);
}

}  // namespace

bool UnlockEverythingApplied() {
  return g_unlock_applied.load(std::memory_order_relaxed);
}

bool UnlockEverythingForced() {
  return UnlockForcedByOnlineSession();
}

bool UnlockEverythingWanted() {
  return UnlockForcedByOnlineSession() ||
         UnlockEverythingFlag();
}

// --- The board-sales milestone bar --------------------------------------
//
// The strip above the menu - "Board Sales Milestone 2/6" and its progress bar -
// is career furniture, and online there is no career for it to be about.
//
// It is not a screen. The whole front-end registry is enumerable now
// (tools/guest_image.pl screens) and there is no entry for it: it is drawn
// INSIDE options/options.swf, so there is nothing to leave unloaded. What the
// movie does have is a binding it asks for the dots, GetMilestoneDotData
// (sub_825AFC98), and that function already contains the case we want:
//
//   sub_828DA2C8(id, id2, &vec)         ; fills a vector, zeroed beforehand
//   r21 = vec.begin ; r26 = vec.end
//   if (r21 == r26) goto loc_825AFE84   ; empty -> skip the fill loop entirely
//
// So an empty vector is a path retail already takes, not a state invented here.
// Declining the fill leaves the caller's own zeroed vector in place, which is
// exactly what "no milestones to draw" looks like from inside the function.
//
// Filtered by return address because sub_828DA2C8 is a general-purpose data
// lookup with many callers; 0x825AFCE8 is the one call site inside
// GetMilestoneDotData, and nothing else is touched.
//
// Online only. A player's own career keeps its progress bar.
namespace {

constexpr uint32_t kMilestoneDotFillCallSite = 0x825AFCE8u;

}  // namespace

extern "C" REX_FUNC(sub_828DA2C8) {
  if (ctx.lr == kMilestoneDotFillCallSite &&
      OnlineSessionFlag()) {
    return;
  }
  __imp__sub_828DA2C8(ctx, base);
}

void Tick() {
  // FIRST: every hook in this file reads its flags from the atomics this
  // fills, precisely so none of them has to touch the cvar registry from
  // inside a guest function that runs thousands of times a frame. See
  // RefreshCachedFlags' comment.
  RefreshCachedFlags();
  RefreshPauseMenuBlock();
  RefreshOnlineMenuPolicy();
  EnforceServerRules();
  EnforceOnlineGameMode();
  ReportGameModeChanges();
  EnforceUnlockEverything(native_scene::g_guest_base.load(
      std::memory_order_relaxed));
}

// --- Trimming the career menu ------------------------------------------
//
// The Career > Main rows come from GetNumOptions(tab) / GetOptionName(tab,row)
// - the movie renders exactly what it is told, so reporting fewer rows removes
// them outright rather than greying them.
//
// ONLY the row COUNT is changed, and only for one tab. That deliberately keeps
// the rows you keep at their original indices, so selection still lands on the
// right thing: no remapping, nothing to get subtly wrong. It follows that this
// can only trim rows off the END of a tab - which is exactly what is wanted
// here (Challenge Map and Edit Skaters are rows 0 and 1).
// The menu rules are the SERVER's, not a local setting.
//
// Single player keeps retail's game whole - a player's own career is theirs.
// Online is where everyone has to be playing the same game, so these are
// applied by a resource the server delivers and cleared when it stops. That
// also means they cannot persist into an offline session by accident, which a
// setting in settings.toml would.
namespace {

struct MenuPolicy {
  bool active = false;
  std::string tab_rows;    // "0:2,1:0,2:1,4:2"
  int settings_rows = 0;   // 0 = leave Game Settings alone
  int settings_skip = 0;   // leading rows to hide
};

MenuPolicy g_menu_policy;
std::mutex g_menu_policy_mutex;

MenuPolicy Policy() {
  std::lock_guard<std::mutex> lock(g_menu_policy_mutex);
  return g_menu_policy;
}

}  // namespace

void SetMenuPolicy(bool active, std::string tab_rows, int settings_rows,
                   int settings_skip) {
  std::lock_guard<std::mutex> lock(g_menu_policy_mutex);
  g_menu_policy.active = active;
  g_menu_policy.tab_rows = std::move(tab_rows);
  g_menu_policy.settings_rows = settings_rows;
  g_menu_policy.settings_skip = settings_skip;
  REXLOG_INFO("retail-ui: menu policy {}", active ? "applied" : "cleared");
}

namespace {

// Parses "0:2,1:0,2:1" into the row count kept for `tab`, or -1 when that tab
// is not listed. Re-parsed only when the setting text changes, since these
// queries run per row per frame.
int RowsKeptForTab(uint32_t tab) {
  static std::string cached;
  static std::unordered_map<uint32_t, int> parsed;
  const std::string current = Policy().tab_rows;
  if (current != cached) {
    cached = current;
    parsed.clear();
    std::size_t at = 0;
    while (at < current.size()) {
      const std::size_t comma = current.find(',', at);
      const std::string entry =
          current.substr(at, comma == std::string::npos ? std::string::npos
                                                       : comma - at);
      const std::size_t colon = entry.find(':');
      if (colon != std::string::npos) {
        // Anything malformed is skipped rather than guessed at: a mis-parsed
        // entry would silently empty the wrong tab.
        try {
          parsed[static_cast<uint32_t>(std::stoul(entry.substr(0, colon)))] =
              std::stoi(entry.substr(colon + 1));
        } catch (...) {
        }
      }
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
  }
  const auto found = parsed.find(tab);
  return found == parsed.end() ? -1 : found->second;
}

// Reads script argument `index` as an integer, the same way every binding
// does. The accessor takes the argument NUMBER in r3, so reading one here does
// not disturb the original reading it again.
uint32_t ScriptArgInt(const PPCContext& ctx, uint8_t* base, uint32_t index) {
  PPCContext probe = ctx;
  probe.r3.u32 = index;
  sub_82E62E00(probe, base);
  sub_82E5F2A8(probe, base);
  return probe.r3.u32;
}

}  // namespace

// GetNumOptions(tab) -> row count
extern "C" REX_FUNC(sub_825B6DC0) {
  const uint32_t tab = ScriptArgInt(ctx, base, 0);
  __imp__sub_825B6DC0(ctx, base);
  if (!Policy().active) {
    return;
  }
  // What the tab REALLY holds, logged once per (tab, count).
  //
  // The rows we hide are still there; the question for adding an entry is
  // whether the list has more than we are showing. If a tab's real count is
  // already what we display, there is nothing hidden to reveal and a new entry
  // would have to be synthesised - including whatever it does when chosen.
  {
    static std::mutex seen_mutex;
    static std::unordered_map<uint32_t, uint32_t> seen;
    PPCContext peek = ctx;
    sub_82E5F2A8(peek, base);  // the wrapped count, as an integer
    const uint32_t real = peek.r3.u32;
    std::lock_guard<std::mutex> lock(seen_mutex);
    const auto found = seen.find(tab);
    if (found == seen.end() || found->second != real) {
      seen[tab] = real;
      REXLOG_INFO("retail-ui: menu tab {} really has {} row(s)", tab, real);
    }
  }

  const int rows = RowsKeptForTab(tab);
  if (rows < 0) {
    return;
  }
  // Re-wrap our own count with the same wrapper the original ends on, so the
  // movie receives an ordinary script integer.
  PPCContext reply = ctx;
  reply.r3.u32 = static_cast<uint32_t>(rows);
  sub_82E86550(reply, base);
  ctx.r3 = reply.r3;
}

// Remapping the Game Settings row index.
//
// Every one of that menu's accessors - title, type, integer value, string
// value, value description - reads exactly ONE script argument: the row. They
// then index their own tables inline, so there is no per-accessor inner
// function to intercept the way the wardrobe had.
//
// So the interception happens at the conversion they all share, filtered to
// calls made from inside those accessors. One hook covers all five and they
// cannot drift out of step with each other - which is the failure mode that
// made remapping look unattractive.
//
// The filter matters: this conversion is used by every binding in the game.
constexpr uint32_t kGameSettingsFirst = 0x825A6C00u;
constexpr uint32_t kGameSettingsLast = 0x825A7100u;

extern "C" REX_FUNC(sub_82E5F2A8) {
  const uint32_t caller = ctx.lr;
  __imp__sub_82E5F2A8(ctx, base);
  if (caller < kGameSettingsFirst || caller > kGameSettingsLast) {
    return;
  }
  const MenuPolicy policy = Policy();
  if (!policy.active) {
    return;
  }
  const int skip = policy.settings_skip;
  if (skip > 0) {
    ctx.r3.u32 += static_cast<uint32_t>(skip);
  }
}

// GameSettings_GetNumOptions - the settings submenu counts its own rows.
extern "C" REX_FUNC(sub_825A6F70) {
  __imp__sub_825A6F70(ctx, base);
  const MenuPolicy policy = Policy();
  if (!policy.active || policy.settings_rows <= 0) {
    return;
  }
  const int rows = policy.settings_rows;
  PPCContext reply = ctx;
  reply.r3.u32 = static_cast<uint32_t>(rows);
  sub_82E86550(reply, base);
  ctx.r3 = reply.r3;
}

// IsItemEnabled(tab, row), as the item database answers it.
//
// Reporting a smaller row COUNT hides rows from the render, but the cursor
// still walks onto them and can select them - the count drives drawing, not
// navigation. Marking the trimmed rows disabled is what stops them being
// reached, and this inner query takes plain integers, so the row number is
// readable without going through the script argument machinery.
extern "C" REX_FUNC(sub_8261E208) {
  const uint32_t tab = ctx.r4.u32;
  const uint32_t row = ctx.r5.u32;
  __imp__sub_8261E208(ctx, base);
  if (!Policy().active) {
    return;
  }
  const int rows = RowsKeptForTab(tab);
  if (rows >= 0 && row >= static_cast<uint32_t>(rows)) {
    ctx.r3.u32 = 0;
  }
}

// Challenges are the server's business online, not retail's.
//
// The action overlay asks this before offering a challenge sign-up - the
// "Chan Center / Sign up" prompt. Answering no online stops a player entering
// a career challenge from a shared session, where it would drop them into
// scripted content nobody else is in.
//
// Whether it also removes the world ICON is the open question: the icon may be
// drawn from the challenge list regardless of whether it can be joined. If it
// stays, the marker needs a different lever and this is still worth keeping -
// being unable to sign up is the part that matters for fair play.
extern "C" REX_FUNC(sub_825C1FE8) {
  if (lua_client::LocalPlayerId() == 0) {
    __imp__sub_825C1FE8(ctx, base);
    return;
  }
  // The same wrapper the original ends with, so the answer is an ordinary
  // script boolean.
  ctx.r3.u32 = 0;
  sub_82E86608(ctx, base);
}

// Coverage capture without a console.
//
// The Lua console only exists in an online session, because resources come
// from the server. But the interesting thing to capture - what retail does
// when the Career menu opens the skater editor - only exists OFFLINE, where
// that menu has an Edit Skaters row. So the capture needs a keybind.
//
// Toggling: the first press arms, the second writes the addresses next to the
// executable and names the file after the capture number, so a control run and
// a real run are simply two presses each.
namespace {

std::atomic<int> g_capture_index{0};
bool g_capture_armed = false;

}  // namespace

void ToggleCoverageCapture() {
  if (!g_capture_armed) {
    function_coverage::ResetAndArm();
    g_capture_armed = true;
    REXLOG_WARN("coverage: ARMED - do the thing, then press the key again");
    return;
  }
  g_capture_armed = false;
  const std::vector<std::uint32_t> addresses =
      function_coverage::SnapshotAndDisarm();
  const int index = g_capture_index.fetch_add(1, std::memory_order_relaxed);
  char name[64];
  std::snprintf(name, sizeof(name), "coverage_key%d.txt", index);
  const std::filesystem::path path = rex::filesystem::GetAppRootFolder() / name;
  if (std::FILE* file = std::fopen(path.string().c_str(), "w")) {
    for (const std::uint32_t address : addresses) {
      std::fprintf(file, "%08X\n", address);
    }
    std::fclose(file);
    REXLOG_WARN("coverage: wrote {} address(es) to {}", addresses.size(),
                path.string());
  } else {
    REXLOG_WARN("coverage: could not write {}", path.string());
  }
}

// --- Opening retail's skater editor ------------------------------------
//
// Traced from the menu itself. Activating a row resolves it to an ACTION id
// (sub_8261BC30 switches on it); "MyCareerTeam" - the Edit Skaters screen - is
// action 25, and its case is three instructions:
//
//     r4 = 63 ; r5 = 0 ; r3 = menu manager ; bl sub_8261AF30
//
// and sub_8261AF30 ends in:
//
//     manager = [0x830CFE1C]
//     sub_82D0AFA0(manager, 63, mode)      <- SetFrontEndState
//
// So screens ARE opened through the front-end state machine after all. The
// earlier attempt failed for want of the state id, and because it read a
// manager captured from a boot-time transition rather than this global.
//
// Note this needs no menu context: it is not "select the row that means Edit
// Skaters", which is unreachable in Free Play. It is the transition that row
// would have performed.
constexpr uint32_t kFrontEndManagerGlobal = 0x830CFE1Cu;
constexpr uint32_t kFrontEndStateSkaterEditor = 63;

namespace {

std::atomic<int> g_pending_front_end_state{-1};

}  // namespace

bool RequestSkaterEditor() {
  g_pending_front_end_state.store(
      static_cast<int>(kFrontEndStateSkaterEditor), std::memory_order_relaxed);
  return true;
}

void ApplyPendingFrontEndState(PPCContext& ctx, uint8_t* base) {
  const int wanted =
      g_pending_front_end_state.exchange(-1, std::memory_order_acq_rel);
  if (wanted < 0) {
    return;
  }
  // Call sub_8261AF30, NOT SetFrontEndState directly.
  //
  // Calling the state change on its own left the pause menu unopenable
  // afterwards: sub_8261AF30 first stores a flag at [menu + 394], and without
  // it the menu manager still believes it is open. Performing the whole
  // transition the game performs - rather than the last step of it - is what
  // keeps the front end consistent.
  //
  // The menu manager, as sub_82620530 resolves it:
  //     [[sub_824AD240() + 8] + 224]
  PPCContext lookup = ctx;
  sub_824AD240(lookup, base);
  const uint32_t manager = lookup.r3.u32;
  uint32_t object = 0, menu = 0;
  if (manager == 0 || !guest_probe::ReadU32(manager + 8, object) ||
      object == 0 || !guest_probe::ReadU32(object + 224, menu) || menu == 0) {
    REXLOG_WARN("retail-ui: no menu manager yet; editor not opened");
    return;
  }
  PPCContext call = ctx;
  call.r3.u32 = menu;
  call.r4.u32 = static_cast<uint32_t>(wanted);
  call.r5.u32 = 0;
  REXLOG_INFO("retail-ui: front-end state {} (menu 0x{:08X})", wanted, menu);
  sub_8261AF30(call, base);
}

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher) {
  if (dispatcher == nullptr) {
    return;
  }
  REXLOG_INFO("retail-ui: wardrobe hooks installed");
}

}  // namespace skate3::retail_ui
