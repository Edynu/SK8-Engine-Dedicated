#include "skate3_retail_bail.h"

#include "skate3_guest_probe.h"
#include "skate3_trick_pipeline.h"
#include "skate3_trick_types.h"

#include <chrono>
#include <mutex>

namespace skate3::retail_bail {

namespace {

std::mutex g_mutex;
State g_published;
std::vector<Event> g_events;

// Working state, touched only by Update on the app thread.
bool g_off_board = false;
bool g_bailed = false;
std::int64_t g_started_ms = 0;
std::uint64_t g_last_wipeout_requests = 0;

// How long after going off-board a wipeout request still counts as evidence
// that this was a crash.
//
// Not zero: the collector flips to Offboard and retail's wipeout query fire in
// the same rough moment but not in a guaranteed order, so demanding they land
// on the same frame would classify real bails as stepping off. Not long
// either, or walking off the board and crashing a second later would be
// backdated into one bail.
constexpr std::int64_t kWipeoutEvidenceWindowMs = 400;

[[nodiscard]] std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Publish(const State &state) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_published = state;
}

void Emit(EventKind kind, std::uint32_t duration_ms) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Capped so a run with nothing draining these cannot grow without bound.
  if (g_events.size() < 32) {
    g_events.push_back(Event{kind, duration_ms});
  }
}

}  // namespace

void Update() {
  const std::uint32_t module = trick_pipeline::CurrentLocalScoreModule();
  std::uint32_t collector_state = 0;
  const bool readable =
      module != 0 &&
      guest_probe::ReadU32(module + trick::ScoreModuleLayout::kCollectorState,
                           collector_state);
  if (!readable) {
    // No module yet - front end, or before the skater exists. Treated as "not
    // off the board" rather than as a bail, so loading does not fire events.
    if (g_off_board) {
      g_off_board = false;
      g_bailed = false;
      Publish(State{});
    }
    return;
  }

  const bool off_board =
      collector_state ==
      static_cast<std::uint32_t>(trick::ScoreCollectorState::Offboard);

  const std::uint64_t wipeouts = trick_pipeline::CurrentLocalWipeoutRequests();
  const bool wipeout_active = wipeouts != g_last_wipeout_requests;
  g_last_wipeout_requests = wipeouts;

  const std::int64_t now = NowMs();

  if (off_board && !g_off_board) {
    g_off_board = true;
    g_started_ms = now;
    // Classified now from whatever evidence exists, then upgraded below if a
    // wipeout request arrives within the window.
    g_bailed = wipeout_active;
    Emit(g_bailed ? EventKind::kBailed : EventKind::kSteppedOff, 0);
  } else if (off_board && g_off_board && !g_bailed && wipeout_active &&
             now - g_started_ms <= kWipeoutEvidenceWindowMs) {
    // Went off-board and the crash evidence arrived a frame or two later. The
    // stepped-off event has already gone out, so a bail is emitted rather than
    // rewriting history - a listener that acted on the first one needs to see
    // the correction, not have it silently swapped.
    g_bailed = true;
    Emit(EventKind::kBailed, 0);
  } else if (!off_board && g_off_board) {
    const std::int64_t elapsed = now - g_started_ms;
    Emit(EventKind::kRecovered,
         elapsed > 0 ? static_cast<std::uint32_t>(elapsed) : 0u);
    g_off_board = false;
    g_bailed = false;
  }

  State state;
  state.off_board = g_off_board;
  state.bailed = g_bailed;
  if (g_off_board) {
    const std::int64_t elapsed = now - g_started_ms;
    state.duration_ms = elapsed > 0 ? static_cast<std::uint32_t>(elapsed) : 0u;
  }
  Publish(state);
}

State Current() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_published;
}

std::vector<Event> TakeEvents() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Event> out;
  out.swap(g_events);
  return out;
}

}  // namespace skate3::retail_bail
