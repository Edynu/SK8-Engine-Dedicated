#pragma once

#include "skate3_multiplayer_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace skate3::multiplayer::protocol_v12 {

// Shared with the legacy protocol so the two cannot drift - a role valid on
// one wire format and rejected on the other is a silent, one-sided failure.
inline constexpr std::uint16_t kMaximumRole =
    static_cast<std::uint16_t>(protocol::kMaximumRole);
static_assert(protocol::kMaximumRole <= 0xFFFFu,
              "sender_role is 16 bits in the v12 envelope");

inline constexpr std::uint32_t kEnvelopeMagic = 0x324D334Bu; // "K3M2"
inline constexpr std::uint16_t kProtocolVersion = 12;
inline constexpr std::uint16_t kEnvelopeBytes = 40;
inline constexpr std::uint16_t kMaximumDatagramBytes = 1200;
inline constexpr std::uint16_t kMaximumPayloadBytes =
    kMaximumDatagramBytes - kEnvelopeBytes;
inline constexpr std::uint16_t kCapabilitiesPayloadBytes = 36;
inline constexpr std::uint16_t kPresenceBeaconPayloadBytes = 20;
inline constexpr std::uint16_t kRelayRegisterPayloadBytes = 24;
inline constexpr std::uint16_t kRelayRegisterAckPayloadBytes = 10;

enum class MessageKind : std::uint8_t {
  kCapabilities = 1,
  kRootSnapshot = 2,
  kPoseBaseline = 3,
  kPoseDelta = 4,
  kAppearanceControl = 5,
  kAppearanceChunk = 6,
  kPoseControl = 7,
  kMapEditControl = 8,
  kMapEditSpawnChunk = 9,
  // Dedicated-relay-only kinds: never negotiated as part of the peer-to-peer
  // capability handshake, consumed solely by the relay process itself.
  kPresenceBeacon = 10,
  kRelayRegister = 11,
  kRelayRegisterAck = 12,
  // General reliable ordered channel - see
  // skate3_multiplayer_protocol_v12_reliable.h. Unlike the kinds above,
  // these carry an application payload the protocol itself does not
  // interpret, so anything needing TCP-like delivery can share them.
  kReliableStream = 13,
  kReliableAck = 14,
};

enum EnvelopeFlag : std::uint8_t {
  kFlagKeyframe = 1u << 0,
  kFlagReliable = 1u << 1,
  kFlagExpires = 1u << 2,
};

inline constexpr std::uint8_t kKnownEnvelopeFlags =
    kFlagKeyframe | kFlagReliable | kFlagExpires;

struct Envelope {
  std::uint32_t magic = kEnvelopeMagic;
  std::uint16_t version = kProtocolVersion;
  MessageKind kind = MessageKind::kCapabilities;
  std::uint8_t flags = 0;
  std::uint16_t header_bytes = kEnvelopeBytes;
  std::uint16_t payload_bytes = 0;
  std::uint16_t sender_role = 0;
  std::uint16_t stream_id = 0;
  std::uint32_t sender_session = 0;
  std::uint32_t sequence = 0;
  std::uint32_t acknowledged_sequence = 0;
  std::uint32_t receive_history = 0;
  std::uint64_t sender_time_us = 0;
};

inline constexpr std::uint64_t kFeatureExplicitLittleEndian = 1ull << 0;
inline constexpr std::uint64_t kFeaturePoseAcknowledgements = 1ull << 1;
inline constexpr std::uint64_t kFeaturePoseGroups = 1ull << 2;
inline constexpr std::uint64_t kFeatureAppearanceRecipes = 1ull << 3;
inline constexpr std::uint64_t kFeatureLiveMapEditing = 1ull << 4;
inline constexpr std::uint64_t kKnownFeatureBits =
    kFeatureExplicitLittleEndian | kFeaturePoseAcknowledgements |
    kFeaturePoseGroups | kFeatureAppearanceRecipes | kFeatureLiveMapEditing;

struct Capabilities {
  std::uint64_t feature_bits = kFeatureExplicitLittleEndian;
  std::uint64_t map_hash = 0;
  std::uint64_t build_hash = 0;
  std::uint64_t content_hash = 0;
  std::uint16_t maximum_datagram_bytes = kMaximumDatagramBytes;
  std::uint8_t maximum_pose_groups = 1;
  std::uint8_t reserved = 0;
};

[[nodiscard]] constexpr bool MessageKindValid(MessageKind kind) {
  return kind >= MessageKind::kCapabilities &&
         kind <= MessageKind::kReliableAck;
}

[[nodiscard]] constexpr bool EnvelopeShapeValid(const Envelope &envelope) {
  return envelope.magic == kEnvelopeMagic &&
         envelope.version == kProtocolVersion &&
         MessageKindValid(envelope.kind) &&
         (envelope.flags & ~kKnownEnvelopeFlags) == 0 &&
         envelope.header_bytes == kEnvelopeBytes &&
         envelope.payload_bytes <= kMaximumPayloadBytes &&
         envelope.sender_role >= 1 && envelope.sender_role <= kMaximumRole &&
         envelope.sender_session != 0;
}

[[nodiscard]] constexpr bool
CapabilitiesShapeValid(const Capabilities &capabilities) {
  return (capabilities.feature_bits & kFeatureExplicitLittleEndian) != 0 &&
         capabilities.maximum_datagram_bytes >=
             kEnvelopeBytes + kCapabilitiesPayloadBytes &&
         capabilities.maximum_datagram_bytes <= kMaximumDatagramBytes &&
         capabilities.maximum_pose_groups >= 1 &&
         capabilities.maximum_pose_groups <= 32 && capabilities.reserved == 0;
}

[[nodiscard]] constexpr std::uint64_t
NegotiateFeatureBits(std::uint64_t local_features,
                     std::uint64_t remote_features) {
  return local_features & remote_features & kKnownFeatureBits;
}

[[nodiscard]] constexpr bool SequenceNewer(std::uint32_t candidate,
                                           std::uint32_t reference) {
  const std::uint32_t distance = candidate - reference;
  return distance != 0 && distance < 0x80000000u;
}

namespace detail {

class LittleEndianWriter {
public:
  explicit LittleEndianWriter(std::span<std::uint8_t> bytes) : bytes_(bytes) {}

  bool U8(std::uint8_t value) {
    if (offset_ >= bytes_.size()) {
      return false;
    }
    bytes_[offset_++] = value;
    return true;
  }

  bool U16(std::uint16_t value) {
    return U8(static_cast<std::uint8_t>(value)) &&
           U8(static_cast<std::uint8_t>(value >> 8));
  }

  bool U32(std::uint32_t value) {
    return U16(static_cast<std::uint16_t>(value)) &&
           U16(static_cast<std::uint16_t>(value >> 16));
  }

  bool U64(std::uint64_t value) {
    return U32(static_cast<std::uint32_t>(value)) &&
           U32(static_cast<std::uint32_t>(value >> 32));
  }

  bool F32(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return U32(bits);
  }

  [[nodiscard]] std::size_t offset() const { return offset_; }

private:
  std::span<std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

class LittleEndianReader {
public:
  explicit LittleEndianReader(std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}

  bool U8(std::uint8_t &value) {
    if (offset_ >= bytes_.size()) {
      return false;
    }
    value = bytes_[offset_++];
    return true;
  }

  bool U16(std::uint16_t &value) {
    std::uint8_t low = 0;
    std::uint8_t high = 0;
    if (!U8(low) || !U8(high)) {
      return false;
    }
    value = static_cast<std::uint16_t>(std::uint16_t(low) |
                                       (std::uint16_t(high) << 8));
    return true;
  }

  bool U32(std::uint32_t &value) {
    std::uint16_t low = 0;
    std::uint16_t high = 0;
    if (!U16(low) || !U16(high)) {
      return false;
    }
    value = std::uint32_t(low) | (std::uint32_t(high) << 16);
    return true;
  }

  bool U64(std::uint64_t &value) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!U32(low) || !U32(high)) {
      return false;
    }
    value = std::uint64_t(low) | (std::uint64_t(high) << 32);
    return true;
  }

  bool F32(float &value) {
    std::uint32_t bits = 0;
    if (!U32(bits)) {
      return false;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  [[nodiscard]] std::size_t offset() const { return offset_; }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0;
};

} // namespace detail

[[nodiscard]] inline bool EncodeEnvelope(const Envelope &envelope,
                                         std::span<std::uint8_t> destination) {
  if (!EnvelopeShapeValid(envelope) || destination.size() < kEnvelopeBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(destination.first(kEnvelopeBytes));
  const bool encoded =
      writer.U32(envelope.magic) && writer.U16(envelope.version) &&
      writer.U8(static_cast<std::uint8_t>(envelope.kind)) &&
      writer.U8(envelope.flags) && writer.U16(envelope.header_bytes) &&
      writer.U16(envelope.payload_bytes) && writer.U16(envelope.sender_role) &&
      writer.U16(envelope.stream_id) && writer.U32(envelope.sender_session) &&
      writer.U32(envelope.sequence) &&
      writer.U32(envelope.acknowledged_sequence) &&
      writer.U32(envelope.receive_history) &&
      writer.U64(envelope.sender_time_us);
  return encoded && writer.offset() == kEnvelopeBytes;
}

[[nodiscard]] inline bool DecodeEnvelope(std::span<const std::uint8_t> packet,
                                         Envelope &output) {
  if (packet.size() < kEnvelopeBytes) {
    return false;
  }
  detail::LittleEndianReader reader(packet.first(kEnvelopeBytes));
  Envelope decoded;
  std::uint8_t kind = 0;
  if (!reader.U32(decoded.magic) || !reader.U16(decoded.version) ||
      !reader.U8(kind) || !reader.U8(decoded.flags) ||
      !reader.U16(decoded.header_bytes) || !reader.U16(decoded.payload_bytes) ||
      !reader.U16(decoded.sender_role) || !reader.U16(decoded.stream_id) ||
      !reader.U32(decoded.sender_session) || !reader.U32(decoded.sequence) ||
      !reader.U32(decoded.acknowledged_sequence) ||
      !reader.U32(decoded.receive_history) ||
      !reader.U64(decoded.sender_time_us) ||
      reader.offset() != kEnvelopeBytes) {
    return false;
  }
  decoded.kind = static_cast<MessageKind>(kind);
  if (!EnvelopeShapeValid(decoded) ||
      packet.size() != std::size_t(kEnvelopeBytes) + decoded.payload_bytes) {
    return false;
  }
  output = decoded;
  return true;
}

[[nodiscard]] inline bool
EncodeCapabilities(const Capabilities &capabilities,
                   std::span<std::uint8_t> destination) {
  if (!CapabilitiesShapeValid(capabilities) ||
      destination.size() < kCapabilitiesPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kCapabilitiesPayloadBytes));
  const bool encoded = writer.U64(capabilities.feature_bits) &&
                       writer.U64(capabilities.map_hash) &&
                       writer.U64(capabilities.build_hash) &&
                       writer.U64(capabilities.content_hash) &&
                       writer.U16(capabilities.maximum_datagram_bytes) &&
                       writer.U8(capabilities.maximum_pose_groups) &&
                       writer.U8(capabilities.reserved);
  return encoded && writer.offset() == kCapabilitiesPayloadBytes;
}

[[nodiscard]] inline bool
DecodeCapabilities(std::span<const std::uint8_t> payload,
                   Capabilities &output) {
  if (payload.size() != kCapabilitiesPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  Capabilities decoded;
  if (!reader.U64(decoded.feature_bits) || !reader.U64(decoded.map_hash) ||
      !reader.U64(decoded.build_hash) || !reader.U64(decoded.content_hash) ||
      !reader.U16(decoded.maximum_datagram_bytes) ||
      !reader.U8(decoded.maximum_pose_groups) || !reader.U8(decoded.reserved) ||
      reader.offset() != kCapabilitiesPayloadBytes ||
      !CapabilitiesShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

[[nodiscard]] constexpr bool FiniteF32(float value) {
  return value == value && value > -3.0e38f && value < 3.0e38f;
}

// Dedicated-relay-only payloads. These never travel peer-to-peer; they are
// exchanged only between a client and the relay process (see
// skate3_multiplayer_routing.h's VisualRelayRouter and the standalone relay
// executable). The relay decodes exactly these two payload shapes and
// forwards every other message kind as opaque bytes.

struct PresenceBeacon {
  std::uint64_t map_hash = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

[[nodiscard]] constexpr bool
PresenceBeaconShapeValid(const PresenceBeacon &beacon) {
  return FiniteF32(beacon.x) && FiniteF32(beacon.y) && FiniteF32(beacon.z);
}

[[nodiscard]] inline bool
EncodePresenceBeacon(const PresenceBeacon &beacon,
                     std::span<std::uint8_t> destination) {
  if (!PresenceBeaconShapeValid(beacon) ||
      destination.size() < kPresenceBeaconPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kPresenceBeaconPayloadBytes));
  const bool encoded = writer.U64(beacon.map_hash) && writer.F32(beacon.x) &&
                       writer.F32(beacon.y) && writer.F32(beacon.z);
  return encoded && writer.offset() == kPresenceBeaconPayloadBytes;
}

[[nodiscard]] inline bool
DecodePresenceBeacon(std::span<const std::uint8_t> payload,
                     PresenceBeacon &output) {
  if (payload.size() != kPresenceBeaconPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  PresenceBeacon decoded;
  if (!reader.U64(decoded.map_hash) || !reader.F32(decoded.x) ||
      !reader.F32(decoded.y) || !reader.F32(decoded.z) ||
      reader.offset() != kPresenceBeaconPayloadBytes ||
      !PresenceBeaconShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

struct RelayRegister {
  std::uint64_t requested_map_hash = 0;
  std::uint64_t token_hash = 0;
  std::uint64_t client_nonce = 0;
};

[[nodiscard]] constexpr bool
RelayRegisterShapeValid(const RelayRegister &request) {
  return request.client_nonce != 0;
}

[[nodiscard]] inline bool
EncodeRelayRegister(const RelayRegister &request,
                    std::span<std::uint8_t> destination) {
  if (!RelayRegisterShapeValid(request) ||
      destination.size() < kRelayRegisterPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kRelayRegisterPayloadBytes));
  const bool encoded = writer.U64(request.requested_map_hash) &&
                       writer.U64(request.token_hash) &&
                       writer.U64(request.client_nonce);
  return encoded && writer.offset() == kRelayRegisterPayloadBytes;
}

[[nodiscard]] inline bool
DecodeRelayRegister(std::span<const std::uint8_t> payload,
                    RelayRegister &output) {
  if (payload.size() != kRelayRegisterPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  RelayRegister decoded;
  if (!reader.U64(decoded.requested_map_hash) ||
      !reader.U64(decoded.token_hash) || !reader.U64(decoded.client_nonce) ||
      reader.offset() != kRelayRegisterPayloadBytes ||
      !RelayRegisterShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

enum class RelayRegisterStatus : std::uint8_t {
  kOk = 0,
  kBadToken = 1,
  kFull = 2,
};

struct RelayRegisterAck {
  std::uint16_t assigned_role = 0;
  RelayRegisterStatus status = RelayRegisterStatus::kOk;
  std::uint8_t reserved = 0;
  std::uint32_t assigned_session = 0;
  // The relay's own dev-console HTTP port (0 if it has none / disabled), so a
  // connecting client can fetch server-provided Lua resources (see
  // skate3_lua_admin_http.h's /api/scripts route) without needing a
  // separately-configured address.
  std::uint16_t admin_http_port = 0;
};

[[nodiscard]] constexpr bool
RelayRegisterAckShapeValid(const RelayRegisterAck &ack) {
  if (ack.reserved != 0 || ack.status > RelayRegisterStatus::kFull) {
    return false;
  }
  if (ack.status != RelayRegisterStatus::kOk) {
    return true;
  }
  return ack.assigned_role >= 1 && ack.assigned_role <= kMaximumRole &&
         ack.assigned_session != 0;
}

[[nodiscard]] inline bool
EncodeRelayRegisterAck(const RelayRegisterAck &ack,
                       std::span<std::uint8_t> destination) {
  if (!RelayRegisterAckShapeValid(ack) ||
      destination.size() < kRelayRegisterAckPayloadBytes) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.first(kRelayRegisterAckPayloadBytes));
  const bool encoded = writer.U16(ack.assigned_role) &&
                       writer.U8(static_cast<std::uint8_t>(ack.status)) &&
                       writer.U8(ack.reserved) &&
                       writer.U32(ack.assigned_session) &&
                       writer.U16(ack.admin_http_port);
  return encoded && writer.offset() == kRelayRegisterAckPayloadBytes;
}

[[nodiscard]] inline bool
DecodeRelayRegisterAck(std::span<const std::uint8_t> payload,
                       RelayRegisterAck &output) {
  if (payload.size() != kRelayRegisterAckPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(payload);
  RelayRegisterAck decoded;
  std::uint8_t status = 0;
  if (!reader.U16(decoded.assigned_role) || !reader.U8(status) ||
      !reader.U8(decoded.reserved) || !reader.U32(decoded.assigned_session) ||
      !reader.U16(decoded.admin_http_port) ||
      reader.offset() != kRelayRegisterAckPayloadBytes) {
    return false;
  }
  decoded.status = static_cast<RelayRegisterStatus>(status);
  if (!RelayRegisterAckShapeValid(decoded)) {
    return false;
  }
  output = decoded;
  return true;
}

// receive_history bit zero acknowledges the packet immediately before
// acknowledged_sequence, and bit 31 acknowledges the packet 32 positions
// behind it. Unsigned subtraction preserves sequence rollover semantics.
[[nodiscard]] constexpr bool
SequenceAcknowledged(std::uint32_t candidate,
                     std::uint32_t acknowledged_sequence,
                     std::uint32_t receive_history) {
  if (candidate == acknowledged_sequence) {
    return true;
  }
  const std::uint32_t distance = acknowledged_sequence - candidate;
  return distance >= 1 && distance <= 32 &&
         (receive_history & (1u << (distance - 1))) != 0;
}

static_assert(kEnvelopeBytes == 40);
static_assert(kMaximumPayloadBytes == 1160);
static_assert(kCapabilitiesPayloadBytes == 36);
static_assert(kPresenceBeaconPayloadBytes == 20);
static_assert(kRelayRegisterPayloadBytes == 24);
static_assert(kRelayRegisterAckPayloadBytes == 10);

} // namespace skate3::multiplayer::protocol_v12
