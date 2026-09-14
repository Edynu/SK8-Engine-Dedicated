#include "skate3_dlc_runtime.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DECLARE(bool, skate3_no_retail_missions);
REXCVAR_DECLARE(bool, skate3_no_retail_challenges);

namespace {

std::atomic<uint32_t> g_manager{0};
std::atomic<bool> g_gunplay_gesture_archive_mounted{false};
std::atomic<uint32_t> g_gunplay_collection{0};
std::atomic<uint64_t> g_last_cac_body_key{UINT64_MAX};

void LogCall(const char* operation, PPCContext& ctx) {
  REXLOG_WARN(
      "skate3-dlc: {} manager=0x{:08X} r4=0x{:08X} r5=0x{:08X} "
      "caller=0x{:08X}",
      operation, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.lr);
}

std::string ReadGuestString(uint8_t* base, uint32_t address,
                            size_t max_length = 512) {
  if (!address) {
    return {};
  }
  std::string value;
  value.reserve(128);
  for (size_t i = 0; i < max_length; ++i) {
    const char character = static_cast<char>(REX_LOAD_U8(address + i));
    if (!character) {
      break;
    }
    value.push_back(character);
  }
  return value;
}

}  // namespace

namespace skate3::dlc_runtime {

bool IsGunplayGestureArchiveMounted() {
  return g_gunplay_gesture_archive_mounted.load(std::memory_order_acquire);
}

uint32_t ProbeGunplayGestureCollection(PPCContext& ctx, uint8_t* base) {
  constexpr uint64_t kCacBodyClass = 0x95E142284549717FULL;
  constexpr uint64_t kGunplayTestKey = 0x7328108B01BE6A05ULL;
  PPCContext probe = ctx;
  probe.r3.u64 = kCacBodyClass;
  probe.r4.u64 = kGunplayTestKey;
  sub_82B69B08(probe, base);
  const uint32_t collection = probe.r3.u32;
  if (collection != 0 &&
      g_gunplay_collection.exchange(collection, std::memory_order_acq_rel) ==
          0) {
    REXLOG_WARN(
        "skate3-dlc: verified registered collection "
        "cac_body/gunplaytest=0x{:08X}",
        collection);
  }
  return collection;
}

void ObserveManagerConstructor(PPCContext& ctx) {
  g_manager.store(ctx.r3.u32, std::memory_order_release);
  g_gunplay_gesture_archive_mounted.store(false, std::memory_order_release);
  LogCall("Manager::Manager", ctx);
}

void ObserveManagerRun(PPCContext& ctx) { LogCall("Manager::Run", ctx); }

void ObserveManagerEnumerate(PPCContext& ctx) {
  LogCall("Manager::EnumerateContent", ctx);
}

void ObserveManagerRefresh(PPCContext& ctx) {
  LogCall("Manager::Refresh", ctx);
}

void ObserveDriverMount(PPCContext& ctx) {
  LogCall("XenonDLCDriver::Mount", ctx);
}

void ObserveContentManagerEnumerate(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kActiveUserIndexAddress = 0x82FC8851;
  const auto active_user_index = REX_LOAD_U8(kActiveUserIndexAddress);
  REXLOG_WARN(
      "skate3-dlc: ContentManager::EnumerateContent manager=0x{:08X} "
      "active_user={} (raw=0x{:02X}) caller=0x{:08X}",
      ctx.r3.u32, static_cast<int8_t>(active_user_index), active_user_index,
      ctx.lr);
}

std::atomic<uint32_t> g_denied_archive_path{0};
std::atomic<uint32_t> g_denied_challenge_path{0};

// A guest-visible C string, allocated once out of the system heap. The same
// idiom skate3_custom_trick.cpp uses to hand retail a string of our own.
uint32_t EnsureGuestString(uint8_t* base, std::atomic<uint32_t>& storage,
                           std::string_view value) {
  uint32_t address = storage.load(std::memory_order_acquire);
  if (address) {
    return address;
  }
  const uint32_t allocated = REX_KERNEL_MEMORY()->SystemHeapAlloc(128, 16);
  if (!allocated) {
    return 0;
  }
  uint32_t expected = 0;
  if (!storage.compare_exchange_strong(expected, allocated,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    REX_KERNEL_MEMORY()->SystemHeapFree(allocated);
    return expected;
  }
  const size_t length = std::min<size_t>(value.size(), 127);
  for (size_t index = 0; index < length; ++index) {
    REX_STORE_U8(allocated + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(value[index]));
  }
  REX_STORE_U8(allocated + static_cast<uint32_t>(length), 0);
  return allocated;
}

// Denies retail's mission ASSET archive: data/content/missions.big, 508
// streamed content packages named <type>_<zone>_<nn> with _Tex/_Pres/_Sim
// sub-packages.
//
// WHAT THIS DOES AND DOES NOT DO. It stops ~61MB of mission scenery
// streaming in. It does NOT remove the mission markers, because the
// challenge DEFINITIONS live elsewhere - 438 XML entries under
// challenge_local_data.class inside data/big/db.big - and those are what
// place a marker in the world. Denying db.big as well was tried and crashes
// the game during boot; see the note in kDeniedArchives. Removing the
// markers themselves is still an open problem.
//
// This is the boundary that opens it:
// BigHandler::AddArchiveFromFile(handler, path, flags), found by reading the
// log line this same function already emitted.
//
// The path is REWRITTEN to one that does not exist, rather than the call
// being skipped or a failure return fabricated. That matters: retail
// already has a real code path for "this archive would not open", because
// DLC archives go through this exact function and are routinely absent. So
// this makes the game take a path it ships with, instead of one invented
// here. Nothing is patched, and no return value is guessed at. Confirmed in
// practice: denying missions.big produced a clean NtCreateFile failure and
// the game carried on loading.
//
// r4 is an argument register (volatile under the PPC ABI), so leaving the
// substituted pointer in place after the call is not a corruption.
bool DenyRetailMissionArchive(PPCContext& ctx, uint8_t* base,
                              const std::string& path) {
  if (!REXCVAR_GET(skate3_no_retail_missions)) {
    return false;
  }
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  // Matched with the directory prefix so a future archive that merely ends
  // in the same filename cannot be caught by accident.
  //
  // db.big is NOT in this list, and must not be added back. It holds the
  // challenge DEFINITIONS, so denying it is the obvious idea - and it
  // crashes the game during boot, before the world loads at all. It is
  // loaded far earlier than the mission content and the data system needs it
  // to exist, whatever else it also carries. Tried and reverted; see the
  // note above kDeniedArchives' effect for what this cvar can and cannot do.
  static constexpr std::string_view kDeniedArchives[] = {
      "content\\missions.big",
  };
  bool denied = false;
  for (std::string_view candidate : kDeniedArchives) {
    if (lower.find(candidate) != std::string_view::npos) {
      denied = true;
      break;
    }
  }
  if (!denied) {
    return false;
  }
  const uint32_t replacement = EnsureGuestString(
      base, g_denied_archive_path, "data\\content\\missions.disabled");
  if (!replacement) {
    REXLOG_WARN(
        "skate3_no_retail_missions: could not allocate a replacement path; "
        "retail mission content will load normally");
    return false;
  }
  ctx.r4.u32 = replacement;
  REXLOG_WARN(
      "skate3_no_retail_missions: DENYING '{}' (path rewritten to a "
      "nonexistent archive so retail's own missing-archive path runs)",
      path);
  return true;
}

void ObserveAddArchiveFromFile(PPCContext& ctx, uint8_t* base) {
  const std::string path = ReadGuestString(base, ctx.r4.u32);
  if (DenyRetailMissionArchive(ctx, base, path)) {
    return;
  }
  if (path.find("dlc_codex_gunplay") != std::string::npos) {
    g_gunplay_gesture_archive_mounted.store(true,
                                            std::memory_order_release);
    REXLOG_WARN("skate3-dlc: armed Gunplay CAC slot bridge for '{}'", path);
  }
  REXLOG_WARN(
      "skate3-dlc: BigHandler::AddArchiveFromFile handler=0x{:08X} "
      "path='{}' flags=0x{:08X} caller=0x{:08X}",
      ctx.r3.u32, path, ctx.r5.u32, ctx.lr);
}

// Retail's career challenges come from VLT files inside db.big -
// "challenge_local_data\default.vlt" and "...\main.vlt", found by searching
// the running game for the class name.
//
// db.big itself CANNOT be denied (it is load-bearing and crashes the game on
// boot - see kDeniedArchives), but the individual files can: this is a normal
// asynchronous file open, and a missing file is a case retail already handles
// because DLC content is routinely absent.
//
// Denying the DATA rather than hiding the markers is what makes the challenges
// leave both the world and the minimap - they are two views of one list, so
// suppressing either view would have left the other.
bool DenyRetailChallengeData(PPCContext& ctx, uint8_t* base,
                             const std::string& lower_path,
                             const std::string& path) {
  if (!REXCVAR_GET(skate3_no_retail_challenges)) {
    return false;
  }
  // Only files INSIDE challenge_local_data - each challenge is its own VLT,
  // e.g. "challenge_local_data\tele_world_to_train_park.vlt".
  //
  // NOT challenge_local_data_framework.vlt, which sits beside the directory
  // and defines the schema rather than any challenge. Denying that one is the
  // same mistake as denying db.big: it is structural, and the game crashed
  // during load with a call through a null function pointer. Measured, and the
  // reason this match needs the separator.
  if (lower_path.find("challenge_local_data\\") == std::string::npos &&
      lower_path.find("challenge_local_data/") == std::string::npos) {
    return false;
  }
  const uint32_t replacement = EnsureGuestString(
      base, g_denied_challenge_path, "challenge_local_data\disabled.vlt");
  if (!replacement) {
    return false;
  }
  ctx.r4.u32 = replacement;
  REXLOG_WARN(
      "skate3_no_retail_challenges: DENYING '{}' (path rewritten to a "
      "nonexistent file so retail's own missing-file path runs)",
      path);
  return true;
}

void ObserveAsyncFileOpen(PPCContext& ctx, uint8_t* base) {
  const std::string path = ReadGuestString(base, ctx.r4.u32);
  std::string lower_path = path;
  std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (DenyRetailChallengeData(ctx, base, lower_path, path)) {
    return;
  }
  const bool gesture_relevant =
      lower_path.find("gunplay") != std::string::npos ||
      lower_path.find("gesture") != std::string::npos ||
      lower_path.find(".vaultlist") != std::string::npos ||
      lower_path.ends_with(".vlt") || lower_path.ends_with(".bin") ||
      lower_path.ends_with(".abin");
  if (!gesture_relevant) {
    return;
  }
  REXLOG_WARN(
      "skate3-dlc: async-open manager=0x{:08X} path='{}' r5=0x{:08X} "
      "r6=0x{:08X} r7=0x{:08X} r8=0x{:08X} caller=0x{:08X}",
      ctx.r3.u32, path, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
      ctx.lr);
}

void ObserveVaultDlcLoadUpdate(PPCContext& ctx, uint8_t* base) {
  LogCall("cVaultManager::DLCLoadUpdate", ctx);
  if (!IsGunplayGestureArchiveMounted() ||
      g_gunplay_collection.load(std::memory_order_acquire) != 0) {
    return;
  }

  ProbeGunplayGestureCollection(ctx, base);
}

void ObserveFindCollection(PPCContext& ctx) {
  constexpr uint64_t kCacBodyClass = 0x95E142284549717FULL;
  if (ctx.r3.u64 != kCacBodyClass) {
    return;
  }
  const uint64_t key = ctx.r4.u64;
  if (g_last_cac_body_key.exchange(key, std::memory_order_acq_rel) == key) {
    return;
  }
  REXLOG_WARN(
      "skate3-dlc: Attrib::FindCollection class=cac_body "
      "key=0x{:016X} caller=0x{:08X}",
      key, ctx.lr);
}

void ObserveLanguageDlcLoadUpdate(PPCContext& ctx) {
  LogCall("LanguageManager::DLCLanguageDBLoadUpdate", ctx);
}

}  // namespace skate3::dlc_runtime
