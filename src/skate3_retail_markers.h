#pragma once

// Retail's session marker.
//
// WHAT IT TURNED OUT TO BE, because it constrains any API built on it: a
// SINGLETON. Reading the bindings' recompiled bodies (generated/, grep
// DEFINE_REX_FUNC(sub_825AE748) and neighbours) shows all four resolve the same
// root as the score block - 0x83067060 - at slot +4, and the whole of
// "is there a marker" is ONE BYTE at +12:
//
//   SetSessionMarker      (0x825AE748)  *(u8*)(marker + 12) = 1, plus two calls
//   RemoveSessionMarker   (0x825AE788)  *(u8*)(marker + 12) = 0, plus the same
//   IsActiveSessionMarker (0x825AE7C8)  returns *(u8*)(marker + 12)
//   IsOverMarker          (0x825AE5F0)  calls sub_82508D88, which walks a
//                                       collection - NOT a stored flag
//
// Three consequences, all of which limit what can be offered:
//
//   1. There is exactly one marker. A CreateMarker/DestroyMarker API returning
//      handles for many cannot be backed by this.
//   2. It carries no type and no text. Those are ours to draw either way.
//   3. SetSessionMarker takes NO POSITION. It marks wherever the player is
//      standing, so a server cannot place one at arbitrary coordinates through
//      it. Where the position is actually stored is not visible statically and
//      needs a runtime dump of the block - which is what BlockAddress is for.
//
// Only the read is implemented here. Set and Remove additionally call
// sub_824AD240 and sub_82D0AC88 (HUD/minimap refresh, by their neighbours), so
// they cannot be faked by writing the byte: the marker would exist without
// anything drawing it. They need a real guest-thread call, which is the
// flash-bridge pattern generalised, and is deliberately not started here.

#include <cstdint>

namespace skate3::retail_markers {

// Whether retail currently has a session marker placed. nullopt when the
// marker block is not resolvable - the front end, or mid-load - which is a
// normal state rather than an error.
struct SessionMarkerState {
  bool valid = false;
  bool active = false;
};

[[nodiscard]] SessionMarkerState ReadSessionMarker();

// The resolved marker block address, or 0. The position fields are in here
// somewhere; dumping this at runtime while standing at a known spot is how they
// get found, and that is a cheaper experiment than any amount of further
// static reading.
[[nodiscard]] std::uint32_t BlockAddress();

}  // namespace skate3::retail_markers
