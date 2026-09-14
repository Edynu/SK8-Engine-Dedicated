#pragma once

// Server-side store of networked world props.
//
// A prop is a package path, a position, and a render distance. The server
// owns the list; clients ask what is near them and spawn it. That inversion
// is the same one the appearance store makes (skate3_appearance_store.h),
// and for the same reason: a prop placed by one player has to exist for a
// player who joins an hour later, so it cannot live in any one session.
//
// WHY THIS IS C++ AND NOT A LUA RESOURCE. Deciding which geometry a client
// should load is engine work: it runs against every player's position, it
// happens whether or not a game mode is loaded, and it must keep working
// while scripts are restarted. Lua is for the rules of a game - who has
// which letters, whose turn it is - not for streaming the world. Keeping
// the two apart also means a broken game mode cannot leave the world
// half-loaded.
//
// The query is a linear scan. At the scale this is for - hundreds of props,
// tens of players, asked once a second per player - that is a few thousand
// comparisons, far below the cost of the packet that carries the answer. A
// grid would be the obvious next step and is deliberately not here yet:
// there is no measurement saying it is needed, and an index that can drift
// out of sync with the list is a real bug in exchange for an imagined win.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace skate3::prop_store {

struct Prop {
  std::uint32_t id = 0;
  std::string path;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  // Render distance in world units, as the client should apply it. Carried
  // with the prop rather than assumed by the client so the placer decides
  // how far their object reads from.
  float lod = 300.0f;
};

class PropStore {
 public:
  // `capacity` bounds the world's prop count. Adding past it fails rather
  // than evicting: a prop is authored content someone placed deliberately,
  // and silently dropping the oldest would make a map decay.
  explicit PropStore(std::size_t capacity) : capacity_(capacity) {}

  // Returns the new prop's id, or 0 when rejected (no path, non-finite
  // position, or the store is full).
  std::uint32_t Add(std::string path, float x, float y, float z, float lod) {
    if (path.empty() || !Finite(x) || !Finite(y) || !Finite(z)) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (props_.size() >= capacity_) {
      return 0;
    }
    Prop prop;
    prop.id = ++next_id_;
    prop.path = std::move(path);
    prop.x = x;
    prop.y = y;
    prop.z = z;
    // A non-positive LOD means "always draw", which is a legitimate choice
    // for something large; only nonsense is corrected.
    prop.lod = Finite(lod) && lod >= 0.0f ? lod : 300.0f;
    props_.push_back(std::move(prop));
    const std::uint32_t id = props_.back().id;
    ++version_;
    changed_.notify_all();
    return id;
  }

  bool Remove(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto prop = props_.begin(); prop != props_.end(); ++prop) {
      if (prop->id == id) {
        props_.erase(prop);
        ++version_;
        changed_.notify_all();
        return true;
      }
    }
    return false;
  }

  // Props within `radius` of a point.
  //
  // Distance is measured to the PROP, not adjusted by its own LOD: the
  // streaming radius is the server's decision about what to send, and the
  // prop's LOD is the client's decision about what to draw. Keeping them
  // separate is what lets a client be sent something slightly before it
  // needs to draw it, instead of popping in at the exact moment it becomes
  // visible.
  std::vector<Prop> Near(float x, float y, float z, float radius) const {
    std::vector<Prop> nearby;
    if (!Finite(x) || !Finite(y) || !Finite(z) || radius <= 0.0f) {
      return nearby;
    }
    const float radius_squared = radius * radius;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Prop& prop : props_) {
      const float dx = prop.x - x;
      const float dy = prop.y - y;
      const float dz = prop.z - z;
      if (dx * dx + dy * dy + dz * dz <= radius_squared) {
        nearby.push_back(prop);
      }
    }
    return nearby;
  }

  std::vector<Prop> All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return props_;
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return props_.size();
  }

  // Bumped by every Add and Remove. A client holds the last version it saw
  // and waits for a higher one, which is what turns "ask every second" into
  // "be told when something changes".
  std::uint64_t Version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
  }

  // Blocks until the store changes past `version`, or the timeout expires.
  // Returns the current version either way, so a caller that timed out
  // simply waits again from where it was.
  //
  // This is what makes delivery a PUSH: a placement wakes every waiting
  // client immediately instead of each one discovering it on its own poll.
  // The waiting costs a parked thread per client and no traffic at all,
  // where polling costs a request per client per interval forever.
  std::uint64_t WaitForChange(std::uint64_t version,
                              std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [&] { return version_ > version; });
    return version_;
  }

  // Releases every waiting client so a server can shut down promptly rather
  // than waiting out their timeouts.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++version_;
    }
    changed_.notify_all();
  }

 private:
  static bool Finite(float value) {
    // Deliberately not std::isfinite on a float that may have come off a
    // wire as anything: NaN compares false against itself, which is the
    // property being used here.
    return value == value && value > -1e30f && value < 1e30f;
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::uint64_t version_ = 0;
  std::size_t capacity_;
  std::uint32_t next_id_ = 0;
  std::vector<Prop> props_;
};

}  // namespace skate3::prop_store
