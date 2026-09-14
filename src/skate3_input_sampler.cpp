#include "skate3_input_sampler.h"

#include "skate3_input_state.h"

#include <rex/cvar.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <string>
#include <string_view>
#include <utility>

namespace skate3 {

namespace {

// Keyboard controls, in the order skate3_input_state.h declares them, so the
// two lists cannot drift apart silently: the static_assert below fails to
// compile if a control is added to one and not the other.
struct KeyMapping {
  input_state::Control control;
  ImGuiKey key;
};

constexpr KeyMapping kKeyMappings[] = {
    {input_state::Control::kKeySpace, ImGuiKey_Space},
    {input_state::Control::kKeyEnter, ImGuiKey_Enter},
    {input_state::Control::kKeyEscape, ImGuiKey_Escape},
    {input_state::Control::kKeyTab, ImGuiKey_Tab},
    {input_state::Control::kKeyShift, ImGuiKey_LeftShift},
    {input_state::Control::kKeyControl, ImGuiKey_LeftCtrl},
    {input_state::Control::kKeyAlt, ImGuiKey_LeftAlt},
    {input_state::Control::kKeyUp, ImGuiKey_UpArrow},
    {input_state::Control::kKeyDown, ImGuiKey_DownArrow},
    {input_state::Control::kKeyLeft, ImGuiKey_LeftArrow},
    {input_state::Control::kKeyRight, ImGuiKey_RightArrow},
};

// The letter and digit ranges are contiguous in both enums, so they are
// walked rather than listed.
static_assert(static_cast<int>(input_state::Control::kKeyZ) -
                      static_cast<int>(input_state::Control::kKeyA) ==
                  25,
              "letter controls must stay contiguous");
static_assert(static_cast<int>(input_state::Control::kKey9) -
                      static_cast<int>(input_state::Control::kKey0) ==
                  9,
              "digit controls must stay contiguous");

// The keybind cvars the SDK's keyboard/mouse driver reads for the D-pad,
// in up/down/left/right order.
constexpr const char* kDpadBindCvars[4] = {
    "keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left",
    "keybind_dpad_right"};

// Key names as the SDK spells them, translated to the names this module's
// own control table uses. Only the spellings that actually differ are
// listed; anything else works by prefixing "KEY_".
std::string_view TranslateKeyName(std::string_view sdk_name) {
  if (sdk_name == "Return") return "enter";
  if (sdk_name == "Control") return "ctrl";
  if (sdk_name == "Backspace") return "back";
  return {};
}

// Resolves a keybind cvar's CURRENT value to one of this module's keyboard
// controls, so rebinding D-pad up away from the arrow key follows here too.
// Returns kNone for a mouse bind or anything unmappable, which simply means
// the keyboard contributes nothing to that direction.
input_state::Control ControlForBind(const char* cvar_name) {
  const auto* info = rex::cvar::GetFlagInfo(cvar_name);
  if (info == nullptr || !info->getter) {
    return input_state::Control::kNone;
  }
  const std::string bind = info->getter();
  if (bind.empty()) {
    return input_state::Control::kNone;
  }
  const std::string_view translated = TranslateKeyName(bind);
  const std::string name =
      std::string("key_") +
      std::string(translated.empty() ? std::string_view(bind) : translated);
  return input_state::ControlFromName(name);
}

}  // namespace

Skate3InputSamplerDialog::Skate3InputSamplerDialog(
    rex::ui::ImGuiDrawer* drawer,
    std::function<cef_gamepad::PadSnapshot()> poll_gamepad,
    std::function<bool()> keyboard_for_gameplay)
    : ImGuiDialog(drawer),
      poll_gamepad_(std::move(poll_gamepad)),
      keyboard_for_gameplay_(std::move(keyboard_for_gameplay)) {}

Skate3InputSamplerDialog::~Skate3InputSamplerDialog() = default;

void Skate3InputSamplerDialog::OnDraw(ImGuiIO& io) {
  input_state::Sample sample;

  if (poll_gamepad_) {
    const cef_gamepad::PadSnapshot pad = poll_gamepad_();
    sample.pad_connected = pad.connected;
    sample.pad_buttons = pad.buttons;
    sample.thumb_lx = pad.thumb_lx;
    sample.thumb_ly = pad.thumb_ly;
    sample.thumb_rx = pad.thumb_rx;
    sample.thumb_ry = pad.thumb_ry;
    sample.left_trigger = pad.left_trigger;
    sample.right_trigger = pad.right_trigger;
  }

  for (const KeyMapping& mapping : kKeyMappings) {
    sample.keys[static_cast<int>(mapping.control)] =
        ImGui::IsKeyDown(mapping.key);
  }
  for (int offset = 0; offset <= 25; ++offset) {
    sample.keys[static_cast<int>(input_state::Control::kKeyA) + offset] =
        ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_A + offset));
  }
  for (int offset = 0; offset <= 9; ++offset) {
    sample.keys[static_cast<int>(input_state::Control::kKey0) + offset] =
        ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_0 + offset));
  }

  // The D-pad's keyboard half. Resolved every frame rather than cached
  // because a keybind cvar can change at runtime from the settings screen,
  // and a stale binding would be a control that silently stopped working.
  for (int direction = 0; direction < 4; ++direction) {
    const input_state::Control bound = ControlForBind(kDpadBindCvars[direction]);
    sample.keyboard_dpad[direction] =
        bound != input_state::Control::kNone &&
        sample.keys[static_cast<int>(bound)];
  }

  // ImGui claiming the keyboard is itself a signal: it means a text field
  // somewhere has focus, which is exactly when a script must not be reading
  // keys. OR'd with the app's own view of who owns input.
  sample.keyboard_belongs_to_gameplay =
      !io.WantCaptureKeyboard &&
      (!keyboard_for_gameplay_ || keyboard_for_gameplay_());

  input_state::Publish(sample);
}

}  // namespace skate3
