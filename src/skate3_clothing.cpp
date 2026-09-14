#include "skate3_clothing.h"

#include "skate3_cac_catalogue.h"
#include "skate3_multiplayer_assets.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace skate3::clothing {

namespace {

struct Category {
  std::vector<std::string> retail;  // sorted
  std::vector<std::string> custom;  // registration order
};

struct State {
  std::mutex mutex;
  std::map<std::string, Category> categories;  // ordered: alphabetical for free
  bool indexed = false;
  std::vector<std::uint8_t> recipe;
  // category -> model id. Ordered so listings are stable.
  std::map<std::string, std::uint64_t> overrides;
  OverrideStats stats;
};

State& Get() {
  static State state;
  return state;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Scans the extracted catalogue: one directory per category, one file per
// item. Called with the lock held.
//
// Returns false while the extraction is still running, WITHOUT marking the
// index built, so a script that asks during startup gets an empty answer now
// and a real one later rather than a cached empty one forever.
bool BuildIndexLocked(State& state) {
  if (state.indexed) {
    return true;
  }
  // Non-waiting: this can be called from a script tick, and blocking the
  // frame loop on an archive extraction would be worse than answering
  // "not yet".
  const std::filesystem::path root = cac_catalogue::Root(false);
  std::error_code error;
  if (root.empty() || !std::filesystem::is_directory(root, error)) {
    return false;
  }
  for (std::filesystem::directory_iterator it(root, error), end;
       it != end; it.increment(error)) {
    if (error) {
      break;
    }
    if (!it->is_directory(error)) {
      continue;
    }
    const std::string name = it->path().filename().string();
    Category& category = state.categories[name];
    category.retail.clear();
    std::error_code inner;
    for (std::filesystem::directory_iterator file(it->path(), inner), file_end;
         file != file_end; file.increment(inner)) {
      if (inner) {
        break;
      }
      if (!file->is_regular_file(inner)) {
        continue;
      }
      // Items are named by their asset id; the extension is the model
      // format and is not part of the identity.
      category.retail.push_back(file->path().stem().string());
    }
    std::sort(category.retail.begin(), category.retail.end());
  }
  // An archive with no category directories at all means the extraction has
  // not produced anything yet - do not latch that as the answer.
  if (state.categories.empty()) {
    return false;
  }
  state.indexed = true;
  return true;
}

}  // namespace

std::vector<CategoryInfo> Categories() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<CategoryInfo> out;
  if (!BuildIndexLocked(state)) {
    return out;
  }
  out.reserve(state.categories.size());
  for (const auto& [name, category] : state.categories) {
    CategoryInfo info;
    info.name = name;
    info.custom = category.custom.size();
    info.count = category.retail.size() + category.custom.size();
    out.push_back(std::move(info));
  }
  return out;
}

std::size_t Count(std::string_view category) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!BuildIndexLocked(state)) {
    return 0;
  }
  const auto entry = state.categories.find(std::string(category));
  if (entry == state.categories.end()) {
    return 0;
  }
  return entry->second.retail.size() + entry->second.custom.size();
}

std::vector<std::string> Ids(std::string_view category,
                             std::string_view filter, std::size_t limit) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<std::string> out;
  if (!BuildIndexLocked(state) || limit == 0) {
    return out;
  }
  const auto entry = state.categories.find(std::string(category));
  if (entry == state.categories.end()) {
    return out;
  }
  const std::string needle = Lowered(filter);
  const auto append = [&](const std::vector<std::string>& source) {
    for (const std::string& id : source) {
      if (out.size() >= limit) {
        return;
      }
      if (!needle.empty() && Lowered(id).find(needle) == std::string::npos) {
        continue;
      }
      out.push_back(id);
    }
  };
  append(entry->second.retail);
  append(entry->second.custom);
  return out;
}

std::string IdAt(std::string_view category, std::size_t index) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!BuildIndexLocked(state)) {
    return {};
  }
  const auto entry = state.categories.find(std::string(category));
  if (entry == state.categories.end()) {
    return {};
  }
  const Category& found = entry->second;
  if (index < found.retail.size()) {
    return found.retail[index];
  }
  index -= found.retail.size();
  if (index < found.custom.size()) {
    return found.custom[index];
  }
  return {};
}

bool Register(std::string category, std::string id) {
  if (category.empty() || id.empty()) {
    return false;
  }
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  // Deliberately does not require the index to be built: a custom item is
  // ours, and registering it must not depend on retail extraction having
  // finished. A brand new category is allowed - that is what a wholly
  // custom slot would be.
  BuildIndexLocked(state);
  Category& entry = state.categories[category];
  const auto known = [&id](const std::vector<std::string>& source) {
    return std::find(source.begin(), source.end(), id) != source.end();
  };
  if (known(entry.retail) || known(entry.custom)) {
    return false;
  }
  entry.custom.push_back(std::move(id));
  return true;
}

void Invalidate() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  // Customs survive: they were registered by a script, not discovered on
  // disk, so a rescan must not silently drop them.
  for (auto& [name, category] : state.categories) {
    category.retail.clear();
  }
  state.indexed = false;
}

bool SetOverride(std::string category, std::uint64_t model_id) {
  if (category.empty() || model_id == 0) {
    return false;
  }
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.overrides[std::move(category)] = model_id;
  return true;
}

void ClearOverride(std::string_view category) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (category.empty()) {
    state.overrides.clear();
    return;
  }
  state.overrides.erase(std::string(category));
}

std::vector<Override> Overrides() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<Override> out;
  out.reserve(state.overrides.size());
  for (const auto& [category, model_id] : state.overrides) {
    out.push_back(Override{category, model_id});
  }
  return out;
}

bool ApplyOverrides(std::vector<std::uint8_t>& recipe) {
  std::map<std::string, std::uint64_t> overrides;
  {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    overrides = state.overrides;
  }
  if (overrides.empty() || recipe.empty()) {
    return false;
  }
  {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.stats.examined;
  }
  std::vector<multiplayer_assets::RecipeIdLocation> locations;
  if (!multiplayer_assets::LocateRecipeIds(recipe, locations)) {
    // Not ours to repair. Patching a buffer the parser rejects would be
    // writing eight bytes at an offset derived from a failed walk.
    return false;
  }
  // Patch a COPY, then prove the copy before letting it out.
  //
  // These bytes are written into guest memory, where a wrong offset does not
  // fail - it quietly corrupts whatever it landed in. So the patched buffer is
  // re-parsed and every override checked against what the parser now reports.
  // A verify that cannot be satisfied throws the whole edit away rather than
  // committing part of it.
  std::vector<std::uint8_t> patched = recipe;
  bool changed = false;
  for (const multiplayer_assets::RecipeIdLocation& location : locations) {
    const auto wanted = overrides.find(location.category);
    if (wanted == overrides.end() || wanted->second == location.model_id) {
      continue;
    }
    if (location.model_id_offset + 8 > patched.size()) {
      continue;
    }
    // Big-endian, matching how the recipe stores every id (it is Xbox 360
    // data): the parser reads these with ReadBe64.
    const std::uint64_t value = wanted->second;
    for (int byte = 0; byte < 8; ++byte) {
      patched[location.model_id_offset + byte] =
          static_cast<std::uint8_t>((value >> (56 - byte * 8)) & 0xFF);
    }
    changed = true;
  }
  if (!changed) {
    return false;
  }

  const auto reject = [] {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.stats.rejected;
    return false;
  };
  std::vector<multiplayer_assets::RecipeIdLocation> after;
  if (!multiplayer_assets::LocateRecipeIds(patched, after)) {
    // The edit broke the structure: the offsets were not what they claimed.
    return reject();
  }
  for (const multiplayer_assets::RecipeIdLocation& location : after) {
    const auto wanted = overrides.find(location.category);
    if (wanted != overrides.end() && location.model_id != wanted->second) {
      return reject();  // an overridden category did not take
    }
  }
  // The trailing metadata has to still be there, or a shorter/longer parse
  // would mean the walk went somewhere else entirely.
  if (after.size() != locations.size()) {
    return reject();
  }
  {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.stats.patched;
  }
  recipe.swap(patched);
  return true;
}

OverrideStats Stats() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.stats;
}

void PublishLocalRecipe(const std::uint8_t* bytes, std::size_t size) {
  if (bytes == nullptr || size == 0) {
    return;
  }
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.recipe.assign(bytes, bytes + size);
}

bool LocalOutfit(std::vector<OutfitPiece>& out) {
  out.clear();
  std::vector<std::uint8_t> recipe;
  {
    State& state = Get();
    std::lock_guard<std::mutex> lock(state.mutex);
    recipe = state.recipe;
  }
  if (recipe.empty()) {
    return false;
  }
  std::vector<multiplayer_assets::RecipePieceInfo> pieces;
  if (!multiplayer_assets::DescribeRecipe(recipe, pieces)) {
    return false;
  }
  out.reserve(pieces.size());
  for (const multiplayer_assets::RecipePieceInfo& piece : pieces) {
    OutfitPiece copy;
    copy.category = piece.category;
    copy.asset_id = piece.asset_id;
    copy.model_id = piece.model_id;
    copy.material_id = piece.material_id;
    out.push_back(std::move(copy));
  }
  return true;
}

}  // namespace skate3::clothing
