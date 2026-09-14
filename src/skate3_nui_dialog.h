#pragma once

// The NUI layer's ImGuiDialog. Draws nothing of its own - NUI's pixels are
// composited by the native render pass (DrawNuiOverlay in
// skate3_native_scene_gpu.cpp) straight into the guest output, in both
// Native and Emulated renderer modes, exactly like the dev console's.
//
// What it actually does, every frame:
//   * publishes whether NUI has anything to draw, so the render pass and
//     the emulated post-processor know to run,
//   * forwards mouse/keyboard into the browser while a script has taken
//     focus with SetNuiFocus,
//   * drives the gamepad pointer (see skate3_cef_gamepad.h), because this
//     is a controller game and there is usually no mouse in play,
//   * tells the app when the cursor needs showing or hiding, since Lua can
//     change NUI focus at any moment from another thread.
//
// Unlike the dev console there is no keybind: NUI's visibility belongs to
// the resources, and its focus belongs to SetNuiFocus.

#include "skate3_cef_gamepad.h"

#include <rex/ui/imgui_dialog.h>

#include <cstdint>
#include <functional>

namespace skate3 {

class Skate3NuiDialog final : public rex::ui::ImGuiDialog {
 public:
  // `poll_gamepad` returns the merged pad state for this frame (the app
  // owns the input system; this dialog must not). `on_cursor_mode_changed`
  // is called only when the answer to "should the OS cursor be visible?"
  // actually flips, so the app can apply its own cursor policy without
  // being spammed every frame.
  Skate3NuiDialog(rex::ui::ImGuiDrawer* drawer,
                  std::function<cef_gamepad::PadSnapshot()> poll_gamepad,
                  std::function<void(bool wants_cursor)> on_cursor_mode_changed);
  ~Skate3NuiDialog();

  // NUI animates and the pad pointer moves continuously, so the UI thread
  // must keep ticking while a script holds focus. When nothing has focus
  // this still needs to run often enough to notice a resource publishing a
  // page, which the ordinary event-driven repaint already covers.
  bool WantsContinuousRepaint() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void ForwardMouse(ImGuiIO& io, int browser_w, int browser_h);
  void ForwardKeyboard(ImGuiIO& io);

  std::function<cef_gamepad::PadSnapshot()> poll_gamepad_;
  std::function<void(bool wants_cursor)> on_cursor_mode_changed_;

  cef_gamepad::PadCursor pad_cursor_;
  cef_gamepad::Sink pad_sink_;

  // Exactly one device drives the page's pointer at a time. With both the
  // pad and the mouse forwarding a position every frame, Blink sees the
  // pointer teleporting between two places at frame rate: hover flickers
  // and a click lands wherever the OTHER device last pointed, which reads
  // as "the mouse does not work" while the stick is live. Ownership goes to
  // whichever device moved most recently.
  enum class PointerOwner { kNone, kMouse, kPad };
  PointerOwner pointer_owner_ = PointerOwner::kNone;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;

  // Mirrors of the last state actually acted on, so a change made from a
  // Lua thread is picked up exactly once.
  bool focused_ = false;
  bool cursor_mode_applied_ = false;
  bool had_content_ = false;

  // Indexed by ImGui's own button order (0 left, 1 right, 2 middle), not
  // CEF's - see kImGuiToCefButton in the .cpp.
  bool mouse_button_down_[3] = {false, false, false};

};

}  // namespace skate3
