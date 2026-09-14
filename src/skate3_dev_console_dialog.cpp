#include "skate3_dev_console_dialog.h"

#include "skate3_cef_console.h"
#include "skate3_native_scene.h"

#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <atomic>

namespace skate3 {

namespace {

// Small, curated set of non-character keys the browser's own JS needs to
// see directly (Enter to submit, Backspace/arrows for editing and command
// history, Escape/Tab). Regular typing (letters/digits/punctuation) goes
// through io.InputQueueCharacters instead - see OnDraw - so this table
// deliberately stays short rather than mapping the full keyboard.
struct KeyMapping {
  ImGuiKey imgui_key;
  int windows_vk;
};
constexpr KeyMapping kKeyMappings[] = {
    {ImGuiKey_Enter, 0x0D},      // VK_RETURN
    {ImGuiKey_KeypadEnter, 0x0D},
    {ImGuiKey_Backspace, 0x08},  // VK_BACK
    {ImGuiKey_Tab, 0x09},        // VK_TAB
    {ImGuiKey_Escape, 0x1B},     // VK_ESCAPE
    {ImGuiKey_Delete, 0x2E},     // VK_DELETE
    {ImGuiKey_Home, 0x24},       // VK_HOME
    {ImGuiKey_End, 0x23},        // VK_END
    {ImGuiKey_UpArrow, 0x26},    // VK_UP
    {ImGuiKey_DownArrow, 0x28},  // VK_DOWN
    {ImGuiKey_LeftArrow, 0x25},  // VK_LEFT
    {ImGuiKey_RightArrow, 0x27}, // VK_RIGHT
};

}  // namespace

Skate3DevConsoleDialog::Skate3DevConsoleDialog(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer) {}

Skate3DevConsoleDialog::~Skate3DevConsoleDialog() = default;

void Skate3DevConsoleDialog::Show() {
  visible_ = true;
  native_scene::SetDevConsoleVisible(true);
  // CEF's OSR browser has no real native window to receive focus/blur from,
  // so this app must tell it explicitly (see SetFocus's own comment) -
  // otherwise a mouse click never moves DOM focus into the <input>, and
  // only keyboard-driven navigation (Tab) does.
  cef_console::SetFocus(true);
}

void Skate3DevConsoleDialog::Hide() {
  visible_ = false;
  native_scene::SetDevConsoleVisible(false);
  cef_console::SetFocus(false);
  for (bool& down : mouse_button_down_) {
    down = false;
  }
  physical_left_down_ = false;
  dragging_ = false;
}

void Skate3DevConsoleDialog::Toggle() {
  if (visible_) {
    Hide();
  } else {
    Show();
  }
}

void Skate3DevConsoleDialog::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    return;
  }

  // io.MousePos is in OS window pixels (see UpdateMousePosition in
  // imgui_drawer.cpp: io.DisplaySize == window_->GetActualPhysicalWidth/
  // Height), which can differ from guest_output's pixel size - the space
  // kConsoleScreenX/Y/Width/Height (and the render pass's own draw rect)
  // are defined in - whenever DLSS or resolution_scale/
  // draw_resolution_scale_x/y make the internal render resolution not
  // match the window 1:1. Scale into guest_output space before comparing.
  uint32_t guest_w = 0, guest_h = 0;
  native_scene::GetLastDevConsoleGuestOutputSize(guest_w, guest_h);
  const float scale_x = (guest_w != 0 && io.DisplaySize.x > 0.0f)
                            ? static_cast<float>(guest_w) / io.DisplaySize.x
                            : 1.0f;
  const float scale_y = (guest_h != 0 && io.DisplaySize.y > 0.0f)
                            ? static_cast<float>(guest_h) / io.DisplaySize.y
                            : 1.0f;
  const float local_x =
      io.MousePos.x * scale_x - static_cast<float>(cef_console::kConsoleScreenX);
  const float local_y =
      io.MousePos.y * scale_y - static_cast<float>(cef_console::kConsoleScreenY);
  const int browser_x = static_cast<int>(local_x);
  const int browser_y = static_cast<int>(local_y);
  // The console's size tracks guest_output's resolution (see
  // kConsoleHeightFraction), so the bounds check reads the CURRENT size
  // rather than a fixed constant - CurrentSize() is exactly what
  // DrawDevConsoleOverlay last resized the browser to.
  int console_w = 0, console_h = 0;
  cef_console::CurrentSize(console_w, console_h);
  const bool inside = local_x >= 0.0f && local_y >= 0.0f &&
                      local_x < static_cast<float>(console_w) &&
                      local_y < static_cast<float>(console_h);

  // Modifier state rides on every forwarded event. Blink needs the
  // left-button bit on MOVE events to treat them as a selection drag rather
  // than a hover, and the control bit on key events to run clipboard
  // shortcuts - without these, selecting and pasting silently do nothing.
  uint32_t modifiers = cef_console::kModNone;
  if (io.KeyShift) {
    modifiers |= cef_console::kModShift;
  }
  if (io.KeyCtrl) {
    modifiers |= cef_console::kModControl;
  }
  if (io.KeyAlt) {
    modifiers |= cef_console::kModAlt;
  }
  if (io.MouseDown[0]) {
    modifiers |= cef_console::kModLeftMouseButton;
  }
  if (io.MouseDown[1]) {
    modifiers |= cef_console::kModRightMouseButton;
  }
  if (io.MouseDown[2]) {
    modifiers |= cef_console::kModMiddleMouseButton;
  }

  // A drag that starts inside must keep being forwarded after the cursor
  // leaves the rect (and must still get its mouse-up), or a selection
  // freezes mid-gesture the moment you overshoot the console's edge.
  if (dragging_ || inside) {
    cef_console::SendMouseMove(browser_x, browser_y, modifiers);
  }

  // Diagnostic: log every PHYSICAL left-click regardless of whether it
  // computed as "inside" - a click outside never reaches the button-state
  // machine below, so without this a coordinate-math bug would silently
  // produce no log at all.
  if (io.MouseDown[0] && !physical_left_down_) {
    static std::atomic<uint32_t> s_click_logged{0};
    if (s_click_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO(
          "dev-console: click mouse=({:.0f},{:.0f}) display=({:.0f}x{:.0f}) "
          "guest_output=({}x{}) scale=({:.3f},{:.3f}) local=({:.0f},{:.0f}) "
          "inside={}",
          io.MousePos.x, io.MousePos.y, io.DisplaySize.x, io.DisplaySize.y,
          guest_w, guest_h, scale_x, scale_y, local_x, local_y, inside);
    }
  }
  physical_left_down_ = io.MouseDown[0];

  // ImGui orders its buttons left/right/middle; cef_console's own API takes
  // left/middle/right (matching CEF). Map rather than pass through, or
  // right-clicks arrive as middle-clicks.
  constexpr int kImGuiToCefButton[3] = {0, 2, 1};
  for (int imgui_button = 0; imgui_button < 3; ++imgui_button) {
    const bool held = io.MouseDown[imgui_button];
    // Press only counts when it starts inside; release is always delivered
    // if we forwarded the press, so Blink can never be left with a stuck
    // button after a drag that ended off-console.
    const bool down_now =
        mouse_button_down_[imgui_button] ? held : (held && inside);
    if (down_now == mouse_button_down_[imgui_button]) {
      continue;
    }
    if (down_now) {
      // Re-assert focus on every click, not just Show(): the browser never
      // gains/loses "window focus" on its own in OSR mode, so this is the
      // only thing that makes clicking an unfocused element (e.g. after
      // Escape blurred it) work again.
      cef_console::SetFocus(true);
    }
    // Blink derives word-select (double) and line-select (triple) from the
    // click count, not from the raw event stream.
    int click_count = 1;
    if (down_now && imgui_button == 0) {
      click_count = io.MouseDoubleClicked[0] ? 2 : 1;
    }
    cef_console::SendMouseButton(browser_x, browser_y,
                                 kImGuiToCefButton[imgui_button], !down_now,
                                 modifiers, click_count);
    mouse_button_down_[imgui_button] = down_now;
    if (imgui_button == 0) {
      dragging_ = down_now;
    }
  }
  if ((inside || dragging_) && io.MouseWheel != 0.0f) {
    cef_console::SendMouseWheel(browser_x, browser_y, 0,
                                static_cast<int>(io.MouseWheel * 100.0f),
                                modifiers);
  }

  // Typed characters (letters/digits/punctuation) - already fully resolved
  // by ImGui's own platform input backend (layout/IME-aware). Skipped while
  // Ctrl/Alt is held: those combinations arrive here as control codepoints
  // (Ctrl+V is 0x16), which would insert garbage into the input box on top
  // of the real shortcut handled below.
  if (!io.KeyCtrl && !io.KeyAlt) {
    for (const ImWchar c : io.InputQueueCharacters) {
      if (c >= 32 || c == '\t') {
        cef_console::SendKeyEvent(0, false, true, static_cast<uint16_t>(c),
                                  modifiers);
      }
    }
  }
  // Non-character keys the browser's JS needs directly (Enter/Backspace/
  // arrows/...).
  for (const KeyMapping& mapping : kKeyMappings) {
    if (ImGui::IsKeyPressed(mapping.imgui_key, false)) {
      cef_console::SendKeyEvent(mapping.windows_vk, true, false, 0, modifiers);
    }
    if (ImGui::IsKeyReleased(mapping.imgui_key)) {
      cef_console::SendKeyEvent(mapping.windows_vk, false, false, 0, modifiers);
    }
  }
  // Clipboard/editing shortcuts (Ctrl+C/V/X/A/Z). These never arrive as
  // characters, so forward the raw letter key with its control modifier and
  // let Blink run its own handling - which talks to the real OS clipboard,
  // so copy/paste crosses the console boundary in both directions.
  if (io.KeyCtrl) {
    for (int offset = 0; offset <= (ImGuiKey_Z - ImGuiKey_A); ++offset) {
      const auto key = static_cast<ImGuiKey>(ImGuiKey_A + offset);
      const int windows_vk = 'A' + offset;
      if (ImGui::IsKeyPressed(key, false)) {
        cef_console::SendKeyEvent(windows_vk, true, false, 0, modifiers);
      }
      if (ImGui::IsKeyReleased(key)) {
        cef_console::SendKeyEvent(windows_vk, false, false, 0, modifiers);
      }
    }
  }
}

}  // namespace skate3
