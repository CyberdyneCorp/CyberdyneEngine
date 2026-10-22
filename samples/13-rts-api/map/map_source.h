// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_MAP_SOURCE_H
#define CY_SAMPLE_RTS_MAP_SOURCE_H
// map_source.h — the map's authoring document, `.cymap`, parsed. content/rts/README.md is the
// statement reference; this is its reader.
//
// The parser is strict: an unknown statement, a missing or malformed number, a layer or a species
// named before it is declared, all fail with the line number. A cook that guessed would produce a
// map nobody wrote.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/terrain/region.h>
#include <cy/terrain/stack.h>

#include <string>
#include <string_view>
#include <vector>

#include "map_format.h"

namespace sample::rts::map {

/// A terrain layer's colour and how the bake gives it detail.
struct SourceLayer {
    u8 index = 0;
    std::string name;
    Vec3 colour{0.5F, 0.5F, 0.5F};
    /// A content texture's name, "noise", or "rows".
    std::string detail = "noise";
    f32 metres = 2.0F;
    f32 contrast = 0.3F;
};

/// One step of the terrain stack, in world metres. Converted to the region's own metres, and to a
/// `terrain::Modifier`, by the cook.
struct SourceModifier {
    cy::terrain::ModifierKind kind = cy::terrain::ModifierKind::Noise;
    cy::terrain::BrushOp brush = cy::terrain::BrushOp::Raise;
    /// Noise: period, amplitude, octaves. Mound: centre x, z, radius, height. Flatten: the box and
    /// height. Road: width and layer. Brush: radius, strength, falloff, and the op's argument.
    f32 numbers[6] = {};
    u32 count = 0;
    u8 layer = 0;
    std::vector<Vec2> points;
};

struct SourceSpecies {
    std::string name;
    std::string mesh;
    /// canopy, understory or scatter.
    std::string klass;
    f32 scale_min = 1.0F;
    f32 scale_max = 1.0F;
    f32 footprint = 1.0F;
    Vec3 tint_a{1.0F, 1.0F, 1.0F};
    Vec3 tint_b{1.0F, 1.0F, 1.0F};
};

/// A rule test as the source spells it: an input and its trapezoid.
struct SourceTest {
    std::string input;
    f32 corners[4] = {};
};

struct SourceRule {
    std::string species;
    f32 density = 100.0F;
    f32 spacing = 2.0F;
    u32 priority = 0;
    std::vector<SourceTest> tests;
    f32 noise_metres = 120.0F;
};

struct SourceExclusion {
    f32 min_x = 0.0F;
    f32 min_z = 0.0F;
    f32 max_x = 0.0F;
    f32 max_z = 0.0F;
    f32 feather = 0.0F;
};

struct SourcePlacement {
    std::string mesh;
    PlacementKind kind = PlacementKind::Prop;
    f32 x = 0.0F;
    f32 z = 0.0F;
    f32 yaw_degrees = 0.0F;
    f32 scale = 1.0F;
    u8 team = 0;
    std::string tag;
};

struct SourceTexture {
    std::string name;
    /// Relative to the content root the cook is given.
    std::string path;
};

/// A parsed `.cymap`.
struct MapSource {
    std::string name;
    u64 seed = 0;
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 size = 128.0F;
    u32 tiles = 2;
    cy::terrain::TerrainGenerator generator;
    f32 water_level = -1.0F;
    Vec3 sun_travel{-0.28F, -0.82F, -0.50F};
    f32 sun_lux = 22000.0F;
    f32 base[4] = {0.0F, 0.0F, 32.0F, 32.0F};
    f32 company[2] = {0.0F, 0.0F};
    std::vector<SourceTexture> textures;
    std::vector<SourceLayer> layers;
    /// Steep ground: the layer, and the slope in degrees where it starts and where it is whole.
    u8 slope_layer = 0;
    f32 slope_from = 90.0F;
    f32 slope_full = 90.0F;
    /// The shore: the layer, and how far above the water it reaches.
    u8 shore_layer = 0;
    f32 shore_metres = 0.0F;
    std::vector<SourceModifier> stack;
    std::vector<MapMaterial> materials;
    std::vector<SourceSpecies> species;
    std::vector<SourceRule> rules;
    std::vector<SourceExclusion> exclusions;
    std::vector<SourcePlacement> placements;
};

/// Parse a document. On failure `line` is the line that failed (one-based).
[[nodiscard]] cy::Status parse_map_source(std::string_view text, MapSource& out,
                                          u32& line) noexcept;

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_MAP_SOURCE_H
