#pragma once

// Retail's live score state, read straight out of guest memory.
//
// WHY IT IS READ AND NOT CALLED. The front end reaches these numbers through
// named bindings - GetCurrentSequenceScore (0x825C2888), GetSequenceMultiplier
// (0x825C2950), GetMomentumScore (0x825C28E0), GetLineScore (0x825C28F8) - and
// calling those was the obvious first plan. It is the wrong plan, for two
// reasons that only became clear from reading the recompiled bodies:
//
//   1. They do not return their value to the caller. Every one of them
//      tail-calls a Flash return-value helper - sub_82E86550 for an integer,
//      sub_82E864F8 for a string - which publishes the result into the
//      ActionScript variant the UI is asking through. A direct call would run
//      the body and leave r3 holding whatever that helper returned.
//      GetSequenceMultiplier is worse than useless that way: it compares the
//      float against constants and returns a DISPLAY STRING, not a number.
//   2. Calling into retail at all has to be marshalled onto the guest thread
//      at a safe point (see skate3_flash_bridge.cpp), because the callee needs
//      a live guest stack. Reading memory needs none of that and can happen on
//      any thread, at any time, without the chance of reentering retail.
//
// So the binding bodies were used as documentation rather than as an entry
// point. Each one resolves the same root and reads a fixed offset, and those
// offsets are what this file encodes. Derived from the recompiled sources in
// generated/ (grep DEFINE_REX_FUNC(sub_825C2888) and its neighbours), which is
// this exact build.

#include <cstdint>

namespace skate3::retail_score {

// One consistent snapshot.
//
// Deliberately one struct and one read rather than a native per field: a game
// mode that fetched the multiplier and the score separately could sample them
// either side of a landing and score a trick at the wrong multiplier. Reading
// them together makes that impossible to write by accident.
struct Snapshot {
  // False when the score block is not resolvable - in the front end, mid-load,
  // or before the player has spawned. Every other field is then zero. This is
  // the normal state for a good fraction of a session, not an error.
  bool valid = false;

  // The 1x-4x sequence multiplier, as a float (retail stores it as one; the
  // HUD's "2x" is that float run through a formatter).
  float multiplier = 0.0f;

  // The trick sequence in progress. Readable MID-TRICK: this is the running
  // total before the trick has landed or failed, which is what makes it worth
  // exposing at all.
  std::uint32_t sequence_score = 0;

  std::uint32_t momentum_score = 0;
  std::uint32_t line_score = 0;
};

[[nodiscard]] Snapshot Read();

// The resolved score block address, or 0. For probing from Lua when a field
// here turns out to be the wrong one - the block plainly holds more than the
// four values the front end happens to ask for.
[[nodiscard]] std::uint32_t BlockAddress();

}  // namespace skate3::retail_score
