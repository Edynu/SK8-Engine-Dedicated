#include "skate3_cef_gamepad.h"

#include <algorithm>
#include <cmath>

namespace skate3::cef_gamepad {

namespace {

// XInput's own recommended dead zones. The sticks rest noisily around
// centre; without these the pointer drifts on its own.
constexpr float kLeftStickDeadzone = 7849.0f;
constexpr float kRightStickDeadzone = 8689.0f;
constexpr float kStickMax = 32767.0f;

// Wheel ticks per second at full right-stick deflection. 120 is one
// notch of a physical wheel, so this is roughly ten notches a second.
constexpr float kScrollTicksPerSecond = 1200.0f;

// Maps a raw stick axis pair to a dead-zone-corrected vector in [-1, 1],
// applied radially rather than per-axis so a diagonal push is not faster
// than a straight one and the dead zone stays circular.
bool StickVector(int16_t raw_x, int16_t raw_y, float deadzone, float& out_x,
                 float& out_y) {
  const float fx = static_cast<float>(raw_x);
  const float fy = static_cast<float>(raw_y);
  const float magnitude = std::sqrt(fx * fx + fy * fy);
  if (magnitude <= deadzone) {
    out_x = 0.0f;
    out_y = 0.0f;
    return false;
  }
  const float normalized =
      std::min(1.0f, (magnitude - deadzone) / (kStickMax - deadzone));
  // Squared response: small pushes stay slow enough to land on a button,
  // full deflection still reaches the top speed.
  const float scaled = normalized * normalized;
  out_x = (fx / magnitude) * scaled;
  out_y = (fy / magnitude) * scaled;
  return true;
}

}  // namespace

bool PadIsDrivingPointer(const PadSnapshot& pad) {
  if (!pad.connected) {
    return false;
  }
  constexpr uint16_t kPointerButtons =
      kPadA | kPadB | kPadX | kPadY | kPadDpadUp | kPadDpadDown |
      kPadDpadLeft | kPadDpadRight | kPadLeftShoulder | kPadRightShoulder;
  if ((pad.buttons & kPointerButtons) != 0) {
    return true;
  }
  float x = 0.0f, y = 0.0f;
  if (StickVector(pad.thumb_lx, pad.thumb_ly, kLeftStickDeadzone, x, y)) {
    return true;
  }
  return StickVector(pad.thumb_rx, pad.thumb_ry, kRightStickDeadzone, x, y);
}

void PadCursor::Reset(int width, int height) {
  fx_ = static_cast<float>(width) * 0.5f;
  fy_ = static_cast<float>(height) * 0.5f;
  x_ = static_cast<int>(fx_);
  y_ = static_cast<int>(fy_);
  visible_ = false;
  left_button_down_ = false;
  a_as_enter_down_ = false;
  escape_down_ = false;
  enter_down_ = false;
  tab_down_ = false;
  page_up_down_ = false;
  page_down_down_ = false;
  dpad_up_ = RepeatKey{};
  dpad_down_ = RepeatKey{};
  dpad_left_ = RepeatKey{};
  dpad_right_ = RepeatKey{};
  wheel_accumulator_ = 0.0f;
}

void PadCursor::EmitKeyEdge(const Sink& sink, uint16_t buttons, uint16_t bit,
                            int win_vk, bool& state) {
  const bool down = (buttons & bit) != 0;
  if (down == state) {
    return;
  }
  state = down;
  if (sink.key) {
    sink.key(win_vk, down, cef_input::kModNone);
  }
}

void PadCursor::EmitRepeatKey(const Sink& sink, uint16_t buttons, uint16_t bit,
                              int win_vk, RepeatKey& state, float dt_seconds) {
  const bool down = (buttons & bit) != 0;
  if (!down) {
    if (state.down) {
      state.down = false;
      if (sink.key) {
        sink.key(win_vk, false, cef_input::kModNone);
      }
    }
    state.timer = 0.0f;
    return;
  }
  if (!state.down) {
    state.down = true;
    state.timer = kRepeatDelaySeconds;
    if (sink.key) {
      sink.key(win_vk, true, cef_input::kModNone);
    }
    return;
  }
  state.timer -= dt_seconds;
  while (state.timer <= 0.0f) {
    state.timer += kRepeatIntervalSeconds;
    if (sink.key) {
      // A repeat is a fresh down/up pair rather than a held-down event:
      // Blink advances focus and scrolls on the DOWN edge, so a repeat has
      // to produce a new one.
      sink.key(win_vk, true, cef_input::kModNone);
      sink.key(win_vk, false, cef_input::kModNone);
    }
  }
}

void PadCursor::Update(const PadSnapshot& pad, float dt_seconds, int width,
                       int height, const Sink& sink, bool pointer_enabled) {
  if (!pad.connected) {
    // A pad that vanished mid-gesture must not leave a button stuck down.
    Release(sink);
    return;
  }
  // A stalled or wildly long frame (a hitch, a breakpoint) would otherwise
  // teleport the pointer across the screen.
  dt_seconds = std::clamp(dt_seconds, 0.0f, 0.1f);

  const float max_x = static_cast<float>(std::max(1, width) - 1);
  const float max_y = static_cast<float>(std::max(1, height) - 1);

  if (!pointer_enabled && (visible_ || left_button_down_)) {
    // Focus was taken without a cursor while the pointer was live: let go
    // of anything held and hide it, but keep translating keys below.
    ReleasePointer(sink);
  }

  float stick_x = 0.0f;
  float stick_y = 0.0f;
  StickVector(pad.thumb_lx, pad.thumb_ly, kLeftStickDeadzone, stick_x, stick_y);
  if (pointer_enabled && (stick_x != 0.0f || stick_y != 0.0f)) {
    fx_ += stick_x * kCursorSpeedPixelsPerSecond * dt_seconds;
    // Stick Y is positive UP; screen Y is positive DOWN.
    fy_ -= stick_y * kCursorSpeedPixelsPerSecond * dt_seconds;
    fx_ = std::clamp(fx_, 0.0f, max_x);
    fy_ = std::clamp(fy_, 0.0f, max_y);
  }
  // Clamped again unconditionally: the surface can be resized under us
  // (a resolution change) while the stick is at rest, which would leave
  // the pointer parked outside the new bounds.
  fx_ = std::clamp(fx_, 0.0f, max_x);
  fy_ = std::clamp(fy_, 0.0f, max_y);

  const int new_x = static_cast<int>(fx_);
  const int new_y = static_cast<int>(fy_);
  const bool moved = new_x != x_ || new_y != y_;
  x_ = new_x;
  y_ = new_y;

  uint32_t modifiers = cef_input::kModNone;
  if (left_button_down_) {
    // Blink only treats a move as a selection/drag when the move itself
    // carries the button bit - the same requirement the mouse path has.
    modifiers |= cef_input::kModLeftMouseButton;
  }

  if (pointer_enabled && (!visible_ || moved)) {
    // The very first frame emits a move even without motion, so the page
    // gets a hover state wherever the pointer was centred.
    visible_ = true;
    if (sink.cursor) {
      sink.cursor(x_, y_, true);
    }
    if (sink.mouse_move) {
      sink.mouse_move(x_, y_, modifiers);
    }
  }

  // A: the click, or - with no pointer to click with - Enter, which is
  // what activates whatever the DOM currently has focused.
  if (pointer_enabled) {
    const bool a_down = (pad.buttons & kPadA) != 0;
    if (a_down != left_button_down_) {
      left_button_down_ = a_down;
      if (sink.mouse_button) {
        sink.mouse_button(x_, y_, /*button=*/0, /*mouse_up=*/!a_down,
                          a_down ? (modifiers | cef_input::kModLeftMouseButton)
                                 : cef_input::kModNone,
                          /*click_count=*/1);
      }
    }
  } else {
    EmitKeyEdge(sink, pad.buttons, kPadA, cef_input::kVkReturn,
                a_as_enter_down_);
  }

  // The rest map onto the keys a DOM already knows how to navigate with,
  // so a page written for keyboard accessibility is pad-navigable for free.
  EmitKeyEdge(sink, pad.buttons, kPadB, cef_input::kVkEscape, escape_down_);
  EmitKeyEdge(sink, pad.buttons, kPadX, cef_input::kVkReturn, enter_down_);
  EmitKeyEdge(sink, pad.buttons, kPadY, cef_input::kVkTab, tab_down_);
  EmitKeyEdge(sink, pad.buttons, kPadLeftShoulder, cef_input::kVkPrior,
              page_up_down_);
  EmitKeyEdge(sink, pad.buttons, kPadRightShoulder, cef_input::kVkNext,
              page_down_down_);

  EmitRepeatKey(sink, pad.buttons, kPadDpadUp, cef_input::kVkUp, dpad_up_,
                dt_seconds);
  EmitRepeatKey(sink, pad.buttons, kPadDpadDown, cef_input::kVkDown,
                dpad_down_, dt_seconds);
  EmitRepeatKey(sink, pad.buttons, kPadDpadLeft, cef_input::kVkLeft,
                dpad_left_, dt_seconds);
  EmitRepeatKey(sink, pad.buttons, kPadDpadRight, cef_input::kVkRight,
                dpad_right_, dt_seconds);

  // Right stick scrolls whatever is under the pointer.
  float scroll_x = 0.0f;
  float scroll_y = 0.0f;
  StickVector(pad.thumb_rx, pad.thumb_ry, kRightStickDeadzone, scroll_x,
              scroll_y);
  if (pointer_enabled && scroll_y != 0.0f) {
    wheel_accumulator_ += scroll_y * kScrollTicksPerSecond * dt_seconds;
    const int ticks = static_cast<int>(wheel_accumulator_);
    if (ticks != 0) {
      wheel_accumulator_ -= static_cast<float>(ticks);
      if (sink.mouse_wheel) {
        // Wheel delta is positive when scrolling UP, which is also the
        // stick's own positive direction - no inversion here, unlike the
        // pointer motion above.
        sink.mouse_wheel(x_, y_, 0, ticks, modifiers);
      }
    }
  } else {
    wheel_accumulator_ = 0.0f;
  }
}

void PadCursor::ReleasePointer(const Sink& sink) {
  if (left_button_down_) {
    left_button_down_ = false;
    if (sink.mouse_button) {
      sink.mouse_button(x_, y_, /*button=*/0, /*mouse_up=*/true,
                        cef_input::kModNone, /*click_count=*/1);
    }
  }
  wheel_accumulator_ = 0.0f;
  if (visible_) {
    visible_ = false;
    if (sink.cursor) {
      sink.cursor(x_, y_, false);
    }
  }
}

void PadCursor::Release(const Sink& sink) {
  ReleasePointer(sink);
  const auto release_key = [&sink](bool& state, int win_vk) {
    if (!state) {
      return;
    }
    state = false;
    if (sink.key) {
      sink.key(win_vk, false, cef_input::kModNone);
    }
  };
  release_key(escape_down_, cef_input::kVkEscape);
  release_key(enter_down_, cef_input::kVkReturn);
  release_key(tab_down_, cef_input::kVkTab);
  release_key(page_up_down_, cef_input::kVkPrior);
  release_key(page_down_down_, cef_input::kVkNext);
  release_key(dpad_up_.down, cef_input::kVkUp);
  release_key(dpad_down_.down, cef_input::kVkDown);
  release_key(dpad_left_.down, cef_input::kVkLeft);
  release_key(dpad_right_.down, cef_input::kVkRight);
  release_key(a_as_enter_down_, cef_input::kVkReturn);
}

}  // namespace skate3::cef_gamepad
