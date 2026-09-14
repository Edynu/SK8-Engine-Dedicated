#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::multiplayer_assets {

// Starts receiver-side discovery of the retail Create-a-Skater archive and
// prepares a small persistent model cache on a background thread. Retail
// files remain local; textures are extracted lazily only when a received
// compact recipe references them.
void StartLocalCatalogue(
    const std::filesystem::path& game_data_root,
    const std::filesystem::path& cache_root);

void ShutdownLocalCatalogue();

// Renderer-ready vanilla bind mesh resolved from the locally installed
// Create-a-Skater catalogue. No retail presentation entity is involved: the
// custom renderer consumes this geometry and the normal replicated canonical
// skeleton.
struct BindMesh {
  std::uint64_t asset_id = 0;
  std::filesystem::path source_path;
  std::vector<float> vertices;
  std::vector<std::uint16_t> indices;
  std::vector<std::uint16_t> palette_to_canonical;
  float bbox_min[3] = {};
  float bbox_max[3] = {};
};

enum class TextureFormat : std::uint8_t {
  kBc1,
  kBc3,
};

// One locally installed texture referenced by a Create-a-Skater recipe.
// Payload bytes are linear host-order BC blocks ready for a native-RHI
// upload; retail texture data never crosses the multiplayer transport.
struct RecipeTexture {
  std::uint64_t texture_id = 0;
  TextureFormat format = TextureFormat::kBc1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::vector<std::uint8_t> bytes;
};

struct RecipePiece {
  std::string category;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t material_id = 0;
  BindMesh mesh;
  std::uint64_t diffuse_texture = 0;
  std::uint64_t normal_texture = 0;
  std::uint64_t alpha_texture = 0;
  std::uint64_t specular_texture = 0;
};

struct RecipeAppearance {
  std::uint8_t gender = 0;
  std::size_t structural_bytes = 0;
  std::vector<RecipePiece> pieces;
  std::unordered_map<std::uint64_t, RecipeTexture> textures;
};

struct RecipeResolveKey {
  std::uint32_t role = 0;
  std::uint32_t session = 0;
  std::uint64_t appearance_id = 0;

  bool operator==(const RecipeResolveKey&) const = default;
};

enum class RecipeResolveStatus : std::uint8_t {
  kUnknown,
  kPending,
  kReady,
  kFailed,
};

// One piece of a parsed `cas_db` recipe: a category and the asset ids that
// make it up. This is the recipe as DESCRIBED, with no asset resolution -
// used by the wardrobe reader (skate3_clothing.h) to report what a skater is
// wearing without touching the renderer's caches.
// Named apart from RecipePiece above deliberately: that one is a RESOLVED
// piece, carrying the mesh and textures the renderer bound. This one is only
// what the recipe SAYS, so a reader cannot mistake unresolved zeros for a
// piece that failed to load.
struct RecipePieceInfo {
  std::string category;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t material_id = 0;
};

// Parses a recipe into its pieces. Returns false when the bytes are not a
// valid recipe - the same strict header/structure check the appearance path
// applies, so a caller can never see a half-read outfit.
bool DescribeRecipe(
    const std::vector<std::uint8_t>& recipe,
    std::vector<RecipePieceInfo>& pieces);

// Where a piece's ids SIT in the recipe bytes, so they can be rewritten in
// place. Every model record is reported, including the lower LODs that
// DescribeRecipe drops: a swapped item has to be swapped at every LOD, or the
// skater changes clothes as the camera pulls back.
//
// Offsets are into the recipe passed in, and each id is 8 bytes big-endian.
// Reported by the same parser that validates the format, rather than by a
// second implementation that could drift from it - a wrong offset here writes
// eight bytes into the middle of somebody's outfit.
struct RecipeIdLocation {
  std::string category;
  std::uint8_t lod = 0;
  std::uint64_t asset_id = 0;
  std::uint64_t model_id = 0;
  std::size_t asset_id_offset = 0;
  std::size_t model_id_offset = 0;
  std::size_t material_id_offset = 0;
};

bool LocateRecipeIds(
    const std::vector<std::uint8_t>& recipe,
    std::vector<RecipeIdLocation>& locations);

// Strictly validates a live `cas_db` recipe and resolves its high-detail
// model/texture IDs against the receiver's own extracted retail catalogue.
// The recipe is network-safe metadata only; this routine never accepts paths
// from a peer.
bool ResolveRecipeAppearance(
    const std::vector<std::uint8_t>& recipe,
    bool load_textures,
    RecipeAppearance& output);

// Queues heavy receiver-side recipe model/texture resolution on one
// background asset worker. Requests are latest-wins per role and results are
// published only when the complete role/session/appearance key still
// matches, preventing a slow old wardrobe decode from replacing a newer one.
void QueueRecipeAppearanceResolve(
    const RecipeResolveKey& key,
    std::vector<std::uint8_t> recipe);

RecipeResolveStatus PollRecipeAppearanceResolve(
    const RecipeResolveKey& key,
    std::shared_ptr<const RecipeAppearance>& output);

// Removes pending/prepared state only when the supplied peer generation
// matches. An in-flight stale result is discarded when it completes.
void ForgetRecipeAppearanceResolve(
    std::uint32_t role, std::uint32_t session);

// Polls the configured local SKATER.P profile and replaces `recipe` only
// when its validated cas_db payload changes. The save container itself is
// never exposed to the network layer. Calls are internally rate-limited so
// the render-thread capture path does not perform filesystem work per frame.
bool PollLocalProfileRecipe(
    std::vector<std::uint8_t>& recipe);

// Resolves a live ROPA mesh by immutable topology. ROPA rewrites positions
// every simulation frame, but its vertex/index layout remains the layout of
// the vanilla RX2 bind mesh. Character family 2 maps to OuterTorso and family
// 4/5 maps to Hair.
bool ResolveRopaBindMesh(
    std::uint8_t character_family,
    std::uint32_t vertex_count,
    std::uint32_t index_count,
    std::uint64_t topology_hash,
    BindMesh& output);

}  // namespace skate3::multiplayer_assets
