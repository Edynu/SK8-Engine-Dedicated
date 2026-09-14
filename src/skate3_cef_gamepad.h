#pragma once

// Gamepad -> CEF input translation.
//
// Skate 3 is a controller game: there is frequently no mouse in the
// player's hands at all, and the game hides the OS cursor during normal
// play. A browser surface, though, only understands mouse and keyboard. So
// a pad drives a VIRTUAL pointer here - the left stick moves it, A clicks
// with it, and the face/shoulder buttons map onto the keys a DOM uses for
// navigation. The pointer itself is drawn inside the page (see
// cef_nui::SetVirtualCursor), because an off-screen browser composited over
// the guest output has no OS cursor of its own to show.
//
// Deliberately free of both CEF and rex: it takes a plain pad snapshot and
// emits through a sink of callbacks, so the dev console and the NUI layer
// can each point it at their own browser, and it stays testable without
// either subsystem present.

#include "skate3_cef_input.h"

#include <cstdint>
#include <functional>

namespace skate3::cef_gamepad {

// XInput's own button bits (XINPUT_GAMEPAD_* from XInput.h). Spelled out
// rather than included so this header stays platform-header-free; the
// values are part of the XInput ABI and do not change.
enum PadButton : uint16_t {
  kPadDpadUp = 0x0001,
  kPadDpadDown = 0x0002,
  kPadDpadLeft = 0x0004,
  kPadDpadRight = 0x0008,
  kPadStart = 0x0010,
  kPadBack = 0x0020,
  kPadLeftThumb = 0x0040,
  kPadRightThumb = 0x0080,
  kPadLeftShoulder = 0x0100,
  kPadRightShoulder = 0x0200,
  kPadA = 0x1000,
  kPadB = 0x2000,
  kPadX = 0x4000,
  kPadY = 0x8000,
};

// A pad's state for one frame, already unpacked from whatever the input
// system hands out (X_INPUT_GAMEPAD stores its fields big-endian).
struct PadSnapshot {
  bool connected = false;
  uint16_t buttons = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
};

// Where translated events go. Coordinates are in the target browser's own
// pixel space. `cursor` may be left empty by a surface that does not draw a
// pointer (the dev console, which is only reachable from a keyboard
// anyway); everything else must be set.
struct Sink {
  std::function<void(int x, int y, uint32_t modifiers)> mouse_move;
  std::function<void(int x, int y, int button, bool mouse_up,
                     uint32_t modifiers, int click_count)>
      mouse_button;
  std::function<void(int x, int y, int delta_x, int delta_y,
                     uint32_t modifiers)>
      mouse_wheel;
  std::function<void(int win_vk, bool is_down, uint32_t modifiers)> key;
  std::function<void(int x, int y, bool visible)> cursor;
};

// How fast the pointer travels at full stick deflection, in browser pixels
// per second. Sized so crossing a 1080p screen takes a bit under a second -
// fast enough not to feel like wading, slow enough to hit a button.
inline constexpr float kCursorSpeedPixelsPerSecond = 1500.0f;

// True when the pad is actively driving a pointer this frame: the left
// stick is past its dead zone, or a button that means something to a page
// is held. Callers use this to decide WHICH device owns the pointer - a pad
// and a mouse both pushing positions into the same browser every frame make
// hover flicker between two places and land clicks wherever the other
// device last pointed.
bool PadIsDrivingPointer(const PadSnapshot& pad);

class PadCursor {
 public:
  // Centres the pointer in a `width` x `height` surface and forgets all
  // held state. Call when the surface gains focus, so every session starts
  // from a predictable place rather than wherever the last one ended.
  void Reset(int width, int height);

  // Translates one frame of pad state. `dt_seconds` is the real elapsed
  // time since the previous call, which is what makes pointer speed
  // frame-rate independent. Emits at most: one move, the button/key edges
  // that actually changed, and one wheel event.
  //
  // `pointer_enabled` is SetNuiFocus's own second argument: false means the
  // script asked for focus WITHOUT a cursor, so the pointer stays hidden
  // and no mouse events are produced at all. The pad still navigates - the
  // D-pad drives arrows, and A becomes Enter rather than a click, since
  // there is nothing to click with.
  void Update(const PadSnapshot& pad, float dt_seconds, int width, int height,
              const Sink& sink, bool pointer_enabled);

  // Releases anything still held (the A-button click, a repeating arrow)
  // and hides the pointer. Call when the surface loses focus - without it
  // Blink is left believing a button is still down, and the next session
  // starts with a phantom drag.
  void Release(const Sink& sink);

  int x() const { return x_; }
  int y() const { return y_; }

 private:
  // Auto-repeat for the D-pad, matching a keyboard's own feel: one event
  // on press, then a pause, then a steady stream while held.
  static constexpr float kRepeatDelaySeconds = 0.40f;
  static constexpr float kRepeatIntervalSeconds = 0.06f;

  struct RepeatKey {
    bool down = false;
    float timer = 0.0f;
  };

  // The pointer half of Release(): drops a held click, stops the scroll
  // accumulator and hides the cursor, without touching the key state.
  // Update() uses it on its own when focus turns out to be cursor-less.
  void ReleasePointer(const Sink& sink);
  void EmitKeyEdge(const Sink& sink, uint16_t buttons, uint16_t bit,
                   int win_vk, bool& state);
  void EmitRepeatKey(const Sink& sink, uint16_t buttons, uint16_t bit,
                     int win_vk, RepeatKey& state, float dt_seconds);

  float fx_ = 0.0f;  // sub-pixel pointer position; x_/y_ are these rounded.
  float fy_ = 0.0f;
  int x_ = 0;
  int y_ = 0;
  bool visible_ = false;

  bool left_button_down_ = false;
  // A's alternate role while the pointer is disabled: Enter, tracked
  // separately so switching modes mid-hold cannot leave both stuck.
  bool a_as_enter_down_ = false;
  bool escape_down_ = false;
  bool enter_down_ = false;
  bool tab_down_ = false;
  bool page_up_down_ = false;
  bool page_down_down_ = false;
  RepeatKey dpad_up_;
  RepeatKey dpad_down_;
  RepeatKey dpad_left_;
  RepeatKey dpad_right_;
  // Right-stick scrolling accumulates sub-wheel-tick motion so a gentle
  // push still scrolls, just slowly, instead of rounding to nothing.
  float wheel_accumulator_ = 0.0f;
};

}  // namespace skate3::cef_gamepad
