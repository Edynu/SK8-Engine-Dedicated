#pragma once

// Samples raw input once per frame and publishes it for scripts.
//
// An ImGuiDialog purely because that is the project's per-frame hook on the
// UI thread with ImGuiIO in scope; it draws nothing. Keyboard state comes
// from ImGui rather than the OS so it is already window-focus aware - a
// script cannot see keys typed into another application.

#include <rex/ui/imgui_dialog.h>

#include <functional>

#include "skate3_cef_gamepad.h"

namespace skate3 {

class Skate3InputSamplerDialog final : public rex::ui::ImGuiDialog {
 public:
  // `poll_gamepad` returns the merged pad state (the app owns the input
  // system). `keyboard_for_gameplay` answers whether the keyboard currently
  // belongs to gameplay rather than to a menu, the console or NUI - script
  // keyboard reads are suppressed while it is false, so typing a console
  // command cannot also drive a game mode.
  Skate3InputSamplerDialog(
      rex::ui::ImGuiDrawer* drawer,
      std::function<cef_gamepad::PadSnapshot()> poll_gamepad,
      std::function<bool()> keyboard_for_gameplay);
  ~Skate3InputSamplerDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::function<cef_gamepad::PadSnapshot()> poll_gamepad_;
  std::function<bool()> keyboard_for_gameplay_;
};

}  // namespace skate3
