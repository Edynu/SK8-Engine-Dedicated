#pragma once

// A general reliable, ordered message channel over the v12 envelope.
//
// The envelope already carries a sequence, an acknowledged sequence and a
// 32-entry receive-history bitfield, but nothing in the protocol RETRANSMITS:
// reliability has so far been arranged per feature, and appearance chunks get
// it by having the receiver ask for a resend. That does not generalise, and
// script events need delivery guarantees closer to TCP's - a dropped
// TriggerServerEvent is a lost game action, not a stale pose.
//
// This adds the missing piece as a reusable channel rather than a
// script-event special case, so state bags, spawns and game-mode traffic can
// share it later. Two message kinds:
//
//   kReliableStream  one fragment of a reliable message
//   kReliableAck     acknowledgement of what the receiver has taken delivery of
//
// Reliability is at MESSAGE granularity, not fragment: a message is
// retransmitted whole until acknowledged. Script events are a few hundred
// bytes and almost always one fragment, so per-fragment selective repair
// would buy very little for a great deal more state to get wrong.
//
// The envelope's stream_id selects the logical CHANNEL, so independent kinds
// of traffic get independent ordering and cannot head-of-line block each
// other.

#include "skate3_multiplayer_protocol_v12.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::multiplayer::protocol_v12 {

// Channel ids carried in the envelope's stream_id. Ordering is guaranteed
// WITHIN a channel and deliberately not across channels.
inline constexpr std::uint16_t kReliableChannelScriptEvent = 1;

inline constexpr std::uint16_t kReliableHeaderBytes = 16;
inline constexpr std::uint16_t kReliableAckPayloadBytes = 12;

// Payload bytes available for message content in a single fragment.
inline constexpr std::uint16_t kReliableFragmentBytes =
    kMaximumPayloadBytes - kReliableHeaderBytes;

// A message may span at most this many fragments. 64 fragments of ~1144
// bytes is a ~73 KiB ceiling - far above any plausible event argument list,
// and low enough that a corrupt fragment_count cannot make a receiver
// allocate wildly.
inline constexpr std::uint16_t kReliableMaximumFragments = 64;
inline constexpr std::uint32_t kReliableMaximumMessageBytes =
    static_cast<std::uint32_t>(kReliableFragmentBytes) *
    kReliableMaximumFragments;

struct ReliableHeader {
  // Per (sender, channel) monotonic, starting at 1. Doubles as the ordering
  // key and the acknowledgement key; 0 is never a valid message id.
  std::uint32_t message_id = 0;
  std::uint32_t total_bytes = 0;
  std::uint16_t fragment_index = 0;
  std::uint16_t fragment_count = 0;
  // Who the message is ultimately for. 0 addresses the server itself,
  // kReliableBroadcastRole every client; any other value a specific role.
  // The relay forwards on this rather than inspecting the message.
  std::uint16_t target_role = 0;
  std::uint16_t reserved = 0;
};

inline constexpr std::uint16_t kReliableBroadcastRole = 0xFFFF;

struct ReliableAck {
  // Highest message id delivered IN ORDER; everything at or below it is
  // complete, so the sender can retire it all.
  std::uint32_t contiguous_id = 0;
  // Messages received beyond contiguous_id, bit n meaning contiguous_id+1+n.
  // Lets a sender retire out-of-order arrivals instead of resending them.
  std::uint32_t ahead_bits = 0;
  std::uint16_t channel = 0;
  std::uint16_t reserved = 0;
};

[[nodiscard]] constexpr std::uint16_t
ReliableFragmentCount(std::uint32_t total_bytes) {
  if (total_bytes == 0) {
    return 1;  // an empty message is still one fragment.
  }
  return static_cast<std::uint16_t>(
      (total_bytes + kReliableFragmentBytes - 1) / kReliableFragmentBytes);
}

[[nodiscard]] constexpr std::uint32_t
ReliableFragmentOffset(std::uint16_t fragment_index) {
  return static_cast<std::uint32_t>(fragment_index) * kReliableFragmentBytes;
}

[[nodiscard]] constexpr std::uint16_t
ReliableFragmentByteCount(std::uint32_t total_bytes,
                          std::uint16_t fragment_index) {
  const std::uint32_t offset = ReliableFragmentOffset(fragment_index);
  if (offset >= total_bytes) {
    return 0;
  }
  const std::uint32_t remaining = total_bytes - offset;
  return static_cast<std::uint16_t>(
      remaining < kReliableFragmentBytes ? remaining : kReliableFragmentBytes);
}

[[nodiscard]] constexpr bool ReliableHeaderShapeValid(
    const ReliableHeader &header) {
  return header.message_id != 0 && header.reserved == 0 &&
         header.total_bytes <= kReliableMaximumMessageBytes &&
         header.fragment_count >= 1 &&
         header.fragment_count <= kReliableMaximumFragments &&
         header.fragment_index < header.fragment_count &&
         header.fragment_count == ReliableFragmentCount(header.total_bytes);
}

[[nodiscard]] inline bool EncodeReliableDatagram(
    const Envelope &envelope, const ReliableHeader &header,
    std::span<const std::uint8_t> message,
    std::span<std::uint8_t> destination) {
  if (envelope.kind != MessageKind::kReliableStream ||
      !ReliableHeaderShapeValid(header) ||
      message.size() != header.total_bytes) {
    return false;
  }
  const std::uint16_t fragment_bytes =
      ReliableFragmentByteCount(header.total_bytes, header.fragment_index);
  if (envelope.payload_bytes != kReliableHeaderBytes + fragment_bytes ||
      destination.size() < kEnvelopeBytes + envelope.payload_bytes) {
    return false;
  }
  if (!EncodeEnvelope(envelope, destination.first(kEnvelopeBytes))) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.subspan(kEnvelopeBytes, kReliableHeaderBytes));
  if (!(writer.U32(header.message_id) && writer.U32(header.total_bytes) &&
        writer.U16(header.fragment_index) && writer.U16(header.fragment_count) &&
        writer.U16(header.target_role) && writer.U16(header.reserved))) {
    return false;
  }
  if (fragment_bytes != 0) {
    const std::uint32_t offset = ReliableFragmentOffset(header.fragment_index);
    std::memcpy(destination.data() + kEnvelopeBytes + kReliableHeaderBytes,
                message.data() + offset, fragment_bytes);
  }
  return true;
}

// Decodes one fragment. `fragment` is left pointing INTO `packet`, so it is
// only valid for as long as the caller's datagram buffer is.
[[nodiscard]] inline bool DecodeReliableDatagram(
    std::span<const std::uint8_t> packet, Envelope &out_envelope,
    ReliableHeader &out_header, std::span<const std::uint8_t> &out_fragment) {
  Envelope envelope;
  if (!DecodeEnvelope(packet, envelope) ||
      envelope.kind != MessageKind::kReliableStream ||
      envelope.payload_bytes < kReliableHeaderBytes ||
      packet.size() < static_cast<std::size_t>(kEnvelopeBytes) +
                          envelope.payload_bytes) {
    return false;
  }
  detail::LittleEndianReader reader(
      packet.subspan(kEnvelopeBytes, kReliableHeaderBytes));
  ReliableHeader header;
  if (!(reader.U32(header.message_id) && reader.U32(header.total_bytes) &&
        reader.U16(header.fragment_index) && reader.U16(header.fragment_count) &&
        reader.U16(header.target_role) && reader.U16(header.reserved))) {
    return false;
  }
  if (!ReliableHeaderShapeValid(header)) {
    return false;
  }
  const std::uint16_t fragment_bytes =
      ReliableFragmentByteCount(header.total_bytes, header.fragment_index);
  if (envelope.payload_bytes != kReliableHeaderBytes + fragment_bytes) {
    return false;
  }
  out_envelope = envelope;
  out_header = header;
  out_fragment = packet.subspan(kEnvelopeBytes + kReliableHeaderBytes,
                                fragment_bytes);
  return true;
}

[[nodiscard]] inline bool EncodeReliableAckDatagram(
    const Envelope &envelope, const ReliableAck &ack,
    std::span<std::uint8_t> destination) {
  if (envelope.kind != MessageKind::kReliableAck || ack.reserved != 0 ||
      envelope.payload_bytes != kReliableAckPayloadBytes ||
      destination.size() < kEnvelopeBytes + kReliableAckPayloadBytes) {
    return false;
  }
  if (!EncodeEnvelope(envelope, destination.first(kEnvelopeBytes))) {
    return false;
  }
  detail::LittleEndianWriter writer(
      destination.subspan(kEnvelopeBytes, kReliableAckPayloadBytes));
  return writer.U32(ack.contiguous_id) && writer.U32(ack.ahead_bits) &&
         writer.U16(ack.channel) && writer.U16(ack.reserved);
}

[[nodiscard]] inline bool DecodeReliableAckDatagram(
    std::span<const std::uint8_t> packet, Envelope &out_envelope,
    ReliableAck &out_ack) {
  Envelope envelope;
  if (!DecodeEnvelope(packet, envelope) ||
      envelope.kind != MessageKind::kReliableAck ||
      envelope.payload_bytes != kReliableAckPayloadBytes ||
      packet.size() < static_cast<std::size_t>(kEnvelopeBytes) +
                          kReliableAckPayloadBytes) {
    return false;
  }
  detail::LittleEndianReader reader(
      packet.subspan(kEnvelopeBytes, kReliableAckPayloadBytes));
  ReliableAck ack;
  if (!(reader.U32(ack.contiguous_id) && reader.U32(ack.ahead_bits) &&
        reader.U16(ack.channel) && reader.U16(ack.reserved))) {
    return false;
  }
  if (ack.reserved != 0) {
    return false;
  }
  out_envelope = envelope;
  out_ack = ack;
  return true;
}

// ---------------------------------------------------------------------
// Script events, the first user of the channel.
//
// The channel itself carries opaque bytes; this is the one format that
// travels on kReliableChannelScriptEvent:
//
//   u16 name_bytes | name | JSON argument array
//
// The argument list is already JSON by the time it reaches the transport
// (the Lua host marshals it), so the wire format has nothing to say about
// it beyond how long the name is.

[[nodiscard]] inline bool EncodeScriptEventMessage(
    std::string_view event, std::string_view json_args,
    std::vector<std::uint8_t> &out) {
  if (event.empty() || event.size() > 0xFFFF) {
    return false;
  }
  const std::size_t total = 2 + event.size() + json_args.size();
  if (total > kReliableMaximumMessageBytes) {
    return false;
  }
  out.resize(total);
  out[0] = static_cast<std::uint8_t>(event.size() & 0xFF);
  out[1] = static_cast<std::uint8_t>((event.size() >> 8) & 0xFF);
  std::memcpy(out.data() + 2, event.data(), event.size());
  if (!json_args.empty()) {
    std::memcpy(out.data() + 2 + event.size(), json_args.data(),
                json_args.size());
  }
  return true;
}

[[nodiscard]] inline bool DecodeScriptEventMessage(
    std::span<const std::uint8_t> message, std::string &out_event,
    std::string &out_json_args) {
  if (message.size() < 2) {
    return false;
  }
  const std::size_t name_bytes =
      static_cast<std::size_t>(message[0]) |
      (static_cast<std::size_t>(message[1]) << 8);
  if (name_bytes == 0 || 2 + name_bytes > message.size()) {
    return false;
  }
  out_event.assign(reinterpret_cast<const char *>(message.data() + 2),
                   name_bytes);
  out_json_args.assign(
      reinterpret_cast<const char *>(message.data() + 2 + name_bytes),
      message.size() - 2 - name_bytes);
  return true;
}

}  // namespace skate3::multiplayer::protocol_v12
