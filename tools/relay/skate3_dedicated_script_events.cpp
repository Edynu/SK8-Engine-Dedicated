// Script events between the server and its clients, over the reliable
// channel of the game protocol.
//
// Split out of the main translation unit: this is a transport, and it has no
// business sitting next to the relay's packet loop. Server-side Lua raises
// events through QueueClientScriptEvent; clients raise them by sending on
// the same channel, and the host dispatches with `source` set from the
// ROUTER's view of who sent it, never from what the datagram claimed.

#include "skate3_dedicated_server.h"

#include "skate3_multiplayer_protocol_v12.h"
#include "skate3_multiplayer_protocol_v12_reliable.h"
#include "skate3_multiplayer_reliable_channel.h"

#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::dedicated {

namespace protocol_v12 = skate3::multiplayer::protocol_v12;
using skate3::multiplayer::routing::VisualRelayRouter;

// ---------------------------------------------------------------------


// Queued by TriggerClientEvent on a script thread, drained by the relay
// thread that owns the channels. `role` is kReliableBroadcastRole for
// "every client".
struct PendingClientEvent {
  std::uint16_t role = 0;
  std::vector<std::uint8_t> message;
};

std::mutex g_client_event_mutex;
std::deque<PendingClientEvent> g_client_events;

void QueueClientScriptEvent(std::uint16_t role, const std::string &event,
                            const std::string &json_args) {
  std::vector<std::uint8_t> message;
  if (!protocol_v12::EncodeScriptEventMessage(event, json_args, message)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_client_event_mutex);
  g_client_events.push_back({role, std::move(message)});
}

// The server is not a peer and holds no role of its own, so outbound
// envelopes carry a placeholder that only has to satisfy EnvelopeShapeValid.
// Clients identify these purely by kind and by the fact that they arrived
// from the server's address.
constexpr std::uint16_t kServerSenderRole = 1;
constexpr std::uint32_t kServerSessionId = 1;

std::uint32_t RoleForConnection(const VisualRelayRouter &router,
                                std::uint64_t connection_id) {
  for (const auto &peer : router.Peers()) {
    if (peer.connection_id == connection_id) {
      return peer.role;
    }
  }
  return 0;
}

void SendScriptDatagram(SocketHandle relay_socket, const sockaddr_in &address,
                        const std::uint8_t *bytes, std::size_t byte_count) {
  sendto(relay_socket, reinterpret_cast<const char *>(bytes),
         static_cast<int>(byte_count), 0,
         reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  skate3::dedicated::metrics::RecordSend(byte_count);
}

void SendScriptEventAck(SocketHandle relay_socket, const sockaddr_in &address,
                        ScriptEventPeer &peer, std::uint64_t now_us) {
  const protocol_v12::ReliableAck ack =
      peer.receiver.BuildAck(protocol_v12::kReliableChannelScriptEvent);
  protocol_v12::Envelope envelope;
  envelope.kind = protocol_v12::MessageKind::kReliableAck;
  envelope.flags = protocol_v12::kFlagReliable;
  envelope.payload_bytes = protocol_v12::kReliableAckPayloadBytes;
  envelope.sender_role = kServerSenderRole;
  envelope.stream_id = protocol_v12::kReliableChannelScriptEvent;
  envelope.sender_session = kServerSessionId;
  envelope.sequence = ++peer.sequence;
  envelope.sender_time_us = now_us;
  std::array<std::uint8_t, protocol_v12::kEnvelopeBytes +
                               protocol_v12::kReliableAckPayloadBytes>
      packet{};
  if (protocol_v12::EncodeReliableAckDatagram(envelope, ack, packet)) {
    SendScriptDatagram(relay_socket, address, packet.data(), packet.size());
  }
}

void HandleScriptEventPacket(ScriptEventPeers &peers,
                             skate3::lua_host::LuaScriptHost &host,
                             const VisualRelayRouter &router,
                             std::uint64_t connection_id,
                             std::span<const std::uint8_t> packet,
                             const protocol_v12::Envelope &envelope) {
  ScriptEventPeer &peer = peers[connection_id];
  if (envelope.kind == protocol_v12::MessageKind::kReliableAck) {
    protocol_v12::Envelope ack_envelope;
    protocol_v12::ReliableAck ack;
    if (protocol_v12::DecodeReliableAckDatagram(packet, ack_envelope, ack) &&
        ack.channel == protocol_v12::kReliableChannelScriptEvent) {
      peer.sender.Acknowledge(ack);
    }
    return;
  }

  protocol_v12::Envelope stream_envelope;
  protocol_v12::ReliableHeader header;
  std::span<const std::uint8_t> fragment;
  if (!protocol_v12::DecodeReliableDatagram(packet, stream_envelope, header,
                                            fragment) ||
      stream_envelope.stream_id !=
          protocol_v12::kReliableChannelScriptEvent) {
    return;
  }
  std::vector<std::vector<std::uint8_t>> delivered;
  peer.receiver.Accept(header, fragment, delivered);
  // Acknowledged even for a duplicate: a duplicate means our previous ack
  // was what went missing, so silence would loop forever.
  peer.ack_due = true;

  // The sender's identity comes from the ROUTER, not the envelope: a client
  // may claim any role it likes in a datagram it wrote itself, and `source`
  // is what server scripts authorise on.
  const std::uint32_t role = RoleForConnection(router, connection_id);
  if (role == 0) {
    return;  // not a registered connection; nothing to attribute this to.
  }
  for (const std::vector<std::uint8_t> &message : delivered) {
    std::string event;
    std::string json_args;
    if (!protocol_v12::DecodeScriptEventMessage(message, event, json_args)) {
      continue;
    }
    if (host.TryApplyStateBagEvent(event, json_args)) {
      continue;
    }
    host.DispatchNetworkEvent(event, json_args, static_cast<int>(role));
  }
}

void DrainScriptEvents(SocketHandle relay_socket, ScriptEventPeers &peers,
                       const VisualRelayRouter &router,
                       const std::unordered_map<std::uint64_t, sockaddr_in>
                           &addresses,
                       std::uint64_t now_us) {
  // Hand anything scripts queued to the right per-connection sender.
  std::deque<PendingClientEvent> pending;
  {
    std::lock_guard<std::mutex> lock(g_client_event_mutex);
    pending.swap(g_client_events);
  }
  if (!pending.empty()) {
    const auto connected = router.Peers();
    for (PendingClientEvent &entry : pending) {
      for (const auto &peer : connected) {
        const bool addressed =
            entry.role == protocol_v12::kReliableBroadcastRole ||
            peer.role == entry.role;
        if (!addressed) {
          continue;
        }
        // Copied per recipient: each connection's channel owns its own
        // retransmit state and may still be resending long after another
        // has acknowledged.
        peers[peer.connection_id].sender.Queue(entry.message, peer.role);
      }
    }
  }

  for (auto entry = peers.begin(); entry != peers.end();) {
    const auto address = addresses.find(entry->first);
    if (address == addresses.end()) {
      entry = peers.erase(entry);  // connection gone; drop its channel.
      continue;
    }
    ScriptEventPeer &peer = entry->second;
    std::vector<skate3::multiplayer::ReliableOutboundFragment> fragments;
    peer.sender.Collect(now_us, fragments);
    for (const auto &fragment : fragments) {
      const std::uint16_t fragment_bytes =
          protocol_v12::ReliableFragmentByteCount(
              fragment.header.total_bytes, fragment.header.fragment_index);
      protocol_v12::Envelope envelope;
      envelope.kind = protocol_v12::MessageKind::kReliableStream;
      envelope.flags = protocol_v12::kFlagReliable;
      envelope.payload_bytes =
          protocol_v12::kReliableHeaderBytes + fragment_bytes;
      envelope.sender_role = kServerSenderRole;
      envelope.stream_id = protocol_v12::kReliableChannelScriptEvent;
      envelope.sender_session = kServerSessionId;
      envelope.sequence = ++peer.sequence;
      envelope.sender_time_us = now_us;
      std::array<std::uint8_t, protocol_v12::kMaximumDatagramBytes> packet{};
      const std::size_t packet_bytes =
          protocol_v12::kEnvelopeBytes + envelope.payload_bytes;
      if (protocol_v12::EncodeReliableDatagram(
              envelope, fragment.header, fragment.payload,
              std::span<std::uint8_t>(packet).first(packet_bytes))) {
        SendScriptDatagram(relay_socket, address->second, packet.data(),
                           packet_bytes);
      }
    }
    if (peer.ack_due) {
      SendScriptEventAck(relay_socket, address->second, peer, now_us);
      peer.ack_due = false;
    }
    ++entry;
  }
}


}  // namespace skate3::dedicated
