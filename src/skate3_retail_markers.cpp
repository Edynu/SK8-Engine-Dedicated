#include "skate3_retail_markers.h"

#include "skate3_guest_probe.h"

namespace skate3::retail_markers {

namespace {

// Same root the score block hangs off (see skate3_retail_score.cpp) at a
// different slot. Worth noting rather than sharing a constant: that they are the
// same global is an observation about this build, not a guarantee, and two
// independent constants fail independently if one moves.
constexpr std::uint32_t kMarkerRoot = 0x83067060u;
constexpr std::uint32_t kBlockPointerOffset = 4u;

// The whole of "a marker exists", as one byte.
constexpr std::uint32_t kActiveFlag = 12u;

[[nodiscard]] bool ResolveBlock(std::uint32_t& block) {
  if (!guest_probe::ReadU32(kMarkerRoot + kBlockPointerOffset, block)) {
    return false;
  }
  return block != 0;
}

}  // namespace

SessionMarkerState ReadSessionMarker() {
  SessionMarkerState state;
  std::uint32_t block = 0;
  if (!ResolveBlock(block)) {
    return state;
  }
  // A byte, read through the 32-bit accessor because that is what guest_probe
  // offers. The flag is at +12, so the word at +12 holds it in its MOST
  // significant byte: ReadU32 has already swapped from the guest's big-endian
  // order, which puts the lowest guest address in the high bits.
  std::uint32_t word = 0;
  if (!guest_probe::ReadU32(block + kActiveFlag, word)) {
    return state;
  }
  state.valid = true;
  state.active = ((word >> 24) & 0xFFu) != 0u;
  return state;
}

std::uint32_t BlockAddress() {
  std::uint32_t block = 0;
  return ResolveBlock(block) ? block : 0u;
}

}  // namespace skate3::retail_markers
