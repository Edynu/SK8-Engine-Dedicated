#pragma once

// The Lua dev console's ImGuiDialog: owns show/hide state and forwards
// input to the CEF browser (skate3_cef_console.h) while visible. Draws
// nothing itself - the console's actual pixels are composited by the
// native render pass (DrawDevConsoleOverlay in skate3_native_scene_gpu.cpp)
// directly into the guest output, in both Native and Emulated renderer
// modes; see the plan this was built from for why.

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

class Skate3DevConsoleDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Skate3DevConsoleDialog(rex::ui::ImGuiDrawer* drawer);
  ~Skate3DevConsoleDialog();

  void Show();
  void Hide();
  void Toggle();
  bool visible() const { return visible_; }

  // Input forwarding only makes sense while visible; no continuous repaint
  // is needed beyond that (this dialog draws nothing of its own).
  bool WantsContinuousRepaint() const override { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool visible_ = false;
  // Indexed by ImGui's own button order (0 left, 1 right, 2 middle), not
  // CEF's - see kImGuiToCefButton in OnDraw.
  bool mouse_button_down_[3] = {false, false, false};
  // Raw physical left-button state (unlike mouse_button_down_[0], not
  // gated on "inside") - only for the click diagnostic log in OnDraw.
  bool physical_left_down_ = false;
  // A left-drag in progress: keeps mouse-moves flowing to the browser even
  // once the cursor leaves the console rect, so a text selection can be
  // dragged past the edge without freezing.
  bool dragging_ = false;
};

}  // namespace skate3
