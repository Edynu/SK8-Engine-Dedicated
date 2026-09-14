// Which peers a given client may see, and what they are wearing.
//
// This is the server deciding visibility rather than the client filtering a
// global list, and it applies the SAME rules the packet router applies in
// RouteRaw: same map, same routing bucket, within sv_radius. A peer the
// relay would not forward packets from is a peer whose appearance that
// client has no business downloading.

#include "skate3_dedicated_server.h"

#include "skate3_appearance_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::dedicated {

skate3::appearance_store::AppearanceStore *g_appearance_store = nullptr;

// Who `viewer_role` can currently see, and what each of them is wearing.
//
// This is the server deciding visibility rather than the client filtering a
// global list, and it deliberately applies the SAME rules the packet router
// applies in RouteRaw: same map, same routing bucket, and within sv_radius.
// A peer the relay would not forward packets from is a peer whose appearance
// this viewer has no business downloading.
//
// Reads the skate3::dedicated::PlayerRegistry snapshot, not the router: the relay loop mutates
// the router on its own thread and this runs on an HTTP worker.
//
// Peers with no stored appearance are omitted - there is nothing to fetch,
// and leaving them out keeps them from perturbing the version hash.
[[nodiscard]] std::vector<VisiblePeer> VisibleAppearances(
    std::uint32_t viewer_role, float radius) {
  std::vector<VisiblePeer> visible;
  if (viewer_role == 0 || g_appearance_store == nullptr) {
    return visible;
  }
  const auto viewer = skate3::dedicated::g_players.Find(viewer_role);
  if (!viewer.has_value()) {
    return visible;
  }
  std::unordered_map<std::uint32_t, std::uint64_t> worn;
  for (const auto &entry : g_appearance_store->Roster()) {
    worn[entry.role] = entry.appearance_id;
  }
  for (const auto &player : skate3::dedicated::g_players.All()) {
    if (player.id == viewer_role || player.map_hash != viewer->map_hash ||
        player.bucket != viewer->bucket) {
      continue;
    }
    if (radius > 0.0f && player.position_valid && viewer->position_valid) {
      const float dx = player.x - viewer->x;
      const float dy = player.y - viewer->y;
      const float dz = player.z - viewer->z;
      if (dx * dx + dy * dy + dz * dz > radius * radius) {
        continue;
      }
    }
    const auto found = worn.find(player.id);
    const std::uint64_t appearance_id =
        found == worn.end() ? 0 : found->second;
    if (appearance_id == 0 && player.name.empty()) {
      continue;  // nothing to say about this peer yet.
    }
    // The NAME rides here too, not just the appearance. It used to reach
    // clients only as a one-shot "skate3:playerNamed" script event, so a
    // client that was not listening at that instant showed a role number
    // ("Player 3") for the rest of the session with nothing to correct it.
    // This answer is re-sent whenever it changes and is scoped to peers the
    // viewer can see, which is exactly the same guarantee appearances get.
    visible.push_back({.role = player.id,
                       .appearance_id = appearance_id,
                       .name = player.name});
  }
  // Sorted so the hash below depends on the CONTENT of the answer and not on
  // the order the registry happened to hand the players over.
  std::sort(visible.begin(), visible.end(),
            [](const auto &left, const auto &right) {
              return left.role < right.role;
            });
  return visible;
}

// FNV-1a over the (role, appearance) pairs. Serves as the long-poll version:
// equal hash means this viewer's answer has not changed, whatever else moved
// in the world.
[[nodiscard]] std::uint64_t HashAppearanceRoster(
    const std::vector<VisiblePeer> &roster) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= (value >> (byte * 8)) & 0xFFull;
      hash *= 1099511628211ull;
    }
  };
  for (const auto &entry : roster) {
    mix(entry.role);
    mix(entry.appearance_id);
    for (const char character : entry.name) {
      mix(static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
    }
  }
  // Never collide with "caller has seen nothing yet".
  return hash == 0 ? 1 : hash;
}


}  // namespace skate3::dedicated
