// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_RENDER_CONTENT_H
#define CY_SAMPLE_FIRST_PERSON_RENDER_CONTENT_H
// render_content.h — the arena as the renderer draws it: the geometry streams and the material
// textures. The frame that draws them is render.h's.
//
// GEOMETRY. One stream of vertices: the unit cube first, which every box that MOVES is a transform
// of — a prop, a target, a part of the weapon — and after it the arena's static boxes BAKED in
// world space, each with texture coordinates in metres along its faces, so a 28 m floor shows its
// tiles a metre apart instead of one tile stretched over it. Every box is twenty-four vertices in
// the same order, so one run of thirty-six indices draws any of them from its vertex offset.
//
// TEXTURES. content/beauty/textures' three albedo images — stone tiles for the floor, gravel for
// the plaster and the paint, weathered copper for the barrel — decoded by the importer, averaged
// into a box-filtered mip chain in linear light, and bound through the frame's material texture
// table. A surface's base colour is its texture times a factor that brings the texture's mean to
// the surface's albedo (arena.h): the GI, which shades flat cards, and the frame then agree about
// what colour a wall is on average. Without the importer (`-D CY_BUILD_TOOLS=OFF`) there are no
// textures and every surface is its flat albedo.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

#include <vector>

#include "arena.h"

namespace sample::fps {

struct ArenaStreams {
    std::vector<cy::Vec3> positions;
    std::vector<cy::u16> normals;
    std::vector<cy::Vec2> uvs;
    /// Thirty-six: one box's, relative to its first vertex.
    std::vector<cy::u32> indices;
};

/// One static box baked into the streams.
struct BakedBox {
    cy::i32 vertex_offset = 0;
    cy::Aabb bounds = cy::Aabb::empty();
    Surface surface = Surface::Concrete;
};

/// The unit cube at vertex 0, then every box of `boxes` baked in world space.
void build_streams(cy::Span<const ArenaBox> boxes, ArenaStreams& out, std::vector<BakedBox>& baked);

/// The textures a surface may be drawn with.
enum class Detail : cy::u8 { None = 0, Stone, Gravel, Copper, Count };
[[nodiscard]] Detail detail_of(Surface surface) noexcept;

/// One decoded texture: RGBA8 sRGB, every mip level from the largest down, and its mean linear
/// colour.
struct DetailTexture {
    cy::u32 width = 0;
    cy::u32 height = 0;
    cy::u32 mips = 0;
    std::vector<cy::u8> pixels;
    cy::Vec3 mean{1.0F, 1.0F, 1.0F};
};

/// Read and decode `detail`'s image under `content_root`. `Unsupported` in a build without the
/// importer, which draws flat colours instead.
[[nodiscard]] cy::Expected<DetailTexture, cy::Error> load_detail(Detail detail,
                                                                 const char* content_root) noexcept;

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_RENDER_CONTENT_H
