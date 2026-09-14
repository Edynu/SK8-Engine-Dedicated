#pragma once

// Floating names over other synced players. Native, not Lua - the game
// itself owns "who is that", the same reason retail's own HUD elements are
// not scripted. Always on while online; there is no toggle and no keybind,
// because there is nothing to configure: it draws a name over every OTHER
// player this client currently knows a position for, and nothing over the
// local player (you do not need your own name floating over your head).
//
// Reuses two things that already exist rather than inventing new plumbing:
//   - multiplayer::LatestRemotePlayers(), the cross-thread published
//     snapshot the ScriptCam/GetPlayerCoords work already added.
//   - lua_client::PlayerName(role), the native (not Lua) name cache already
//     filled by the server's playerNamed/playerRoster broadcasts.

#include <cstdint>
#include <unordered_map>

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

class PlayerNameplateOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PlayerNameplateOverlay(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer) {}

 protected:
  // Rides the guest frame paints like RenderModeIndicator/FpsOverlayDialog -
  // the game is already rendering continuously while online, so nothing
  // here needs to force an extra repaint of its own.
  bool WantsContinuousRepaint() const override { return false; }
  void OnDraw(ImGuiIO& io) override;

 private:
  // Per-role smoothed head position - see OnDraw's own comment for why
  // this exists (the raw replicated position visibly wobbles; the visible
  // character mesh does not, because it is smoothed separately upstream
  // and the nameplate was not). Owned by the dialog instance, so it lives
  // and dies with it rather than as a file-scope global.
  struct SmoothedHead {
    float position[3] = {};
    bool initialized = false;
  };
  std::unordered_map<std::uint32_t, SmoothedHead> smoothed_heads_;
};

}  // namespace skate3
