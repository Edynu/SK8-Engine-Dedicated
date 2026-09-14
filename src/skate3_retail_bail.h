#pragma once

// Bails and recoveries, as events.
//
// WHY THIS IS NOT JUST IsOffBoard. A skater is off the board for two very
// different reasons - they crashed, or they stepped off and walked away - and
// a game mode cares enormously which. IsOffBoard cannot tell them apart, so a
// mode built on it counts walking to the spawn point as a failed attempt.
//
// HOW THEY ARE TOLD APART. Two independent signals, neither sufficient alone:
//
//   ScoreModule's collector state reading Offboard says the skater is not on
//   the board. Edge-triggered, so it gives the MOMENT it happens rather than a
//   predicate that has to be polled.
//
//   PhysicalPlayerHiLOD::IsWipeoutRequested says retail wants the skater on
//   the floor. Retail polls it several times a frame while a wipeout runs, so
//   the counter's RATE is the signal, not its value
//   (trick_pipeline::CurrentLocalWipeoutRequests).
//
// A transition to off-board with wipeout requests arriving around it is a
// bail. The same transition with none is someone stepping off.
//
// Kept separate from skate3_retail_grind.{h,cpp} even though both read the
// collector state: they are independent concerns with independent failure
// modes, and merging them would mean one wrong offset breaking both.

#include <cstdint>
#include <vector>

namespace skate3::retail_bail {

// Current state, for a mode that wants to poll rather than listen.
struct State {
  // Off the board right now, for any reason.
  bool off_board = false;
  // Off the board because of a crash rather than a choice.
  bool bailed = false;
  // Milliseconds since the bail began; 0 when not bailed.
  std::uint32_t duration_ms = 0;
};

[[nodiscard]] State Current();

// Edge events, drained rather than polled so one that starts and ends between
// two script frames is still reported.
enum class EventKind : std::uint8_t {
  kBailed,     // crashed off the board
  kSteppedOff, // left the board deliberately
  kRecovered,  // back on the board
};

struct Event {
  EventKind kind = EventKind::kBailed;
  // For kRecovered: how long the player spent off the board.
  std::uint32_t duration_ms = 0;
};

[[nodiscard]] std::vector<Event> TakeEvents();

// Once per frame, on the app thread.
void Update();

}  // namespace skate3::retail_bail
