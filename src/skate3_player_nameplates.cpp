#include "skate3_player_nameplates.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "skate3_lua_client_natives.h"
#include "skate3_multiplayer.h"
#include "skate3_native_scene.h"

namespace skate3 {

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

}  // namespace

void PlayerNameplateOverlay::OnDraw(ImGuiIO& io) {
  // Nothing to draw offline: LatestRemotePlayers() is naturally empty with
  // no session, but check the session flag directly anyway rather than
  // infer "online" from "the list happens to be non-empty" - a moment
  // between disconnect and the list clearing should not still draw stale
  // names.
  if (!rex::cvar::Query<bool>("skate3_multiplayer_relay_active")) {
    smoothed_heads_.clear();
    return;
  }
  // Pre-runtime (installer wizards) or no guest frame published yet - same
  // guard RenderModeIndicator uses, for the same reason.
  if (rex::ui::Presenter* presenter = imgui_drawer()->presenter()) {
    const rex::ui::Presenter::GuestOutputPaintRect rect =
        presenter->GetLastGuestOutputPaintRect();
    if (rect.width <= 0 || rect.height <= 0) {
      return;
    }
  }
  const std::shared_ptr<const std::vector<multiplayer::RemotePlayer>> players =
      multiplayer::LatestRemotePlayers();
  if (!players || players->empty()) {
    smoothed_heads_.clear();
    return;
  }
  float camera_position[3] = {};
  const bool have_camera = native_scene::CameraPosition(camera_position);
  const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;
  const float ease = 1.0f - std::exp(-dt / kSmoothingTimeConstant);

  const double head_clearance = REXCVAR_GET(skate3_nameplate_height);

  std::unordered_set<std::uint32_t> seen_this_frame;
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();
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
      // tag down to ground level; the fixed offset below is wrong by a bit,
      // but it is never wrong by a whole body.
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
    SmoothedHead& smoothed = smoothed_heads_[player.role];
    if (anchored_to_head) {
      // Deliberately NOT smoothed. The head bone is already the presentation
      // pose the character mesh is drawn from this frame, so easing toward
      // it could only make the tag lag the head it is supposed to sit on.
      // Damping belongs on the raw replicated root below, not here.
      smoothed.position[0] = raw_head[0];
      smoothed.position[1] = raw_head[1];
      smoothed.position[2] = raw_head[2];
      smoothed.initialized = true;
    } else if (!smoothed.initialized) {
      // First frame this role has been seen (just connected, or the
      // overlay just cleared on a reconnect) - snap straight to the raw
      // position rather than easing in from (0,0,0), which would draw the
      // tag sliding in from the world origin.
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
    float screen_x = 0.0f, screen_y = 0.0f, depth = 0.0f;
    if (!native_scene::WorldToScreen(head, screen_x, screen_y, depth)) {
      continue;  // behind the camera
    }
    if (screen_x < 0.0f || screen_x > 1.0f || screen_y < 0.0f || screen_y > 1.0f) {
      continue;  // off the edge of the frame
    }

    std::string name = lua_client::PlayerName(static_cast<int>(player.role));
    if (name.empty()) {
      // Not silently dropped: a player whose name has not arrived yet (a
      // brief window right after they connect) still gets a placeholder,
      // so "someone is there" is never invisible while "who" catches up.
      char fallback[32];
      std::snprintf(fallback, sizeof(fallback), "Player %u", player.role);
      name = fallback;
    }

    // Linear fade/shrink between kFadeStartDistance and kMaxDistance; full
    // size and opacity closer than that. have_camera already gated the
    // distance culling above, so this only runs when a distance exists.
    float scale = 1.0f;
    float alpha = 1.0f;
    if (have_camera && distance > kFadeStartDistance) {
      const float t = (distance - kFadeStartDistance) /
                      (kMaxDistance - kFadeStartDistance);
      scale = 1.0f - 0.4f * t;
      alpha = 1.0f - 0.7f * t;
    }

    const float base_font_size = ImGui::GetFontSize();
    const float font_size = base_font_size * scale;
    const ImVec2 text_size =
        ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, name.c_str());
    const ImVec2 center(screen_x * io.DisplaySize.x, screen_y * io.DisplaySize.y);
    const ImVec2 text_pos(center.x - text_size.x * 0.5f,
                          center.y - text_size.y * 0.5f);

    // A soft shadow rather than a background pill - readable over both
    // bright sky and dark geometry without a box competing with the world
    // for attention.
    const ImU32 shadow_color = IM_COL32(0, 0, 0, static_cast<int>(160 * alpha));
    const ImU32 text_color = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
    draw_list->AddText(ImGui::GetFont(), font_size,
                       ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), shadow_color,
                       name.c_str());
    draw_list->AddText(ImGui::GetFont(), font_size, text_pos, text_color,
                       name.c_str());
  }

  // Forget anyone not in this frame's list (left, or the role was reused
  // by someone new) - otherwise a departed player's smoothing state sits
  // around forever, and worse, would make a NEW player who is later
  // assigned that same role number ease in from the old occupant's last
  // position instead of snapping to their own.
  for (auto it = smoothed_heads_.begin(); it != smoothed_heads_.end();) {
    if (seen_this_frame.contains(it->first)) {
      ++it;
    } else {
      it = smoothed_heads_.erase(it);
    }
  }
}

}  // namespace skate3
