// Delivery guarantees for the reliable channel, exercised without a socket.
//
// These are the properties script events depend on and that the rest of the
// v12 protocol deliberately does not provide: retransmission until
// acknowledged, duplicate suppression, and in-order release across
// reordering and loss.

#include "skate3_multiplayer_reliable_channel.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using skate3::multiplayer::ReliableOutboundFragment;
using skate3::multiplayer::ReliableReceiver;
using skate3::multiplayer::ReliableSender;
using skate3::multiplayer::kReliableRetransmitIntervalUs;
namespace protocol = skate3::multiplayer::protocol_v12;

int g_failures = 0;

void Check(bool condition, const char *what) {
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

std::vector<std::uint8_t> Bytes(const std::string &text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string Text(const std::vector<std::uint8_t> &bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Feeds every fragment straight through, as a lossless link would.
void Deliver(const std::vector<ReliableOutboundFragment> &fragments,
             ReliableReceiver &receiver,
             std::vector<std::vector<std::uint8_t>> &delivered) {
  for (const auto &fragment : fragments) {
    const std::uint16_t bytes = protocol::ReliableFragmentByteCount(
        fragment.header.total_bytes, fragment.header.fragment_index);
    const std::uint32_t offset =
        protocol::ReliableFragmentOffset(fragment.header.fragment_index);
    receiver.Accept(fragment.header,
                    fragment.payload.subspan(offset, bytes), delivered);
  }
}

void OrderedDeliveryOnACleanLink() {
  ReliableSender sender;
  ReliableReceiver receiver;
  sender.Queue(Bytes("first"), 0);
  sender.Queue(Bytes("second"), 0);
  sender.Queue(Bytes("third"), 0);

  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(1000, fragments);
  std::vector<std::vector<std::uint8_t>> delivered;
  Deliver(fragments, receiver, delivered);

  Check(delivered.size() == 3, "clean link delivers every message");
  Check(delivered.size() == 3 && Text(delivered[0]) == "first" &&
            Text(delivered[1]) == "second" && Text(delivered[2]) == "third",
        "clean link preserves order");

  sender.Acknowledge(receiver.BuildAck(protocol::kReliableChannelScriptEvent));
  Check(sender.idle(), "acknowledged messages are retired");
}

void RetransmitsUntilAcknowledged() {
  ReliableSender sender;
  sender.Queue(Bytes("payload"), 0);

  std::vector<ReliableOutboundFragment> first;
  sender.Collect(0, first);
  Check(first.size() == 1, "message is sent once immediately");

  // Nothing acknowledged, but the retransmit interval has not elapsed.
  std::vector<ReliableOutboundFragment> tooSoon;
  sender.Collect(kReliableRetransmitIntervalUs - 1, tooSoon);
  Check(tooSoon.empty(), "no resend before the retransmit interval");

  std::vector<ReliableOutboundFragment> resend;
  sender.Collect(kReliableRetransmitIntervalUs + 1, resend);
  Check(resend.size() == 1, "unacknowledged message is retransmitted");
  Check(sender.retransmissions() == 1, "retransmission is counted");
}

void SuppressesDuplicates() {
  ReliableSender sender;
  ReliableReceiver receiver;
  sender.Queue(Bytes("once"), 0);

  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(0, fragments);
  std::vector<std::vector<std::uint8_t>> delivered;
  Deliver(fragments, receiver, delivered);
  // The whole message arrives a second time - exactly what a lost ack
  // produces on a real link.
  Deliver(fragments, receiver, delivered);

  Check(delivered.size() == 1, "a redelivered message is not delivered twice");
}

void HoldsOutOfOrderArrivalsUntilTheGapFills() {
  ReliableSender sender;
  ReliableReceiver receiver;
  sender.Queue(Bytes("one"), 0);
  sender.Queue(Bytes("two"), 0);

  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(0, fragments);
  Check(fragments.size() == 2, "two single-fragment messages");

  std::vector<std::vector<std::uint8_t>> delivered;
  // Second message arrives first; the first is still in flight.
  const auto &second = fragments[1];
  receiver.Accept(second.header,
                  second.payload.subspan(0, second.header.total_bytes),
                  delivered);
  Check(delivered.empty(), "a message ahead of a gap is withheld");

  // Its completion is still acknowledged, so the sender need not resend it.
  const protocol::ReliableAck ack =
      receiver.BuildAck(protocol::kReliableChannelScriptEvent);
  Check(ack.contiguous_id == 0 && ack.ahead_bits == 0b10,
        "ack reports a complete message beyond the gap");

  const auto &first = fragments[0];
  receiver.Accept(first.header,
                  first.payload.subspan(0, first.header.total_bytes),
                  delivered);
  Check(delivered.size() == 2, "filling the gap releases both");
  Check(delivered.size() == 2 && Text(delivered[0]) == "one" &&
            Text(delivered[1]) == "two",
        "released messages are in queue order, not arrival order");
}

void ReassemblesAMultiFragmentMessage() {
  ReliableSender sender;
  ReliableReceiver receiver;
  // Comfortably over one fragment, so this genuinely spans several.
  const std::string large(protocol::kReliableFragmentBytes * 2 + 17, 'x');
  sender.Queue(Bytes(large), 0);

  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(0, fragments);
  Check(fragments.size() == 3, "message spans three fragments");

  // Delivered back to front, to prove reassembly does not assume order.
  std::vector<std::vector<std::uint8_t>> delivered;
  for (auto fragment = fragments.rbegin(); fragment != fragments.rend();
       ++fragment) {
    const std::uint16_t bytes = protocol::ReliableFragmentByteCount(
        fragment->header.total_bytes, fragment->header.fragment_index);
    const std::uint32_t offset =
        protocol::ReliableFragmentOffset(fragment->header.fragment_index);
    receiver.Accept(fragment->header, fragment->payload.subspan(offset, bytes),
                    delivered);
  }
  Check(delivered.size() == 1, "fragments reassemble into one message");
  Check(delivered.size() == 1 && Text(delivered[0]) == large,
        "reassembled bytes match what was queued");
}

void SurvivesLoss() {
  ReliableSender sender;
  ReliableReceiver receiver;
  sender.Queue(Bytes("alpha"), 0);
  sender.Queue(Bytes("beta"), 0);

  std::vector<std::vector<std::uint8_t>> delivered;
  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(0, fragments);

  // The first message is lost outright; only the second arrives.
  const auto &second = fragments[1];
  receiver.Accept(second.header,
                  second.payload.subspan(0, second.header.total_bytes),
                  delivered);
  sender.Acknowledge(receiver.BuildAck(protocol::kReliableChannelScriptEvent));
  Check(sender.pending_count() == 1, "only the lost message stays queued");
  Check(delivered.empty(), "nothing is delivered while the gap stands");

  // The retransmit repairs it and both are released, in order.
  fragments.clear();
  sender.Collect(kReliableRetransmitIntervalUs + 1, fragments);
  Deliver(fragments, receiver, delivered);
  Check(delivered.size() == 2, "retransmission repairs the gap");
  Check(delivered.size() == 2 && Text(delivered[0]) == "alpha" &&
            Text(delivered[1]) == "beta",
        "order survives loss and repair");
}

void RoundTripsThroughTheWireFormat() {
  ReliableSender sender;
  sender.Queue(Bytes("over the wire"), 7);
  std::vector<ReliableOutboundFragment> fragments;
  sender.Collect(0, fragments);
  Check(fragments.size() == 1, "single fragment to encode");

  protocol::Envelope envelope;
  envelope.kind = protocol::MessageKind::kReliableStream;
  envelope.flags = protocol::kFlagReliable;
  envelope.sender_role = 3;
  envelope.stream_id = protocol::kReliableChannelScriptEvent;
  envelope.sender_session = 0x1234;
  envelope.sequence = 9;
  envelope.payload_bytes =
      protocol::kReliableHeaderBytes +
      protocol::ReliableFragmentByteCount(fragments[0].header.total_bytes, 0);

  std::vector<std::uint8_t> packet(protocol::kEnvelopeBytes +
                                   envelope.payload_bytes);
  const bool encoded = protocol::EncodeReliableDatagram(
      envelope, fragments[0].header, fragments[0].payload, packet);
  Check(encoded, "reliable datagram encodes");

  protocol::Envelope decoded_envelope;
  protocol::ReliableHeader decoded_header;
  std::span<const std::uint8_t> decoded_fragment;
  const bool decoded = protocol::DecodeReliableDatagram(
      packet, decoded_envelope, decoded_header, decoded_fragment);
  Check(decoded, "reliable datagram decodes");
  Check(decoded && decoded_header.message_id == fragments[0].header.message_id,
        "message id survives the wire");
  Check(decoded && decoded_header.target_role == 7,
        "target role survives the wire");
  Check(decoded && std::string(decoded_fragment.begin(),
                               decoded_fragment.end()) == "over the wire",
        "payload survives the wire");
}

void RejectsMalformedDatagrams() {
  protocol::Envelope envelope;
  envelope.kind = protocol::MessageKind::kReliableStream;
  envelope.sender_role = 1;
  envelope.sender_session = 1;
  envelope.payload_bytes = protocol::kReliableHeaderBytes;

  protocol::ReliableHeader header;
  header.message_id = 0;  // never valid
  header.total_bytes = 0;
  header.fragment_index = 0;
  header.fragment_count = 1;
  std::vector<std::uint8_t> packet(protocol::kEnvelopeBytes +
                                   envelope.payload_bytes);
  Check(!protocol::EncodeReliableDatagram(envelope, header, {}, packet),
        "message id 0 is refused");

  header.message_id = 1;
  header.fragment_count = 2;  // contradicts total_bytes 0
  Check(!protocol::EncodeReliableDatagram(envelope, header, {}, packet),
        "fragment count inconsistent with size is refused");
}

}  // namespace

int main() {
  OrderedDeliveryOnACleanLink();
  RetransmitsUntilAcknowledged();
  SuppressesDuplicates();
  HoldsOutOfOrderArrivalsUntilTheGapFills();
  ReassemblesAMultiFragmentMessage();
  SurvivesLoss();
  RoundTripsThroughTheWireFormat();
  RejectsMalformedDatagrams();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d reliable-channel check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("reliable channel: all checks passed\n");
  return 0;
}
