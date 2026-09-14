#pragma once

// Grinds: whether the player is on one, and how long it has run.
//
// WHERE "IS GRINDING" COMES FROM. Retail's ScoreModule owns a set of typed
// collectors - air, grind, ground, handplant, offboard - and a word saying
// which is currently live (`ScoreModuleLayout::kCollectorState`, matching
// `ScoreCollectorState`). A grind is that word reading `Grind`. The module
// pointer for the local player is already tracked
// (`trick_pipeline::CurrentLocalScoreModule`), so this needs no new hook into
// retail at all - it is a memory read, like the score.
//
// WHERE "HOW LONG" COMES FROM: us, not retail. Duration and distance are not
// fields anywhere in the collector layouts, and there is no reason to expect
// them to be - retail only ever needed the reward. Measuring them here is both
// easier and more trustworthy than hunting for a field that may not exist:
// duration is a clock, and distance is the path the board actually travelled
// while the collector was live, integrated per frame rather than taken as a
// straight line from start to end, so a grind round a curved rail measures the
// rail rather than the chord.

#include <cstdint>
#include <vector>

namespace skate3::retail_grind {

// The grind in progress, if any.
struct Grind {
  bool active = false;
  // Milliseconds since the grind collector went live.
  std::uint32_t duration_ms = 0;
  // World units travelled along the grind, integrated per frame.
  float distance = 0.0f;
  // Retail's own accumulating grind reward (GrindCollectorLayout::
  // kCurrentReward). Distinct from length: a long slow grind and a short fast
  // one can score very differently.
  float reward = 0.0f;
};

[[nodiscard]] Grind Current();

// A grind that has just finished. Drained rather than polled so a script
// cannot miss one that started and ended between two of its own frames.
struct Completed {
  std::uint32_t duration_ms = 0;
  float distance = 0.0f;
  float reward = 0.0f;
};
[[nodiscard]] std::vector<Completed> TakeCompleted();

// Once per frame, on the app thread.
void Update();

}  // namespace skate3::retail_grind
