#include "skate3_world_markers.h"

#include "skate3_input_state.h"
#include "skate3_trick_pipeline.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace skate3::world_markers {

namespace {

struct Marker {
  MarkerDesc desc;
  std::string owner;
};

std::mutex g_mutex;
std::unordered_map<std::uint32_t, Marker> g_markers;
// Never reset, so an id from a destroyed marker stays dead for the session
// rather than coming back attached to something else.
std::uint32_t g_next_id = 1;

// Written by Update (app thread), read by the renderer and by script. Kept
// under the same lock as the registry: they are read together, and a prompt
// naming a marker that has just been destroyed is exactly the tear worth
// avoiding.
std::uint32_t g_active = 0;
float g_hold_progress = 0.0f;

ActivationHandler g_handler;

// Hold state. Not under the lock - only Update touches it.
std::uint32_t g_holding_marker = 0;
std::int64_t g_hold_started_ms = 0;
// Set when a hold completes, cleared when the button is released. Without it a
// completed hold fires again on every subsequent frame the button stays down.
bool g_hold_consumed = false;

[[nodiscard]] std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] float DistanceSquared(const MarkerDesc& desc,
                                    const float position[3]) {
  const float dx = desc.x - position[0];
  const float dy = desc.y - position[1];
  const float dz = desc.z - position[2];
  return dx * dx + dy * dy + dz * dz;
}

}  // namespace

std::uint32_t Create(const MarkerDesc& desc, std::string owner_resource) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::uint32_t id = g_next_id++;
  g_markers.emplace(id, Marker{desc, std::move(owner_resource)});
  return id;
}

bool Destroy(std::uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_markers.erase(id) == 0) {
    return false;
  }
  // Clearing the active marker here rather than waiting for the next Update
  // means a script that destroys the marker it just activated does not leave a
  // prompt pointing at something gone for a frame.
  if (g_active == id) {
    g_active = 0;
    g_hold_progress = 0.0f;
  }
  return true;
}

void DestroyAllForResource(const std::string& owner_resource) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_markers.begin(); it != g_markers.end();) {
    if (it->second.owner == owner_resource) {
      if (g_active == it->first) {
        g_active = 0;
        g_hold_progress = 0.0f;
      }
      it = g_markers.erase(it);
    } else {
      ++it;
    }
  }
}

bool SetText(std::uint32_t id, std::string text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_markers.find(id);
  if (it == g_markers.end()) {
    return false;
  }
  it->second.desc.text = std::move(text);
  return true;
}

bool SetPosition(std::uint32_t id, float x, float y, float z) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_markers.find(id);
  if (it == g_markers.end()) {
    return false;
  }
  it->second.desc.x = x;
  it->second.desc.y = y;
  it->second.desc.z = z;
  return true;
}

std::uint32_t ActiveMarker() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_active;
}

float HoldProgress() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_hold_progress;
}

void SetActivationHandler(ActivationHandler handler) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handler = std::move(handler);
}

void Update() {
  float position[3] = {0.0f, 0.0f, 0.0f};
  const bool have_player =
      trick_pipeline::CurrentLocalBoardPosition(position);

  std::uint32_t nearest = 0;
  std::uint32_t hold_ms = 0;
  ActivationHandler handler;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // No player position means no marker can be active - in the front end, or
    // before the skater exists. Not an error; just nothing to do.
    if (have_player) {
      float best = 0.0f;
      for (const auto& [id, marker] : g_markers) {
        const float radius = marker.desc.radius;
        if (radius <= 0.0f) {
          continue;
        }
        const float distance = DistanceSquared(marker.desc, position);
        if (distance > radius * radius) {
          continue;
        }
        // Nearest wins, so overlapping markers are unambiguous rather than
        // depending on hash order.
        if (nearest == 0 || distance < best) {
          nearest = id;
          best = distance;
          hold_ms = marker.desc.hold_ms;
        }
      }
    }
    g_active = nearest;
    handler = g_handler;
  }

  if (nearest == 0) {
    g_holding_marker = 0;
    g_hold_consumed = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_progress = 0.0f;
    return;
  }

  const bool pressed =
      input_state::IsPressed(input_state::ControlFromName("DPADUP"));
  if (!pressed) {
    // Release is what re-arms activation, so holding the button down across a
    // completed activation cannot fire a second one.
    g_holding_marker = 0;
    g_hold_consumed = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_progress = 0.0f;
    return;
  }

  if (g_hold_consumed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_progress = 1.0f;
    return;
  }

  const std::int64_t now = NowMs();
  // Walking from one marker to another mid-hold restarts the hold, rather than
  // carrying progress over to a marker the player only just reached.
  if (g_holding_marker != nearest) {
    g_holding_marker = nearest;
    g_hold_started_ms = now;
  }

  const std::int64_t held = now - g_hold_started_ms;
  const float progress =
      hold_ms == 0 ? 1.0f
                   : std::clamp(static_cast<float>(held) /
                                    static_cast<float>(hold_ms),
                                0.0f, 1.0f);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_progress = progress;
  }

  if (progress >= 1.0f) {
    g_hold_consumed = true;
    // Called with the lock released: the handler runs script, which will call
    // straight back into Destroy or Create.
    if (handler) {
      handler(nearest);
    }
  }
}

std::vector<Billboard> Snapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Billboard> out;
  out.reserve(g_markers.size());
  for (const auto& [id, marker] : g_markers) {
    out.push_back(Billboard{marker.desc.x, marker.desc.y, marker.desc.z,
                            marker.desc.size, marker.desc.color,
                            id == g_active, marker.desc.text,
                            marker.desc.texture});
  }
  return out;
}

Prompt CurrentPrompt() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Prompt prompt;
  if (g_active == 0) {
    return prompt;
  }
  const auto it = g_markers.find(g_active);
  if (it == g_markers.end()) {
    return prompt;
  }
  prompt.text = it->second.desc.text;
  prompt.progress = g_hold_progress;
  return prompt;
}

}  // namespace skate3::world_markers
