#pragma once

// Server-side store of player appearances.
//
// WHY THE SERVER HOLDS THESE. Appearance used to travel peer-to-peer: every
// client fanned its own blob out to every other client over UDP, in chunks,
// with a blind retry budget. That has two problems no amount of retrying
// fixes properly. The cost is O(peers) per sender, repeated for every
// joiner. And the SENDER decides when to transmit, which is necessarily the
// wrong moment - a player already in the world builds their appearance while
// the joining player is still loading, so the transfer reliably runs before
// the recipient can accept it.
//
// Putting the blob on the server inverts that: a client uploads ONCE, and
// each peer fetches when IT is ready. The server is always ready to receive,
// so there is no window to miss.
//
// DEDUPLICATION IS FREE, and worth having. An appearance id is a content
// hash of the recipe, so two players wearing the same thing are one entry,
// and a player who reconnects or changes back to a previous look re-uploads
// nothing.
//
// CHANGING APPEARANCE MID-SESSION is just a new id: the role's current id
// moves, the old blob stays until nothing references it. Peers notice
// because the id they were told no longer matches what they have installed,
// and fetch the new one. Nothing needs to push bytes at anyone.
//
// This header is deliberately free of rex, sockets, and JSON so the server
// and the tests share exactly one implementation of the eviction and
// reference rules - the parts that are easy to get subtly wrong.

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skate3::appearance_store {

using Blob = std::shared_ptr<const std::vector<std::uint8_t>>;

// One appearance a role is currently wearing.
struct RosterEntry {
  std::uint32_t role = 0;
  std::uint64_t appearance_id = 0;
};

class AppearanceStore {
 public:
  // `capacity` bounds DISTINCT stored blobs, not roles. It should exceed the
  // player limit: a player who changes appearance briefly leaves the old
  // blob behind until it is evicted, and evicting an appearance that is
  // still worn would send a peer to fetch something that is no longer there.
  explicit AppearanceStore(std::size_t capacity) : capacity_(capacity) {}

  // Stores a blob under its content id. Returns false for an empty blob or
  // one over `max_bytes`, which is the only validation possible here - the
  // bytes are a client's recipe and the server never interprets them.
  bool Put(std::uint64_t appearance_id, std::vector<std::uint8_t> bytes,
           std::size_t max_bytes) {
    if (appearance_id == 0 || bytes.empty() || bytes.size() > max_bytes) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = blobs_.find(appearance_id);
    if (existing != blobs_.end()) {
      // Same id means same content, so re-uploading is a no-op rather than a
      // replace. Touching it still matters: it keeps a blob that peers are
      // actively fetching away from the eviction end.
      existing->second.last_touch = ++clock_;
      return true;
    }
    blobs_.emplace(appearance_id,
                   Entry{std::make_shared<const std::vector<std::uint8_t>>(
                             std::move(bytes)),
                         ++clock_});
    EvictLocked();
    return true;
  }

  // The blob for an id, or nullptr. Fetching counts as use, so an appearance
  // peers keep asking for is not the one thrown away.
  Blob Get(std::uint64_t appearance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = blobs_.find(appearance_id);
    if (entry == blobs_.end()) {
      return nullptr;
    }
    entry->second.last_touch = ++clock_;
    return entry->second.bytes;
  }

  bool Has(std::uint64_t appearance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return blobs_.find(appearance_id) != blobs_.end();
  }

  // Records what a role is wearing now. Returns true when this CHANGES the
  // role's appearance, which is the caller's signal to tell the other
  // players - so an unchanged re-announcement does not cause a broadcast.
  bool SetRoleAppearance(std::uint32_t role, std::uint64_t appearance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = roles_.find(role);
    if (existing != roles_.end() && existing->second == appearance_id) {
      return false;
    }
    roles_[role] = appearance_id;
    return true;
  }

  std::uint64_t RoleAppearance(std::uint32_t role) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = roles_.find(role);
    return entry == roles_.end() ? 0 : entry->second;
  }

  // A disconnecting player stops wearing anything. Their blob deliberately
  // STAYS: they are the most likely person to reconnect, and it costs one
  // entry to make that free.
  void ForgetRole(std::uint32_t role) {
    std::lock_guard<std::mutex> lock(mutex_);
    roles_.erase(role);
  }

  std::vector<RosterEntry> Roster() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RosterEntry> roster;
    roster.reserve(roles_.size());
    for (const auto& [role, appearance_id] : roles_) {
      roster.push_back({role, appearance_id});
    }
    return roster;
  }

  std::size_t StoredBlobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return blobs_.size();
  }

 private:
  struct Entry {
    Blob bytes;
    std::uint64_t last_touch = 0;
  };

  // Least-recently-used, except that anything a connected role is WEARING is
  // never evicted regardless of age. A worn appearance is precisely the one
  // a joining player is about to ask for, and dropping it would hand them
  // the featureless proxy - the failure this store exists to remove.
  void EvictLocked() {
    while (blobs_.size() > capacity_) {
      std::uint64_t victim = 0;
      std::uint64_t oldest = UINT64_MAX;
      for (const auto& [appearance_id, entry] : blobs_) {
        // The newest entry is never the victim. An upload and the
        // announcement of who is wearing it arrive SEPARATELY, so a blob
        // that has just landed is routinely not yet worn by anybody - and
        // evicting on that basis would throw away the appearance of the
        // player who just joined, which is the one case this store exists
        // to serve. Caught by a test rather than in a session.
        if (entry.last_touch == clock_) {
          continue;
        }
        if (entry.last_touch < oldest && !WornLocked(appearance_id)) {
          oldest = entry.last_touch;
          victim = appearance_id;
        }
      }
      if (victim == 0) {
        // Everything left is in use. Going over capacity is the right
        // outcome: the alternative is evicting an appearance someone is
        // wearing, and the bound exists to cap idle history, not to refuse
        // to dress the players who are actually here.
        return;
      }
      blobs_.erase(victim);
    }
  }

  bool WornLocked(std::uint64_t appearance_id) const {
    for (const auto& [role, worn] : roles_) {
      (void)role;
      if (worn == appearance_id) {
        return true;
      }
    }
    return false;
  }

  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::uint64_t clock_ = 0;
  std::unordered_map<std::uint64_t, Entry> blobs_;
  std::unordered_map<std::uint32_t, std::uint64_t> roles_;
};

}  // namespace skate3::appearance_store
