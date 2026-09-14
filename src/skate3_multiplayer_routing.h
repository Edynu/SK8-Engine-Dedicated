#pragma once

#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_protocol_v12_pose.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::multiplayer::routing {

enum class RouteMode : std::uint8_t {
  kDirectPeerMesh,
  kDedicatedRelay,
};

struct RouteRecommendation {
  RouteMode mode = RouteMode::kDirectPeerMesh;
  bool dedicated_preferred = false;
  bool supported = false;
};

[[nodiscard]] constexpr RouteRecommendation RecommendRoute(
    std::uint32_t participant_count, bool dedicated_available) {
  if (participant_count < 2 || participant_count > 100) {
    return {};
  }
  if (participant_count <= 20 || !dedicated_available) {
    return {
        .mode = RouteMode::kDirectPeerMesh,
        .dedicated_preferred = participant_count > 20,
        .supported = true,
    };
  }
  return {
      .mode = RouteMode::kDedicatedRelay,
      .dedicated_preferred = true,
      .supported = true,
  };
}

struct BandwidthProjection {
  double client_upload_bytes_per_second = 0.0;
  double client_download_bytes_per_second = 0.0;
  double relay_ingress_bytes_per_second = 0.0;
  double relay_egress_bytes_per_second = 0.0;
  double aggregate_bytes_per_second = 0.0;
};

[[nodiscard]] constexpr BandwidthProjection ProjectBandwidth(
    std::uint32_t participant_count, double stream_bytes_per_second,
    RouteMode mode) {
  if (participant_count < 2 || participant_count > 100 ||
      !(stream_bytes_per_second >= 0.0)) {
    return {};
  }
  const double peers = static_cast<double>(participant_count - 1);
  const double players = static_cast<double>(participant_count);
  if (mode == RouteMode::kDirectPeerMesh) {
    return {
        .client_upload_bytes_per_second = stream_bytes_per_second * peers,
        .client_download_bytes_per_second = stream_bytes_per_second * peers,
        .relay_ingress_bytes_per_second = 0.0,
        .relay_egress_bytes_per_second = 0.0,
        .aggregate_bytes_per_second =
            stream_bytes_per_second * players * peers,
    };
  }
  return {
      .client_upload_bytes_per_second = stream_bytes_per_second,
      .client_download_bytes_per_second = stream_bytes_per_second * peers,
      .relay_ingress_bytes_per_second = stream_bytes_per_second * players,
      .relay_egress_bytes_per_second =
          stream_bytes_per_second * players * peers,
      .aggregate_bytes_per_second =
          stream_bytes_per_second * players * players,
  };
}

struct RelayPeer {
  std::uint64_t connection_id = 0;
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  // Interest management: populated from the peer's periodic kPresenceBeacon
  // datagrams (skate3_multiplayer_protocol_v12.h), never from the opaque
  // realtime pose stream. map_hash stays 0 ("unknown") and position_valid
  // stays false until the first beacon arrives.
  std::uint64_t map_hash = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool position_valid = false;
  // Routing bucket ("dimension"): peers only see peers in the same bucket.
  // Bucket 0 is the default world. Server-assigned, never client-declared,
  // so a client cannot place itself into someone else's instance.
  std::uint32_t bucket = 0;
  // How many other peers share this peer's immediate area, as of the last
  // RefreshCrowdCounts(). Drives hot-zone thinning - see CrowdPolicy.
  std::uint32_t neighbours = 0;
  // Client-reported display name (POST /api/players/name). Empty until the
  // client has sent one - see skate3_dedicated_main.cpp's own comment on why
  // this is client-declared, unlike bucket/position.
  std::string name;
  std::uint64_t last_seen_us = 0;
};

enum class RelayDisposition : std::uint8_t {
  kForward,
  kUnknownConnection,
  kInvalidEnvelope,
  kSpoofedRole,
  kStaleSession,
  kInvalidTarget,
};

struct RelayRoute {
  RelayDisposition disposition = RelayDisposition::kInvalidEnvelope;
  std::vector<std::uint64_t> recipient_connections;
};

// Distance bands controlling how much of a peer's stream each recipient
// receives. Zero disables a band; zero across the board means every peer
// gets everything (the original unlimited behaviour).
//
// WHAT IS SAFE TO DROP, from reading the receiver in skate3_multiplayer.cpp:
//
//   * Pose DELTAS are each encoded against an installed BASELINE, not
//     against the previous delta (see the send path: baseline_id is the
//     baseline's sequence). They therefore do not chain, and dropping any
//     subset of them is safe - the receiver simply does not advance that
//     peer's pose until the next one arrives.
//
//   * BASELINES must never be dropped. The receiver checks each delta's
//     baseline_id against the keyframe it holds, and on a mismatch calls
//     NotifyBaselineUnavailable and REQUESTS a fresh baseline. Withholding
//     baselines while forwarding deltas would therefore trigger a stream of
//     baseline requests, and baselines are larger than deltas - thinning
//     that way would *increase* bandwidth rather than reduce it.
//
// Hence: thin deltas, always pass keyframes.
struct FidelityTiers {
  float high_distance = 0.0f;    // full stream
  float medium_distance = 0.0f;  // half the deltas
  float low_distance = 0.0f;     // sparse deltas (see kSparseDeltaInterval)
};

// How much of the stream a recipient at a given distance should receive.
enum class FidelityLevel : std::uint8_t {
  kFull,
  kHalfDeltas,
  // Historical name: this band no longer drops every delta, it keeps one in
  // kSparseDeltaInterval. Kept as-is because the ordering is what
  // DegradeForCrowd steps through and what the routing tests pin.
  kKeyframesOnly,
  kOutOfRange,
};

[[nodiscard]] inline FidelityLevel FidelityForDistanceSquared(
    const FidelityTiers& tiers, float distance_squared) {
  if (tiers.high_distance > 0.0f &&
      distance_squared <= tiers.high_distance * tiers.high_distance) {
    return FidelityLevel::kFull;
  }
  if (tiers.medium_distance > 0.0f &&
      distance_squared <= tiers.medium_distance * tiers.medium_distance) {
    return FidelityLevel::kHalfDeltas;
  }
  if (tiers.low_distance > 0.0f &&
      distance_squared <= tiers.low_distance * tiers.low_distance) {
    return FidelityLevel::kKeyframesOnly;
  }
  return FidelityLevel::kOutOfRange;
}

// How many pose deltas survive at each thinned band, as a divisor of the
// sender's own rate. A server sets these from the Hz it wants each band to
// run at: with a 20Hz sender, medium_divisor 2 gives 10Hz and low_divisor 4
// gives 5Hz. One means "no thinning".
//
// Configurable because the right answer depends entirely on how full the
// server is - four players can afford everything at full rate, twenty
// cannot - and that is the server's call, not the client's.
struct FidelityRates {
  std::uint32_t medium_divisor = 2;
  std::uint32_t low_divisor = 4;
};

// One in this many pose deltas survives at the lowest band.
//
// This used to be zero - the band passed keyframes and nothing else. With a
// keyframe once per second that left a distant skater updating at 1 Hz,
// which does not read as "lower detail", it reads as frozen, and no amount
// of receiver-side interpolation can invent the motion in between.
//
// Every delta is encoded against the KEYFRAME rather than against the
// previous delta, so any subset of them decodes correctly - thinning only
// lowers the update rate, it never breaks the chain.
inline constexpr std::uint32_t kSparseDeltaInterval = 4;

// Whether one datagram survives a given fidelity level. Thinning keys off
// the envelope sequence rather than per-pair counters, so it needs no state
// and spreads the drops evenly.
[[nodiscard]] inline bool PassesFidelity(FidelityLevel level,
                                         protocol_v12::MessageKind kind,
                                         std::uint8_t flags,
                                         std::uint32_t sequence,
                                         const FidelityRates& rates = {}) {
  if (level == FidelityLevel::kOutOfRange) {
    return false;
  }
  if (level == FidelityLevel::kFull) {
    return true;
  }
  // Everything that is not a pose delta - baselines, root snapshots,
  // control, appearance - is either self-contained or required for
  // correctness, and always passes.
  const bool is_delta = kind == protocol_v12::MessageKind::kPoseDelta &&
                        (flags & protocol_v12::EnvelopeFlag::kFlagKeyframe) == 0;
  if (!is_delta) {
    return true;
  }
  const std::uint32_t divisor = level == FidelityLevel::kHalfDeltas
                                    ? rates.medium_divisor
                                    : rates.low_divisor;
  if (divisor <= 1u) {
    return true;
  }
  return (sequence % divisor) == 0u;
}

// Hot zones: how much to thin a stream when players PILE UP.
//
// Distance bands alone do not bound anything. A relay's egress is
// O(recipients x senders), so sixty players standing on one spot are all
// inside each other's high band and every one of them pays for the other
// fifty-nine at full fidelity. That is the case that falls over, and it is
// exactly the case a skate spot produces - everyone gathers at the same
// ledge.
//
// So density degrades fidelity on top of distance: past `medium_above`
// neighbours everyone in that crowd drops one band, past `low_above` two.
// The effect is that per-peer cost falls as the crowd grows, which is what
// keeps a client's INBOUND bandwidth roughly flat instead of linear in the
// crowd size. It also degrades gracefully in the right direction: a dense
// scrum is where a single skater's exact bone pose matters least, because
// they are one of sixty and mostly occluded.
//
// Zero disables a threshold, so an unconfigured policy changes nothing.
struct CrowdPolicy {
  std::uint32_t medium_above = 0;
  std::uint32_t low_above = 0;
};

// Applies the crowd policy to a distance-derived level. `neighbours` is the
// count around the RECIPIENT, not the sender: the quantity being bounded is
// what one client has to receive.
[[nodiscard]] inline FidelityLevel DegradeForCrowd(
    FidelityLevel level, std::uint32_t neighbours,
    const CrowdPolicy& policy) {
  // Out of range stays out of range; nothing to degrade.
  if (level == FidelityLevel::kOutOfRange) {
    return level;
  }
  int steps = 0;
  if (policy.low_above != 0 && neighbours > policy.low_above) {
    steps = 2;
  } else if (policy.medium_above != 0 && neighbours > policy.medium_above) {
    steps = 1;
  }
  // Never past keyframes-only: dropping keyframes too would not thin a
  // stream, it would stop the peer updating at all, and a frozen skater
  // reads as a bug rather than as reduced detail.
  int degraded = static_cast<int>(level) + steps;
  const int floor_level = static_cast<int>(FidelityLevel::kKeyframesOnly);
  if (degraded > floor_level) {
    degraded = floor_level;
  }
  return static_cast<FidelityLevel>(degraded);
}

// Metadata-only visual relay. It authenticates sender identity and session,
// then forwards the original datagram bytes unchanged. It never needs retail
// skeletons, meshes, textures, or animation decoding.
class VisualRelayRouter {
 public:
  [[nodiscard]] bool Register(RelayPeer peer) {
    if (peer.connection_id == 0 || peer.role < 1 ||
        peer.role > protocol_v12::kMaximumRole ||
        peer.session == 0) {
      return false;
    }
    const auto role = role_to_connection_.find(peer.role);
    if (role != role_to_connection_.end() &&
        role->second != peer.connection_id) {
      peers_.erase(role->second);
    }
    const auto connection = peers_.find(peer.connection_id);
    if (connection != peers_.end() &&
        connection->second.role != peer.role) {
      role_to_connection_.erase(connection->second.role);
    }
    peers_[peer.connection_id] = peer;
    role_to_connection_[peer.role] = peer.connection_id;
    return true;
  }

  void Remove(std::uint64_t connection_id) {
    const auto peer = peers_.find(connection_id);
    if (peer == peers_.end()) {
      return;
    }
    role_to_connection_.erase(peer->second.role);
    peers_.erase(peer);
  }

  // Authenticates and routes a datagram whose sender role/session have
  // already been extracted by the caller - used for wire formats other than
  // the v12 envelope (see the v11 legacy header's shared 16-byte prefix in
  // skate3_multiplayer_protocol.h: magic/version/byte_count/sender_role/
  // sender_session, identical across PosePacket/AnimationFragmentPacket/
  // AppearanceFragmentPacket/ControlPacket). Route() below is the v12-decode
  // convenience wrapper around this.
  [[nodiscard]] RelayRoute RouteRaw(
      std::uint64_t source_connection,
      std::uint32_t sender_role,
      std::uint32_t sender_session,
      std::uint32_t target_role = 0,
      float radius = 0.0f,
      const FidelityTiers& tiers = {},
      protocol_v12::MessageKind kind = protocol_v12::MessageKind::kCapabilities,
      std::uint8_t flags = 0,
      std::uint32_t sequence = 0,
      const CrowdPolicy& crowd = {},
      const FidelityRates& rates = {}) const {
    const auto source = peers_.find(source_connection);
    if (source == peers_.end()) {
      return {
          .disposition = RelayDisposition::kUnknownConnection,
          .recipient_connections = {},
      };
    }
    if (sender_role != source->second.role) {
      return {
          .disposition = RelayDisposition::kSpoofedRole,
          .recipient_connections = {},
      };
    }
    if (sender_session != source->second.session) {
      return {
          .disposition = RelayDisposition::kStaleSession,
          .recipient_connections = {},
      };
    }

    RelayRoute result{
        .disposition = RelayDisposition::kForward,
        .recipient_connections = {},
    };
    if (target_role != 0) {
      const auto target = role_to_connection_.find(target_role);
      if (target == role_to_connection_.end() ||
          target->second == source_connection) {
        result.disposition = RelayDisposition::kInvalidTarget;
        return result;
      }
      result.recipient_connections.push_back(target->second);
      return result;
    }
    result.recipient_connections.reserve(peers_.size() - 1);
    for (const auto& [connection_id, peer] : peers_) {
      if (connection_id == source_connection) {
        continue;
      }
      // Interest management: never fan a packet out to a peer on a
      // different map. Both sides default to map_hash == 0 ("unknown")
      // until their first presence beacon arrives, so newly-joined peers
      // are not silently excluded before they've announced a map.
      if (peer.map_hash != source->second.map_hash) {
        continue;
      }
      // Routing buckets are the same idea one level up: separate instances
      // of the same map never see each other.
      if (peer.bucket != source->second.bucket) {
        continue;
      }
      if (source->second.position_valid && peer.position_valid) {
        const float dx = peer.x - source->second.x;
        const float dy = peer.y - source->second.y;
        const float dz = peer.z - source->second.z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (radius > 0.0f && distance_squared > radius * radius) {
          continue;
        }
        // Distance-banded fidelity, when configured. Only applied where
        // both ends have a known position; an unlocated peer keeps the full
        // stream rather than being thinned on a guess.
        if (tiers.high_distance > 0.0f || tiers.medium_distance > 0.0f ||
            tiers.low_distance > 0.0f) {
          const FidelityLevel level = DegradeForCrowd(
              FidelityForDistanceSquared(tiers, distance_squared),
              peer.neighbours, crowd);
          if (!PassesFidelity(level, kind, flags, sequence, rates)) {
            continue;
          }
        }
      }
      result.recipient_connections.push_back(connection_id);
    }
    std::sort(result.recipient_connections.begin(),
              result.recipient_connections.end());
    return result;
  }

  [[nodiscard]] RelayRoute Route(
      std::uint64_t source_connection,
      std::span<const std::uint8_t> datagram,
      std::uint32_t target_role = 0,
      float radius = 0.0f,
      const FidelityTiers& tiers = {},
      const CrowdPolicy& crowd = {},
      const FidelityRates& rates = {}) const {
    protocol_v12::Envelope envelope;
    if (!protocol_v12::DecodeEnvelope(datagram, envelope)) {
      return {
          .disposition = RelayDisposition::kInvalidEnvelope,
          .recipient_connections = {},
      };
    }
    // Thinning key: the POSE GROUP, not the datagram.
    //
    // A v12 pose group is split across several fragments with CONSECUTIVE
    // envelope sequences, so thinning on the envelope sequence drops every
    // other fragment and no group ever reassembles - the receiver sees a
    // permanently broken delta chain, keeps asking for baselines, and the
    // remote player collapses to the featureless proxy. That is a v12-only
    // hazard: a v11 animation frame is one datagram, so dropping alternate
    // datagrams really did just halve its rate.
    //
    // Every fragment of a group carries the same pose_id, so keying on that
    // drops or passes a group WHOLE, which is the only granularity at which
    // thinning a fragmented stream means anything.
    std::uint32_t fidelity_key = envelope.sequence;
    if (envelope.kind == protocol_v12::MessageKind::kPoseDelta) {
      protocol_v12::PoseGroupHeader header;
      if (protocol_v12::DecodePoseGroupHeader(
              datagram.subspan(protocol_v12::kEnvelopeBytes), envelope.kind,
              header)) {
        fidelity_key = header.pose_id;
      }
    }
    return RouteRaw(source_connection, envelope.sender_role,
                    envelope.sender_session, target_role, radius, tiers,
                    envelope.kind, envelope.flags, fidelity_key, crowd, rates);
  }

  // Applies a decoded presence beacon (skate3_multiplayer_protocol_v12.h's
  // PresenceBeacon) to the registered peer's interest-management state.
  // Returns false if the connection is not registered.
  [[nodiscard]] bool UpdatePresence(std::uint64_t connection_id,
                                    std::uint64_t map_hash, float x, float y,
                                    float z, std::uint64_t now_us) {
    const auto peer = peers_.find(connection_id);
    if (peer == peers_.end()) {
      return false;
    }
    peer->second.map_hash = map_hash;
    peer->second.x = x;
    peer->second.y = y;
    peer->second.z = z;
    peer->second.position_valid = true;
    peer->second.last_seen_us = now_us;
    return true;
  }

  // A copy of every registered peer, for callers outside the relay loop
  // (the server's Lua player registry). Deliberately a snapshot rather than
  // a reference into peers_: the relay mutates this map on its own thread
  // while scripts read it on another.
  [[nodiscard]] std::vector<RelayPeer> Peers() const {
    std::vector<RelayPeer> snapshot;
    snapshot.reserve(peers_.size());
    for (const auto& [connection_id, peer] : peers_) {
      (void)connection_id;
      snapshot.push_back(peer);
    }
    return snapshot;
  }

  // Moves a peer into a routing bucket. Server-side policy only - there is
  // deliberately no wire message for a client to set its own.
  [[nodiscard]] bool SetBucket(std::uint32_t role, std::uint32_t bucket) {
    const auto connection = role_to_connection_.find(role);
    if (connection == role_to_connection_.end()) {
      return false;
    }
    const auto peer = peers_.find(connection->second);
    if (peer == peers_.end()) {
      return false;
    }
    peer->second.bucket = bucket;
    return true;
  }

  // Records a peer's self-reported display name. Unlike SetBucket this
  // takes client-provided input, so the caller is expected to have already
  // length-capped/sanitized it (see skate3_dedicated_main.cpp) - this layer
  // just stores whatever string it is given.
  [[nodiscard]] bool SetName(std::uint32_t role, std::string name) {
    const auto connection = role_to_connection_.find(role);
    if (connection == role_to_connection_.end()) {
      return false;
    }
    const auto peer = peers_.find(connection->second);
    if (peer == peers_.end()) {
      return false;
    }
    peer->second.name = std::move(name);
    return true;
  }

  // Refreshes liveness without changing interest-management state (called
  // for every datagram a registered connection sends, beacon or otherwise).
  void Touch(std::uint64_t connection_id, std::uint64_t now_us) {
    const auto peer = peers_.find(connection_id);
    if (peer != peers_.end()) {
      peer->second.last_seen_us = now_us;
    }
  }

  // Evicts every connection not heard from within timeout_us and returns
  // their connection ids so the caller can release any owned socket state.
  [[nodiscard]] std::vector<std::uint64_t> RemoveStale(
      std::uint64_t now_us, std::uint64_t timeout_us) {
    std::vector<std::uint64_t> removed;
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (now_us - it->second.last_seen_us > timeout_us) {
        removed.push_back(it->first);
        role_to_connection_.erase(it->second.role);
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
    return removed;
  }

  [[nodiscard]] std::size_t peer_count() const { return peers_.size(); }

 private:
 public:
  // Recomputes every peer's neighbour count. O(peers^2), so it is called on
  // the relay's existing periodic sweep rather than per packet - a count
  // that is a second stale is harmless, because it only shifts a fidelity
  // band and the crowd it describes does not form in a second.
  //
  // Neighbours are counted within `high_distance` and only across peers that
  // could actually route to each other (same map, same bucket): players in
  // another instance of the same spot cost this one nothing, which is the
  // whole point of buckets.
  void RefreshCrowdCounts(float high_distance) {
    for (auto& [connection_id, peer] : peers_) {
      (void)connection_id;
      peer.neighbours = 0;
    }
    if (high_distance <= 0.0f) {
      return;
    }
    const float radius_squared = high_distance * high_distance;
    for (auto outer = peers_.begin(); outer != peers_.end(); ++outer) {
      if (!outer->second.position_valid) {
        continue;
      }
      auto inner = outer;
      for (++inner; inner != peers_.end(); ++inner) {
        if (!inner->second.position_valid ||
            inner->second.map_hash != outer->second.map_hash ||
            inner->second.bucket != outer->second.bucket) {
          continue;
        }
        const float dx = inner->second.x - outer->second.x;
        const float dy = inner->second.y - outer->second.y;
        const float dz = inner->second.z - outer->second.z;
        if (dx * dx + dy * dy + dz * dz <= radius_squared) {
          ++outer->second.neighbours;
          ++inner->second.neighbours;
        }
      }
    }
  }

  // Largest neighbour count currently observed, for telemetry: it is the
  // one number that says whether hot-zone thinning is doing anything.
  [[nodiscard]] std::uint32_t PeakNeighbours() const {
    std::uint32_t peak = 0;
    for (const auto& [connection_id, peer] : peers_) {
      (void)connection_id;
      peak = std::max(peak, peer.neighbours);
    }
    return peak;
  }

 private:
  std::unordered_map<std::uint64_t, RelayPeer> peers_;
  std::unordered_map<std::uint32_t, std::uint64_t> role_to_connection_;
};

}  // namespace skate3::multiplayer::routing
