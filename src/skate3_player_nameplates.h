#pragma once

// Floating names over other synced players. Native, not Lua - the game itself
// owns "who is that", the same reason retail's own HUD elements are not
// scripted. Always on while online; there is no toggle and no keybind, because
// there is nothing to configure: it names every OTHER player this client knows
// a position for, and never the local player.
//
// NO LONGER AN IMGUI OVERLAY. It used to be an ImGuiDialog drawing into the
// foreground draw list at WorldToScreen coordinates. That put ImGui in the
// shipped rendering path for a permanent gameplay element, and it made the
// name a flat screen sprite rather than a thing in the world. It is now a
// world-space billboard drawn by the native renderer, sharing the pipeline the
// in-world markers use (skate3_world_markers.h) - specifically the
// depth-disabled variant, so a name is never swallowed by geometry while still
// being perspective-scaled and world-anchored.
//
// Reuses two things that already exist rather than inventing new plumbing:
//   - multiplayer::LatestRemotePlayers(), the cross-thread published snapshot.
//   - lua_client::PlayerName(role), the native name cache filled by the
//     server's playerNamed/playerRoster broadcasts.

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::nameplates {

// One name to draw, in world space.
struct Plate {
  float position[3] = {};
  std::string name;
  // Distance fade, already applied as a factor rather than a distance so the
  // renderer does not need to know the fade curve.
  float alpha = 1.0f;
  // Height of the text in world units, already shrunk with distance.
  float height = 0.25f;
};

// Once per frame, from the render hook. Computes head anchors, smooths them,
// culls by distance and publishes the result. Derives its own delta time, so
// no caller has to plumb one.
void Update();

// What Update last published. Safe from the render thread.
[[nodiscard]] std::vector<Plate> Snapshot();

}  // namespace skate3::nameplates
