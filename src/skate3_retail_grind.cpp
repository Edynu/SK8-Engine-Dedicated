#include "skate3_retail_grind.h"

#include "skate3_guest_probe.h"
#include "skate3_trick_pipeline.h"
#include "skate3_trick_types.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace skate3::retail_grind {

namespace {

std::mutex g_mutex;
// Published state, read by script/other threads. Only ever written under the
// lock, and only from Publish below.
Grind g_published;
std::vector<Completed> g_completed;

// Working state, touched by Update alone on the app thread. Kept separate from
// the published copy so a reader can never observe a half-updated grind - the
// first version mutated the shared struct field by field and raced.
Grind g_working;
std::int64_t g_started_ms = 0;
float g_last_position[3] = {};
bool g_have_last_position = false;

[[nodiscard]] std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] float BitsToFloat(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Retail's grind reward, or 0 when the collector is not resolvable. Read
// rather than accumulated: this is retail's own number, and the point of
// exposing it is that it agrees with what the game scored.
[[nodiscard]] float ReadGrindReward(std::uint32_t module) {
  std::uint32_t collector = 0;
  if (!guest_probe::ReadU32(module + trick::ScoreModuleLayout::kGrindCollector,
                            collector) ||
      collector == 0) {
    return 0.0f;
  }
  std::uint32_t bits = 0;
  if (!guest_probe::ReadU32(
          collector + trick::GrindCollectorLayout::kCurrentReward, bits)) {
    return 0.0f;
  }
  const float reward = BitsToFloat(bits);
  // The collector holds stale bits between grinds, and a NaN there would
  // propagate into script arithmetic silently.
  return std::isfinite(reward) ? reward : 0.0f;
}

void Publish() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_published = g_working;
}

void Finish() {
  if (!g_working.active) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Capped so a run with nothing draining these cannot grow without bound.
    if (g_completed.size() < 32) {
      g_completed.push_back(Completed{g_working.duration_ms,
                                      g_working.distance, g_working.reward});
    }
    g_published = Grind{};
  }
  g_working = Grind{};
}

}  // namespace

void Update() {
  const std::uint32_t module = trick_pipeline::CurrentLocalScoreModule();
  std::uint32_t state = 0;
  const bool readable =
      module != 0 &&
      guest_probe::ReadU32(module + trick::ScoreModuleLayout::kCollectorState,
                           state);
  const bool grinding =
      readable && state == static_cast<std::uint32_t>(
                               trick::ScoreCollectorState::Grind);

  if (!grinding) {
    Finish();
    g_have_last_position = false;
    return;
  }

  float position[3] = {};
  const bool have_position =
      trick_pipeline::CurrentLocalBoardPosition(position);

  if (!g_working.active) {
    g_working = Grind{};
    g_working.active = true;
    g_started_ms = NowMs();
    g_have_last_position = false;
  }

  if (have_position) {
    if (g_have_last_position) {
      const float dx = position[0] - g_last_position[0];
      const float dy = position[1] - g_last_position[1];
      const float dz = position[2] - g_last_position[2];
      const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
      // A teleport or a respawn mid-grind would otherwise add the whole map to
      // the total in one frame. Nothing legitimate moves this far in a frame.
      constexpr float kMaximumStep = 5.0f;
      if (std::isfinite(step) && step < kMaximumStep) {
        g_working.distance += step;
      }
    }
    std::memcpy(g_last_position, position, sizeof(g_last_position));
    g_have_last_position = true;
  }

  const std::int64_t elapsed = NowMs() - g_started_ms;
  g_working.duration_ms =
      elapsed > 0 ? static_cast<std::uint32_t>(elapsed) : 0u;
  g_working.reward = ReadGrindReward(module);
  Publish();
}

Grind Current() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_published;
}

std::vector<Completed> TakeCompleted() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Completed> out;
  out.swap(g_completed);
  return out;
}

}  // namespace skate3::retail_grind
