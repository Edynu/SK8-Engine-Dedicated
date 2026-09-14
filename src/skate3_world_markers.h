#pragma once

// In-world markers: the real thing, not a screen overlay pretending.
//
// WHY THIS EXISTS. Markers were a Lua resource that ran WorldToScreen every
// frame and drew icons in an HTML page (resources/Markers). That works, but it
// pays a Lua tick plus a NUI message per frame, it cannot be occluded by the
// world, and it is wrong in the one way that matters for a game mode: the
// marker is not in the scene, so it does not behave like one.
//
// Retail's own session marker was the obvious alternative and is not usable -
// see skate3_retail_markers.h. It is a SINGLETON with no text, no type, and no
// settable position. So markers are ours: this module owns their state and
// activation, and the native renderer draws them as world-space billboards
// (DrawWorldMarkers, skate3_native_scene_gpu.cpp).
//
// SPLIT OF RESPONSIBILITIES. Nothing here knows about Lua or about the GPU.
// Activation is published through a callback the script layer installs, and the
// renderer pulls a snapshot. That keeps the proximity/hold logic testable and
// keeps two threads out of each other's way: Update() runs on the app thread
// once per frame, Snapshot() on the render thread.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skate3::world_markers {

// What a script asks for when it creates one.
struct MarkerDesc {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  // How close the player has to be for this marker to become the active one,
  // in world units.
  float radius = 3.0f;
  // Billboard size in world units.
  float size = 1.0f;
  // 0xAARRGGBB, straight alpha.
  std::uint32_t color = 0xFFFFFFFFu;
  // How long D-pad up must be held to activate. 0 activates on the press.
  // A hold rather than a tap because D-pad up is not a dedicated button - a
  // tap would fire markers while the player was doing something else.
  std::uint32_t hold_ms = 500;
  // Shown in the prompt while this marker is active.
  std::string text;
  // Name of an embedded retail icon (src/generated/skate3_marker_icons.h) to
  // draw instead of the built-in ring. Empty = the ring. An unknown name also
  // falls back to the ring rather than drawing nothing, so a typo is visible
  // as "wrong art" rather than as a missing marker.
  std::string texture;
};

// Returns a marker id, or 0 if the marker could not be created. Ids are never
// reused within a session, so a stale id from a destroyed marker fails rather
// than silently addressing a new one.
[[nodiscard]] std::uint32_t Create(const MarkerDesc& desc,
                                   std::string owner_resource);
bool Destroy(std::uint32_t id);
// Every marker a resource created. Called when it stops, so a restarted
// resource does not leave its markers standing in the world forever.
void DestroyAllForResource(const std::string& owner_resource);
bool SetText(std::uint32_t id, std::string text);
bool SetPosition(std::uint32_t id, float x, float y, float z);

// The marker the player is currently at, or 0. Nearest wins when radii
// overlap.
[[nodiscard]] std::uint32_t ActiveMarker();
// How far through the hold the player is, 0..1. Meaningless with no active
// marker, and reported as 0 then.
[[nodiscard]] float HoldProgress();

// Called when a hold completes. Installed by the script layer; called on the
// app thread from inside Update, so the handler must not block.
using ActivationHandler = std::function<void(std::uint32_t marker_id)>;
void SetActivationHandler(ActivationHandler handler);

// Once per frame, app thread. Recomputes the active marker, advances or resets
// the hold, and fires the handler on completion.
void Update();

// What the renderer needs, copied under the lock so the render thread never
// touches the registry.
struct Billboard {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float size = 0.0f;
  std::uint32_t color = 0xFFFFFFFFu;
  // The active marker is drawn differently (it is the one the prompt refers
  // to), so the renderer is told which it is rather than working it out again.
  bool active = false;
  // Drawn as a label under the ring. Carried in the snapshot rather than
  // fetched separately so the label can never belong to a different marker
  // than the ring it sits under.
  std::string text;
  std::string texture;
};
[[nodiscard]] std::vector<Billboard> Snapshot();

// Text and progress for the prompt, so the renderer can draw it without
// reaching into the registry. Empty text means draw no prompt.
struct Prompt {
  std::string text;
  float progress = 0.0f;
};
[[nodiscard]] Prompt CurrentPrompt();

}  // namespace skate3::world_markers
