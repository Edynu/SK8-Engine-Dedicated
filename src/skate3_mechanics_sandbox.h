#pragma once

#include <cstdint>
#include <iosfwd>
#include <vector>

struct PPCContext;

namespace skate3::mechanics_sandbox {

enum class PresentationDecision : uint8_t {
  Keep = 0,
  DropNonLocal = 1,
  DropUnresolved = 2,
};

// Presentation-only diagnostic modes. CandidateOnly is the normal sandbox
// policy; the other modes are reversible probes for isolating a black frame.
enum class DiagnosticMode : uint8_t {
  CandidateOnly = 0,
  BackgroundOnly,
  AllDynamic,
  CandidateWorldOn,
};

enum class RenderStage : uint8_t {
  None = 0,
  Entered,
  YieldedForMenus,
  YieldedForPhoto,
  YieldedForMovie,
  SceneReady,
  PipelineReady,
  BackgroundCleared,
  MainPassComplete,
  HdrPostComplete,
  Presented,
};

// Called by the verified player-0 ActionGraph input lane. This is the only
// activation path: normal boot must reach gameplay before the presentation
// shell changes anything.
void ObserveLocalActionGraphActor(uint64_t frame, uint32_t actor);

// Called with the PhysOut proven by the player-0 action-graph ownership
// chain. This is the presentation identity used by the native scene filter;
// it is deliberately not inferred from the most-recent ground predicate.
void ObserveLocalPresentationEntity(uint64_t frame, uint32_t entity);

// Called by actor-scoped ground-state telemetry. It only completes a pending
// reset after the exact local actor returns to a grounded, non-air state.
void ObserveLocalMotionState(uint64_t frame, uint32_t actor, bool on_ground,
                             uint32_t action_graph_actor,
                             uint32_t phys_out, bool player_owned,
                             bool in_air);

bool Requested();
bool Active();
const char* StateName();
uint32_t LocalActor();
uint32_t LocalPresentationEntity();
// Provisional render-side PresentationEntity selected from the first plain
// local skater after player-0 activation. Unlike LocalPresentationEntity
// (the verified PhysOut), this value is directly comparable with
// native_entity::CtxInfo::entity.
uint32_t LocalPresentationCandidate();

// Wardrobe changes replace the local PresentationEntity. The renderer's
// verified view-removal seam clears only the exact current candidate so the
// next rendered skater entity can become the new local capture owner.
void ObservePresentationCandidateRemoved(uint32_t entity);

bool VisualMapEnabled();
bool NativeCollisionObserverEnabled();
bool OwnedWorldCollisionEnabled();
bool ShouldPublishOwnedWorldGround(uint64_t frame, uint32_t phys_out);
bool ObserveSandboxCamera(const float camera[3]);
bool SandboxMapOrigin(float out_origin[3]);
// Presentation uses the calibrated board-contact plane so the visible floor
// matches the position bridge rather than the earlier pre-settle origin.
bool SandboxMapRenderOrigin(float out_origin[3]);
void RecordMapContact(bool hit, uint32_t id, const float normal[3],
                      float penetration);

// Runs after the verified SkateboardController::FillPhysOut call. The bridge
// is default-off and corrects only the exact player-owned board vertically
// against owned-world floor/ramp surfaces. Retail still owns forces,
// orientation, and lateral contact.
void ApplyOwnedWorldCollisionAfterPhysOut(PPCContext& ctx, uint8_t* base,
                                          uint32_t controller,
                                          uint32_t phys_out);

// The local player's position-verified SkaterPresEntity (see
// UpdateLocalSkaterEntityIdentity in the .cpp), or 0 before a lock is
// established. Distinct from LocalPresentationCandidate: that is an
// order-of-first-sight heuristic that can latch onto another skater/NPC;
// this is continuously re-derived from a sustained position match against
// the verified local SkateboardController.
uint32_t LocalSkaterPresEntity();

// Teleports the local player to an arbitrary world position by posting the
// engine's OWN cMsgTeleport onto its message queue - the same queue
// SetSessionMarker posts to - rather than patching physics memory. The
// destination inherits the player's current orientation. Callable from any
// thread; posted from the guest thread at the next FillPhysOut boundary.
void RequestMessageTeleport(float x, float y, float z);

// The human skater's Physics::Skeleton (SkateboardController+432), or 0
// before gameplay. This is the rider's actual physics body - the object
// that owns the position the visible character is drawn at - as opposed to
// the Skateboard at +428 that entity 0 currently teleports.
uint32_t LocalSkaterSkeleton();

// Requests the RETAIL "teleport local player to the session marker" path
// (the game's own reposition code, reached the way its
// TeleportLocalPlayerToSessionMarker script native does). Callable from any
// thread; performed on the guest thread at the next FillPhysOut boundary.
// Unlike RequestLocalPlayerTeleport below this moves the whole character,
// not just the board - but only to wherever the session marker already is.
void RequestSessionMarkerTeleport();

// Requests a teleport of the local player's board to the given world
// position (orientation preserved). Callable from any thread - applied on
// the guest CPU thread starting the next time the local player's
// FillPhysOut hook runs, and re-applied for a short window of subsequent
// ticks (see kTeleportHoldTicks in the .cpp) because a single write does
// not survive retail's own per-tick transform recomputation. Entity handle
// 0 (the only entity Lua's SetEntityPosition native currently supports)
// always means "local player".
void RequestLocalPlayerTeleport(float x, float y, float z);

// Runs after the verified SkateboardController::FillPhysOut call, applying
// any pending RequestLocalPlayerTeleport for this exact phys_out. Must run
// after ApplyOwnedWorldCollisionAfterPhysOut so a same-tick teleport always
// wins over an owned-world ground-snap correction.
void ApplyPendingTeleportAfterPhysOut(PPCContext& ctx, uint8_t* base,
                                      uint32_t controller,
                                      uint32_t phys_out);

// Native-scene presentation policy. The scene renderer remains the only
// consumer; no generated guest update or mechanics path is disabled here.
PresentationDecision ClassifyPresentationEntity(uint32_t entity,
                                                 uint8_t entity_class);
DiagnosticMode CurrentDiagnosticMode();
const char* DiagnosticModeName();
bool SetDiagnosticMode(const char* mode);
void BeginPresentationFrame();
void RecordPresentation(PresentationDecision decision);
void RecordPresentationIdentity(uint32_t entity, uint32_t instance,
                                uint8_t entity_class);
void RecordRenderedPresentation(uint32_t entity, uint32_t draw_count);

void RecordRenderStage(RenderStage stage);
void RecordRenderedItems(uint32_t draw_count);
void RecordMapDraw(bool submitted);
void RecordMapChunks(uint32_t total, uint32_t candidates,
                     uint32_t visible, uint32_t occluded,
                     uint32_t resident,
                     uint32_t draw_calls);
void RecordMapEditorObjects(uint32_t total, uint32_t pose_ready,
                            uint32_t editor_pose_fallbacks,
                            uint32_t visible, uint32_t resident,
                            uint32_t draw_calls);
void RecordSkyDraw(uint32_t draw_calls);

// Harness reset uses the verified session-marker chord. This only records the
// reset lifecycle; it never writes a guest transform or velocity.
bool RequestReset();

// Appends compact machine-readable fields to STATUS/OBSERVE responses.
void AppendTelemetry(std::ostream& out);

}  // namespace skate3::mechanics_sandbox
