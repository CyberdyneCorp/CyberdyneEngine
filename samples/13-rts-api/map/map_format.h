// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_MAP_FORMAT_H
#define CY_SAMPLE_RTS_MAP_FORMAT_H
// map_format.h — the RTS test map as it is cooked and loaded: its assets, their payloads, and the
// data a loaded map hands a host and a renderer.
//
// ONE PACKAGE, EIGHT KINDS OF ASSET. The cook (map_cook.h) writes a `.cypak`; the loader
// (map_load.h) mounts it and reads every asset back BY ID through `cy::assets::AssetSystem`. Each
// payload is wrapped in the engine's cooked-asset header (`cy/core/assets/cooked.h`) and then
// starts with its own magic and version, so the loader refuses a wrong kind, a newer version and a
// truncated payload with three different messages rather than misreading any of them.
//
//   layout     Binary   the map's numbers, its materials, and every placement: buildings,
//                       resources, props, the water. Depends on everything below.
//   terrain    Terrain  the heightfield the stack evaluated (u16 against the layout's range) and
//                       the bounded material texels the paint and the roads wrote
//   ground     Mesh     `terrain::mesh_tile()`'s triangles over every tile, joined
//   albedo     Texture  the painted layers baked into one sRGB image, with its mip chain
//   props      Mesh     the mesh library: every building, rock, tree and crate, in parts
//   worker     Mesh     the skinned worker: eight joints, one rigid influence a vertex
//   foliage    Binary   `foliage::generate_region()`'s clusters, sixteen-byte instances as the
//                       engine stores them, decoded on load through `ClusterBounds`
//   stone, gravel, copper   Texture   content/beauty's three albedo images, with mips
//
// Everything is little-endian and every count is bounded before it is trusted.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>

#include <string>
#include <vector>

namespace sample::rts::map {

using cy::f32;
using cy::f64;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::Vec2;
using cy::Vec3;

// --- Asset identities ---------------------------------------------------------------------------

/// The map's namespace, beside the worker rig's `0x13A0...`. Fixed ids: the cook is a pure function
/// of the source, so a minted id would buy nothing and would make two cooks differ.
inline constexpr u64 kMapNamespace = 0x13A1'0000'0000'0001ULL;
inline constexpr cy::AssetId kLayoutAsset{kMapNamespace, 1};
inline constexpr cy::AssetId kTerrainAsset{kMapNamespace, 2};
inline constexpr cy::AssetId kGroundAsset{kMapNamespace, 3};
inline constexpr cy::AssetId kAlbedoAsset{kMapNamespace, 4};
inline constexpr cy::AssetId kPropsAsset{kMapNamespace, 5};
inline constexpr cy::AssetId kWorkerAsset{kMapNamespace, 6};
inline constexpr cy::AssetId kFoliageAsset{kMapNamespace, 7};
/// Content textures take ids from here up, in the order the source declares them.
inline constexpr u64 kFirstContentTexture = 16;
inline constexpr u32 kMaxContentTextures = 8;

/// Each payload's own magic, after the cooked-asset header.
inline constexpr u32 kLayoutMagic = 0x5941'4C4DU;   // "MLAY"
inline constexpr u32 kTerrainMagic = 0x5245'544DU;  // "MTER"
inline constexpr u32 kMeshMagic = 0x4853'454DU;     // "MESH"
inline constexpr u32 kSkinMagic = 0x4E49'4B53U;     // "SKIN"
inline constexpr u32 kTextureMagic = 0x5845'544DU;  // "MTEX"
inline constexpr u32 kFoliageMagic = 0x4C4F'464DU;  // "MFOL"
/// One version for every payload: the map's format moves as one.
inline constexpr u32 kFormatVersion = 1;

/// Bounds on what a payload may declare, so a corrupt count is refused rather than allocated.
inline constexpr u32 kMaxMaterials = 64;
inline constexpr u32 kMaxMeshes = 128;
inline constexpr u32 kMaxParts = 1024;
inline constexpr u32 kMaxVertices = 1U << 22U;
inline constexpr u32 kMaxIndices = 1U << 24U;
inline constexpr u32 kMaxPlacements = 1U << 16U;
inline constexpr u32 kMaxTextureEdge = 4096;
inline constexpr u32 kMaxTerrainEdge = 2049;
inline constexpr u32 kMaxFoliage = 1U << 18U;
inline constexpr u32 kMaxSpecies = 16;

/// `MapMaterial::texture` when a material samples nothing.
inline constexpr u32 kNoTexture = 0xFFFF'FFFFU;
/// The albedo map's slot in `MapMaterial::texture`'s numbering: the content textures are 0..n-1.
inline constexpr u32 kAlbedoTexture = 0x100U;

// --- What a loaded map is ------------------------------------------------------------------------

enum class Blend : u8 { Opaque = 0, Transparent = 1 };

/// A material the map's surfaces are drawn with: the engine's standard material's parameters.
struct MapMaterial {
    std::string name;
    Vec3 base_colour{0.8F, 0.8F, 0.8F};
    f32 roughness = 0.8F;
    f32 metallic = 0.0F;
    /// Radiance, in the frame's physical units.
    Vec3 emission{0.0F, 0.0F, 0.0F};
    /// A content texture index, `kAlbedoTexture`, or `kNoTexture`.
    u32 texture = kNoTexture;
    Blend blend = Blend::Opaque;
};

/// A run of a mesh's indices drawn with one material.
struct MeshPart {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
};

/// One mesh of a library: its vertices and its parts, as ranges of the library's arrays.
struct MeshRange {
    std::string name;
    u32 first_vertex = 0;
    u32 vertex_count = 0;
    u32 first_part = 0;
    u32 part_count = 0;
    /// Model-space bounds.
    Vec3 min{0.0F, 0.0F, 0.0F};
    Vec3 max{0.0F, 0.0F, 0.0F};
};

/// Rigid meshes: one set of streams for all of them. Indices are mesh-relative.
struct MeshLibrary {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;
    std::vector<MeshPart> parts;
    std::vector<MeshRange> meshes;

    /// The mesh named `name`, or `kNoMesh`.
    [[nodiscard]] u32 find(const char* name) const noexcept;
};
inline constexpr u32 kNoMesh = 0xFFFF'FFFFU;

/// The skinned worker: the library's streams plus one rigid influence a vertex.
struct SkinnedMesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<u8> joints;
    std::vector<u32> indices;
    std::vector<MeshPart> parts;
    u32 joint_count = 0;
};

/// What a placement is, for the renderer, the GI and a test.
enum class PlacementKind : u8 {
    Building = 0,
    Resource = 1,
    Prop = 2,
    Water = 3,
};

/// One placed mesh, in world metres.
struct Placement {
    u32 mesh = 0;
    PlacementKind kind = PlacementKind::Prop;
    /// 0 for none, 1 blue, 2 red: which team's colour a part named "team" is drawn in.
    u8 team = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// Radians about +Y.
    f32 yaw = 0.0F;
    Vec3 scale{1.0F, 1.0F, 1.0F};
    Vec3 tint{1.0F, 1.0F, 1.0F};
    /// The source's name for it, for a host that looks a placement up.
    std::string tag;
};

/// The terrain as cooked: a stitched lattice over the map, heights against the layout's range.
struct TerrainData {
    /// Samples along an edge.
    u32 edge = 0;
    /// World position of sample (0, 0), and metres between samples.
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 spacing = 1.0F;
    f32 height_min = 0.0F;
    f32 height_max = 1.0F;
    std::vector<u16> heights;
    /// `(edge - 1)^2` texels, one a quad: up to four layers and their weights.
    std::vector<u8> texel_layers;
    std::vector<u8> texel_weights;

    /// The height at a world position, bilinear over the lattice; clamped to the edge outside it.
    [[nodiscard]] f32 height_at(f32 x, f32 z) const noexcept;
    /// The stored height of sample (i, j), in metres.
    [[nodiscard]] f32 sample(u32 i, u32 j) const noexcept;
    /// The dominant layer of the quad under a world position.
    [[nodiscard]] u8 layer_at(f32 x, f32 z) const noexcept;
};

/// A cooked texture: RGBA8, level 0 first, each level tightly packed.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    u32 mips = 0;
    bool srgb = true;
    std::vector<u8> pixels;
};

/// One foliage instance, decoded from its cluster's record.
struct FoliagePlant {
    /// Index into `FoliageData::species`.
    u32 species = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    f32 yaw = 0.0F;
    /// Tilt from vertical about x and z, radians.
    f32 tilt_x = 0.0F;
    f32 tilt_z = 0.0F;
    f32 scale = 1.0F;
    u8 variation = 0;
};

struct FoliageSpecies {
    std::string name;
    /// The library mesh it is drawn with.
    u32 mesh = kNoMesh;
    /// The declared scale range a stored instance's unit scale maps onto.
    f32 scale_min = 1.0F;
    f32 scale_max = 1.0F;
    /// Tints its `variation` byte picks between.
    Vec3 tint_a{1.0F, 1.0F, 1.0F};
    Vec3 tint_b{1.0F, 1.0F, 1.0F};
};

/// One generated cluster as the cook stores it: the engine's bounds and its sixteen-byte records,
/// with each record's species slot mapped to an index into the species list.
struct CookedCluster {
    cy::f64 min_x = 0.0;
    cy::f64 min_z = 0.0;
    cy::f64 max_x = 0.0;
    cy::f64 max_z = 0.0;
    f32 min_y = 0.0F;
    f32 max_y = 0.0F;
    /// `slot_species[record.species_slot]` is the species index.
    std::vector<u8> slot_species;
    /// `foliage::FoliageInstance`, verbatim: 16 bytes each.
    std::vector<u8> records;
};

struct FoliageData {
    std::vector<FoliageSpecies> species;
    std::vector<FoliagePlant> plants;
    /// What the cook's generation reported: clusters and the engine's per-instance bytes.
    u32 clusters = 0;
    u64 cluster_bytes = 0;
};

/// The map's numbers and placements.
struct MapLayout {
    std::string name;
    /// The square the terrain covers, in world metres.
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 size = 128.0F;
    u64 seed = 0;
    /// The lake's surface.
    f32 water_level = -1.0F;
    /// The direction the sun's light TRAVELS, and its illuminance in lux.
    Vec3 sun_travel{-0.28F, -0.82F, -0.50F};
    f32 sun_lux = 22000.0F;
    /// The play area the level's gameplay runs on, flat at height zero.
    f32 base_min_x = 0.0F;
    f32 base_min_z = 0.0F;
    f32 base_max_x = 32.0F;
    f32 base_max_z = 32.0F;
    /// Where the lockstep company's field is drawn: added to its field metres.
    f32 company_x = 0.0F;
    f32 company_z = 0.0F;
    std::vector<MapMaterial> materials;
    std::vector<Placement> placements;
    /// The content textures, in index order: their names.
    std::vector<std::string> textures;

    /// The material named `name`, or `kNoMaterial`.
    [[nodiscard]] u32 material(const char* wanted) const noexcept;
};
inline constexpr u32 kNoMaterial = 0xFFFF'FFFFU;

/// A whole loaded map.
struct MapContent {
    MapLayout layout;
    TerrainData terrain;
    MeshLibrary ground;
    TextureData albedo;
    MeshLibrary props;
    SkinnedMesh worker;
    FoliageData foliage;
    std::vector<TextureData> textures;
};

// --- Payloads ------------------------------------------------------------------------------------
//
// Each encoder appends its payload (no cooked-asset header) to `out`; each decoder reads one and
// refuses anything malformed, naming what.

void encode_layout(const MapLayout& layout, std::vector<u8>& out);
void encode_terrain(const TerrainData& terrain, std::vector<u8>& out);
void encode_meshes(const MeshLibrary& library, std::vector<u8>& out);
void encode_skinned(const SkinnedMesh& mesh, std::vector<u8>& out);
void encode_texture(const TextureData& texture, std::vector<u8>& out);
void encode_foliage(const std::vector<FoliageSpecies>& species,
                    const std::vector<CookedCluster>& clusters, std::vector<u8>& out);

[[nodiscard]] cy::Status decode_layout(cy::Span<const u8> bytes, MapLayout& out);
[[nodiscard]] cy::Status decode_terrain(cy::Span<const u8> bytes, TerrainData& out);
[[nodiscard]] cy::Status decode_meshes(cy::Span<const u8> bytes, MeshLibrary& out);
[[nodiscard]] cy::Status decode_skinned(cy::Span<const u8> bytes, SkinnedMesh& out);
[[nodiscard]] cy::Status decode_texture(cy::Span<const u8> bytes, TextureData& out);
[[nodiscard]] cy::Status decode_foliage(cy::Span<const u8> bytes, FoliageData& out);

/// The ground the map spans on the plane, in world metres: the cooked terrain's square and the
/// level's together; y is zero. What a host sizes the fog of war's grid over, so nothing the map
/// draws lies off it.
[[nodiscard]] cy::Aabb map_bounds(const MapContent& content) noexcept;

/// A 64-bit digest of everything a loaded map holds, in a fixed order: what two cooks of one
/// source, and a cook and its load, are compared by.
[[nodiscard]] u64 content_digest(const MapContent& content) noexcept;

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_MAP_FORMAT_H
