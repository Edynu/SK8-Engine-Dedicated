#include "skate3_input_state.h"

#include "skate3_cef_gamepad.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace skate3::input_state {

namespace {

constexpr int kControlCount = static_cast<int>(Control::kCount);

// Past this, an analog trigger counts as pressed. Chosen well above the
// resting noise of a worn trigger and below any deliberate pull.
constexpr uint8_t kTriggerPressThreshold = 96;

// How far the left stick must be pushed before it counts as a digital
// direction. Deliberately well past the noise dead zone the pointer uses:
// these are "the player meant a direction" thresholds, not "the stick is
// off centre", and a skater mid-turn should not trip a menu.
constexpr float kStickDirectionThreshold = 0.6f;

std::mutex g_mutex;
bool g_held[kControlCount] = {};
// Edges accumulated since the last script tick. OR'd rather than
// overwritten, so a press and release inside one tick are both reported.
bool g_pending_pressed[kControlCount] = {};
bool g_pending_released[kControlCount] = {};
// What the current script tick sees. Stable for the whole tick, so every
// script in it agrees about what happened.
bool g_frame_pressed[kControlCount] = {};
bool g_frame_released[kControlCount] = {};
float g_axes[static_cast<int>(Axis::kCount)] = {};
bool g_pad_connected = false;
bool g_keyboard_for_gameplay = true;

std::string Lowered(std::string_view name) {
  std::string out(name);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

const std::unordered_map<std::string, Control>& ControlNames() {
  static const std::unordered_map<std::string, Control> table = {
      {"a", Control::kPadA},
      {"b", Control::kPadB},
      {"x", Control::kPadX},
      {"y", Control::kPadY},
      {"lb", Control::kPadLeftShoulder},
      {"rb", Control::kPadRightShoulder},
      {"lt", Control::kPadLeftTrigger},
      {"rt", Control::kPadRightTrigger},
      {"back", Control::kPadBack},
      {"start", Control::kPadStart},
      {"lstick", Control::kPadLeftThumb},
      {"rstick", Control::kPadRightThumb},
      {"dpadup", Control::kPadDpadUp},
      {"dpaddown", Control::kPadDpadDown},
      {"dpadleft", Control::kPadDpadLeft},
      {"dpadright", Control::kPadDpadRight},
      // Bare direction names mean the D-PAD, never the stick - see the
      // header. The stick has its own names below.
      {"up", Control::kPadDpadUp},
      {"down", Control::kPadDpadDown},
      {"left", Control::kPadDpadLeft},
      {"right", Control::kPadDpadRight},

      {"lstickup", Control::kPadLeftStickUp},
      {"lstickdown", Control::kPadLeftStickDown},
      {"lstickleft", Control::kPadLeftStickLeft},
      {"lstickright", Control::kPadLeftStickRight},

      {"key_space", Control::kKeySpace},
      {"key_enter", Control::kKeyEnter},
      {"key_escape", Control::kKeyEscape},
      {"key_tab", Control::kKeyTab},
      {"key_shift", Control::kKeyShift},
      {"key_ctrl", Control::kKeyControl},
      {"key_alt", Control::kKeyAlt},
      {"key_up", Control::kKeyUp},
      {"key_down", Control::kKeyDown},
      {"key_left", Control::kKeyLeft},
      {"key_right", Control::kKeyRight},
  };
  return table;
}

}  // namespace

Control ControlFromName(std::string_view name) {
  const std::string key = Lowered(name);
  const auto& table = ControlNames();
  const auto found = table.find(key);
  if (found != table.end()) {
    return found->second;
  }
  // key_a .. key_z and key_0 .. key_9, so the table above stays short.
  if (key.size() == 5 && key.rfind("key_", 0) == 0) {
    const char c = key[4];
    if (c >= 'a' && c <= 'z') {
      return static_cast<Control>(static_cast<int>(Control::kKeyA) + (c - 'a'));
    }
    if (c >= '0' && c <= '9') {
      return static_cast<Control>(static_cast<int>(Control::kKey0) + (c - '0'));
    }
  }
  return Control::kNone;
}

Axis AxisFromName(std::string_view name) {
  const std::string key = Lowered(name);
  if (key == "leftx" || key == "lx") return Axis::kLeftStickX;
  if (key == "lefty" || key == "ly") return Axis::kLeftStickY;
  if (key == "rightx" || key == "rx") return Axis::kRightStickX;
  if (key == "righty" || key == "ry") return Axis::kRightStickY;
  if (key == "lt") return Axis::kLeftTrigger;
  if (key == "rt") return Axis::kRightTrigger;
  return Axis::kNone;
}

void Publish(const Sample& sample) {
  bool held[kControlCount] = {};

  if (sample.pad_connected) {
    using namespace cef_gamepad;
    const uint16_t buttons = sample.pad_buttons;
    const auto button = [&](Control control, uint16_t bit) {
      held[static_cast<int>(control)] = (buttons & bit) != 0;
    };
    button(Control::kPadA, kPadA);
    button(Control::kPadB, kPadB);
    button(Control::kPadX, kPadX);
    button(Control::kPadY, kPadY);
    button(Control::kPadLeftShoulder, kPadLeftShoulder);
    button(Control::kPadRightShoulder, kPadRightShoulder);
    button(Control::kPadBack, kPadBack);
    button(Control::kPadStart, kPadStart);
    button(Control::kPadLeftThumb, kPadLeftThumb);
    button(Control::kPadRightThumb, kPadRightThumb);
    button(Control::kPadDpadUp, kPadDpadUp);
    button(Control::kPadDpadDown, kPadDpadDown);
    button(Control::kPadDpadLeft, kPadDpadLeft);
    button(Control::kPadDpadRight, kPadDpadRight);
    held[static_cast<int>(Control::kPadLeftTrigger)] =
        sample.left_trigger >= kTriggerPressThreshold;
    held[static_cast<int>(Control::kPadRightTrigger)] =
        sample.right_trigger >= kTriggerPressThreshold;

    // Left stick as digital directions. Y is inverted relative to the
    // stick's own sign: the stick reads positive UP, which is the opposite
    // of every screen axis in this engine, and getting it backwards here
    // would be invisible until a menu scrolled the wrong way.
    const float stick_x = sample.thumb_lx / 32767.0f;
    const float stick_y = sample.thumb_ly / 32767.0f;
    held[static_cast<int>(Control::kPadLeftStickUp)] =
        stick_y >= kStickDirectionThreshold;
    held[static_cast<int>(Control::kPadLeftStickDown)] =
        stick_y <= -kStickDirectionThreshold;
    held[static_cast<int>(Control::kPadLeftStickRight)] =
        stick_x >= kStickDirectionThreshold;
    held[static_cast<int>(Control::kPadLeftStickLeft)] =
        stick_x <= -kStickDirectionThreshold;
  }

  // The keyboard's D-pad binds, OR'd on top of whatever the real pad said.
  // Gated here rather than at read time: the pad half of these controls must
  // stay readable while an overlay owns the keyboard, so the gate has to
  // apply to this contribution alone.
  if (sample.keyboard_belongs_to_gameplay) {
    static constexpr Control kDpad[4] = {
        Control::kPadDpadUp, Control::kPadDpadDown, Control::kPadDpadLeft,
        Control::kPadDpadRight};
    for (int direction = 0; direction < 4; ++direction) {
      if (sample.keyboard_dpad[direction]) {
        held[static_cast<int>(kDpad[direction])] = true;
      }
    }
  }

  // Keyboard is copied through as sampled; the gameplay-ownership question
  // is answered at read time so a script can still see the flag itself.
  for (int index = static_cast<int>(Control::kKeySpace);
       index < kControlCount; ++index) {
    held[index] = sample.keys[index];
  }

  const float axes[static_cast<int>(Axis::kCount)] = {
      0.0f,
      sample.pad_connected ? sample.thumb_lx / 32767.0f : 0.0f,
      sample.pad_connected ? sample.thumb_ly / 32767.0f : 0.0f,
      sample.pad_connected ? sample.thumb_rx / 32767.0f : 0.0f,
      sample.pad_connected ? sample.thumb_ry / 32767.0f : 0.0f,
      sample.pad_connected ? sample.left_trigger / 255.0f : 0.0f,
      sample.pad_connected ? sample.right_trigger / 255.0f : 0.0f,
  };

  std::lock_guard<std::mutex> lock(g_mutex);
  for (int index = 0; index < kControlCount; ++index) {
    if (held[index] && !g_held[index]) {
      g_pending_pressed[index] = true;
    } else if (!held[index] && g_held[index]) {
      g_pending_released[index] = true;
    }
    g_held[index] = held[index];
  }
  std::memcpy(g_axes, axes, sizeof(axes));
  g_pad_connected = sample.pad_connected;
  g_keyboard_for_gameplay = sample.keyboard_belongs_to_gameplay;
}

void BeginScriptFrame() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::memcpy(g_frame_pressed, g_pending_pressed, sizeof(g_frame_pressed));
  std::memcpy(g_frame_released, g_pending_released, sizeof(g_frame_released));
  std::memset(g_pending_pressed, 0, sizeof(g_pending_pressed));
  std::memset(g_pending_released, 0, sizeof(g_pending_released));
}

namespace {

// A keyboard control read while something else owns the keyboard reports
// nothing - see the header. Pad controls are never gated this way.
bool Readable(Control control) {
  if (control <= Control::kNone || control >= Control::kCount) {
    return false;
  }
  if (control < Control::kKeySpace) {
    return true;  // pad
  }
  return g_keyboard_for_gameplay;
}

}  // namespace

bool IsPressed(Control control) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Readable(control) && g_held[static_cast<int>(control)];
}

bool IsJustPressed(Control control) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Readable(control) && g_frame_pressed[static_cast<int>(control)];
}

bool IsJustReleased(Control control) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Readable(control) && g_frame_released[static_cast<int>(control)];
}

float Value(Axis axis) {
  if (axis <= Axis::kNone || axis >= Axis::kCount) {
    return 0.0f;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_axes[static_cast<int>(axis)];
}

bool PadConnected() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pad_connected;
}

bool ScriptsShouldReadKeyboard() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_keyboard_for_gameplay;
}

}  // namespace skate3::input_state
