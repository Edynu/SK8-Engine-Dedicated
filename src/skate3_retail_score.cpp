#include "skate3_retail_score.h"

#include "skate3_guest_probe.h"

#include <cstring>

namespace skate3::retail_score {

namespace {

// All four bindings resolve the same way:
//
//   lis  r11, -31994        ; 0x83060000
//   addi r10, r11, 28768    ; 0x83067060
//   lwz  r11, 16(r10)       ; the score block
//   lwz  r3,  <offset>(r11) ; the value
//
// so the root is a fixed global holding a pointer at +16.
constexpr std::uint32_t kScoreRoot = 0x83067060u;
constexpr std::uint32_t kBlockPointerOffset = 16u;

// Offsets within the block, each taken from the binding that reads it.
constexpr std::uint32_t kMultiplier = 140u;      // GetSequenceMultiplier
constexpr std::uint32_t kSequenceScore = 144u;   // GetCurrentSequenceScore
constexpr std::uint32_t kMomentumScore = 176u;   // GetMomentumScore
constexpr std::uint32_t kLineScore = 192u;       // GetLineScore

// Guest floats are stored big-endian, which ReadU32 has already corrected for,
// so what is left is a bit pattern in host order to reinterpret. memcpy rather
// than a pointer cast because the cast is undefined behaviour and this is
// exactly the kind of place a compiler is entitled to surprise you.
[[nodiscard]] float BitsToFloat(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

[[nodiscard]] bool ResolveBlock(std::uint32_t& block) {
  // A null or unreadable pointer here is the ordinary "no score right now"
  // case, not a failure worth logging: it is what the front end and the load
  // screens look like.
  if (!guest_probe::ReadU32(kScoreRoot + kBlockPointerOffset, block)) {
    return false;
  }
  return block != 0;
}

}  // namespace

Snapshot Read() {
  Snapshot snapshot;
  std::uint32_t block = 0;
  if (!ResolveBlock(block)) {
    return snapshot;
  }

  std::uint32_t multiplier_bits = 0;
  std::uint32_t sequence = 0;
  std::uint32_t momentum = 0;
  std::uint32_t line = 0;
  // All four or none. A partial snapshot would be worse than no snapshot: a
  // caller cannot tell which fields it can trust, and the whole point of
  // reading them together is that they agree with each other.
  if (!guest_probe::ReadU32(block + kMultiplier, multiplier_bits) ||
      !guest_probe::ReadU32(block + kSequenceScore, sequence) ||
      !guest_probe::ReadU32(block + kMomentumScore, momentum) ||
      !guest_probe::ReadU32(block + kLineScore, line)) {
    return snapshot;
  }

  snapshot.valid = true;
  snapshot.multiplier = BitsToFloat(multiplier_bits);
  snapshot.sequence_score = sequence;
  snapshot.momentum_score = momentum;
  snapshot.line_score = line;
  return snapshot;
}

std::uint32_t BlockAddress() {
  std::uint32_t block = 0;
  return ResolveBlock(block) ? block : 0u;
}

}  // namespace skate3::retail_score
