#pragma once

// The retransmit/reorder engine behind the reliable channel. Deliberately
// free of sockets, threads and clocks: it is handed bytes and a timestamp and
// hands back the datagrams that should go out, which is what makes the
// delivery guarantees testable without a network.
//
// Guarantees, per (peer, channel):
//   * every message is delivered at least once, and duplicates are dropped,
//   * messages are delivered in the order they were queued,
//   * a message is retransmitted whole until acknowledged.
//
// It does NOT guarantee anything across channels, on purpose: script events
// must not be held up behind a large state-bag sync, and vice versa.

#include "skate3_multiplayer_protocol_v12_reliable.h"

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skate3::multiplayer {

// A retransmission is due after this long without an acknowledgement. Chosen
// well above any plausible LAN round trip so a healthy link never resends,
// and low enough that a real loss is repaired within a couple of frames of
// human perception.
inline constexpr std::uint64_t kReliableRetransmitIntervalUs = 250'000;
// After this many attempts the peer is treated as gone and the message is
// dropped rather than queued forever. At the interval above this is ~5s.
inline constexpr std::uint32_t kReliableMaximumAttempts = 20;
// A sender refuses to queue beyond this depth. A peer that has stopped
// acknowledging must not be able to grow our memory without bound.
inline constexpr std::size_t kReliableMaximumQueuedMessages = 256;

// One message the engine wants sent, already framed as fragments.
struct ReliableOutboundFragment {
  protocol_v12::ReliableHeader header;
  std::span<const std::uint8_t> payload;  // into the queued message's bytes
};

// Sender half for one (peer, channel).
class ReliableSender {
 public:
  // Queues a message for delivery. Returns 0 if it was refused - too large,
  // or the queue is full because the peer stopped acknowledging.
  std::uint32_t Queue(std::vector<std::uint8_t> bytes,
                      std::uint16_t target_role) {
    if (bytes.size() > protocol_v12::kReliableMaximumMessageBytes ||
        pending_.size() >= kReliableMaximumQueuedMessages) {
      return 0;
    }
    Pending entry;
    entry.message_id = ++last_message_id_;
    entry.target_role = target_role;
    entry.bytes = std::move(bytes);
    entry.next_send_us = 0;  // send on the very next Collect
    pending_.push_back(std::move(entry));
    return pending_.back().message_id;
  }

  // Fills `out` with every fragment due to be sent at `now_us`, marking them
  // sent. Call once per network tick.
  void Collect(std::uint64_t now_us,
               std::vector<ReliableOutboundFragment> &out) {
    for (auto entry = pending_.begin(); entry != pending_.end();) {
      if (entry->next_send_us > now_us) {
        ++entry;
        continue;
      }
      if (entry->attempts >= kReliableMaximumAttempts) {
        // Give up on this one; the peer is not acknowledging anything.
        ++dropped_messages_;
        entry = pending_.erase(entry);
        continue;
      }
      const std::uint16_t fragment_count = protocol_v12::ReliableFragmentCount(
          static_cast<std::uint32_t>(entry->bytes.size()));
      for (std::uint16_t index = 0; index < fragment_count; ++index) {
        protocol_v12::ReliableHeader header;
        header.message_id = entry->message_id;
        header.total_bytes = static_cast<std::uint32_t>(entry->bytes.size());
        header.fragment_index = index;
        header.fragment_count = fragment_count;
        header.target_role = entry->target_role;
        out.push_back({header, std::span<const std::uint8_t>(entry->bytes)});
      }
      ++entry->attempts;
      if (entry->attempts > 1) {
        ++retransmissions_;
      }
      entry->next_send_us = now_us + kReliableRetransmitIntervalUs;
      ++entry;
    }
  }

  // Retires everything the peer says it has taken delivery of.
  void Acknowledge(const protocol_v12::ReliableAck &ack) {
    for (auto entry = pending_.begin(); entry != pending_.end();) {
      bool acknowledged = entry->message_id <= ack.contiguous_id;
      if (!acknowledged && entry->message_id > ack.contiguous_id) {
        const std::uint32_t distance = entry->message_id - ack.contiguous_id;
        if (distance >= 1 && distance <= 32) {
          acknowledged = (ack.ahead_bits & (1u << (distance - 1))) != 0;
        }
      }
      entry = acknowledged ? pending_.erase(entry) : std::next(entry);
    }
  }

  [[nodiscard]] bool idle() const { return pending_.empty(); }
  [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }
  [[nodiscard]] std::uint64_t retransmissions() const {
    return retransmissions_;
  }
  [[nodiscard]] std::uint64_t dropped_messages() const {
    return dropped_messages_;
  }

 private:
  struct Pending {
    std::uint32_t message_id = 0;
    std::uint16_t target_role = 0;
    std::uint32_t attempts = 0;
    std::uint64_t next_send_us = 0;
    std::vector<std::uint8_t> bytes;
  };

  std::deque<Pending> pending_;
  std::uint32_t last_message_id_ = 0;
  std::uint64_t retransmissions_ = 0;
  std::uint64_t dropped_messages_ = 0;
};

// Receiver half for one (peer, channel): reassembles fragments, discards
// duplicates, and releases messages strictly in order.
class ReliableReceiver {
 public:
  // Takes one fragment. Any messages that became deliverable are appended to
  // `out` in order. Returns false if the fragment was malformed or a
  // duplicate of something already delivered.
  bool Accept(const protocol_v12::ReliableHeader &header,
              std::span<const std::uint8_t> fragment,
              std::vector<std::vector<std::uint8_t>> &out) {
    if (!protocol_v12::ReliableHeaderShapeValid(header)) {
      return false;
    }
    if (header.message_id <= delivered_through_) {
      return false;  // already delivered; the ack was lost, not the message.
    }
    if (header.message_id - delivered_through_ > kReliableReorderWindow) {
      // Far beyond anything we can hold. Dropping is correct: the sender
      // will keep retransmitting, and accepting it would let a peer push the
      // window arbitrarily far ahead.
      return false;
    }

    Assembly &assembly = assemblies_[header.message_id];
    if (assembly.fragment_count == 0) {
      assembly.fragment_count = header.fragment_count;
      assembly.total_bytes = header.total_bytes;
      assembly.bytes.assign(header.total_bytes, 0);
      assembly.received.assign(header.fragment_count, false);
    } else if (assembly.fragment_count != header.fragment_count ||
               assembly.total_bytes != header.total_bytes) {
      return false;  // sender contradicting itself; ignore the odd one out.
    }
    if (assembly.received[header.fragment_index]) {
      return true;  // duplicate fragment of a message still assembling.
    }
    const std::uint16_t fragment_bytes = protocol_v12::ReliableFragmentByteCount(
        header.total_bytes, header.fragment_index);
    if (fragment.size() != fragment_bytes) {
      return false;
    }
    if (fragment_bytes != 0) {
      std::memcpy(assembly.bytes.data() +
                      protocol_v12::ReliableFragmentOffset(header.fragment_index),
                  fragment.data(), fragment_bytes);
    }
    assembly.received[header.fragment_index] = true;
    ++assembly.received_count;
    if (assembly.received_count == assembly.fragment_count) {
      assembly.complete = true;
    }
    ReleaseInOrder(out);
    return true;
  }

  // What to tell the sender: everything through `contiguous_id` is delivered,
  // plus a bitfield of complete-but-not-yet-deliverable messages beyond it so
  // it need not resend those either.
  [[nodiscard]] protocol_v12::ReliableAck BuildAck(std::uint16_t channel) const {
    protocol_v12::ReliableAck ack;
    ack.channel = channel;
    ack.contiguous_id = delivered_through_;
    for (const auto &[message_id, assembly] : assemblies_) {
      if (!assembly.complete || message_id <= delivered_through_) {
        continue;
      }
      const std::uint32_t distance = message_id - delivered_through_;
      if (distance >= 1 && distance <= 32) {
        ack.ahead_bits |= 1u << (distance - 1);
      }
    }
    return ack;
  }

  [[nodiscard]] std::uint32_t delivered_through() const {
    return delivered_through_;
  }

 private:
  // How far ahead of the next expected message the receiver will hold
  // out-of-order arrivals. 32 matches the ack bitfield exactly, so nothing
  // can be held that the ack cannot describe.
  static constexpr std::uint32_t kReliableReorderWindow = 32;

  struct Assembly {
    std::uint16_t fragment_count = 0;
    std::uint16_t received_count = 0;
    std::uint32_t total_bytes = 0;
    bool complete = false;
    std::vector<bool> received;
    std::vector<std::uint8_t> bytes;
  };

  void ReleaseInOrder(std::vector<std::vector<std::uint8_t>> &out) {
    for (;;) {
      const auto next = assemblies_.find(delivered_through_ + 1);
      if (next == assemblies_.end() || !next->second.complete) {
        return;
      }
      out.push_back(std::move(next->second.bytes));
      ++delivered_through_;
      assemblies_.erase(next);
    }
  }

  // Ordered so ReleaseInOrder and BuildAck can walk it predictably.
  std::map<std::uint32_t, Assembly> assemblies_;
  std::uint32_t delivered_through_ = 0;
};

}  // namespace skate3::multiplayer
