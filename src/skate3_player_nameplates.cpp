#include "skate3_player_nameplates.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <chrono>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "skate3_lua_client_natives.h"
#include "skate3_multiplayer.h"
#include "skate3_native_scene.h"

namespace skate3::nameplates {

namespace {

// Height (world units) above the replicated board/root position to draw the
// tag. RemotePose.position is the same board transform GetEntityCoords(0)
// reports for the local player - it is NOT a head bone, and this engine has
// no per-bone replicated skeleton to hang one off - so this is a fixed
// offset, not a measurement, and the right value is whatever looks right
// against the actual rig.
//
// Exposed as a cvar rather than a constant because that judgement needs a
// running game to make: set skate3_nameplate_height and see it move, rather
// than rebuilding to try a number.
REXCVAR_DEFINE_DOUBLE(
    skate3_nameplate_height, 0.25, "Skate 3/Multiplayer",
    "Clearance in world units between a remote player's head and their "
    "floating name.")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// HEAD in the canonical CAC/ABIN skeleton - see CanonicalBoneIndex in
// skate3_multiplayer_assets.cpp for the full order (0 TRAJECTORY, 1 HIPS,
// 2-5 SPINE*, 6 NECK, 7 NECK1, 8 HEAD, ... 25-31 the skateboard).
constexpr std::size_t kCanonicalHeadBone = 8;

// Each bone is three float4 affine rows, so the translation sits in the
// fourth column of each row.
constexpr std::size_t kBoneStride = 12;

// Used only when no head bone is available: roughly head height above the
// replicated root, so the fallback still lands somewhere sensible.
constexpr float kApproximateHeadHeight = 1.5f;

// How far the head bone may sit from the replicated root before it is
// treated as nonsense. The two are produced by different code paths in
// different spaces, and a silent space mismatch would fling the tag across
// the map; falling back is far better than drawing it somewhere absurd.
constexpr float kMaximumHeadDistanceFromRoot = 6.0f;

// A head is at least this far above the root. Anything lower is some other
// bone and is rejected in favour of the fixed offset.
constexpr float kMinimumHeadHeight = 0.8f;

// Dumps the canonical skeleton's shape ONCE per role, so the right anchor
// bone can be identified from data instead of guessed. Logs each bone's
// height above the replicated root; the head is whichever index sits
// consistently near the top while the player is stood upright.
void LogCanonicalSkeletonOnce(std::uint32_t role,
                              const multiplayer::AnimationTrack& track,
                              const float root_position[3]) {
  static std::mutex mutex;
  static std::unordered_set<std::uint32_t> logged;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!logged.insert(role).second) {
      return;
    }
  }
  const std::size_t bones = track.bone_rows.size() / kBoneStride;
  std::string heights;
  for (std::size_t bone = 0; bone < bones && bone < 40; ++bone) {
    const float* rows = track.bone_rows.data() + bone * kBoneStride;
    char entry[48];
    std::snprintf(entry, sizeof(entry), " %zu:%.2f/%.2f", bone,
                  rows[7] - root_position[1], rows[10] - root_position[1]);
    heights += entry;
  }
  REXLOG_INFO(
      "nameplate-skeleton: role={} bones={} root=({:.2f},{:.2f},{:.2f}) "
      "index:dY(rowmajor)/dY(colmajor){}",
      role, bones, root_position[0], root_position[1], root_position[2],
      heights);
}

// The head bone's world translation, or nullptr when this peer has no
// canonical skeleton this frame (a proxy-rendered peer, or one whose
// animation has not arrived yet).
//
// WHY THE HEAD BONE AND NOT RemotePose.position: the replicated pose root is
// the BOARD/root transform. Anchoring to it meant the tag rode the
// skateboard - so it sat too low, and during a flip trick, where the board
// spins and travels independently of the skater, it was thrown around with
// it. The head bone comes from the same interpolated canonical skeleton the
// character mesh itself is drawn from, so the tag tracks the actual head and
// is exactly as steady as the body is.
const float* CanonicalHeadBone(const multiplayer::AnimationPose& animation) {
  for (const multiplayer::AnimationTrack& track : animation.tracks) {
    if (track.mesh_key != multiplayer::kCanonicalSkeletonTrackKey) {
      continue;
    }
    const std::size_t offset = kCanonicalHeadBone * kBoneStride;
    if (track.bone_rows.size() < offset + kBoneStride) {
      return nullptr;  // a shorter skeleton than this build expects
    }
    return track.bone_rows.data() + offset;
  }
  return nullptr;
}

// Beyond this, a nameplate is more clutter than information - skip the
// WorldToScreen call entirely rather than draw text nobody can read.
constexpr float kMaxDistance = 60.0f;
// Text shrinks smoothly between these two distances rather than popping
// from full size to gone at the cutoff.
constexpr float kFadeStartDistance = 40.0f;

// How quickly the drawn position eases toward the raw one, for the FALLBACK
// anchor only - a peer with no canonical skeleton, where all we have is the
// replicated board/root position. That root visibly wobbles frame to frame
// when drawn as a single point, even though the character mesh looks steady,
// because the mesh is smoothed at the bone level upstream and the root is
// what that smoothing feeds FROM rather than its result.
//
// Kept short: anything slower reads as the tag TRAILING a moving skater,
// which is its own kind of wrong - at skating speed 0.15s of lag put the
// name most of a body-length behind the player.
//
// The head-bone anchor skips this entirely; see OnDraw.
constexpr float kSmoothingTimeConstant = 0.05f;

// World height of the text at full size, before the distance shrink.
constexpr float kPlateHeight = 0.22f;

// Per-role smoothed head position. A file-scope global now rather than a
// dialog member, because the dialog is gone - see the header.
struct SmoothedHead {
  float position[3] = {};
  bool initialized = false;
};
std::unordered_map<std::uint32_t, SmoothedHead> g_smoothed_heads;

std::mutex g_plates_mutex;
std::vector<Plate> g_plates;

// Update derives its own delta rather than having one plumbed in, so the
// render hook does not have to know or care that this needs one.
std::chrono::steady_clock::time_point g_last_update{};

void PublishPlates(std::vector<Plate> plates) {
  std::lock_guard<std::mutex> lock(g_plates_mutex);
  g_plates = std::move(plates);
}

}  // namespace

void Update() {
  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (g_last_update.time_since_epoch().count() != 0) {
    dt = std::chrono::duration<float>(now - g_last_update).count();
  }
  g_last_update = now;
  // A long stall (a load, a breakpoint) would otherwise produce an ease of
  // ~1 and snap every plate; clamping keeps the smoothing meaningful.
  dt = std::clamp(dt, 1.0f / 480.0f, 0.1f);

  // Nothing to draw offline. The session flag is checked directly rather than
  // inferring "online" from a non-empty list, so the moment between
  // disconnecting and the list clearing does not keep stale names up.
  if (!rex::cvar::Query<bool>("skate3_multiplayer_relay_active")) {
    g_smoothed_heads.clear();
    PublishPlates({});
    return;
  }

  const std::shared_ptr<const std::vector<multiplayer::RemotePlayer>> players =
      multiplayer::LatestRemotePlayers();
  if (!players || players->empty()) {
    g_smoothed_heads.clear();
    PublishPlates({});
    return;
  }

  float camera_position[3] = {};
  const bool have_camera = native_scene::CameraPosition(camera_position);
  const float ease = 1.0f - std::exp(-dt / kSmoothingTimeConstant);
  const double head_clearance = REXCVAR_GET(skate3_nameplate_height);

  std::vector<Plate> plates;
  plates.reserve(players->size());
  std::unordered_set<std::uint32_t> seen_this_frame;

  for (const multiplayer::RemotePlayer& player : *players) {
    seen_this_frame.insert(player.role);
    bool anchored_to_head = false;
    float raw_head[3] = {
        player.pose.position[0],
        player.pose.position[1] + kApproximateHeadHeight +
            static_cast<float>(head_clearance),
        player.pose.position[2]};
    for (const multiplayer::AnimationTrack& track : player.animation.tracks) {
      if (track.mesh_key == multiplayer::kCanonicalSkeletonTrackKey) {
        LogCanonicalSkeletonOnce(player.role, track, player.pose.position);
        break;
      }
    }
    if (const float* head_bone = CanonicalHeadBone(player.animation)) {
      // Translation is the fourth column of each of the three affine rows.
      const float candidate[3] = {head_bone[3], head_bone[7], head_bone[11]};
      float offset_from_root = 0.0f;
      bool finite = true;
      for (int axis = 0; axis < 3; ++axis) {
        finite = finite && std::isfinite(candidate[axis]);
        const float delta = candidate[axis] - player.pose.position[axis];
        offset_from_root += delta * delta;
      }
      // Must actually be above the root to be a head. This is what stops a
      // mis-identified bone - a foot, a wheel, the board - from dragging the
      // tag down to ground level.
      const bool plausibly_a_head =
          candidate[1] - player.pose.position[1] >= kMinimumHeadHeight;
      if (finite && plausibly_a_head &&
          offset_from_root <= kMaximumHeadDistanceFromRoot *
                                  kMaximumHeadDistanceFromRoot) {
        raw_head[0] = candidate[0];
        raw_head[1] = candidate[1] + static_cast<float>(head_clearance);
        raw_head[2] = candidate[2];
        anchored_to_head = true;
      }
    }

    SmoothedHead& smoothed = g_smoothed_heads[player.role];
    if (anchored_to_head || !smoothed.initialized) {
      // The head bone is already the presentation pose the mesh is drawn
      // from, so easing toward it could only make the tag lag the head it
      // sits on. A first sighting snaps too, rather than sliding in from the
      // world origin.
      smoothed.position[0] = raw_head[0];
      smoothed.position[1] = raw_head[1];
      smoothed.position[2] = raw_head[2];
      smoothed.initialized = true;
    } else {
      for (int axis = 0; axis < 3; ++axis) {
        smoothed.position[axis] +=
            (raw_head[axis] - smoothed.position[axis]) * ease;
      }
    }
    const float* head = smoothed.position;

    float distance = 0.0f;
    if (have_camera) {
      const float dx = head[0] - camera_position[0];
      const float dy = head[1] - camera_position[1];
      const float dz = head[2] - camera_position[2];
      distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (distance > kMaxDistance) {
        continue;
      }
    }

    // No WorldToScreen and no frustum test any more: these are world-space
    // billboards now, so the GPU clips them. Culling by screen rectangle here
    // would only duplicate that - and got it subtly wrong, since a plate
    // whose anchor was just off screen could still have visible text.

    std::string name = lua_client::PlayerName(static_cast<int>(player.role));
    if (name.empty()) {
      // A player whose name has not arrived yet still gets a placeholder, so
      // "someone is there" is never invisible while "who" catches up.
      char fallback[32];
      std::snprintf(fallback, sizeof(fallback), "Player %u", player.role);
      name = fallback;
    }

    // Linear fade/shrink between kFadeStartDistance and kMaxDistance.
    float scale = 1.0f;
    float alpha = 1.0f;
    if (have_camera && distance > kFadeStartDistance) {
      const float t = (distance - kFadeStartDistance) /
                      (kMaxDistance - kFadeStartDistance);
      scale = 1.0f - 0.4f * t;
      alpha = 1.0f - 0.7f * t;
    }

    Plate plate;
    plate.position[0] = head[0];
    plate.position[1] = head[1];
    plate.position[2] = head[2];
    plate.name = std::move(name);
    plate.alpha = alpha;
    plate.height = kPlateHeight * scale;
    plates.push_back(std::move(plate));
  }

  // Forget anyone not in this frame's list (left, or the role was reused by
  // someone new) - otherwise a departed player's smoothing state sits around
  // forever, and worse, a NEW player later assigned that role would ease in
  // from the old occupant's last position instead of snapping to their own.
  for (auto it = g_smoothed_heads.begin(); it != g_smoothed_heads.end();) {
    if (seen_this_frame.contains(it->first)) {
      ++it;
    } else {
      it = g_smoothed_heads.erase(it);
    }
  }

  PublishPlates(std::move(plates));
}

std::vector<Plate> Snapshot() {
  std::lock_guard<std::mutex> lock(g_plates_mutex);
  return g_plates;
}

}  // namespace skate3::nameplates
