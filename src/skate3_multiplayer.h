#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::multiplayer {

inline constexpr std::uint32_t kCanonicalSkeletonTrackKey = 0x314E4143u;

// Renderer-neutral replicated pose. Positions are expressed in the active
// custom map's local coordinate frame so two recomp processes do not need to
// share the retail world's hidden absolute origin.
struct RemotePose {
  float position[3] = {};
  float x_axis[3] = {1.0f, 0.0f, 0.0f};
  float y_axis[3] = {0.0f, 1.0f, 0.0f};
  float z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::uint32_t board_state_flags = 0xFFFFFFFFu;
};

// Canonical model-to-world skeleton produced by Skate 3 before per-mesh bone
// remapping. Protocol v6 sends one kCanonicalSkeletonTrackKey track; each
// receiver rebuilds its locally loaded clothing palettes with the game's own
// captured mesh remaps. Each bone is three float4 affine rows; translations
// are map-local while in transit and restored to the active map origin.
struct AnimationTrack {
  std::uint32_t mesh_key = 0;
  std::vector<float> bone_rows;
};

struct AnimationPose {
  std::uint64_t sender_time_us = 0;
  std::uint32_t sequence = 0;
  std::uint16_t root_bone = 0xFFFFu;
  float root_position[3] = {};
  // Final rendered character root used by the lightweight pose stream and
  // receiver-side clone mapping. This deliberately stays separate from
  // root_position, which is the translation reference for encoded bones.
  bool presentation_root_valid = false;
  float presentation_root_position[3] = {};
  float presentation_root_x_axis[3] = {1.0f, 0.0f, 0.0f};
  float presentation_root_z_axis[3] = {0.0f, 0.0f, 1.0f};
  std::vector<AnimationTrack> tracks;
};

// Versioned engine-owned appearance payload. The primary format carries the
// sender's compact CAC recipe plus animation-binding metadata; the receiver
// resolves meshes and textures from its local vanilla asset catalogue.
// Runtime animation remains a separate compact stream. The older assembled
// mesh/texture bundle remains readable as a compatibility fallback.
struct AppearanceBlob {
  std::uint64_t identity = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

struct RemotePlayer {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  RemotePose pose;
  AnimationPose animation;
  AppearanceBlob appearance;
};

// One renderer-owned peer generation that the replication core has
// definitively replaced or forgotten. The session prevents a delayed
// retirement from releasing a newer occupant of the same reusable role.
struct RemotePeerRetirement {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
};

// Immutable prepared presentation published by the replication core. A
// worker can replace the shared player vector without mutating data currently
// consumed by the renderer; retirement events remain one-shot.
struct RemotePresentationFrame {
  std::uint64_t sequence = 0;
  std::shared_ptr<const std::vector<RemotePlayer>> players;
  std::vector<RemotePeerRetirement> retirements;
};

// The ONE exception to "everything here is render-thread-only": a
// cross-thread published copy of the most recent RemotePresentationFrame's
// players, so a Lua native (which can run on the render thread via a
// resource's Skate.CreateThread, or on the admin-HTTP thread pool via a
// console command - two different threads, both real) has something safe
// to read regardless of which one it landed on.
//
// PublishLatestRemotePlayers is called exactly once, from the render
// thread, right after TickLocalVisuals returns each frame (see the call
// site in skate3_native_scene_gpu.cpp) - never compute a position here,
// only publish what TickLocalVisuals already computed. That is deliberate:
// the smoothing/interpolation that turns raw network samples into a
// position lives in exactly one place (TickLocalVisuals), and duplicating
// it to reach a second thread would be two implementations of the same
// math drifting apart, not a feature.
//
// The atomic swap is real, not a formality: `players` is already a
// shared_ptr the render thread hands off wholesale each frame, so this is
// one atomic store of a pointer - no copy of the vector, no lock held
// across a read a Lua native might do at an inconvenient moment.
// The animation send rate this server wants, in Hz, from its /api/settings.
// Zero restores the local cvar. The server owns this because it is the one
// that knows how many players it has to fan the stream out to - the relay can
// thin a stream down for distant peers, but it cannot invent samples that
// were never sent, so the ceiling has to be set at the source.
void SetServerAnimationRate(int hz);

// The root-snapshot rate this server wants, in Hz. Separate from the
// animation rate because position and pose are sent on different schedules -
// position was defaulting to 60Hz while the pose ran at 10, which made it a
// fifth of the stream for detail nobody was asking for.
void SetServerPoseRate(int hz);

// Refresh rates for the finger and face bone groups, in Hz. Negative means
// the server has not specified and the local cvars apply; 0 means keyframes
// only. Both ends derive the same schedule from these, which is what keeps
// the sender and receiver agreeing on which frames carry those bones.
void SetServerDetailRates(int hands_hz, int face_hz);

void PublishLatestRemotePlayers(
    std::shared_ptr<const std::vector<RemotePlayer>> players);
// nullptr if nothing has been published yet (no session, or the first
// frame hasn't run) - never a dangling or half-written list.
std::shared_ptr<const std::vector<RemotePlayer>> LatestRemotePlayers();

// Samples the verified local board, services the current transport, and
// returns independently smoothed remote players alive on the same map. The
// first transport is localhost UDP; the packet and pose seam is deliberately
// independent from it so another transport can replace only networking.
bool TickLocalVisuals(const char* map_name,
                       const float map_render_origin[3],
                       const AnimationPose* local_animation,
                       const AppearanceBlob* local_appearance,
                       RemotePresentationFrame& out_presentation);

void AppendTelemetry(std::ostream& out);

// JSON twin of AppendTelemetry, for the admin HTTP /api/metrics route: just
// upload/download bandwidth and packet rates, not the full internal dump.
// Safe to call from any thread.
std::string NetworkTelemetryJson();

// One line for a "netstat" console command - the human-readable twin of
// NetworkTelemetryJson. Safe to call from any thread.
std::string FormatNetworkTelemetryLine();

// --- Script events -------------------------------------------------
//
// Lua net events ride the game protocol's general reliable channel (see
// skate3_multiplayer_protocol_v12_reliable.h), addressed to the dedicated
// server rather than to peers: the server is the authority and decides who
// an event reaches.

// Queues one event for the server. `json_args` is the argument list already
// encoded as a JSON array by the Lua host. Returns false when there is no
// server connection yet, or the channel refused the message - the caller
// should surface that rather than assume it was sent.
bool SendScriptEvent(std::string_view event, std::string_view json_args);

// Called on the network worker thread for every script event that arrives
// from the server, in the order the server queued them. Install before
// connecting; passing an empty handler drops inbound events.
using ScriptEventHandler =
    std::function<void(const std::string& event, const std::string& json_args)>;
void SetScriptEventHandler(ScriptEventHandler handler);

// Reports that the renderer committed the complete sender-owned appearance
// currently associated with this role, process session, and identity. Stale
// reports are ignored by the replication runtime.
void ReportRemoteAppearanceInstalled(std::uint32_t role,
                                     std::uint32_t session,
                                     std::uint64_t appearance_id);

}  // namespace skate3::multiplayer
