// Property tests for the server-side prop store.
//
// The rules worth pinning down are the ones that decide whether a player
// sees the right geometry: what the radius query includes, what a full or
// malformed request does, and that ids stay stable as props come and go.

#include "skate3_prop_store.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

using skate3::prop_store::PropStore;

void AddAssignsIncreasingIds() {
  PropStore store(16);
  const std::uint32_t first = store.Add("a.skateobj", 0, 0, 0, 300.0f);
  const std::uint32_t second = store.Add("b.skateobj", 1, 0, 0, 300.0f);
  Check(first == 1 && second == 2, "ids increase from 1");
  Check(store.Size() == 2, "both props stored");
}

void RejectsUnusableProps() {
  PropStore store(16);
  Check(store.Add("", 0, 0, 0, 300.0f) == 0, "empty path rejected");
  Check(store.Add("a", std::nanf(""), 0, 0, 300.0f) == 0, "NaN rejected");
  Check(store.Add("a", 1e38f, 0, 0, 300.0f) == 0, "absurd position rejected");
  Check(store.Size() == 0, "nothing unusable was stored");
}

// A prop is content someone placed. Dropping the oldest to make room would
// make a map quietly decay, so a full store refuses instead.
void FullStoreRefusesRatherThanEvicting() {
  PropStore store(2);
  Check(store.Add("a", 0, 0, 0, 300.0f) != 0, "first fits");
  Check(store.Add("b", 0, 0, 0, 300.0f) != 0, "second fits");
  Check(store.Add("c", 0, 0, 0, 300.0f) == 0, "third refused");
  Check(store.Size() == 2, "nothing was evicted");
}

void NearUsesTrueDistance() {
  PropStore store(16);
  store.Add("close", 10.0f, 0.0f, 0.0f, 300.0f);
  store.Add("far", 500.0f, 0.0f, 0.0f, 300.0f);
  // Diagonal: inside on each axis but outside by true distance, which a
  // box test would wrongly include.
  store.Add("diagonal", 80.0f, 0.0f, 80.0f, 300.0f);

  const auto nearby = store.Near(0.0f, 0.0f, 0.0f, 100.0f);
  bool saw_close = false, saw_far = false, saw_diagonal = false;
  for (const auto& prop : nearby) {
    if (prop.path == "close") saw_close = true;
    if (prop.path == "far") saw_far = true;
    if (prop.path == "diagonal") saw_diagonal = true;
  }
  Check(saw_close, "a nearby prop is returned");
  Check(!saw_far, "a distant prop is excluded");
  // 80,80 is inside the radius on each axis but 113 units away in truth -
  // a box test would wrongly include it, a sphere test must not.
  Check(!saw_diagonal, "a diagonal prop outside the true radius is excluded");
}

void NearRejectsNonsenseQueries() {
  PropStore store(16);
  store.Add("a", 0, 0, 0, 300.0f);
  Check(store.Near(0, 0, 0, 0.0f).empty(), "zero radius returns nothing");
  Check(store.Near(std::nanf(""), 0, 0, 100.0f).empty(),
        "NaN query returns nothing");
}

// The LOD travels with the prop: the placer decides how far their object
// reads from, not each client.
void LodIsCarriedAndSanitised() {
  PropStore store(16);
  store.Add("a", 0, 0, 0, 150.0f);
  store.Add("b", 0, 0, 0, std::nanf(""));
  store.Add("c", 0, 0, 0, 0.0f);
  const auto all = store.All();
  Check(all.size() == 3, "three props");
  Check(all[0].lod == 150.0f, "explicit lod is kept");
  Check(all[1].lod == 300.0f, "a nonsense lod falls back to the default");
  Check(all[2].lod == 0.0f, "zero is kept - it legitimately means 'always'");
}

void RemoveTakesOneWithoutDisturbingIds() {
  PropStore store(16);
  const std::uint32_t a = store.Add("a", 0, 0, 0, 300.0f);
  const std::uint32_t b = store.Add("b", 0, 0, 0, 300.0f);
  Check(store.Remove(a), "removing an existing prop succeeds");
  Check(!store.Remove(a), "removing it twice fails");
  const auto all = store.All();
  Check(all.size() == 1 && all[0].id == b,
        "the other prop keeps its id after a removal");
  Check(store.Add("c", 0, 0, 0, 300.0f) == b + 1,
        "ids keep increasing rather than reusing a removed one");
}

}  // namespace

int main() {
  AddAssignsIncreasingIds();
  RejectsUnusableProps();
  FullStoreRefusesRatherThanEvicting();
  NearUsesTrueDistance();
  NearRejectsNonsenseQueries();
  LodIsCarriedAndSanitised();
  RemoveTakesOneWithoutDisturbingIds();

  if (g_failures != 0) {
    std::printf("%d prop-store check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("prop store: all checks passed\n");
  return 0;
}
