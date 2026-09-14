#pragma once

// Input vocabulary shared by every CEF-backed overlay (the F7 dev console
// and the NUI layer). Deliberately free of CEF headers so the input
// forwarding code, the dialogs, and the non-CEF stub builds can all speak
// the same enum without the SDK present.

#include <cstdint>

namespace skate3::cef_input {

// Event modifier bits. Values mirror CEF's own cef_event_flags_t (see the
// CEF SDK's include/internal/cef_types.h); duplicated here so this header
// stays compilable in a build without the CEF SDK, and static_assert'd
// against the real values in skate3_cef_console.cpp.
//
// These matter more than they look: Blink only treats a mouse-move as a
// text-selection DRAG when the move carries the left-button bit, and only
// runs clipboard shortcuts when the key event carries the control bit.
enum ModifierFlags : uint32_t {
  kModNone = 0,
  kModShift = 1u << 1,
  kModControl = 1u << 2,
  kModAlt = 1u << 3,
  kModLeftMouseButton = 1u << 4,
  kModMiddleMouseButton = 1u << 5,
  kModRightMouseButton = 1u << 6,
};

// Windows virtual-key codes the overlays forward as raw key events (as
// opposed to typed characters, which go through the character queue).
// Spelled out rather than pulled from <windows.h> because this header is
// included by rex-free, CEF-free translation units.
enum VirtualKey : int {
  kVkBack = 0x08,
  kVkTab = 0x09,
  kVkReturn = 0x0D,
  kVkEscape = 0x1B,
  kVkPrior = 0x21,  // Page Up
  kVkNext = 0x22,   // Page Down
  kVkEnd = 0x23,
  kVkHome = 0x24,
  kVkLeft = 0x25,
  kVkUp = 0x26,
  kVkRight = 0x27,
  kVkDown = 0x28,
  kVkDelete = 0x2E,
};

}  // namespace skate3::cef_input
