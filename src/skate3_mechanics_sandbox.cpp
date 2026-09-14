#include "skate3_mechanics_sandbox.h"

#include "skate3_map_editor.h"
#include "generated/skate3_init.h"
#include "native/skate3_native_entity.h"
#include "skate3_mechanics_sandbox_map.h"
#include "skate3_multiplayer.h"
#include "skate3_native_collision.h"
#include "skate3_native_grind.h"
#include "skate3_native_raytraced_mirror.h"
#include "skate3_owned_world_boundary.h"
#include "skate3_trick_pipeline.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <ostream>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox, true, "Skate 3",
    "Use the Custom Engine Layer after stable gameplay is reached. Retain the "
    "generated mechanics kernel and show the verified local skater inside the "
    "project-owned world.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_visual_map, true, "Skate 3",
    "Draw the native Blender graybox inside the mechanics sandbox. This is "
    "presentation-only and does not replace game collision. Enable explicitly "
    "after the retained-skater presentation is verified.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_collision_observer, false, "Skate 3",
    "Run the native test-map hitbox query as read-only telemetry. It never "
    "feeds or overrides the generated physics kernel.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_owned_collision, false, "Skate 3",
    "Experimental: constrain only the verified local board vertically to "
    "floor and ramp surfaces in the project-owned map after FillPhysOut. "
    "Retail physics still supplies forces, orientation, and lateral contact.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_teleport_patch_rider, false, "Skate 3",
    "Experimental, CONFIRMED INEFFECTIVE by an in-game test (2026-09-08): "
    "hold-patch the position-correlated local SkaterPresEntity's own "
    "m_MatLtoWTrans translation (entity+416) during a teleport. The patch "
    "fires (log line confirms it) but the visible skinned rider still does "
    "not move - this field evidently drives only rigid/ROPA garment world "
    "serving, not the primary skeleton. Kept, default off, for reference; "
    "see skate3_mechanics_sandbox_teleport_patch_bones for the newer "
    "attempt against the actual bone palette.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_teleport_patch_bones, false, "Skate 3",
    "SUPERSEDED by skate3_mechanics_sandbox_teleport_patch_skeleton, which "
    "moves the rider's real physics body instead of its drawn output. "
    "Default off; kept because it is still the only lever on the render "
    "palette specifically. Original description follows. "
    "Experimental: hold-patch every bone/rigid-part world matrix "
    "(cModelInstance::m_matrices, all instances owned by the position-"
    "correlated local SkaterPresEntity) by the same rigid delta the "
    "teleport applies to the SkateboardController, instead of overwriting "
    "SkaterPresEntity::m_MatLtoWTrans (see "
    "skate3_mechanics_sandbox_teleport_patch_rider, confirmed not to move "
    "the visible rider). Unverified whether the animation system re-derives "
    "these palettes fresh from an untouched actor position every tick "
    "(in which case this needs a per-tick reapply, already provided by the "
    "teleport hold window) or ignores a direct guest-memory patch entirely - "
    "needs an in-game check.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_rider_identity_log, false, "Skate 3",
    "Log local SkaterPresEntity identity-lock diagnostics (candidate "
    "changes, lock acquisition/loss, distance at lock time).")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_teleport_patch_skeleton, true, "Skate 3",
    "Move the HUMAN skater during a teleport, not just the skateboard, by "
    "writing Physics::Skeleton's own world matrix. The skeleton hangs off "
    "SkateboardController+432 (its constructor sub_82D74DD8 stores it right "
    "beside the Skateboard* at +428 and ProcessedPhysIn* at +436 the board "
    "teleport already uses), and FillPhysOut itself reads it back through "
    "that same field every tick. This is the rider's real body - unlike the "
    "render-side SkaterPresEntity fields the earlier attempts patched - so "
    "it should also cover the case where the player is walking rather than "
    "riding.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    skate3_mechanics_sandbox_actor_position_scan, false, "Skate 3",
    "Diagnostic, LIKELY POINTLESS NOW: scans the local ActionGraph actor's "
    "memory for a float3 matching the known-good board position. Static "
    "analysis has since shown that object is a pure component container "
    "(a sorted interface_id -> component* array) with no position field at "
    "all, and that the rider's position lives on Physics::Skeleton instead "
    "(see skate3_mechanics_sandbox_teleport_patch_skeleton). Kept only in "
    "case the scan turns up a mirrored copy anyway. Read-only, rate-limited.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, skate3_native_render);
REXCVAR_DECLARE(bool, skate3_native_render_scene);
REXCVAR_DECLARE(bool, skate3_native_render_scene_2d);
REXCVAR_DECLARE(bool, skate3_native_render_scene_splines);
REXCVAR_DECLARE(bool, skate3_native_render_scene_selection_outline);
REXCVAR_DECLARE(bool, skate3_native_render_scene_world_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_dynamic_items);
REXCVAR_DECLARE(bool, skate3_native_render_scene_entity_fade);
REXCVAR_DECLARE(bool, skate3_native_render_scene_hdr);
REXCVAR_DECLARE(int32_t, skate3_native_render_scene_msaa);

namespace skate3::mechanics_sandbox {
namespace {

enum class State : uint8_t {
  Disabled = 0,
  WaitingForGameplay,
  Active,
  ResetPending,
  ResetSucceeded,
  ResetFailed,
};

std::atomic<State> g_state{State::Disabled};
std::atomic<uint32_t> g_local_actor{0};
std::atomic<uint32_t> g_local_presentation_entity{0};
std::atomic<uint32_t> g_presentation_candidate{0};
std::atomic<uint64_t> g_last_gameplay_frame{0};
std::atomic<uint64_t> g_reset_frame{0};
std::atomic<uint32_t> g_reset_requests{0};
std::atomic<uint32_t> g_reset_completions{0};
std::atomic<uint32_t> g_reset_failures{0};
std::atomic<uint32_t> g_visible_items{0};
std::atomic<uint32_t> g_dropped_nonlocal{0};
std::atomic<uint32_t> g_dropped_unresolved{0};
std::atomic<uint32_t> g_visible_nonlocal_skater_items{0};
std::atomic<uint32_t> g_visible_nonlocal_skater_entities{0};
std::atomic<uint8_t> g_diagnostic_mode{
    static_cast<uint8_t>(DiagnosticMode::CandidateOnly)};
std::atomic<uint32_t> g_verified_ground_entity{0};
std::atomic<uint32_t> g_reset_expected_ground_entity{0};
std::atomic<uint32_t> g_reset_expected_action_actor{0};
std::atomic<uint32_t> g_reset_expected_phys_out{0};
std::atomic<uint8_t> g_render_stage{
    static_cast<uint8_t>(RenderStage::None)};
std::atomic<uint64_t> g_render_stage_frames{0};
std::atomic<uint32_t> g_rendered_draws{0};
std::atomic<uint32_t> g_rendered_frames{0};
std::atomic<uint32_t> g_candidate_draws{0};
std::atomic<uint32_t> g_candidate_draw_frames{0};
std::atomic<uint32_t> g_map_draws{0};
std::atomic<uint32_t> g_map_chunk_count{0};
std::atomic<uint32_t> g_map_candidate_chunks{0};
std::atomic<uint32_t> g_map_visible_chunks{0};
std::atomic<uint32_t> g_map_occluded_chunks{0};
std::atomic<uint32_t> g_map_resident_chunks{0};
std::atomic<uint32_t> g_map_chunk_draws{0};
std::atomic<uint32_t> g_map_editor_objects{0};
std::atomic<uint32_t> g_map_editor_pose_ready{0};
std::atomic<uint32_t> g_map_editor_pose_fallbacks{0};
std::atomic<uint32_t> g_map_editor_visible_objects{0};
std::atomic<uint32_t> g_map_editor_resident_objects{0};
std::atomic<uint32_t> g_map_editor_object_draws{0};
std::atomic<uint32_t> g_sky_draws{0};
std::atomic<uint32_t> g_map_contact_count{0};
std::atomic<uint32_t> g_map_last_contact_id{0};
std::atomic<uint32_t> g_map_last_penetration_bits{0};
std::atomic<uint32_t> g_map_origin_x_bits{0};
std::atomic<uint32_t> g_map_origin_y_bits{0};
std::atomic<uint32_t> g_map_origin_z_bits{0};
std::atomic<uint32_t> g_map_origin_phys_out{0};
std::atomic<bool> g_map_origin_valid{false};
std::atomic<uint64_t> g_owned_collision_checks{0};
std::atomic<uint64_t> g_owned_collision_corrections{0};
std::atomic<uint64_t> g_owned_collision_step_rejections{0};
std::atomic<uint64_t> g_owned_collision_floor_recoveries{0};
std::atomic<uint32_t> g_owned_collision_last_surface{0};
std::atomic<uint32_t> g_owned_collision_last_normal_y_bits{
    std::bit_cast<uint32_t>(1.0f)};
std::atomic<uint32_t> g_owned_collision_last_target_y_bits{0};
std::atomic<uint32_t> g_owned_collision_floor_offset_bits{0};
std::atomic<uint32_t> g_owned_collision_floor_samples{0};
std::atomic<uint64_t> g_owned_collision_last_correction_check{0};
std::atomic<uint64_t> g_owned_collision_last_correction_frame{0};
std::atomic<uint64_t> g_owned_ground_override_count{0};
std::mutex g_identity_log_mutex;
std::unordered_set<uint32_t> g_logged_presentation_entities;
std::mutex g_visible_skater_mutex;
std::unordered_set<uint32_t> g_visible_nonlocal_skaters_this_frame;

struct SavedRenderSettings {
  bool captured = false;
  bool scene = true;
  bool scene_2d = true;
  bool splines = true;
  bool outline = true;
  bool world_items = true;
  bool dynamic_items = true;
  bool entity_fade = true;
  bool hdr = true;
  int32_t msaa = 1;
};

std::mutex g_settings_mutex;
SavedRenderSettings g_saved_settings;

const char* StateNameFor(State state) {
  switch (state) {
    case State::WaitingForGameplay: return "waiting_for_gameplay";
    case State::Active: return "active";
    case State::ResetPending: return "reset_pending";
    case State::ResetSucceeded: return "reset_succeeded";
    case State::ResetFailed: return "reset_failed";
    default: return "disabled";
  }
}

const char* DiagnosticModeNameFor(DiagnosticMode mode) {
  switch (mode) {
    case DiagnosticMode::BackgroundOnly: return "background_only";
    case DiagnosticMode::AllDynamic: return "all_dynamic";
    case DiagnosticMode::CandidateWorldOn: return "candidate_world_on";
    default: return "candidate_only";
  }
}

const char* RenderStageNameFor(RenderStage stage) {
  switch (stage) {
    case RenderStage::Entered: return "entered";
    case RenderStage::YieldedForMenus: return "yielded_menus";
    case RenderStage::YieldedForPhoto: return "yielded_photo";
    case RenderStage::YieldedForMovie: return "yielded_movie";
    case RenderStage::SceneReady: return "scene_ready";
    case RenderStage::PipelineReady: return "pipeline_ready";
    case RenderStage::BackgroundCleared: return "background_cleared";
    case RenderStage::MainPassComplete: return "main_pass_complete";
    case RenderStage::HdrPostComplete: return "hdr_post_complete";
    case RenderStage::Presented: return "presented";
    default: return "none";
  }
}

DiagnosticMode DiagnosticModeValue() {
  return static_cast<DiagnosticMode>(
      g_diagnostic_mode.load(std::memory_order_acquire));
}

void ApplySandboxPresentation() {
  std::lock_guard lock(g_settings_mutex);
  if (!g_saved_settings.captured) {
    g_saved_settings.captured = true;
    g_saved_settings.scene = REXCVAR_GET(skate3_native_render_scene);
    g_saved_settings.scene_2d = REXCVAR_GET(skate3_native_render_scene_2d);
    g_saved_settings.splines = REXCVAR_GET(skate3_native_render_scene_splines);
    g_saved_settings.outline =
        REXCVAR_GET(skate3_native_render_scene_selection_outline);
    g_saved_settings.world_items =
        REXCVAR_GET(skate3_native_render_scene_world_items);
    g_saved_settings.dynamic_items =
        REXCVAR_GET(skate3_native_render_scene_dynamic_items);
    g_saved_settings.entity_fade =
        REXCVAR_GET(skate3_native_render_scene_entity_fade);
    g_saved_settings.hdr = REXCVAR_GET(skate3_native_render_scene_hdr);
    g_saved_settings.msaa = REXCVAR_GET(skate3_native_render_scene_msaa);
  }

  // These are presentation-only gates. Dynamic captures stay on because the
  // local player/board still need their normal native scene path; the scene
  // builder applies the ownership filter below. The game's 2D/APT stream is
  // independent of the retail 3D world and must remain enabled: it carries
  // the live vanilla HUD, trick text, notifications, pause UI, and loading
  // UI that the native renderer composites after the custom world.
  REXCVAR_SET(skate3_native_render_scene, true);
  REXCVAR_SET(skate3_native_render_scene_2d,
              g_saved_settings.scene_2d);
  REXCVAR_SET(skate3_native_render_scene_splines, false);
  REXCVAR_SET(skate3_native_render_scene_selection_outline, false);
  // Keep world-item capture alive even when the sandbox hides those items at
  // the final draw boundary. The native renderer still needs submitted scene
  // state for a valid camera/frame target; disabling this capture produced
  // the black sandbox frame in the render matrix.
  REXCVAR_SET(skate3_native_render_scene_world_items, true);
  REXCVAR_SET(skate3_native_render_scene_dynamic_items, true);
  // The provisional local presentation candidate is not a LivingWorld NPC,
  // but its retained character rows can carry a spawn/distance alpha from
  // the presentation bridge. Keep the local skater opaque in the sandbox;
  // this is presentation-only and is restored on deactivation.
  REXCVAR_SET(skate3_native_render_scene_entity_fade, false);
  // The background-only probe deliberately bypasses the HDR tonemap and
  // MSAA resolve. If the colored clear appears on the classic path, the
  // failure is in the HDR/post chain rather than scene submission.
  if (DiagnosticModeValue() == DiagnosticMode::BackgroundOnly) {
    REXCVAR_SET(skate3_native_render_scene_hdr, false);
    REXCVAR_SET(skate3_native_render_scene_msaa, 1);
  } else {
    REXCVAR_SET(skate3_native_render_scene_hdr, g_saved_settings.hdr);
    REXCVAR_SET(skate3_native_render_scene_msaa, g_saved_settings.msaa);
  }
}

void RestorePresentation() {
  std::lock_guard lock(g_settings_mutex);
  if (!g_saved_settings.captured) {
    return;
  }
  REXCVAR_SET(skate3_native_render_scene, g_saved_settings.scene);
  REXCVAR_SET(skate3_native_render_scene_2d, g_saved_settings.scene_2d);
  REXCVAR_SET(skate3_native_render_scene_splines, g_saved_settings.splines);
  REXCVAR_SET(skate3_native_render_scene_selection_outline,
              g_saved_settings.outline);
  REXCVAR_SET(skate3_native_render_scene_world_items,
              g_saved_settings.world_items);
  REXCVAR_SET(skate3_native_render_scene_dynamic_items,
              g_saved_settings.dynamic_items);
  REXCVAR_SET(skate3_native_render_scene_entity_fade,
              g_saved_settings.entity_fade);
  REXCVAR_SET(skate3_native_render_scene_hdr, g_saved_settings.hdr);
  REXCVAR_SET(skate3_native_render_scene_msaa, g_saved_settings.msaa);
  g_saved_settings = {};
}

bool CaptureMapOriginFromOwnedPlayer(uint32_t phys_out,
                                     const char* reason) {
  if (phys_out == 0) {
    return false;
  }
  float position[3] = {};
  if (!trick_pipeline::CurrentLocalBoardPosition(position)) {
    return false;
  }
  g_map_origin_x_bits.store(std::bit_cast<uint32_t>(position[0]),
                            std::memory_order_release);
  g_map_origin_y_bits.store(std::bit_cast<uint32_t>(position[1]),
                            std::memory_order_release);
  g_map_origin_z_bits.store(std::bit_cast<uint32_t>(position[2]),
                            std::memory_order_release);
  g_map_origin_phys_out.store(phys_out, std::memory_order_release);
  g_map_origin_valid.store(true, std::memory_order_release);
  g_owned_collision_floor_offset_bits.store(0,
                                             std::memory_order_release);
  g_owned_collision_floor_samples.store(0, std::memory_order_release);
  g_owned_collision_floor_recoveries.store(0,
                                            std::memory_order_release);
  g_owned_collision_last_surface.store(0, std::memory_order_release);
  g_owned_collision_last_normal_y_bits.store(
      std::bit_cast<uint32_t>(1.0f), std::memory_order_release);
  g_owned_collision_last_correction_check.store(
      0, std::memory_order_release);
  REXLOG_INFO(
      "mechanics-sandbox: owned map origin {} physout=0x{:08X} "
      "position=({:.3f},{:.3f},{:.3f})",
      reason, phys_out, position[0], position[1], position[2]);
  return true;
}

uint32_t LoadGuestU32(uint8_t* base, uint32_t address) {
  if (!base || !address) {
    return 0;
  }
  return REX_LOAD_U32(address);
}

float LoadGuestF32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(LoadGuestU32(base, address));
}

bool IsGuestHeapAddress(uint32_t address) {
  constexpr uint32_t kGuestHeapStart = 0x40000000u;
  constexpr uint32_t kGuestHeapEnd = 0x72000000u;
  return address >= kGuestHeapStart && address < kGuestHeapEnd;
}

void MaybeDeactivate() {
  if (REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  const State previous = g_state.exchange(State::Disabled);
  if (previous != State::Disabled) {
    RestorePresentation();
    g_map_origin_valid.store(false, std::memory_order_release);
    g_map_origin_phys_out.store(0, std::memory_order_release);
    g_map_contact_count.store(0, std::memory_order_relaxed);
    g_owned_collision_checks.store(0, std::memory_order_relaxed);
    g_owned_collision_corrections.store(0, std::memory_order_relaxed);
    g_owned_collision_step_rejections.store(0, std::memory_order_relaxed);
    g_owned_collision_floor_recoveries.store(0,
                                              std::memory_order_relaxed);
    g_owned_collision_last_surface.store(0, std::memory_order_relaxed);
    g_owned_collision_last_normal_y_bits.store(
        std::bit_cast<uint32_t>(1.0f), std::memory_order_relaxed);
    g_owned_collision_floor_offset_bits.store(0,
                                               std::memory_order_relaxed);
    g_owned_collision_floor_samples.store(0, std::memory_order_relaxed);
    g_owned_collision_last_correction_check.store(
        0, std::memory_order_relaxed);
    g_owned_collision_last_correction_frame.store(
        0, std::memory_order_relaxed);
    g_owned_ground_override_count.store(0, std::memory_order_relaxed);
    REXLOG_INFO("mechanics-sandbox: disabled; retail presentation restored");
  }
}

void TryActivate(uint64_t frame) {
  if (!REXCVAR_GET(skate3_mechanics_sandbox) ||
      g_local_actor.load(std::memory_order_acquire) == 0 ||
      g_local_presentation_entity.load(std::memory_order_acquire) == 0) {
    return;
  }
  const bool gameplay =
      rex::kernel::guest_presence::GameplayContextValue() == 1 &&
      REXCVAR_GET(skate3_native_render);
  State state = g_state.load(std::memory_order_acquire);
  if (gameplay && state != State::Active && state != State::ResetPending) {
    g_last_gameplay_frame.store(frame, std::memory_order_release);
    ApplySandboxPresentation();
    CaptureMapOriginFromOwnedPlayer(
        g_local_presentation_entity.load(std::memory_order_acquire),
        "captured");
    g_state.store(State::Active, std::memory_order_release);
    REXLOG_INFO(
        "mechanics-sandbox: active after stable freeroam; local actor=0x{:08X} "
        "physout=0x{:08X} presentation=local-only background=flat-clear "
        "collision=retail-bootstrap",
        g_local_actor.load(std::memory_order_acquire),
        g_local_presentation_entity.load(std::memory_order_acquire));
  }
}

}  // namespace

void ObserveLocalActionGraphActor(uint64_t frame, uint32_t actor) {
  MaybeDeactivate();
  if (!REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  if (actor == 0 || frame == 0) {
    if (g_state.load(std::memory_order_acquire) == State::Disabled) {
      g_state.store(State::WaitingForGameplay, std::memory_order_release);
    }
    return;
  }
  g_local_actor.store(actor, std::memory_order_release);
  g_last_gameplay_frame.store(frame, std::memory_order_release);
  TryActivate(frame);
}

void ObserveLocalPresentationEntity(uint64_t frame, uint32_t entity) {
  MaybeDeactivate();
  if (!REXCVAR_GET(skate3_mechanics_sandbox)) {
    return;
  }
  if (entity == 0 || frame == 0) {
    if (g_state.load(std::memory_order_acquire) == State::Disabled) {
      g_state.store(State::WaitingForGameplay, std::memory_order_release);
    }
    return;
  }
  g_local_presentation_entity.store(entity, std::memory_order_release);
  g_last_gameplay_frame.store(frame, std::memory_order_release);
  TryActivate(frame);
}

void ObserveLocalMotionState(uint64_t frame, uint32_t actor, bool on_ground,
                             uint32_t action_graph_actor, uint32_t phys_out,
                             bool player_owned, bool in_air) {
  if (actor == 0 || !Active()) {
    return;
  }
  if (player_owned && phys_out != 0 &&
      phys_out !=
          g_map_origin_phys_out.load(std::memory_order_acquire)) {
    CaptureMapOriginFromOwnedPlayer(phys_out, "rebased");
  }
  if (NativeCollisionObserverEnabled() && player_owned) {
    float position[3] = {};
    float origin[3] = {};
    if (trick_pipeline::CurrentLocalBoardPosition(position) &&
        SandboxMapOrigin(origin)) {
      const float local_position[3] = {position[0] - origin[0],
                                       position[1] - origin[1],
                                       position[2] - origin[2]};
      map::Contact contact;
      if (map::QueryContact(local_position, 0.18f, contact)) {
        RecordMapContact(true, contact.id, contact.normal,
                         contact.penetration);
      }
    }
  }
  const State state = g_state.load(std::memory_order_acquire);
  // GroundStatePredicateHook is shared by many actors. A nonzero actor and a
  // grounded bit are therefore insufficient evidence for reset completion.
  // Before a reset, remember only the ground entity observed concurrently
  // with the verified player-0 ActionGraph -> PhysOut ownership lane. While a
  // reset is pending, do not replace that identity with ambient actors.
  if (state != State::ResetPending && player_owned &&
      action_graph_actor == g_local_actor.load(std::memory_order_acquire) &&
      phys_out == g_local_presentation_entity.load(std::memory_order_acquire)) {
    g_verified_ground_entity.store(actor, std::memory_order_release);
  }
  if (state == State::ResetPending && on_ground && !in_air &&
      frame > g_reset_frame.load(std::memory_order_acquire) + 8 &&
      player_owned &&
      actor == g_reset_expected_ground_entity.load(std::memory_order_acquire) &&
      action_graph_actor ==
          g_reset_expected_action_actor.load(std::memory_order_acquire) &&
      phys_out == g_reset_expected_phys_out.load(std::memory_order_acquire)) {
    g_reset_completions.fetch_add(1, std::memory_order_relaxed);
    g_state.store(State::ResetSucceeded, std::memory_order_release);
    CaptureMapOriginFromOwnedPlayer(phys_out, "reset");
    REXLOG_INFO(
        "mechanics-sandbox: session-marker reset observed on verified player-0 "
        "ground entity=0x{:08X} action=0x{:08X} physout=0x{:08X}",
        actor, action_graph_actor, phys_out);
  }
}

bool Requested() { return REXCVAR_GET(skate3_mechanics_sandbox); }

bool Active() {
  const State state = g_state.load(std::memory_order_acquire);
  return state == State::Active || state == State::ResetPending ||
         state == State::ResetSucceeded;
}

DiagnosticMode CurrentDiagnosticMode() { return DiagnosticModeValue(); }

const char* DiagnosticModeName() {
  return DiagnosticModeNameFor(DiagnosticModeValue());
}

bool SetDiagnosticMode(const char* mode) {
  if (mode == nullptr) {
    return false;
  }
  DiagnosticMode next;
  if (std::strcmp(mode, "candidate_only") == 0 ||
      std::strcmp(mode, "candidate") == 0) {
    next = DiagnosticMode::CandidateOnly;
  } else if (std::strcmp(mode, "background_only") == 0 ||
             std::strcmp(mode, "background") == 0) {
    next = DiagnosticMode::BackgroundOnly;
  } else if (std::strcmp(mode, "all_dynamic") == 0 ||
             std::strcmp(mode, "all") == 0) {
    next = DiagnosticMode::AllDynamic;
  } else if (std::strcmp(mode, "candidate_world_on") == 0 ||
             std::strcmp(mode, "world_on") == 0) {
    next = DiagnosticMode::CandidateWorldOn;
  } else {
    return false;
  }
  g_diagnostic_mode.store(static_cast<uint8_t>(next), std::memory_order_release);
  if (Active()) {
    ApplySandboxPresentation();
  }
  REXLOG_INFO("mechanics-sandbox: diagnostic mode={}",
              DiagnosticModeNameFor(next));
  return true;
}

const char* StateName() {
  return StateNameFor(g_state.load(std::memory_order_acquire));
}

uint32_t LocalActor() { return g_local_actor.load(std::memory_order_acquire); }

uint32_t LocalPresentationEntity() {
  return g_local_presentation_entity.load(std::memory_order_acquire);
}

uint32_t LocalPresentationCandidate() {
  return g_presentation_candidate.load(std::memory_order_acquire);
}


void ObservePresentationCandidateRemoved(uint32_t entity) {
  if (!Active() || entity == 0) {
    return;
  }
  uint32_t candidate = entity;
  if (!g_presentation_candidate.compare_exchange_strong(
          candidate, 0, std::memory_order_acq_rel)) {
    return;
  }
  REXLOG_INFO(
      "mechanics-sandbox: local presentation candidate removed "
      "entity=0x{:08X}; awaiting replacement",
      entity);
}

bool VisualMapEnabled() {
  return Active() && REXCVAR_GET(skate3_mechanics_sandbox_visual_map);
}

bool NativeCollisionObserverEnabled() {
  return Active() &&
         REXCVAR_GET(skate3_mechanics_sandbox_collision_observer);
}

bool OwnedWorldCollisionEnabled() {
  return Active() &&
         REXCVAR_GET(skate3_mechanics_sandbox_owned_collision);
}

bool ShouldPublishOwnedWorldGround(uint64_t frame, uint32_t phys_out) {
  if (!OwnedWorldCollisionEnabled() || phys_out == 0 ||
      phys_out != g_map_origin_phys_out.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t correction_frame =
      g_owned_collision_last_correction_frame.load(
          std::memory_order_acquire);
  const bool active =
      correction_frame != 0 && frame >= correction_frame &&
      frame - correction_frame <= 2;
  if (active) {
    g_owned_ground_override_count.fetch_add(
        1, std::memory_order_relaxed);
  }
  return active;
}

void ApplyOwnedWorldCollisionAfterPhysOut(PPCContext& ctx, uint8_t* base,
                                          uint32_t controller,
                                          uint32_t phys_out) {
  // Runtime props on the RETAIL map, before any gate below.
  //
  // Everything past this point is about owned-map placement - the origin,
  // the phys_out that observed it, owned static installation - and Vanilla
  // Mode performs none of it, so the whole function returns early there. A
  // prop spawned onto the retail world still needs its collision
  // registered, and the collection it registers into is retail's own.
  //
  // Only when the owned collision path is OFF, so the sandbox keeps exactly
  // its existing single call per frame. UpdateEditableObjects self-limits to
  // runtime spawns, so this does nothing until something has been spawned.
  if (base != nullptr && !native_collision::Enabled()) {
    native_collision::UpdateEditableObjects(ctx, base);
  }
  if (!base || controller == 0 || phys_out == 0 ||
      phys_out != trick_pipeline::CurrentLocalPhysOut() ||
      phys_out != g_map_origin_phys_out.load(std::memory_order_acquire)) {
    return;
  }

  // SkateboardController ownership/layout is established by the existing
  // FillPhysOut observer: +428 is Skateboard*, +436 is ProcessedPhysIn*, and
  // +448 selects the active board transform at +112 or +192.
  const uint32_t skateboard = LoadGuestU32(base, controller + 428);
  const uint32_t processed_phys_in =
      LoadGuestU32(base, controller + 436);
  const uint32_t transform_state =
      LoadGuestU32(base, controller + 448);
  if (!IsGuestHeapAddress(skateboard) ||
      !IsGuestHeapAddress(processed_phys_in) ||
      transform_state > 4) {
    return;
  }

  const uint32_t transform =
      processed_phys_in + (transform_state == 3 ? 112u : 192u);
  float position[3] = {
      LoadGuestF32(base, transform + 48),
      LoadGuestF32(base, transform + 52),
      LoadGuestF32(base, transform + 56),
  };
  if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
      !std::isfinite(position[2])) {
    return;
  }

  float origin[3] = {};
  if (!SandboxMapOrigin(origin)) {
    return;
  }

  native_collision::EnsureInstalled(ctx, base, skateboard, origin);
  map_editor::ObservePlayerState();
  native_collision::UpdateEditableObjects(ctx, base);
  native_collision::UpdateKinematicObjects(ctx, base);
  native_collision::UpdateHingedDoors(ctx, base);
  native_grind::EnsureInstalled(ctx, base);
  trick_pipeline::LiveSpatialSnapshot collision_snapshot;
  if (trick_pipeline::CurrentLiveSpatialSnapshot(collision_snapshot) &&
      collision_snapshot.phys_out == phys_out) {
    native_collision::ObservePlayerCollisionTelemetry(
        position, collision_snapshot.frame);
  }

  if (!OwnedWorldCollisionEnabled()) {
    return;
  }

  float local_position[3] = {
      position[0] - origin[0],
      position[1] - origin[1],
      position[2] - origin[2],
  };

  const uint64_t check =
      g_owned_collision_checks.fetch_add(1, std::memory_order_relaxed) + 1;
  constexpr float kRecoveryProbeAbove = 64.0f;
  constexpr float kProbeBelow = 8.0f;
  map::GroundHit ground;
  if (!map::QueryGround(local_position, kRecoveryProbeAbove, kProbeBelow,
                        ground) ||
      ground.normal[1] < 0.45f) {
    return;
  }

  const bool calibration_floor =
      ground.normal[1] > 0.99f && std::abs(ground.point[1]) < 0.02f;
  if (calibration_floor) {
    // The transform origin is not the wheel contact point. Learn its settled
    // offset from any level zero-height park tile and preserve that clearance
    // when the board transitions onto a ramp or obstacle.
    const uint32_t samples =
        g_owned_collision_floor_samples.load(std::memory_order_relaxed);
    if (samples < 30 &&
        std::abs(local_position[1] - ground.point[1]) < 0.5f) {
      const float measured = local_position[1] - ground.point[1];
      const float previous =
          std::bit_cast<float>(g_owned_collision_floor_offset_bits.load(
              std::memory_order_relaxed));
      const float filtered =
          samples == 0 ? measured : previous + (measured - previous) * 0.08f;
      g_owned_collision_floor_offset_bits.store(
          std::bit_cast<uint32_t>(filtered), std::memory_order_relaxed);
      g_owned_collision_floor_samples.store(samples + 1,
                                             std::memory_order_relaxed);
    }
  }

  const float floor_offset =
      std::bit_cast<float>(g_owned_collision_floor_offset_bits.load(
          std::memory_order_relaxed));
  if (g_owned_collision_floor_samples.load(std::memory_order_relaxed) < 30) {
    return;
  }
  float target_y =
      origin[1] + ground.point[1] + floor_offset;
  float correction = target_y - position[1];
  // A board already more than four centimetres above the surface is airborne.
  // A large
  // upward step is a wall/high edge, not a ramp entry, and must not teleport
  // the player onto the top.
  constexpr float kMaximumLandingDistance = 0.04f;
  constexpr float kMaximumStepUp = 0.22f;
  const uint64_t previous_correction_check =
      g_owned_collision_last_correction_check.load(
          std::memory_order_relaxed);
  const bool continuing_surface =
      ground.id ==
          g_owned_collision_last_surface.load(std::memory_order_relaxed) &&
      check <= previous_correction_check + 4;
  bool recovered_to_lowest_ground = false;
  if (correction > kMaximumStepUp && !continuing_surface) {
    // A board that has penetrated below the park can be more than the old
    // four-metre probe above its support. Recover to the lowest upward-facing
    // skateable surface at this X/Z, selecting the floor below overlapping
    // ramps and pads rather than teleporting onto their tops.
    map::GroundHit recovery_ground;
    if (map::QueryLowestGround(local_position, kRecoveryProbeAbove,
                               kProbeBelow, recovery_ground) &&
        recovery_ground.normal[1] >= 0.99f) {
      const float recovery_target_y =
          origin[1] + recovery_ground.point[1] + floor_offset;
      const float recovery_correction =
          recovery_target_y - position[1];
      if (recovery_correction > kMaximumStepUp) {
        ground = recovery_ground;
        target_y = recovery_target_y;
        correction = recovery_correction;
        recovered_to_lowest_ground = true;
      }
    }
  }
  if (correction < -kMaximumLandingDistance ||
      (correction > kMaximumStepUp && !continuing_surface &&
       !recovered_to_lowest_ground)) {
    g_owned_collision_step_rejections.fetch_add(
        1, std::memory_order_relaxed);
    return;
  }
  if (recovered_to_lowest_ground) {
    g_owned_collision_floor_recoveries.fetch_add(
        1, std::memory_order_relaxed);
  }

  position[1] = target_y;

  float matrix[16] = {};
  for (uint32_t component = 0; component < 16; ++component) {
    matrix[component] = LoadGuestF32(
        base, transform + component * sizeof(float));
  }
  matrix[12] = position[0];
  matrix[13] = position[1];
  matrix[14] = position[2];

  // The transform rows are right/up/forward/translation. On an inclined
  // surface, project the retail forward axis onto the contact plane and
  // rebuild an orthonormal frame. Also return to level on the first flat
  // correction after a ramp. Ordinary flat-ground frames preserve retail
  // orientation so manuals and grounded animation are not flattened.
  const float previous_normal_y =
      std::bit_cast<float>(g_owned_collision_last_normal_y_bits.load(
          std::memory_order_relaxed));
  const bool align_to_surface =
      ground.normal[1] < 0.999f || previous_normal_y < 0.999f;
  if (align_to_surface) {
    float up[3] = {
        ground.normal[0], ground.normal[1], ground.normal[2]};
    const float up_length = std::sqrt(
        up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    float forward[3] = {matrix[8], matrix[9], matrix[10]};
    if (up_length > 1.0e-5f) {
      for (float& component : up) {
        component /= up_length;
      }
      const float forward_up =
          forward[0] * up[0] + forward[1] * up[1] +
          forward[2] * up[2];
      for (uint32_t component = 0; component < 3; ++component) {
        forward[component] -= up[component] * forward_up;
      }
      float forward_length = std::sqrt(
          forward[0] * forward[0] + forward[1] * forward[1] +
          forward[2] * forward[2]);
      if (forward_length < 1.0e-5f) {
        // Degenerate only when the current forward is almost vertical.
        // Recover heading from the current right axis.
        const float right_up =
            matrix[0] * up[0] + matrix[1] * up[1] +
            matrix[2] * up[2];
        float projected_right[3] = {
            matrix[0] - up[0] * right_up,
            matrix[1] - up[1] * right_up,
            matrix[2] - up[2] * right_up,
        };
        const float right_length = std::sqrt(
            projected_right[0] * projected_right[0] +
            projected_right[1] * projected_right[1] +
            projected_right[2] * projected_right[2]);
        if (right_length > 1.0e-5f) {
          for (float& component : projected_right) {
            component /= right_length;
          }
          forward[0] =
              projected_right[1] * up[2] - projected_right[2] * up[1];
          forward[1] =
              projected_right[2] * up[0] - projected_right[0] * up[2];
          forward[2] =
              projected_right[0] * up[1] - projected_right[1] * up[0];
          forward_length = 1.0f;
        }
      }
      if (forward_length > 1.0e-5f) {
        for (float& component : forward) {
          component /= forward_length;
        }
        float right[3] = {
            up[1] * forward[2] - up[2] * forward[1],
            up[2] * forward[0] - up[0] * forward[2],
            up[0] * forward[1] - up[1] * forward[0],
        };
        const float right_length = std::sqrt(
            right[0] * right[0] + right[1] * right[1] +
            right[2] * right[2]);
        if (right_length > 1.0e-5f) {
          for (float& component : right) {
            component /= right_length;
          }
          // Recompute forward to remove accumulated basis error.
          forward[0] = right[1] * up[2] - right[2] * up[1];
          forward[1] = right[2] * up[0] - right[0] * up[2];
          forward[2] = right[0] * up[1] - right[1] * up[0];
          for (uint32_t component = 0; component < 3; ++component) {
            matrix[component] = right[component];
            matrix[4 + component] = up[component];
            matrix[8 + component] = forward[component];
          }
        }
      }
    }
  }

  // SetTransform retains its source matrix throughout a 352-byte guest stack
  // frame, so keep the argument below that frame.
  const uint32_t matrix_address = ctx.r1.u32 - 512u;
  for (uint32_t component = 0; component < 16; ++component) {
    REX_STORE_U32(
        matrix_address + component * sizeof(float),
        std::bit_cast<uint32_t>(matrix[component]));
  }

  PPCContext correction_ctx = ctx;
  const uint32_t skateboard_body = LoadGuestU32(base, skateboard + 12);
  if (!IsGuestHeapAddress(skateboard_body)) {
    return;
  }
  correction_ctx.r3.u64 = skateboard_body;
  correction_ctx.r4.u64 = matrix_address;
  sub_82C0B2C8(correction_ctx, base);
  // FillPhysOut samples this active ProcessedPhysIn matrix. Publish the same
  // corrected transform so the following mechanics frame cannot immediately
  // reintroduce hidden retail terrain height or level a ramp-aligned board.
  for (uint32_t component = 0; component < 16; ++component) {
    REX_STORE_U32(
        transform + component * sizeof(float),
        std::bit_cast<uint32_t>(matrix[component]));
  }

  g_owned_collision_corrections.fetch_add(1,
                                           std::memory_order_relaxed);
  g_owned_collision_last_surface.store(ground.id,
                                        std::memory_order_release);
  g_owned_collision_last_normal_y_bits.store(
      std::bit_cast<uint32_t>(ground.normal[1]),
      std::memory_order_release);
  g_owned_collision_last_target_y_bits.store(
      std::bit_cast<uint32_t>(target_y), std::memory_order_release);
  g_owned_collision_last_correction_check.store(
      check, std::memory_order_release);
  trick_pipeline::LiveSpatialSnapshot snapshot;
  if (trick_pipeline::CurrentLiveSpatialSnapshot(snapshot) &&
      snapshot.phys_out == phys_out) {
    g_owned_collision_last_correction_frame.store(
        snapshot.frame, std::memory_order_release);
  }
}

namespace {
// A one-shot write here does not stick: FillPhysOut re-derives the active
// transform every tick from a separate internal state (unrelated to
// anything this file or sub_82C0B2C8 touches) and diffs/blends it back
// toward ProcessedPhysIn, so a single patch only survives "one stale tick" -
// exactly the same limitation ApplyOwnedWorldCollisionAfterPhysOut's own
// comment above describes for its ground-snap correction. That function
// gets away with a per-tick patch only because it *keeps re-applying it,
// every tick, indefinitely*. A teleport can't do that forever (the player
// needs to be able to skate away from the new spot), so instead it
// re-applies the same fixed target for a short window of ticks, long
// enough to outlast the transient reversion.
constexpr int kTeleportHoldTicks = 30;
std::atomic<int> g_teleport_ticks_remaining{0};
std::atomic<uint32_t> g_teleport_x_bits{0};
std::atomic<uint32_t> g_teleport_y_bits{0};
std::atomic<uint32_t> g_teleport_z_bits{0};
// The player's own position (trick_pipeline::CurrentLocalBoardPosition) at
// the moment RequestLocalPlayerTeleport was called - the origin end of the
// rigid delta the bone-palette patch applies every hold tick (see
// kSkaterIdentityLockTicks's neighbor below). Captured once per request,
// not re-derived per tick, so a fresh delta is not silently shrunk to zero
// once the physics object itself has already been teleported.
std::atomic<uint32_t> g_teleport_origin_x_bits{0};
std::atomic<uint32_t> g_teleport_origin_y_bits{0};
std::atomic<uint32_t> g_teleport_origin_z_bits{0};
std::atomic<bool> g_teleport_origin_valid{false};

// Local SkaterPresEntity identity, established by continuous position
// correlation against the verified local SkateboardController (see
// UpdateLocalSkaterEntityIdentity) rather than the order-of-first-sight
// heuristic g_presentation_candidate uses - that heuristic is known to
// misidentify other skaters/NPCs (see ApplyPendingTeleportAfterPhysOut's
// own long-standing comment), which made every earlier rider-follow patch
// attempt untestable: there was no way to tell a wrong-entity patch from a
// wrong-field patch. Position correlation reuses entity+416
// (m_MatLtoWTrans), already proven bit-exact against the game's own
// accepted constant bank elsewhere in native_entity, so a close, sustained
// match to the physics object's own position is real evidence of identity,
// not a guess.
constexpr int kSkaterIdentityLockTicks = 45;  // ~0.75s at 60Hz
constexpr float kSkaterIdentityMaxDistance = 2.2f;  // meters
std::atomic<uint32_t> g_local_skater_entity{0};
std::atomic<uint32_t> g_local_skater_candidate{0};
std::atomic<int> g_local_skater_streak{0};
// The human skater's Physics::Skeleton (SkateboardController+432),
// republished every tick so non-guest threads (the Lua natives) can read a
// handle without walking guest memory themselves.
std::atomic<uint32_t> g_local_skeleton{0};

// Called every FillPhysOut tick for the verified local phys_out (not just
// while a teleport is pending), so a lock is already established by the
// time a teleport is actually requested. Finds the closest live skater-
// family entity to the physics object's own current position; if the SAME
// entity stays closest (and within kSkaterIdentityMaxDistance) for
// kSkaterIdentityLockTicks consecutive ticks, it becomes the lock. An
// existing lock is left alone on a transient miss (occlusion/culling)
// rather than dropped immediately - only a new, different, equally-
// sustained candidate can replace it.
void UpdateLocalSkaterEntityIdentity(uint8_t* base, const float local_pos[3]) {
  uint32_t best = 0;
  float best_d2 = kSkaterIdentityMaxDistance * kSkaterIdentityMaxDistance;
  for (uint32_t entity : native_entity::LiveSkaterFamilyEntities()) {
    float pos[3];
    if (!native_entity::ReadSkaterEntityWorldPosition(base, entity, pos)) {
      continue;
    }
    const float dx = pos[0] - local_pos[0];
    const float dy = pos[1] - local_pos[1];
    const float dz = pos[2] - local_pos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = entity;
    }
  }
  const uint32_t candidate = g_local_skater_candidate.load(std::memory_order_relaxed);
  if (best != 0 && best == candidate) {
    const int streak =
        g_local_skater_streak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (streak == kSkaterIdentityLockTicks) {
      const uint32_t prev =
          g_local_skater_entity.exchange(best, std::memory_order_release);
      if (prev != best) {
        REXLOG_INFO(
            "mechanics-sandbox: local skater identity locked entity=0x{:08X} "
            "(was 0x{:08X}) distance={:.3f}",
            best, prev, std::sqrt(best_d2));
      }
    }
  } else {
    if (REXCVAR_GET(skate3_mechanics_sandbox_rider_identity_log) &&
        best != candidate) {
      REXLOG_INFO(
          "mechanics-sandbox: local skater identity candidate changed "
          "0x{:08X} -> 0x{:08X}",
          candidate, best);
    }
    g_local_skater_candidate.store(best, std::memory_order_relaxed);
    g_local_skater_streak.store(best != 0 ? 1 : 0, std::memory_order_relaxed);
  }
}

// ---- Retail's own reposition path ------------------------------------
//
// Recovered from the game's internal script-native table (908 entries;
// names + handler addresses live in the executable's data segment). The
// native TeleportLocalPlayerToSessionMarker maps to sub_82D5CA98, which is
// six instructions long: fetch a teleport manager, load the object at
// manager+0x28, and call one virtual method on it. It takes no arguments.
//
// sub_82D5CA98 reaches the manager through a script context we do not
// have, but RepositionAI (sub_8284A950) reaches the SAME manager through a
// pure static pointer chain, visible in the register setup immediately
// before its own sub_82D54C90 call: 0x830CFD94 -> +0xDC -> +0x184. We use
// that chain instead.
//
// This matters because it is the game's OWN reposition logic - the code
// path a restart-at-marker already uses - so unlike every hand-rolled
// transform patch above it moves the board, the rider and the camera
// coherently, which is exactly what those patches could not do.
constexpr uint32_t kSkaterManagerGlobal = 0x830CFD94;
constexpr uint32_t kTeleportManagerChain0 = 0xDC;
constexpr uint32_t kTeleportManagerChain1 = 0x184;
constexpr uint32_t kTeleportTargetObject = 0x28;
constexpr uint32_t kTeleportToMarkerVtableSlot = 0x20;

std::atomic<bool> g_session_marker_teleport_pending{false};

// ---- Retail's own teleport MESSAGE ------------------------------------
//
// The engine routes repositions through a message queue rather than a
// setter, which is why no "set position" field exists to patch. The class
// reflection table in the executable's data segment gives each message a
// literal 32-bit id, a total size, and its field layout:
//
//   cMsgTeleport               id 0x8FCDC5E2  size 0x60  mat4@+16, 2 bools@+80
//   cMsgSpawnPlayerAtPosition  id 0x04AE9407  size 0x50  mat4@+16
//
// The queue object hangs off the same global the teleport manager does:
// with G = *(u32*)0x830CFD94, the queue is G+0x40, its critical section
// G+0x50, and the sender id it stamps into the record G+0x44.
//
// Posting cMsgTeleport ourselves is a byte-for-byte replay of what
// SetSessionMarker (sub_82D55AE8) does, with our own matrix substituted -
// so it drives the identical retail code path that already moves the whole
// character correctly, but to arbitrary coordinates.
constexpr uint32_t kMsgTeleportId = 0x8FCDC5E2;
constexpr uint32_t kMsgTeleportSize = 0x60;
constexpr uint32_t kMsgQueueOffset = 0x40;
constexpr uint32_t kMsgQueueSenderOffset = 0x44;
constexpr uint32_t kMsgQueueCriticalSection = 0x50;
constexpr uint32_t kMsgQueueIndex = 3;

std::atomic<bool> g_message_teleport_pending{false};
// Ticks a pending cMsgTeleport will keep trying for before giving up.
//
// A teleport can legitimately be asked for on a tick where the controller
// is not ready to take one - right after connecting, mid map load, during a
// respawn. Consuming the request on that tick would drop it silently, which
// for a game mode means a player who simply never arrives at the play zone
// with nothing in any log to say why. Retrying across a short window turns
// that into "it lands as soon as the engine can", and the window has an end
// so a permanently unready controller reports rather than retries forever.
// ~2s at the physics tick rate.
constexpr int kMessageTeleportRetryTicks = 120;
std::atomic<int> g_message_teleport_retries{0};
std::atomic<uint32_t> g_message_teleport_x_bits{0};
std::atomic<uint32_t> g_message_teleport_y_bits{0};
std::atomic<uint32_t> g_message_teleport_z_bits{0};

// Guest pointers here live in the executable's own data/static region as
// well as the heap, so IsGuestHeapAddress (heap-only) is too strict.
bool PlausibleGuestPointer(uint32_t address) {
  return address >= 0x10000 && address < 0x84A10000 && (address & 3) == 0;
}

// Posts cMsgTeleport with `matrix` (a Matrix44 whose row 3 carries the
// destination, matching the SkateboardController transform convention this
// file already uses). The record and the sender slot must live in GUEST
// memory - sub_82B938D8 reads both through the guest address space - so
// they go in a real stack frame below the caller's, leaving the callee's
// own frames room underneath.
bool PostTeleportMessage(PPCContext& ctx, uint8_t* base, const float matrix[16]) {
  const uint32_t global = LoadGuestU32(base, kSkaterManagerGlobal);
  if (!PlausibleGuestPointer(global)) {
    return false;
  }
  const uint32_t queue = global + kMsgQueueOffset;
  const uint32_t sender = LoadGuestU32(base, global + kMsgQueueSenderOffset);
  const uint32_t critical_section = global + kMsgQueueCriticalSection;

  const uint32_t saved_sp = ctx.r1.u32;
  const uint32_t frame_sp = (saved_sp - 384u) & ~0xFu;
  REX_STORE_U32(frame_sp, saved_sp);  // back chain
  const uint32_t record = frame_sp + 256u;   // 16-aligned: the payload copy
  const uint32_t sender_slot = frame_sp + 240u;  // is a vector move
  ctx.r1.u32 = frame_sp;

  for (uint32_t offset = 0; offset < kMsgTeleportSize; offset += 4) {
    REX_STORE_U32(record + offset, 0);
  }
  REX_STORE_U32(record + 0, kMsgTeleportId);
  REX_STORE_U32(record + 8, kMsgTeleportSize);
  for (uint32_t component = 0; component < 16; ++component) {
    REX_STORE_U32(record + 16 + component * sizeof(float),
                  std::bit_cast<uint32_t>(matrix[component]));
  }
  // The two trailing bools: retail's own marker post always sets the first.
  REX_STORE_U8(record + 80, 1);
  REX_STORE_U8(record + 81, 0);
  REX_STORE_U32(sender_slot, sender);

  ctx.r3.u64 = critical_section;
  ctx.lr = 0x82D55BC4;
  __imp__RtlEnterCriticalSection(ctx, base);

  ctx.r3.u64 = record;
  ctx.r4.u64 = sender_slot;
  ctx.r5.u64 = kMsgQueueIndex;
  ctx.lr = 0x82D55BDC;
  sub_82B938D8(ctx, base);

  ctx.r3.u64 = critical_section;
  ctx.lr = 0x82D55BE4;
  __imp__RtlLeaveCriticalSection(ctx, base);

  ctx.r1.u32 = saved_sp;
  (void)queue;
  return true;
}

bool RunSessionMarkerTeleport(PPCContext& ctx, uint8_t* base) {
  const uint32_t skater_manager = LoadGuestU32(base, kSkaterManagerGlobal);
  if (!PlausibleGuestPointer(skater_manager)) {
    return false;
  }
  const uint32_t intermediate =
      LoadGuestU32(base, skater_manager + kTeleportManagerChain0);
  if (!PlausibleGuestPointer(intermediate)) {
    return false;
  }
  const uint32_t manager =
      LoadGuestU32(base, intermediate + kTeleportManagerChain1);
  if (!PlausibleGuestPointer(manager)) {
    return false;
  }
  const uint32_t target = LoadGuestU32(base, manager + kTeleportTargetObject);
  if (!PlausibleGuestPointer(target)) {
    return false;
  }
  const uint32_t vtable = LoadGuestU32(base, target);
  if (!PlausibleGuestPointer(vtable)) {
    return false;
  }
  const uint32_t method =
      LoadGuestU32(base, vtable + kTeleportToMarkerVtableSlot);
  if (!PlausibleGuestPointer(method)) {
    return false;
  }

  REXLOG_INFO(
      "mechanics-sandbox: session-marker teleport manager=0x{:08X} "
      "target=0x{:08X} vtable=0x{:08X} method=0x{:08X}",
      manager, target, vtable, method);

  const PPCContext saved = ctx;
  ctx.r3.u64 = target;
  // The return address sub_82D5CA98's own call site publishes, so anything
  // walking the link register sees a genuine retail frame.
  ctx.lr = 0x82D5CABC;
  REX_CALL_INDIRECT_FUNC(method);
  ctx = saved;
  return true;
}

// Diagnostic only - see skate3_mechanics_sandbox_actor_position_scan's own
// comment. Scans a bounded window of the actor's own memory for a
// consecutive float3 close to known_pos (the verified board position,
// available only while riding). IsGuestHeapAddress-range reads are safe
// here (this project's whole guest address space is one committed
// mapping - reading past an object's real allocation returns unrelated
// data, never faults), so a generous scan window is fine.
void ScanActorForPositionOffset(uint8_t* base, uint32_t actor,
                                const float known_pos[3]) {
  constexpr uint32_t kScanBytes = 8192;
  constexpr float kMatchTolerance = 0.01f;
  static std::atomic<uint32_t> s_scan_logged{0};
  for (uint32_t offset = 0; offset + 12 <= kScanBytes; offset += 4) {
    const float x = LoadGuestF32(base, actor + offset);
    const float y = LoadGuestF32(base, actor + offset + 4);
    const float z = LoadGuestF32(base, actor + offset + 8);
    const float dx = x - known_pos[0];
    const float dy = y - known_pos[1];
    const float dz = z - known_pos[2];
    if (std::fabs(dx) <= kMatchTolerance && std::fabs(dy) <= kMatchTolerance &&
        std::fabs(dz) <= kMatchTolerance) {
      if (s_scan_logged.fetch_add(1, std::memory_order_relaxed) < 64) {
        REXLOG_INFO(
            "mechanics-sandbox: actor position-scan match actor=0x{:08X} "
            "offset=+{} value=({:.3f},{:.3f},{:.3f}) known=({:.3f},{:.3f},{:.3f})",
            actor, offset, x, y, z, known_pos[0], known_pos[1], known_pos[2]);
      }
    }
  }
}
}  // namespace

uint32_t LocalSkaterPresEntity() {
  return g_local_skater_entity.load(std::memory_order_acquire);
}

uint32_t LocalSkaterSkeleton() {
  return g_local_skeleton.load(std::memory_order_relaxed);
}

void RequestSessionMarkerTeleport() {
  g_session_marker_teleport_pending.store(true, std::memory_order_release);
}

void RequestMessageTeleport(float x, float y, float z) {
  g_message_teleport_x_bits.store(std::bit_cast<uint32_t>(x),
                                  std::memory_order_relaxed);
  g_message_teleport_y_bits.store(std::bit_cast<uint32_t>(y),
                                  std::memory_order_relaxed);
  g_message_teleport_z_bits.store(std::bit_cast<uint32_t>(z),
                                  std::memory_order_relaxed);
  g_message_teleport_retries.store(kMessageTeleportRetryTicks,
                                   std::memory_order_relaxed);
  g_message_teleport_pending.store(true, std::memory_order_release);
}

void RequestLocalPlayerTeleport(float x, float y, float z) {
  g_teleport_x_bits.store(std::bit_cast<uint32_t>(x), std::memory_order_relaxed);
  g_teleport_y_bits.store(std::bit_cast<uint32_t>(y), std::memory_order_relaxed);
  g_teleport_z_bits.store(std::bit_cast<uint32_t>(z), std::memory_order_relaxed);
  float origin[3];
  if (trick_pipeline::CurrentLocalBoardPosition(origin)) {
    g_teleport_origin_x_bits.store(std::bit_cast<uint32_t>(origin[0]),
                                   std::memory_order_relaxed);
    g_teleport_origin_y_bits.store(std::bit_cast<uint32_t>(origin[1]),
                                   std::memory_order_relaxed);
    g_teleport_origin_z_bits.store(std::bit_cast<uint32_t>(origin[2]),
                                   std::memory_order_relaxed);
    g_teleport_origin_valid.store(true, std::memory_order_relaxed);
  } else {
    g_teleport_origin_valid.store(false, std::memory_order_relaxed);
  }
  g_teleport_ticks_remaining.store(kTeleportHoldTicks, std::memory_order_release);
}

void ApplyPendingTeleportAfterPhysOut(PPCContext& ctx, uint8_t* base,
                                      uint32_t controller,
                                      uint32_t phys_out) {
  if (base && controller != 0 && phys_out != 0 &&
      phys_out == trick_pipeline::CurrentLocalPhysOut()) {
    // Continuous identity tracking regardless of whether a teleport is
    // pending, so the lock is already established by the time one is
    // actually requested - see UpdateLocalSkaterEntityIdentity's comment.
    g_local_skeleton.store(LoadGuestU32(base, controller + 432),
                           std::memory_order_relaxed);
    float local_pos[3];
    if (trick_pipeline::CurrentLocalBoardPosition(local_pos)) {
      UpdateLocalSkaterEntityIdentity(base, local_pos);
      if (REXCVAR_GET(skate3_mechanics_sandbox_actor_position_scan)) {
        const uint32_t actor = g_local_actor.load(std::memory_order_relaxed);
        // Expensive (2048 guest reads); once every ~30 ticks (~0.5s) is
        // plenty for a diagnostic that's looking for a STABLE offset.
        static std::atomic<uint32_t> s_scan_tick{0};
        if (actor != 0 &&
            s_scan_tick.fetch_add(1, std::memory_order_relaxed) % 30 == 0) {
          ScanActorForPositionOffset(base, actor, local_pos);
        }
      }
    }
  }

  // Retail reposition request (see RunSessionMarkerTeleport). Consumed here
  // because this is the one place with both a live PPCContext and the guest
  // thread; the flag is cleared BEFORE the call so a fault inside retail's
  // own teleport cannot turn into a re-entry loop on the next tick.
  if (base != nullptr &&
      g_session_marker_teleport_pending.exchange(false,
                                                 std::memory_order_acquire)) {
    if (!RunSessionMarkerTeleport(ctx, base)) {
      REXLOG_WARN(
          "mechanics-sandbox: session-marker teleport skipped - manager chain "
          "did not resolve (not in gameplay yet?)");
    }
  }

  // Arbitrary-coordinate teleport via retail's own cMsgTeleport.
  //
  // The request is NOT consumed just because this tick came round: it is
  // held until the controller is actually in a state to take it, or the
  // retry window runs out. It is still cleared BEFORE the call itself, so a
  // fault inside retail's own teleport cannot become a re-entry loop.
  if (base != nullptr &&
      g_message_teleport_pending.load(std::memory_order_acquire)) {
    const uint32_t processed_phys_in =
        controller != 0 ? LoadGuestU32(base, controller + 436) : 0;
    const uint32_t transform_state =
        controller != 0 ? LoadGuestU32(base, controller + 448) : 0;
    const bool ready = controller != 0 &&
                       IsGuestHeapAddress(processed_phys_in) &&
                       transform_state <= 4;
    if (!ready) {
      const int remaining_retries =
          g_message_teleport_retries.fetch_sub(1, std::memory_order_relaxed) - 1;
      if (remaining_retries <= 0) {
        g_message_teleport_pending.store(false, std::memory_order_relaxed);
        REXLOG_WARN(
            "mechanics-sandbox: cMsgTeleport gave up after {} ticks - the "
            "skateboard controller never became ready (phys_in=0x{:08X} "
            "transform_state={})",
            kMessageTeleportRetryTicks, processed_phys_in, transform_state);
      }
    } else {
      g_message_teleport_pending.store(false, std::memory_order_relaxed);
      // Start from the live transform so the destination inherits the
      // player's current orientation, then replace only the translation.
      const uint32_t transform =
          processed_phys_in + (transform_state == 3 ? 112u : 192u);
      float matrix[16];
      for (uint32_t component = 0; component < 16; ++component) {
        matrix[component] =
            LoadGuestF32(base, transform + component * sizeof(float));
      }
      matrix[12] = std::bit_cast<float>(
          g_message_teleport_x_bits.load(std::memory_order_relaxed));
      matrix[13] = std::bit_cast<float>(
          g_message_teleport_y_bits.load(std::memory_order_relaxed));
      matrix[14] = std::bit_cast<float>(
          g_message_teleport_z_bits.load(std::memory_order_relaxed));
      const bool posted = PostTeleportMessage(ctx, base, matrix);
      REXLOG_INFO(
          "mechanics-sandbox: cMsgTeleport post {} -> ({:.2f},{:.2f},{:.2f})",
          posted ? "sent" : "FAILED (queue global did not resolve)", matrix[12],
          matrix[13], matrix[14]);
    }
  }

  const int remaining = g_teleport_ticks_remaining.load(std::memory_order_acquire);
  if (remaining <= 0) {
    return;
  }
  if (!base || controller == 0 || phys_out == 0 ||
      phys_out != trick_pipeline::CurrentLocalPhysOut()) {
    return;
  }
  // Benign race: a concurrent RequestLocalPlayerTeleport may overwrite this
  // with a fresh kTeleportHoldTicks in between the load above and this
  // store - that just means the new request's hold window wins, which is
  // the desired behavior (a newer teleport always restarts the window).
  g_teleport_ticks_remaining.store(remaining - 1, std::memory_order_relaxed);

  // Same SkateboardController layout as ApplyOwnedWorldCollisionAfterPhysOut
  // above: +428 is Skateboard*, +436 is ProcessedPhysIn*, +448 selects the
  // active board transform at +112 or +192.
  const uint32_t skateboard = LoadGuestU32(base, controller + 428);
  const uint32_t processed_phys_in = LoadGuestU32(base, controller + 436);
  const uint32_t transform_state = LoadGuestU32(base, controller + 448);
  if (!IsGuestHeapAddress(skateboard) ||
      !IsGuestHeapAddress(processed_phys_in) || transform_state > 4) {
    return;
  }

  const uint32_t transform =
      processed_phys_in + (transform_state == 3 ? 112u : 192u);

  float matrix[16];
  for (uint32_t component = 0; component < 16; ++component) {
    matrix[component] = LoadGuestF32(base, transform + component * sizeof(float));
  }
  // Preserve current orientation (rows 0-2 of the 4x4); overwrite only the
  // translation row so a teleport never fights the retail-owned facing.
  matrix[12] = std::bit_cast<float>(g_teleport_x_bits.load(std::memory_order_relaxed));
  matrix[13] = std::bit_cast<float>(g_teleport_y_bits.load(std::memory_order_relaxed));
  matrix[14] = std::bit_cast<float>(g_teleport_z_bits.load(std::memory_order_relaxed));

  // SetTransform retains its source matrix throughout a 352-byte guest stack
  // frame, so keep the argument below that frame (same slot used above).
  const uint32_t matrix_address = ctx.r1.u32 - 512u;
  for (uint32_t component = 0; component < 16; ++component) {
    REX_STORE_U32(matrix_address + component * sizeof(float),
                  std::bit_cast<uint32_t>(matrix[component]));
  }

  PPCContext correction_ctx = ctx;
  const uint32_t skateboard_body = LoadGuestU32(base, skateboard + 12);
  if (!IsGuestHeapAddress(skateboard_body)) {
    return;
  }
  correction_ctx.r3.u64 = skateboard_body;
  correction_ctx.r4.u64 = matrix_address;
  sub_82C0B2C8(correction_ctx, base);

  // FillPhysOut samples this active ProcessedPhysIn matrix. Publish the same
  // teleported transform so the following mechanics frame cannot
  // immediately reintroduce the pre-teleport position for one stale tick.
  for (uint32_t component = 0; component < 16; ++component) {
    REX_STORE_U32(transform + component * sizeof(float),
                  std::bit_cast<uint32_t>(matrix[component]));
  }

  // EXPERIMENTAL rider follow: earlier attempts (a PresentationEntity
  // world-matrix patch, then a render-side proxy/clone mechanism modeled on
  // remote-peer rendering) were untestable, because the "nearest owned
  // mesh" identification heuristic used to pick the target entity can
  // misidentify OTHER skaters/NPCs - a wrong-entity patch and a wrong-field
  // patch look identical from the outside. UpdateLocalSkaterEntityIdentity
  // now gives a position-verified lock instead, so this at least isolates
  // the remaining unknown to a single question: does patching
  // SkaterPresEntity::m_MatLtoWTrans's translation (entity+416, the same
  // field native_entity already proves is read for rigid/ROPA garment
  // world serving) also move the primary skinned skeleton, or only rigid-
  // attached sub-meshes? That's unverified until checked in-game - hence
  // the cvar gate.
  if (REXCVAR_GET(skate3_mechanics_sandbox_teleport_patch_rider)) {
    const uint32_t local_skater_entity = LocalSkaterPresEntity();
    if (IsGuestHeapAddress(local_skater_entity)) {
      constexpr uint32_t kSkaterWorldOffset = 416;  // m_MatLtoWTrans
      float ent_matrix[16];
      for (uint32_t component = 0; component < 16; ++component) {
        ent_matrix[component] =
            LoadGuestF32(base, local_skater_entity + kSkaterWorldOffset +
                                   component * sizeof(float));
      }
      // Same structural sanity native_entity's own ReadWorldRowsChecked
      // uses (homogeneous (0,0,0,1) tail row): refuse to patch anything
      // that doesn't look like a live, well-formed Matrix44, so a
      // misclassified/mid-teardown entity can't corrupt unrelated memory.
      const bool tail_ok =
          std::fabs(ent_matrix[12]) <= 1e-4f &&
          std::fabs(ent_matrix[13]) <= 1e-4f &&
          std::fabs(ent_matrix[14]) <= 1e-4f &&
          std::fabs(ent_matrix[15] - 1.0f) <= 1e-4f;
      if (tail_ok) {
        // Preserve orientation; overwrite only the translation embedded in
        // column 3 of rows 0-2 (this field's own layout - distinct from the
        // SkateboardController matrix above, which carries translation in
        // row 3 instead).
        ent_matrix[3] = matrix[12];
        ent_matrix[7] = matrix[13];
        ent_matrix[11] = matrix[14];
        for (uint32_t component = 0; component < 16; ++component) {
          REX_STORE_U32(local_skater_entity + kSkaterWorldOffset +
                            component * sizeof(float),
                        std::bit_cast<uint32_t>(ent_matrix[component]));
        }
        static std::atomic<uint32_t> s_rider_patch_logged{0};
        const uint32_t ln =
            s_rider_patch_logged.fetch_add(1, std::memory_order_relaxed);
        if (ln < 8) {
          REXLOG_INFO(
              "mechanics-sandbox: teleport rider-entity patch applied "
              "entity=0x{:08X} -> ({:.2f},{:.2f},{:.2f})",
              local_skater_entity, matrix[12], matrix[13], matrix[14]);
        }
      }
    }
  }

  // The human skater. Physics::Skeleton carries its own 64-byte world
  // Matrix44 at +11920, translation row at +11968, and a dirty byte at
  // +16417 that makes the per-part local matrices at +12624 rebuild from
  // it (see sub_82BDDD70/sub_82BDE060, retail's own place-the-skeleton
  // routines, which write exactly these three things). Held across the
  // same window as the board patch above, for the same reason.
  if (REXCVAR_GET(skate3_mechanics_sandbox_teleport_patch_skeleton)) {
    constexpr uint32_t kControllerSkeleton = 432;
    constexpr uint32_t kSkeletonWorldTranslation = 11968;
    constexpr uint32_t kSkeletonDirtyByte = 16417;
    const uint32_t skeleton = LoadGuestU32(base, controller + kControllerSkeleton);
    if (IsGuestHeapAddress(skeleton)) {
      REX_STORE_U32(skeleton + kSkeletonWorldTranslation,
                    std::bit_cast<uint32_t>(matrix[12]));
      REX_STORE_U32(skeleton + kSkeletonWorldTranslation + 4,
                    std::bit_cast<uint32_t>(matrix[13]));
      REX_STORE_U32(skeleton + kSkeletonWorldTranslation + 8,
                    std::bit_cast<uint32_t>(matrix[14]));
      REX_STORE_U8(skeleton + kSkeletonDirtyByte, 1);
      static std::atomic<uint32_t> s_skeleton_patch_logged{0};
      if (s_skeleton_patch_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_INFO(
            "mechanics-sandbox: teleport skeleton patch applied "
            "skeleton=0x{:08X} -> ({:.2f},{:.2f},{:.2f})",
            skeleton, matrix[12], matrix[13], matrix[14]);
      }
    }
  }

  // EXPERIMENTAL rider follow, attempt 2: rigidly translate every bone/
  // rigid-part world matrix owned by the locked entity, by the SAME delta
  // (target - pre-teleport origin) applied to the SkateboardController
  // above, rather than overwriting a single coarse world matrix. Applied
  // every hold tick (not just once) on the assumption this palette gets
  // re-derived from an untouched base position each tick, same as the
  // physics correction this mirrors.
  if (REXCVAR_GET(skate3_mechanics_sandbox_teleport_patch_bones) &&
      g_teleport_origin_valid.load(std::memory_order_relaxed)) {
    const uint32_t local_skater_entity = LocalSkaterPresEntity();
    if (IsGuestHeapAddress(local_skater_entity)) {
      const float dx = matrix[12] - std::bit_cast<float>(
                                        g_teleport_origin_x_bits.load(
                                            std::memory_order_relaxed));
      const float dy = matrix[13] - std::bit_cast<float>(
                                        g_teleport_origin_y_bits.load(
                                            std::memory_order_relaxed));
      const float dz = matrix[14] - std::bit_cast<float>(
                                        g_teleport_origin_z_bits.load(
                                            std::memory_order_relaxed));
      uint32_t patched_instances = 0;
      uint32_t patched_rows = 0;
      for (uint32_t instance :
           native_entity::EntityInstances(local_skater_entity)) {
        if (!IsGuestHeapAddress(instance)) {
          continue;
        }
        const uint32_t matrices = LoadGuestU32(base, instance + 0x14);
        const uint32_t count = LoadGuestU32(base, instance + 0x18);
        if (!IsGuestHeapAddress(matrices) || count < 1 || count > 96) {
          continue;
        }
        for (uint32_t row = 0; row < count; ++row) {
          const uint32_t row_addr = matrices + row * 48u;
          float m[12];
          for (uint32_t component = 0; component < 12; ++component) {
            m[component] = LoadGuestF32(
                base, row_addr + component * sizeof(float));
          }
          // Same row-sanity ServeInstancePalette applies (packed 3x3
          // rotation norms in a plausible range): refuse to patch anything
          // that doesn't look like a live bone matrix.
          float norm = 0.0f;
          for (int j = 0; j < 3; ++j) {
            norm += m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j];
          }
          if (!(norm > 0.0016f && norm < 2000.0f)) {
            continue;
          }
          m[3] += dx;
          m[7] += dy;
          m[11] += dz;
          for (uint32_t component = 0; component < 12; ++component) {
            REX_STORE_U32(row_addr + component * sizeof(float),
                          std::bit_cast<uint32_t>(m[component]));
          }
          ++patched_rows;
        }
        ++patched_instances;
      }
      static std::atomic<uint32_t> s_bone_patch_logged{0};
      const uint32_t ln =
          s_bone_patch_logged.fetch_add(1, std::memory_order_relaxed);
      if (ln < 8 && patched_instances > 0) {
        REXLOG_INFO(
            "mechanics-sandbox: teleport bone-palette patch applied "
            "entity=0x{:08X} instances={} rows={} delta=({:.2f},{:.2f},{:.2f})",
            local_skater_entity, patched_instances, patched_rows, dx, dy, dz);
      }
    }
  }

  // NOTE: the above relocates the SkateboardController's own invisible
  // physics object (always) and, experimentally, the position-locked
  // SkaterPresEntity's world (skate3_mechanics_sandbox_teleport_patch_rider,
  // confirmed NOT to move the visible rider) or its bone/rigid-part palettes
  // (skate3_mechanics_sandbox_teleport_patch_bones, unverified). Watch the
  // "bone-palette patch applied" log line during an in-game teleport test
  // and report what actually moves.
}

bool ObserveSandboxCamera(const float camera[3]) {
  if (!Active() || camera == nullptr ||
      g_map_origin_valid.load(std::memory_order_acquire)) {
    return false;
  }
  // Camera fallback is only for a visual frame before the board spatial seam
  // publishes. It is never accepted as ownership proof for collision data.
  g_map_origin_x_bits.store(std::bit_cast<uint32_t>(camera[0]),
                            std::memory_order_release);
  g_map_origin_y_bits.store(std::bit_cast<uint32_t>(camera[1]),
                            std::memory_order_release);
  g_map_origin_z_bits.store(std::bit_cast<uint32_t>(camera[2]),
                            std::memory_order_release);
  g_map_origin_phys_out.store(0, std::memory_order_release);
  g_map_origin_valid.store(true, std::memory_order_release);
  return true;
}

bool SandboxMapOrigin(float out_origin[3]) {
  if (out_origin == nullptr ||
      !g_map_origin_valid.load(std::memory_order_acquire)) {
    return false;
  }
  out_origin[0] = std::bit_cast<float>(
      g_map_origin_x_bits.load(std::memory_order_acquire));
  out_origin[1] = std::bit_cast<float>(
      g_map_origin_y_bits.load(std::memory_order_acquire));
  out_origin[2] = std::bit_cast<float>(
      g_map_origin_z_bits.load(std::memory_order_acquire));
  return std::isfinite(out_origin[0]) && std::isfinite(out_origin[1]) &&
         std::isfinite(out_origin[2]);
}

bool SandboxMapRenderOrigin(float out_origin[3]) {
  if (!SandboxMapOrigin(out_origin)) {
    return false;
  }
  if (native_collision::MapWorldOrigin(out_origin)) {
    return true;
  }
  if (OwnedWorldCollisionEnabled() &&
      g_owned_collision_floor_samples.load(std::memory_order_acquire) >= 30) {
    // The board transform sits near the deck centre, not at the bottom of the
    // wheels. Put the visible plane below the transform/contact target so the
    // board and wheels render above it instead of intersecting it.
    constexpr float kBoardVisualClearance = 0.055f;
    out_origin[1] +=
        std::bit_cast<float>(g_owned_collision_floor_offset_bits.load(
            std::memory_order_acquire)) -
        kBoardVisualClearance;
  }
  return std::isfinite(out_origin[1]);
}

void RecordMapContact(bool hit, uint32_t id, const float normal[3],
                      float penetration) {
  if (!hit) {
    return;
  }
  g_map_contact_count.fetch_add(1, std::memory_order_relaxed);
  g_map_last_contact_id.store(id, std::memory_order_relaxed);
  g_map_last_penetration_bits.store(std::bit_cast<uint32_t>(penetration),
                                    std::memory_order_relaxed);
  (void)normal;
}

PresentationDecision ClassifyPresentationEntity(uint32_t entity,
                                                 uint8_t entity_class) {
  if (!Active()) {
    return PresentationDecision::Keep;
  }
  const DiagnosticMode mode = DiagnosticModeValue();
  if (mode == DiagnosticMode::BackgroundOnly) {
    return PresentationDecision::DropNonLocal;
  }
  if (mode == DiagnosticMode::AllDynamic) {
    return PresentationDecision::Keep;
  }
  if (entity == 0) {
    return PresentationDecision::DropUnresolved;
  }
  // The verified player-0 PhysOut is not itself a PresentationEntity. Until
  // the direct PhysOut->SkaterPresEntity link is instrumented, retain the
  // first plain SkaterPresEntity as the local candidate.
  constexpr uint8_t kSkaterPresEntityClass =
      static_cast<uint8_t>(native_entity::EntClass::kSkater);
  uint32_t candidate = g_presentation_candidate.load(std::memory_order_acquire);
  if (candidate == 0 && entity_class == kSkaterPresEntityClass) {
    if (g_presentation_candidate.compare_exchange_strong(
            candidate, entity, std::memory_order_acq_rel)) {
      REXLOG_INFO(
          "mechanics-sandbox: provisional local presentation candidate="
          "0x{:08X} source=first-skater-after-player0-activation",
          entity);
      candidate = entity;
    }
  }
  if (entity == candidate) {
    return PresentationDecision::Keep;
  }

  // Skater-family entities are now part of the owned presentation world.
  // Their AI/mechanics can continue running while retail Living World and
  // scene actors are stopped at their spawn boundaries.
  const auto cls = static_cast<native_entity::EntClass>(entity_class);
  const bool skater_family =
      cls == native_entity::EntClass::kSkater ||
      cls == native_entity::EntClass::kColorized ||
      cls == native_entity::EntClass::kCac ||
      cls == native_entity::EntClass::kSkaterAux;
  if (skater_family) {
    g_visible_nonlocal_skater_items.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(g_visible_skater_mutex);
      g_visible_nonlocal_skaters_this_frame.insert(entity);
      g_visible_nonlocal_skater_entities.store(
          static_cast<uint32_t>(
              g_visible_nonlocal_skaters_this_frame.size()),
          std::memory_order_relaxed);
    }
    return PresentationDecision::Keep;
  }
  return PresentationDecision::DropNonLocal;
}

void BeginPresentationFrame() {
  if (!Active()) {
    return;
  }
  g_visible_items.store(0, std::memory_order_relaxed);
  g_dropped_nonlocal.store(0, std::memory_order_relaxed);
  g_dropped_unresolved.store(0, std::memory_order_relaxed);
  g_visible_nonlocal_skater_items.store(0, std::memory_order_relaxed);
  g_visible_nonlocal_skater_entities.store(0, std::memory_order_relaxed);
  g_candidate_draws.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_visible_skater_mutex);
    g_visible_nonlocal_skaters_this_frame.clear();
  }
}

void RecordPresentation(PresentationDecision decision) {
  switch (decision) {
    case PresentationDecision::Keep:
      g_visible_items.fetch_add(1, std::memory_order_relaxed);
      break;
    case PresentationDecision::DropNonLocal:
      g_dropped_nonlocal.fetch_add(1, std::memory_order_relaxed);
      break;
    case PresentationDecision::DropUnresolved:
      g_dropped_unresolved.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void RecordPresentationIdentity(uint32_t entity, uint32_t instance,
                                uint8_t entity_class) {
  if (!Active() || entity == 0) {
    return;
  }
  std::lock_guard lock(g_identity_log_mutex);
  if (g_logged_presentation_entities.size() >= 96 ||
      !g_logged_presentation_entities.insert(entity).second) {
    return;
  }
  REXLOG_INFO("mechanics-sandbox: presentation identity entity=0x{:08X} "
              "instance=0x{:08X} class={}{}",
              entity, instance, entity_class,
              entity == LocalPresentationEntity() ? " local-physout" : "");
}

void RecordRenderStage(RenderStage stage) {
  if (!Active()) {
    return;
  }
  g_render_stage.store(static_cast<uint8_t>(stage), std::memory_order_release);
  if (stage == RenderStage::Presented) {
    g_render_stage_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordRenderedItems(uint32_t draw_count) {
  if (!Active()) {
    return;
  }
  g_rendered_draws.store(draw_count, std::memory_order_relaxed);
  g_rendered_frames.fetch_add(1, std::memory_order_relaxed);
}

void RecordMapDraw(bool submitted) {
  if (submitted) {
    g_map_draws.fetch_add(1, std::memory_order_relaxed);
  }
}

void RecordMapChunks(uint32_t total, uint32_t candidates,
                     uint32_t visible, uint32_t occluded,
                     uint32_t resident,
                     uint32_t draw_calls) {
  g_map_chunk_count.store(total, std::memory_order_relaxed);
  g_map_candidate_chunks.store(candidates, std::memory_order_relaxed);
  g_map_visible_chunks.store(visible, std::memory_order_relaxed);
  g_map_occluded_chunks.store(occluded, std::memory_order_relaxed);
  g_map_resident_chunks.store(resident, std::memory_order_relaxed);
  g_map_chunk_draws.store(draw_calls, std::memory_order_relaxed);
}

void RecordMapEditorObjects(uint32_t total, uint32_t pose_ready,
                            uint32_t editor_pose_fallbacks,
                            uint32_t visible, uint32_t resident,
                            uint32_t draw_calls) {
  g_map_editor_objects.store(total, std::memory_order_relaxed);
  g_map_editor_pose_ready.store(pose_ready, std::memory_order_relaxed);
  g_map_editor_pose_fallbacks.store(editor_pose_fallbacks,
                                    std::memory_order_relaxed);
  g_map_editor_visible_objects.store(visible, std::memory_order_relaxed);
  g_map_editor_resident_objects.store(resident, std::memory_order_relaxed);
  g_map_editor_object_draws.store(draw_calls, std::memory_order_relaxed);
}

void RecordSkyDraw(uint32_t draw_calls) {
  g_sky_draws.store(draw_calls, std::memory_order_relaxed);
}

void RecordRenderedPresentation(uint32_t entity, uint32_t draw_count) {
  if (!Active() || entity == 0 || draw_count == 0) {
    return;
  }
  if (entity == g_presentation_candidate.load(std::memory_order_acquire)) {
    g_candidate_draws.fetch_add(draw_count, std::memory_order_relaxed);
    g_candidate_draw_frames.fetch_add(1, std::memory_order_relaxed);
  }
}

bool RequestReset() {
  if (!Active()) {
    return false;
  }
  const uint32_t ground = g_verified_ground_entity.load(std::memory_order_acquire);
  const uint32_t action = g_local_actor.load(std::memory_order_acquire);
  const uint32_t phys_out =
      g_local_presentation_entity.load(std::memory_order_acquire);
  if (ground == 0 || action == 0 || phys_out == 0) {
    g_reset_failures.fetch_add(1, std::memory_order_relaxed);
    g_state.store(State::ResetFailed, std::memory_order_release);
    REXLOG_WARN(
        "mechanics-sandbox: reset rejected; no verified player-0 ground "
        "identity (ground=0x{:08X} action=0x{:08X} physout=0x{:08X})",
        ground, action, phys_out);
    return false;
  }
  g_presentation_candidate.store(0, std::memory_order_release);
  g_reset_expected_ground_entity.store(ground, std::memory_order_release);
  g_reset_expected_action_actor.store(action, std::memory_order_release);
  g_reset_expected_phys_out.store(phys_out, std::memory_order_release);
  g_reset_frame.store(g_last_gameplay_frame.load(std::memory_order_acquire),
                      std::memory_order_release);
  g_reset_requests.fetch_add(1, std::memory_order_relaxed);
  g_state.store(State::ResetPending, std::memory_order_release);
  return true;
}

void AppendTelemetry(std::ostream& out) {
  const map::WaterTelemetry water = map::ActiveWaterTelemetry();
  const skate::world::PhysicsTelemetry physics =
      map::ActivePhysicsTelemetry();
  const map::WeatherSnapshot weather =
      map::ActiveWeatherSnapshot();
  const skate::world::DayNightState celestial =
      map::ActiveDayNightState();
  const native_scene::RaytracedMirrorTelemetry mirror =
      native_scene::GetRaytracedMirrorTelemetry();
  out << " sandbox_requested=" << (Requested() ? 1 : 0)
      << " sandbox_state=" << StateName()
      << " sandbox_active=" << (Active() ? 1 : 0)
      << " sandbox_local_actor=" << LocalActor()
      << " sandbox_local_presentation_entity=" << LocalPresentationEntity()
      << " sandbox_presentation_candidate="
      << g_presentation_candidate.load(std::memory_order_acquire)
      << " sandbox_visible_items="
      << g_visible_items.load(std::memory_order_relaxed)
      << " sandbox_dropped_nonlocal="
      << g_dropped_nonlocal.load(std::memory_order_relaxed)
      << " sandbox_dropped_unresolved="
      << g_dropped_unresolved.load(std::memory_order_relaxed)
      << " sandbox_visible_nonlocal_skater_items="
      << g_visible_nonlocal_skater_items.load(std::memory_order_relaxed)
      << " sandbox_visible_nonlocal_skater_entities="
      << g_visible_nonlocal_skater_entities.load(std::memory_order_relaxed)
      << " sandbox_diag_mode=" << DiagnosticModeName()
      << " sandbox_render_stage="
      << RenderStageNameFor(static_cast<RenderStage>(
             g_render_stage.load(std::memory_order_acquire)))
      << " sandbox_rendered_draws="
      << g_rendered_draws.load(std::memory_order_relaxed)
      << " sandbox_rendered_frames="
      << g_rendered_frames.load(std::memory_order_relaxed)
      << " sandbox_candidate_draws="
      << g_candidate_draws.load(std::memory_order_relaxed)
      << " sandbox_candidate_draw_frames="
      << g_candidate_draw_frames.load(std::memory_order_relaxed)
      << " sandbox_map_draws="
      << g_map_draws.load(std::memory_order_relaxed)
      << " sandbox_map_chunk_count="
      << g_map_chunk_count.load(std::memory_order_relaxed)
      << " sandbox_map_candidate_chunks="
      << g_map_candidate_chunks.load(std::memory_order_relaxed)
      << " sandbox_map_visible_chunks="
      << g_map_visible_chunks.load(std::memory_order_relaxed)
      << " sandbox_map_occluded_chunks="
      << g_map_occluded_chunks.load(std::memory_order_relaxed)
      << " sandbox_map_resident_chunks="
      << g_map_resident_chunks.load(std::memory_order_relaxed)
      << " sandbox_map_chunk_draws="
      << g_map_chunk_draws.load(std::memory_order_relaxed)
      << " sandbox_map_editor_objects="
      << g_map_editor_objects.load(std::memory_order_relaxed)
      << " sandbox_map_editor_pose_ready="
      << g_map_editor_pose_ready.load(std::memory_order_relaxed)
      << " sandbox_map_editor_pose_fallbacks="
      << g_map_editor_pose_fallbacks.load(std::memory_order_relaxed)
      << " sandbox_map_editor_visible_objects="
      << g_map_editor_visible_objects.load(std::memory_order_relaxed)
      << " sandbox_map_editor_resident_objects="
      << g_map_editor_resident_objects.load(std::memory_order_relaxed)
      << " sandbox_map_editor_object_draws="
      << g_map_editor_object_draws.load(std::memory_order_relaxed)
      << " sandbox_sky_draws="
      << g_sky_draws.load(std::memory_order_relaxed)
      << " sandbox_presented_frames="
      << g_render_stage_frames.load(std::memory_order_relaxed)
      << " sandbox_verified_ground_entity="
      << g_verified_ground_entity.load(std::memory_order_acquire)
      << " sandbox_reset_expected_ground_entity="
      << g_reset_expected_ground_entity.load(std::memory_order_acquire)
      << " sandbox_reset_expected_action_actor="
      << g_reset_expected_action_actor.load(std::memory_order_acquire)
      << " sandbox_reset_expected_physout="
      << g_reset_expected_phys_out.load(std::memory_order_acquire)
      << " sandbox_background=flat_clear"
      << " sandbox_visual_map=" << (VisualMapEnabled() ? 1 : 0)
      << " sandbox_map_origin_valid="
      << (g_map_origin_valid.load(std::memory_order_acquire) ? 1 : 0)
      << " sandbox_map_origin_physout="
      << g_map_origin_phys_out.load(std::memory_order_acquire)
      << " sandbox_map_contact_count="
      << g_map_contact_count.load(std::memory_order_relaxed)
      << " sandbox_map_last_contact_id="
      << g_map_last_contact_id.load(std::memory_order_relaxed)
      << " sandbox_map_source=owned"
      << " sandbox_map_name=" << map::ActiveMapName()
      << " sandbox_map_surface_count=" << map::ActiveSurfaceCount()
      << " sandbox_local_light_count=" << map::ActiveMovingLightCount()
      << " sandbox_map_ramp_count="
      << map::ActiveRampCount()
      << " sandbox_box3d_generation="
      << physics.world_generation
      << " sandbox_box3d_steps="
      << physics.world_steps
      << " sandbox_box3d_dropped_step_batches="
      << physics.dropped_step_batches
      << " sandbox_box3d_transform_updates="
      << physics.transform_updates
      << " sandbox_box3d_static_bodies="
      << physics.static_body_count
      << " sandbox_box3d_dynamic_bodies="
      << physics.dynamic_body_count
      << " sandbox_box3d_contacts="
      << physics.contact_count
      << " sandbox_box3d_sleeping_bodies="
      << physics.sleeping_body_count
      << " sandbox_box3d_player_proxy_active="
      << (physics.player_proxy_active ? 1 : 0)
      << " sandbox_box3d_player_contacts="
      << physics.player_contact_count
      << " sandbox_box3d_player_proxy_updates="
      << physics.player_proxy_updates
      << " sandbox_box3d_breakable_bodies="
      << physics.breakable_body_count
      << " sandbox_box3d_broken_groups="
      << physics.broken_group_count
      << " sandbox_box3d_glass_break_events="
      << physics.glass_break_events
      << " sandbox_box3d_last_broken_group="
      << physics.last_broken_group
      << " sandbox_box3d_last_break_speed_bits="
      << std::bit_cast<std::uint32_t>(physics.last_break_speed)
      << " sandbox_box3d_accumulator_bits="
      << std::bit_cast<std::uint64_t>(physics.accumulator_seconds)
      << " sandbox_box3d_representative_valid="
      << (physics.representative_dynamic_pose.valid ? 1 : 0)
      << " sandbox_box3d_representative_x_bits="
      << std::bit_cast<std::uint32_t>(
             physics.representative_dynamic_pose.position.x)
      << " sandbox_box3d_representative_y_bits="
      << std::bit_cast<std::uint32_t>(
             physics.representative_dynamic_pose.position.y)
      << " sandbox_box3d_representative_z_bits="
      << std::bit_cast<std::uint32_t>(
             physics.representative_dynamic_pose.position.z)
      << " sandbox_box3d_representative_basis_xx_bits="
      << std::bit_cast<std::uint32_t>(
             physics.representative_dynamic_pose.x_axis.x)
      << " sandbox_box3d_representative_awake="
      << (physics.representative_dynamic_pose.awake ? 1 : 0)
      << " sandbox_water_basin_count="
      << map::ActiveWaterBasinCount()
      << " sandbox_raytraced_mirror_count="
      << map::ActiveRaytracedMirrorCount()
      << " sandbox_raytraced_puddle_count="
      << map::ActiveRaytracedPuddleCount()
      << " sandbox_weather_elapsed_bits="
      << std::bit_cast<uint32_t>(weather.elapsed_seconds)
      << " sandbox_rain_intensity_bits="
      << std::bit_cast<uint32_t>(weather.rain_intensity)
      << " sandbox_lightning_flash_bits="
      << std::bit_cast<uint32_t>(weather.flash_intensity)
      << " sandbox_lightning_strikes="
      << weather.strike_count
      << " sandbox_thunder_events="
      << weather.thunder_count
      << " sandbox_day_night_elapsed_bits="
      << std::bit_cast<uint32_t>(celestial.elapsed_seconds)
      << " sandbox_dynamic_world_lighting="
      << (map::DynamicWorldLightingEnabled() ? 1 : 0)
      << " sandbox_day_night_phase_bits="
      << std::bit_cast<uint32_t>(celestial.phase)
      << " sandbox_time_of_day_hours_bits="
      << std::bit_cast<uint32_t>(celestial.time_of_day_hours)
      << " sandbox_sun_elevation_bits="
      << std::bit_cast<uint32_t>(
             celestial.sun_direction_to_light.y)
      << " sandbox_daylight_amount_bits="
      << std::bit_cast<uint32_t>(celestial.daylight_amount)
      << " sandbox_night_amount_bits="
      << std::bit_cast<uint32_t>(celestial.night_amount)
      << " sandbox_celestial_key="
      << (celestial.sun_is_key_light ? "sun" : "moon")
      << " sandbox_dxr_supported=" << (mirror.supported ? 1 : 0)
      << " sandbox_dxr_initialized=" << (mirror.initialized ? 1 : 0)
      << " sandbox_dxr_as_recorded="
      << (mirror.acceleration_structure_recorded ? 1 : 0)
      << " sandbox_dxr_dispatches=" << mirror.dispatches
      << " sandbox_dxr_width=" << mirror.width
      << " sandbox_dxr_height=" << mirror.height
      << " sandbox_dxr_triangles=" << mirror.triangle_count
      << " sandbox_dxr_dynamic_triangles="
      << mirror.dynamic_triangle_count
      << " sandbox_dxr_reflectors="
      << mirror.reflector_count
      << " sandbox_dxr_puddles="
      << mirror.puddle_count
      << " sandbox_water_steps=" << water.simulation_steps
      << " sandbox_water_dropped_frames=" << water.dropped_frames
      << " sandbox_water_min_bits="
      << std::bit_cast<uint32_t>(water.minimum_displacement)
      << " sandbox_water_max_bits="
      << std::bit_cast<uint32_t>(water.maximum_displacement)
      << " sandbox_water_mean_bits="
      << std::bit_cast<uint32_t>(water.mean_displacement)
      << " sandbox_water_energy_bits="
      << std::bit_cast<uint32_t>(water.kinetic_energy)
      << " sandbox_owned_collision="
      << (OwnedWorldCollisionEnabled() ? 1 : 0)
      << " sandbox_owned_collision_checks="
      << g_owned_collision_checks.load(std::memory_order_relaxed)
      << " sandbox_owned_collision_corrections="
      << g_owned_collision_corrections.load(std::memory_order_relaxed)
      << " sandbox_owned_collision_step_rejections="
      << g_owned_collision_step_rejections.load(std::memory_order_relaxed)
      << " sandbox_owned_collision_floor_recoveries="
      << g_owned_collision_floor_recoveries.load(
             std::memory_order_relaxed)
      << " sandbox_owned_collision_last_surface="
      << g_owned_collision_last_surface.load(std::memory_order_acquire)
      << " sandbox_owned_collision_last_normal_y_bits="
      << g_owned_collision_last_normal_y_bits.load(
             std::memory_order_acquire)
      << " sandbox_owned_collision_last_target_y_bits="
      << g_owned_collision_last_target_y_bits.load(
             std::memory_order_acquire)
      << " sandbox_owned_collision_floor_offset_bits="
      << g_owned_collision_floor_offset_bits.load(
             std::memory_order_relaxed)
      << " sandbox_owned_collision_floor_samples="
      << g_owned_collision_floor_samples.load(std::memory_order_relaxed)
      << " sandbox_owned_collision_last_correction_check="
      << g_owned_collision_last_correction_check.load(
             std::memory_order_relaxed)
      << " sandbox_owned_collision_last_correction_frame="
      << g_owned_collision_last_correction_frame.load(
             std::memory_order_relaxed)
      << " sandbox_owned_ground_override_count="
      << g_owned_ground_override_count.load(std::memory_order_relaxed)
      << " sandbox_collision="
      << (OwnedWorldCollisionEnabled()
              ? "owned_world_position_bridge"
              : (native_collision::Enabled()
                     ? "owned_native_clustered_mesh"
                     : "retail_flat_bootstrap"))
      << " sandbox_camera=retail_chase"
      << " sandbox_reset_requests="
      << g_reset_requests.load(std::memory_order_relaxed)
      << " sandbox_reset_completions="
      << g_reset_completions.load(std::memory_order_relaxed)
      << " sandbox_reset_failures="
      << g_reset_failures.load(std::memory_order_relaxed);
  native_collision::AppendTelemetry(out);
  map_editor::AppendTelemetry(out);
  native_grind::AppendTelemetry(out);
  owned_world_boundary::AppendTelemetry(out);
  multiplayer::AppendTelemetry(out);
}

}  // namespace skate3::mechanics_sandbox
