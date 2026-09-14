#pragma once

// What a skater can wear, and what they are wearing.
//
// Skate 3 stores an outfit as a `cas_db` RECIPE: a short list of pieces,
// each naming a category ("Hair", "OuterTorso", "SkateBoard") and the asset
// ids that make it up. The items themselves live in createacharacter.big,
// which skate3_cac_catalogue.h already unpacks so the multiplayer appearance
// path can bind other players' clothes.
//
// This module is the READ half of Lua-driven wardrobe control: it answers
// what exists and what is currently worn. Nothing here changes an outfit -
// applying a piece needs retail's own rebuild path, which is not solved yet,
// and a half-working setter would be worse than an honest reader.
//
// WHY THE CATALOGUE IS NOT THE FRONT-END'S LIST. Retail's menu shows only
// items the player has unlocked through progression; this lists everything
// the archive contains, because the whole point is to take that decision
// away from the save file. An id here is loadable whether or not a career
// ever offered it.
//
// AN ID HERE IS A MODEL ID, NOT A RECIPE ASSET ID. This was measured, not
// assumed: reading a live outfit and testing both of its ids against the
// catalogue matched the model id on 12 of 12 pieces and the asset id on 0.
//
//   Arm  asset=0xc1bfb2aab883833c  model=0x00000dca03e38811  <- catalogue
//
// Asset ids are full-entropy 64-bit hashes; catalogue keys are structured
// (a table word plus an index) and come in two families - 539 items ending
// 03e38811 and 423 beginning 2c7f3811. A single outfit draws on both, so the
// families are not a base/DLC split along category lines. Anything trying to
// name an item by its recipe ASSET id will find nothing, and the reason will
// not be obvious from the failure.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::clothing {

struct CategoryInfo {
  std::string name;
  std::size_t count = 0;   // retail items plus any registered custom ones
  std::size_t custom = 0;  // how many of `count` were added by Register
};

// Categories and their item counts, alphabetically. Empty until the CAC
// catalogue has finished extracting (it runs on its own thread at startup),
// which is a "not yet" rather than an error - ask again.
std::vector<CategoryInfo> Categories();

std::size_t Count(std::string_view category);

// Model ids in `category`, lowercase hex, sorted. `filter` is a substring
// match against the id; `limit` caps the result. Retail items come first,
// then custom ones, so an index stays stable as customs are added.
std::vector<std::string> Ids(std::string_view category,
                             std::string_view filter, std::size_t limit);

// One id by position (0-based), or an empty string when out of range. This
// is the shape a script actually wants - "how many are there, give me the
// nth" - without shipping the whole list into Lua to index it.
std::string IdAt(std::string_view category, std::size_t index);

// Adds an id on top of the retail set, which is how custom clothing will
// enter this list later. Refuses a duplicate or an unusable name and
// returns false. Registering does NOT make an asset loadable on its own;
// it makes it VISIBLE to scripts, so the listing and the loader can be
// built one at a time instead of in one leap.
bool Register(std::string category, std::string id);

// Forgets the cached index so the next query rescans. Needed because the
// catalogue extracts asynchronously: a query made too early must not cache
// "no categories" forever.
void Invalidate();

// --- what is worn now -------------------------------------------------

struct OutfitPiece {
  std::string category;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t material_id = 0;
};

// Hands over the local player's live recipe bytes. Called from the render
// path, which already reads and validates them every frame for the
// multiplayer appearance blob - this module deliberately does not read guest
// memory itself, so there is exactly one place that knows where the recipe
// lives.
void PublishLocalRecipe(const std::uint8_t* bytes, std::size_t size);

// The pieces of the last published recipe. False when nothing has been
// published yet (no skater in the world) or the bytes did not parse.
bool LocalOutfit(std::vector<OutfitPiece>& out);

// --- changing what is worn ---------------------------------------------
//
// An override rewrites a category's MODEL id inside the live recipe. It is
// held here and re-applied every time the recipe is read, so it survives the
// game rewriting those bytes and does not have to win a race.
//
// WHAT THIS DOES AND DOES NOT CHANGE. The recipe is what this engine's own
// appearance pipeline resolves, so an override changes the skater OTHER
// PLAYERS see immediately. Whether retail redraws the LOCAL skater depends on
// it rebuilding the character from the recipe, which it does at its own
// moments and not on demand - so expect the local view to lag or need a
// respawn. That asymmetry is real and is why this is phrased as an override
// rather than "wear".
//
// A category's override applies to EVERY asset record in that category and at
// every LOD, so an item cannot change as the camera pulls back.
//
// The material and textures are NOT overridden: the recipe binds them per
// piece, and the catalogue only names models. A swapped model therefore wears
// the previous item's material - which for a differently-shaped garment can
// look wrong. Solving that needs the item database (cas_db XML) that maps an
// item to its full binding set.
bool SetOverride(std::string category, std::uint64_t model_id);

// Drops one category's override, or all of them when `category` is empty.
void ClearOverride(std::string_view category);

struct Override {
  std::string category;
  std::uint64_t model_id = 0;
};
std::vector<Override> Overrides();

// Rewrites `recipe` in place for every active override. Returns true when a
// byte actually changed, which is the caller's signal to write it back.
// False for an unparseable recipe, so a malformed buffer is left alone
// rather than half-patched.
bool ApplyOverrides(std::vector<std::uint8_t>& recipe);

// How the override is behaving against the live recipe. This answers one
// question: is the recipe buffer an INPUT retail builds the character from, or
// an OUTPUT it serialises its own state into?
//
//   patched far below examined  ->  the write sticks. The buffer is inert or
//                                   input-only, and what is missing is
//                                   whatever makes retail rebuild.
//   patched tracks examined     ->  retail overwrites it every frame from its
//                                   own item state. The buffer is a
//                                   serialisation, and no amount of patching
//                                   it will change the local skater - the
//                                   change has to go in via retail's own
//                                   recipe LOAD path.
//
// Worth measuring rather than reasoning about: both stories predict "peers see
// it, the local skater does not", so the visible symptom cannot tell them
// apart.
struct OverrideStats {
  std::uint64_t examined = 0;   // times the live recipe was checked
  std::uint64_t patched = 0;    // times bytes actually had to be rewritten
  std::uint64_t rejected = 0;   // times the verify threw an edit away
};
OverrideStats Stats();

}  // namespace skate3::clothing
