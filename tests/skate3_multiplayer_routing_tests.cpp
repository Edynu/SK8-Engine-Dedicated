#include "skate3_multiplayer_bandwidth.h"
#include "skate3_multiplayer_routing.h"
#include "skate3_multiplayer_transport.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace skate3::multiplayer;
using namespace skate3::multiplayer::bandwidth;
using namespace skate3::multiplayer::protocol_v12;
using namespace skate3::multiplayer::routing;

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

std::vector<std::uint8_t> Datagram(std::uint16_t role,
                                   std::uint32_t session,
                                   std::uint32_t sequence = 1) {
  Envelope envelope{
      .kind = MessageKind::kRootSnapshot,
      .flags = kFlagExpires,
      .payload_bytes = 0,
      .sender_role = role,
      .stream_id = 1,
      .sender_session = session,
      .sequence = sequence,
      .sender_time_us = 1000,
  };
  std::vector<std::uint8_t> bytes(kEnvelopeBytes);
  Expect(EncodeEnvelope(envelope, bytes), "test envelope did not encode");
  return bytes;
}

void TestTopologyPolicyAndBudgets() {
  for (const std::uint32_t players : {2u, 5u, 20u}) {
    const auto route = RecommendRoute(players, true);
    Expect(route.supported && route.mode == RouteMode::kDirectPeerMesh &&
               !route.dedicated_preferred,
           "small/medium session did not retain direct P2P");
  }
  for (const std::uint32_t players : {50u, 100u}) {
    const auto with_server = RecommendRoute(players, true);
    const auto p2p_fallback = RecommendRoute(players, false);
    Expect(with_server.supported &&
               with_server.mode == RouteMode::kDedicatedRelay &&
               with_server.dedicated_preferred,
           "large session did not prefer dedicated relay");
    Expect(p2p_fallback.supported &&
               p2p_fallback.mode == RouteMode::kDirectPeerMesh &&
               p2p_fallback.dedicated_preferred,
           "large session lost explicit P2P fallback");
  }
  Expect(!RecommendRoute(1, true).supported &&
             !RecommendRoute(101, true).supported,
         "invalid player count was accepted");

  constexpr double stream = kPerPeerApplicationBudgetBytesPerSecond;
  for (const std::uint32_t players : {2u, 5u, 20u, 50u, 100u}) {
    const auto mesh =
        ProjectBandwidth(players, stream, RouteMode::kDirectPeerMesh);
    const auto relay =
        ProjectBandwidth(players, stream, RouteMode::kDedicatedRelay);
    const double peers = static_cast<double>(players - 1);
    Expect(mesh.client_upload_bytes_per_second == stream * peers &&
               mesh.client_download_bytes_per_second == stream * peers,
           "mesh per-client projection changed");
    Expect(relay.client_upload_bytes_per_second == stream &&
               relay.client_download_bytes_per_second == stream * peers,
           "relay client projection changed");
    Expect(relay.relay_egress_bytes_per_second ==
               stream * static_cast<double>(players) * peers,
           "relay egress projection changed");
  }

  constexpr double ten_player_upload =
      DirectMeshUploadBytesPerSecond(10);
  static_assert(kRootSnapshotRateHz == 60);
  static_assert(kAnimationSnapshotRateHz == 20);
  static_assert(kMinimumInterpolationDelayMs == 0);
  Expect(ten_player_upload == 1008.0 * 1024.0,
         "ten-player direct-mesh byte budget changed");
  Expect(BitsPerSecond(ten_player_upload) < 8.3 * 1024.0 * 1024.0,
         "ten-player direct-mesh baseline exceeded 8.3 Mibit/s");
}

void TestAuthenticatedVisualRelay() {
  VisualRelayRouter relay;
  for (std::uint32_t role = 1; role <= 100; ++role) {
    Expect(relay.Register({
               .connection_id = 1000 + role,
               .role = role,
               .session = 5000 + role,
           }),
           "valid relay peer did not register");
  }
  Expect(relay.peer_count() == 100, "relay peer count was not bounded to 100");

  const auto source = Datagram(50, 5050);
  const auto broadcast = relay.Route(1050, source);
  Expect(broadcast.disposition == RelayDisposition::kForward &&
             broadcast.recipient_connections.size() == 99,
         "100-player full-fidelity broadcast did not reach 99 peers");
  const auto directed = relay.Route(1050, source, 72);
  Expect(directed.disposition == RelayDisposition::kForward &&
             directed.recipient_connections ==
                 std::vector<std::uint64_t>{1072},
         "directed relay control did not reach exact role");
  Expect(relay.Route(9999, source).disposition ==
             RelayDisposition::kUnknownConnection,
         "unknown relay connection was accepted");
  Expect(relay.Route(1051, source).disposition ==
             RelayDisposition::kSpoofedRole,
         "relay accepted role spoofing");

  const auto stale = Datagram(50, 5051);
  Expect(relay.Route(1050, stale).disposition ==
             RelayDisposition::kStaleSession,
         "relay accepted stale sender generation");
  Expect(relay.Route(1050, source, 101).disposition ==
             RelayDisposition::kInvalidTarget,
         "relay accepted invalid target");

  relay.Remove(1072);
  Expect(relay.peer_count() == 99 &&
             relay.Route(1050, source, 72).disposition ==
                 RelayDisposition::kInvalidTarget,
         "relay retained removed connection");
  Expect(relay.Register({
             .connection_id = 2072,
             .role = 72,
             .session = 6072,
         }),
         "replacement relay generation did not register");
  Expect(relay.Route(1050, source, 72).recipient_connections ==
             std::vector<std::uint64_t>{2072},
         "role reuse did not route to new authenticated generation");
}

void TestRelayInterestManagement() {
  VisualRelayRouter relay;
  Expect(relay.Register({.connection_id = 1, .role = 1, .session = 100}),
         "peer 1 did not register");
  Expect(relay.Register({.connection_id = 2, .role = 2, .session = 200}),
         "peer 2 did not register");
  Expect(relay.Register({.connection_id = 3, .role = 3, .session = 300}),
         "peer 3 did not register");

  const auto source = Datagram(1, 100);

  // Before any beacon arrives every peer defaults to map_hash 0 / no known
  // position, so a fresh join is never silently excluded.
  {
    const auto route = relay.Route(1, source);
    Expect(route.disposition == RelayDisposition::kForward &&
               route.recipient_connections ==
                   (std::vector<std::uint64_t>{2, 3}),
           "unbeaconed peers were excluded from broadcast");
  }

  // Peer 2 joins a different map; peer 3 stays on the sender's map.
  Expect(relay.UpdatePresence(1, /*map_hash=*/111, 0.0f, 0.0f, 0.0f, 1000),
         "presence update for connection 1 failed");
  Expect(relay.UpdatePresence(2, /*map_hash=*/222, 0.0f, 0.0f, 0.0f, 1000),
         "presence update for connection 2 failed");
  Expect(relay.UpdatePresence(3, /*map_hash=*/111, 500.0f, 0.0f, 0.0f, 1000),
         "presence update for connection 3 failed");
  {
    const auto route = relay.Route(1, source);
    Expect(route.disposition == RelayDisposition::kForward &&
               route.recipient_connections == std::vector<std::uint64_t>{3},
           "relay broadcast a different map to a peer");
  }

  // Same map, but far outside a configured radius: excluded.
  {
    const auto route = relay.Route(1, source, /*target_role=*/0,
                                   /*radius=*/10.0f);
    Expect(route.disposition == RelayDisposition::kForward &&
               route.recipient_connections.empty(),
           "relay broadcast beyond the configured radius");
  }

  // Move peer 3 within radius: included again.
  Expect(relay.UpdatePresence(3, 111, 5.0f, 0.0f, 0.0f, 1001),
         "presence update did not move peer 3 into range");
  {
    const auto route = relay.Route(1, source, /*target_role=*/0,
                                   /*radius=*/10.0f);
    Expect(route.disposition == RelayDisposition::kForward &&
               route.recipient_connections == std::vector<std::uint64_t>{3},
           "relay excluded a peer that moved back into radius");
  }

  // Stale eviction.
  const auto removed = relay.RemoveStale(/*now_us=*/2'000'000,
                                         /*timeout_us=*/500'000);
  Expect(removed.size() == 3 && relay.peer_count() == 0,
         "stale peers were not evicted");
}

void TestRelayHandshakeCodecs() {
  std::array<std::uint8_t, kPresenceBeaconPayloadBytes> beacon_bytes{};
  const PresenceBeacon beacon{.map_hash = 0x1234, .x = 1.5f, .y = -2.5f,
                              .z = 3.0f};
  Expect(EncodePresenceBeacon(beacon, beacon_bytes),
         "presence beacon did not encode");
  PresenceBeacon decoded_beacon;
  Expect(DecodePresenceBeacon(beacon_bytes, decoded_beacon) &&
             decoded_beacon.map_hash == beacon.map_hash &&
             decoded_beacon.x == beacon.x && decoded_beacon.y == beacon.y &&
             decoded_beacon.z == beacon.z,
         "presence beacon did not round-trip");

  std::array<std::uint8_t, kRelayRegisterPayloadBytes> register_bytes{};
  const RelayRegister request{
      .requested_map_hash = 0xAAAA, .token_hash = 0, .client_nonce = 42};
  Expect(EncodeRelayRegister(request, register_bytes),
         "relay register did not encode");
  RelayRegister decoded_request;
  Expect(DecodeRelayRegister(register_bytes, decoded_request) &&
             decoded_request.requested_map_hash == request.requested_map_hash &&
             decoded_request.client_nonce == request.client_nonce,
         "relay register did not round-trip");
  const RelayRegister invalid_request{.client_nonce = 0};
  Expect(!EncodeRelayRegister(invalid_request, register_bytes),
         "relay register accepted a zero nonce");

  std::array<std::uint8_t, kRelayRegisterAckPayloadBytes> ack_bytes{};
  const RelayRegisterAck ack{.assigned_role = 7,
                             .status = RelayRegisterStatus::kOk,
                             .assigned_session = 999};
  Expect(EncodeRelayRegisterAck(ack, ack_bytes),
         "relay register ack did not encode");
  RelayRegisterAck decoded_ack;
  Expect(DecodeRelayRegisterAck(ack_bytes, decoded_ack) &&
             decoded_ack.assigned_role == ack.assigned_role &&
             decoded_ack.status == ack.status &&
             decoded_ack.assigned_session == ack.assigned_session,
         "relay register ack did not round-trip");
  const RelayRegisterAck rejected{.status = RelayRegisterStatus::kFull};
  Expect(EncodeRelayRegisterAck(rejected, ack_bytes),
         "a rejection ack with no assigned role/session did not encode");
}

class RecordingTransport final : public TransportAdapter {
 public:
  [[nodiscard]] TransportKind kind() const override {
    return TransportKind::kDedicatedServer;
  }

  TransportBatchResult SendBatch(
      std::span<const TransportDatagramView> datagrams,
      std::uint64_t now_us) override {
    TransportBatchResult result;
    for (const auto& datagram : datagrams) {
      if (!datagram.Valid(now_us)) {
        ++result.rejected_datagrams;
        result.rejected_bytes += datagram.bytes.size();
        continue;
      }
      ++result.accepted_datagrams;
      result.accepted_bytes += datagram.bytes.size();
      ++snapshot_.sent_datagrams;
      snapshot_.sent_bytes += datagram.bytes.size();
    }
    snapshot_.captured_at_us = now_us;
    return result;
  }

  std::vector<TransportReceivedDatagram> ReceiveBatch(
      std::size_t maximum_datagrams, std::uint64_t now_us) override {
    (void)maximum_datagrams;
    (void)now_us;
    return {};
  }

  [[nodiscard]] TransportQueueSnapshot QueueSnapshot(
      std::uint64_t now_us) const override {
    TransportQueueSnapshot result = snapshot_;
    result.captured_at_us = now_us;
    return result;
  }

 private:
  TransportQueueSnapshot snapshot_;
};

void TestTransportBatchContract() {
  RecordingTransport transport;
  const auto bytes = Datagram(1, 100);
  const TransportEndpoint endpoint{
      .kind = TransportKind::kDedicatedServer,
      .connection_id = 7,
      .role = 2,
      .generation = 1,
  };
  const std::array<TransportDatagramView, 3> batch = {{
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kControl,
          .expires_at_us = 0,
          .bytes = bytes,
      },
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kRealtime,
          .expires_at_us = 2000,
          .bytes = bytes,
      },
      {
          .target = endpoint,
          .traffic_class = OutboundTrafficClass::kRealtime,
          .expires_at_us = 999,
          .bytes = bytes,
      },
  }};
  const auto sent = transport.SendBatch(batch, 1000);
  Expect(sent.accepted_datagrams == 2 && sent.rejected_datagrams == 1 &&
             sent.accepted_bytes == bytes.size() * 2,
         "transport batch did not preserve class/expiry contract");
  const auto queue = transport.QueueSnapshot(1500);
  Expect(queue.sent_datagrams == 2 &&
             queue.sent_bytes == bytes.size() * 2 &&
             queue.captured_at_us == 1500,
         "transport queue snapshot lost batch accounting");
}


// Hot zones: density has to bound what one client receives, because the
// relay's egress is O(recipients x senders) and a skate spot puts everyone
// inside everyone else's high band at once.
void TestCrowdFidelity() {
  using skate3::multiplayer::routing::CrowdPolicy;
  using skate3::multiplayer::routing::DegradeForCrowd;
  using skate3::multiplayer::routing::FidelityLevel;

  const CrowdPolicy policy{.medium_above = 8, .low_above = 20};

  Expect(DegradeForCrowd(FidelityLevel::kFull, 4, policy) ==
             FidelityLevel::kFull,
         "a quiet area must not be thinned");
  Expect(DegradeForCrowd(FidelityLevel::kFull, 12, policy) ==
             FidelityLevel::kHalfDeltas,
         "a busy area drops one band");
  Expect(DegradeForCrowd(FidelityLevel::kFull, 40, policy) ==
             FidelityLevel::kKeyframesOnly,
         "a packed area drops two bands");

  // Degradation stacks on distance rather than replacing it: someone far
  // away in a crowd is thinned by both.
  Expect(DegradeForCrowd(FidelityLevel::kHalfDeltas, 12, policy) ==
             FidelityLevel::kKeyframesOnly,
         "crowd degradation must compose with distance banding");

  // Never past keyframes: thinning those would freeze the peer rather than
  // reduce its detail, which reads as a bug and not as a lower LOD.
  Expect(DegradeForCrowd(FidelityLevel::kKeyframesOnly, 999, policy) ==
             FidelityLevel::kKeyframesOnly,
         "keyframes must survive any crowd");

  // Out of range is already excluded; density must not resurrect it.
  Expect(DegradeForCrowd(FidelityLevel::kOutOfRange, 999, policy) ==
             FidelityLevel::kOutOfRange,
         "out-of-range must stay excluded");

  // An unconfigured policy is inert, so enabling hot zones is opt-in.
  Expect(DegradeForCrowd(FidelityLevel::kFull, 5000, CrowdPolicy{}) ==
             FidelityLevel::kFull,
         "an unset policy must change nothing");
}

// Neighbour counting is what feeds the policy, and it must respect the same
// isolation the router itself enforces.
void TestCrowdCounting() {
  VisualRelayRouter relay;
  for (std::uint32_t id = 1; id <= 4; ++id) {
    Expect(relay.Register({.connection_id = id, .role = id, .session = id * 10}),
           "peer did not register");
  }
  // 1, 2, 3 stand together; 4 is far away on the same map.
  Expect(relay.UpdatePresence(1, 7, 0.0f, 0.0f, 0.0f, 10), "presence 1");
  Expect(relay.UpdatePresence(2, 7, 5.0f, 0.0f, 0.0f, 10), "presence 2");
  Expect(relay.UpdatePresence(3, 7, 0.0f, 0.0f, 5.0f, 10), "presence 3");
  Expect(relay.UpdatePresence(4, 7, 900.0f, 0.0f, 0.0f, 10), "presence 4");

  relay.RefreshCrowdCounts(100.0f);
  Expect(relay.PeakNeighbours() == 2,
         "three co-located peers should each see two neighbours");

  // A peer in another routing bucket shares the spot physically but can
  // never be routed to, so it must not inflate the crowd.
  Expect(relay.SetBucket(3, 1), "bucket assignment failed");
  relay.RefreshCrowdCounts(100.0f);
  Expect(relay.PeakNeighbours() == 1,
         "a peer in another bucket must not count toward the crowd");

  relay.RefreshCrowdCounts(0.0f);
  Expect(relay.PeakNeighbours() == 0,
         "a disabled radius must report no crowd");
}

}  // namespace

int main() {
  TestTopologyPolicyAndBudgets();
  TestAuthenticatedVisualRelay();
  TestRelayInterestManagement();
  TestRelayHandshakeCodecs();
  TestTransportBatchContract();
  TestCrowdFidelity();
  TestCrowdCounting();
  if (g_failures != 0) {
    std::cerr << g_failures << " routing/transport test(s) failed\n";
    return 1;
  }
  std::cout << "multiplayer routing/transport tests passed\n";
  return 0;
}
