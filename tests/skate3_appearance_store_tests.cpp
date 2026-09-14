// Property tests for the server-side appearance store.
//
// The interesting rules are the ones that decide whether a joining player
// gets dressed or gets the featureless proxy, so those are what is tested:
// deduplication, change detection, and above all that eviction can never
// throw away an appearance somebody is currently wearing.

#include "skate3_appearance_store.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

std::vector<std::uint8_t> Bytes(std::size_t count, std::uint8_t fill) {
  return std::vector<std::uint8_t>(count, fill);
}

constexpr std::size_t kMaxBytes = 1024 * 1024;

using skate3::appearance_store::AppearanceStore;

void PutAndGetRoundTrips() {
  AppearanceStore store(8);
  Check(store.Put(0x1111, Bytes(16, 0xAB), kMaxBytes), "put succeeds");
  const auto blob = store.Get(0x1111);
  Check(blob != nullptr, "get returns the blob");
  Check(blob->size() == 16 && blob->front() == 0xAB, "bytes round-trip");
  Check(store.Get(0x2222) == nullptr, "unknown id returns nothing");
}

void RejectsUnusableBlobs() {
  AppearanceStore store(8);
  Check(!store.Put(0, Bytes(16, 1), kMaxBytes), "id 0 is rejected");
  Check(!store.Put(0x10, {}, kMaxBytes), "empty blob is rejected");
  Check(!store.Put(0x10, Bytes(32, 1), 16), "oversized blob is rejected");
  Check(store.StoredBlobs() == 0, "nothing unusable was stored");
}

void ReuploadIsANoOp() {
  AppearanceStore store(8);
  store.Put(0x1111, Bytes(16, 0xAB), kMaxBytes);
  store.Put(0x1111, Bytes(16, 0xAB), kMaxBytes);
  Check(store.StoredBlobs() == 1, "same id stores once");
}

void RoleChangeIsReportedOnlyWhenItChanges() {
  AppearanceStore store(8);
  Check(store.SetRoleAppearance(1, 0xAAAA), "first assignment is a change");
  Check(!store.SetRoleAppearance(1, 0xAAAA), "same id again is not a change");
  Check(store.SetRoleAppearance(1, 0xBBBB), "a new id is a change");
  Check(store.RoleAppearance(1) == 0xBBBB, "role reports its current id");
  Check(store.RoleAppearance(2) == 0, "unknown role wears nothing");
}

void ForgettingARoleKeepsItsBlob() {
  AppearanceStore store(8);
  store.Put(0x1111, Bytes(16, 1), kMaxBytes);
  store.SetRoleAppearance(1, 0x1111);
  store.ForgetRole(1);
  Check(store.RoleAppearance(1) == 0, "role is forgotten");
  Check(store.Get(0x1111) != nullptr,
        "blob survives disconnect so a reconnect is free");
}

void EvictionDropsTheLeastRecentlyUsed() {
  AppearanceStore store(2);
  store.Put(0x01, Bytes(8, 1), kMaxBytes);
  store.Put(0x02, Bytes(8, 2), kMaxBytes);
  // Touch 0x01 so 0x02 becomes the oldest.
  (void)store.Get(0x01);
  store.Put(0x03, Bytes(8, 3), kMaxBytes);
  Check(store.StoredBlobs() == 2, "capacity is held");
  Check(store.Get(0x01) != nullptr, "recently used entry survives");
  Check(store.Get(0x03) != nullptr, "newest entry survives");
  Check(store.Get(0x02) == nullptr, "least recently used was evicted");
}

// The one that matters: a player standing in the world must never have their
// appearance evicted, however old it is, or a joining player fetches a 404
// and stays a box.
void WornAppearancesAreNeverEvicted() {
  AppearanceStore store(2);
  store.Put(0xA1, Bytes(8, 1), kMaxBytes);
  store.SetRoleAppearance(1, 0xA1);
  store.Put(0xA2, Bytes(8, 2), kMaxBytes);
  store.SetRoleAppearance(2, 0xA2);
  // 0xA1 is the oldest by far, but it is being worn.
  for (std::uint64_t id = 0xB0; id < 0xB4; ++id) {
    store.Put(id, Bytes(8, 9), kMaxBytes);
  }
  Check(store.Get(0xA1) != nullptr, "worn appearance survives eviction");
  Check(store.Get(0xA2) != nullptr, "other worn appearance survives too");
}

void CapacityIsExceededRatherThanEvictingWornEntries() {
  AppearanceStore store(1);
  store.Put(0xA1, Bytes(8, 1), kMaxBytes);
  store.SetRoleAppearance(1, 0xA1);
  store.Put(0xA2, Bytes(8, 2), kMaxBytes);
  store.SetRoleAppearance(2, 0xA2);
  Check(store.StoredBlobs() == 2,
        "two worn appearances both stay despite capacity 1");
}

void RosterListsEveryDressedRole() {
  AppearanceStore store(8);
  store.SetRoleAppearance(3, 0x33);
  store.SetRoleAppearance(7, 0x77);
  const auto roster = store.Roster();
  Check(roster.size() == 2, "roster has both roles");
  bool saw_three = false, saw_seven = false;
  for (const auto& entry : roster) {
    if (entry.role == 3 && entry.appearance_id == 0x33) saw_three = true;
    if (entry.role == 7 && entry.appearance_id == 0x77) saw_seven = true;
  }
  Check(saw_three && saw_seven, "roster reports the right ids");
}

}  // namespace

int main() {
  PutAndGetRoundTrips();
  RejectsUnusableBlobs();
  ReuploadIsANoOp();
  RoleChangeIsReportedOnlyWhenItChanges();
  ForgettingARoleKeepsItsBlob();
  EvictionDropsTheLeastRecentlyUsed();
  WornAppearancesAreNeverEvicted();
  CapacityIsExceededRatherThanEvictingWornEntries();
  RosterListsEveryDressedRole();

  if (g_failures != 0) {
    std::printf("%d appearance-store check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("appearance store: all checks passed\n");
  return 0;
}
