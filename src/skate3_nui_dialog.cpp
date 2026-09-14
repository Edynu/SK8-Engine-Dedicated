#include "skate3_nui_dialog.h"

#include "skate3_cef_nui.h"
#include "skate3_native_scene.h"

#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <utility>

namespace skate3 {

namespace {

// Non-character keys a page's own JS and Blink's focus handling need
// directly. Regular typing goes through io.InputQueueCharacters instead
// (already layout/IME-resolved by ImGui's platform backend), so this table
// deliberately stays short rather than mapping the whole keyboard.
struct KeyMapping {
  ImGuiKey imgui_key;
  int windows_vk;
};
constexpr KeyMapping kKeyMappings[] = {
    {ImGuiKey_Enter, cef_input::kVkReturn},
    {ImGuiKey_KeypadEnter, cef_input::kVkReturn},
    {ImGuiKey_Backspace, cef_input::kVkBack},
    {ImGuiKey_Tab, cef_input::kVkTab},
    {ImGuiKey_Escape, cef_input::kVkEscape},
    {ImGuiKey_Delete, cef_input::kVkDelete},
    {ImGuiKey_Home, cef_input::kVkHome},
    {ImGuiKey_End, cef_input::kVkEnd},
    {ImGuiKey_PageUp, cef_input::kVkPrior},
    {ImGuiKey_PageDown, cef_input::kVkNext},
    {ImGuiKey_UpArrow, cef_input::kVkUp},
    {ImGuiKey_DownArrow, cef_input::kVkDown},
    {ImGuiKey_LeftArrow, cef_input::kVkLeft},
    {ImGuiKey_RightArrow, cef_input::kVkRight},
};

// ImGui orders its buttons left/right/middle; the CEF wrappers take
// left/middle/right (matching CEF itself). Map rather than pass through, or
// right-clicks arrive as middle-clicks.
constexpr int kImGuiToCefButton[3] = {0, 2, 1};

uint32_t ModifiersFromIo(const ImGuiIO& io) {
  uint32_t modifiers = cef_input::kModNone;
  if (io.KeyShift) modifiers |= cef_input::kModShift;
  if (io.KeyCtrl) modifiers |= cef_input::kModControl;
  if (io.KeyAlt) modifiers |= cef_input::kModAlt;
  if (io.MouseDown[0]) modifiers |= cef_input::kModLeftMouseButton;
  if (io.MouseDown[1]) modifiers |= cef_input::kModRightMouseButton;
  if (io.MouseDown[2]) modifiers |= cef_input::kModMiddleMouseButton;
  return modifiers;
}

}  // namespace

Skate3NuiDialog::Skate3NuiDialog(
    rex::ui::ImGuiDrawer* drawer,
    std::function<cef_gamepad::PadSnapshot()> poll_gamepad,
    std::function<void(bool wants_cursor)> on_cursor_mode_changed)
    : ImGuiDialog(drawer),
      poll_gamepad_(std::move(poll_gamepad)),
      on_cursor_mode_changed_(std::move(on_cursor_mode_changed)) {
  pad_sink_.mouse_move = [](int x, int y, uint32_t modifiers) {
    cef_nui::SendMouseMove(x, y, modifiers);
  };
  pad_sink_.mouse_button = [](int x, int y, int button, bool mouse_up,
                              uint32_t modifiers, int click_count) {
    cef_nui::SendMouseButton(x, y, button, mouse_up, modifiers, click_count);
  };
  pad_sink_.mouse_wheel = [](int x, int y, int delta_x, int delta_y,
                             uint32_t modifiers) {
    // Blink's wheel delta is in pixels, not notches: one notch is 120.
    cef_nui::SendMouseWheel(x, y, delta_x * 120, delta_y * 120, modifiers);
  };
  pad_sink_.key = [](int win_vk, bool is_down, uint32_t modifiers) {
    cef_nui::SendKeyEvent(win_vk, is_down, /*is_char=*/false, 0, modifiers);
  };
  pad_sink_.cursor = [](int x, int y, bool visible) {
    cef_nui::SetVirtualCursor(x, y, visible);
  };
}

Skate3NuiDialog::~Skate3NuiDialog() = default;

bool Skate3NuiDialog::WantsContinuousRepaint() const {
  return cef_nui::HasFocus() || cef_nui::HasContent();
}

void Skate3NuiDialog::OnDraw(ImGuiIO& io) {
  // Publish visibility first, unconditionally: a resource can publish or
  // drop its page at any time, and the render pass reads this to decide
  // whether NUI is composited at all. The post-processor arming inside
  // SetNuiVisible is sticky (PostProcessGuestOutput only disarms itself
  // explicitly), so this only needs to fire on the transition.
  const bool has_content = cef_nui::HasContent();
  if (has_content != had_content_) {
    had_content_ = has_content;
    native_scene::SetNuiVisible(has_content);
  }

  // SetNuiFocus/SetNuiFocusKeepInput are called from Lua, on whatever
  // thread the script runs on. Reconciled against local mirrors every
  // frame rather than gated on a change notification: three bool compares
  // cost nothing, and there is then no way for a state change to be missed.
  const bool focused = cef_nui::HasFocus();
  const bool wants_cursor = focused && cef_nui::HasCursor();
  if (focused != focused_) {
    focused_ = focused;
    if (focused) {
      int width = 0, height = 0;
      cef_nui::CurrentSize(width, height);
      pad_cursor_.Reset(width, height);
    } else {
      // Without this the browser is left believing a pad button is still
      // held, and the next focus starts mid-drag.
      pad_cursor_.Release(pad_sink_);
      for (bool& down : mouse_button_down_) {
        down = false;
      }
    }
  }
  if (wants_cursor != cursor_mode_applied_) {
    cursor_mode_applied_ = wants_cursor;
    if (on_cursor_mode_changed_) {
      on_cursor_mode_changed_(wants_cursor);
    }
  }

  if (!focused) {
    return;
  }

  int browser_w = 0, browser_h = 0;
  cef_nui::CurrentSize(browser_w, browser_h);
  if (browser_w <= 0 || browser_h <= 0) {
    return;
  }

  // Decide who owns the pointer this frame. A real mouse movement claims
  // it; any pad activity claims it back. Ties go to the pad, since the pad
  // only reports activity on deliberate input while io.MousePos can jitter.
  const bool mouse_moved = io.MousePos.x != last_mouse_x_ ||
                           io.MousePos.y != last_mouse_y_;
  last_mouse_x_ = io.MousePos.x;
  last_mouse_y_ = io.MousePos.y;
  const cef_gamepad::PadSnapshot pad =
      poll_gamepad_ ? poll_gamepad_() : cef_gamepad::PadSnapshot{};
  if (mouse_moved) {
    pointer_owner_ = PointerOwner::kMouse;
  }
  if (cef_gamepad::PadIsDrivingPointer(pad)) {
    pointer_owner_ = PointerOwner::kPad;
  }

  // The pad drives the page only when the page actually owns the pad.
  // Under SetNuiFocusKeepInput the skater is still riding on the same
  // sticks and buttons, so a pointer drifting with the left stick (and an
  // A press that both ollies and clicks) is exactly wrong - the page stays
  // mouse-and-keyboard-driven there, which is what a HUD wants anyway.
  const bool pad_drives_page =
      !cef_nui::KeepInput() && pointer_owner_ != PointerOwner::kMouse;
  if (poll_gamepad_ && pad_drives_page) {
    pad_cursor_.Update(pad, io.DeltaTime, browser_w, browser_h, pad_sink_,
                       /*pointer_enabled=*/wants_cursor);
  } else {
    // Hides the pointer and drops anything the pad was holding. Cheap to
    // repeat: Release is a no-op once everything is already released.
    pad_cursor_.Release(pad_sink_);
  }
  // SetNuiFocus(true, false) is the "keyboard focus, no cursor" form: the
  // page still receives typing and navigation keys, but nothing pointer-
  // shaped, so a mouse left resting over the screen cannot hover or click
  // things the script did not intend to expose.
  if (wants_cursor && pointer_owner_ != PointerOwner::kPad) {
    ForwardMouse(io, browser_w, browser_h);
  }
  ForwardKeyboard(io);
}

void Skate3NuiDialog::ForwardMouse(ImGuiIO& io, int browser_w, int browser_h) {
  // io.MousePos is in OS window pixels, which differ from guest_output's
  // pixel size - the space NUI's full-screen rect is defined in - whenever
  // DLSS or resolution_scale/draw_resolution_scale_x/y make the internal
  // render resolution not match the window 1:1.
  uint32_t guest_w = 0, guest_h = 0;
  native_scene::GetLastNuiGuestOutputSize(guest_w, guest_h);
  const float scale_x = (guest_w != 0 && io.DisplaySize.x > 0.0f)
                            ? static_cast<float>(guest_w) / io.DisplaySize.x
                            : 1.0f;
  const float scale_y = (guest_h != 0 && io.DisplaySize.y > 0.0f)
                            ? static_cast<float>(guest_h) / io.DisplaySize.y
                            : 1.0f;
  const int browser_x = static_cast<int>(io.MousePos.x * scale_x);
  const int browser_y = static_cast<int>(io.MousePos.y * scale_y);
  if (browser_x < 0 || browser_y < 0 || browser_x >= browser_w ||
      browser_y >= browser_h) {
      return;  // pointer is outside the window entirely.
  }

  const uint32_t modifiers = ModifiersFromIo(io);
  cef_nui::SendMouseMove(browser_x, browser_y, modifiers);

  for (int imgui_button = 0; imgui_button < 3; ++imgui_button) {
    const bool held = io.MouseDown[imgui_button];
    if (held == mouse_button_down_[imgui_button]) {
      continue;
    }
    mouse_button_down_[imgui_button] = held;
    if (held) {
      // An OSR browser never gains or loses window focus on its own, so a
      // click on a previously-blurred element (after Escape, or after the
      // dev console took focus) only works if focus is re-asserted here.
      // Same fix the dev console needed.
      // Re-asserting focus must not change WHICH resource owns pointer
      // input, so this deliberately does not go through cef_nui::SetFocus.
      cef_nui::ReassertBrowserFocus();
    }
    // Blink derives word-select (double) and line-select (triple) from the
    // click count, not from the raw event stream.
    const int click_count =
        (held && imgui_button == 0 && io.MouseDoubleClicked[0]) ? 2 : 1;
    cef_nui::SendMouseButton(browser_x, browser_y,
                             kImGuiToCefButton[imgui_button], !held, modifiers,
                             click_count);
  }
  if (io.MouseWheel != 0.0f) {
    cef_nui::SendMouseWheel(browser_x, browser_y, 0,
                            static_cast<int>(io.MouseWheel * 100.0f),
                            modifiers);
  }
}

void Skate3NuiDialog::ForwardKeyboard(ImGuiIO& io) {
  const uint32_t modifiers = ModifiersFromIo(io);
  // Typed characters - already fully resolved by ImGui's own platform input
  // backend (layout/IME-aware). Skipped while Ctrl/Alt is held: those
  // combinations arrive here as control codepoints (Ctrl+V is 0x16), which
  // would insert garbage on top of the real shortcut handled below.
  if (!io.KeyCtrl && !io.KeyAlt) {
    for (const ImWchar c : io.InputQueueCharacters) {
      if (c >= 32 || c == '\t') {
        cef_nui::SendKeyEvent(0, false, /*is_char=*/true,
                              static_cast<uint16_t>(c), modifiers);
      }
    }
  }
  for (const KeyMapping& mapping : kKeyMappings) {
    if (ImGui::IsKeyPressed(mapping.imgui_key, false)) {
      cef_nui::SendKeyEvent(mapping.windows_vk, true, false, 0, modifiers);
    }
    if (ImGui::IsKeyReleased(mapping.imgui_key)) {
      cef_nui::SendKeyEvent(mapping.windows_vk, false, false, 0, modifiers);
    }
  }
  // Clipboard/editing shortcuts (Ctrl+C/V/X/A/Z) never arrive as
  // characters, so forward the raw letter key with its control modifier and
  // let Blink run its own handling - which talks to the real OS clipboard.
  if (io.KeyCtrl) {
    for (int offset = 0; offset <= (ImGuiKey_Z - ImGuiKey_A); ++offset) {
      const auto key = static_cast<ImGuiKey>(ImGuiKey_A + offset);
      const int windows_vk = 'A' + offset;
      if (ImGui::IsKeyPressed(key, false)) {
        cef_nui::SendKeyEvent(windows_vk, true, false, 0, modifiers);
      }
      if (ImGui::IsKeyReleased(key)) {
        cef_nui::SendKeyEvent(windows_vk, false, false, 0, modifiers);
      }
    }
  }
}

}  // namespace skate3
