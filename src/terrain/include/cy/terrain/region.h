// SPDX-License-Identifier: MIT
#pragma once
// A bounded, square terrain region evaluated from a modifier stack for an author: what an editor
// previews while it sculpts, paints and cuts holes. Issue #29, "Terrain (finish)".
//
// The editor is a client. It records brush strokes as modifiers in its document and sends the
// stack; this is where they become terrain — the same `ModifierStack::evaluate()` a cook runs, the
// same `mesh_tile()` rendering takes and the same `build_collision()` physics takes — so the
// heights, weights and holes an author sees are the engine's, and a hole the preview shows open is
// one collision and rendering both left open.
//
// The region is `tiles` x `tiles` level-0 tiles over `extent` metres from the origin, stitched into
// one lattice: tiles share their boundary samples exactly (tile.h, decision 5), so the stitched
// grid has `tiles * kTileQuads + 1` samples along an edge and takes each shared sample from either
// tile.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/terrain/stack.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// The most tiles along a region edge. Sixteen 64-quad tiles is a 1025-sample lattice, two MiB of
/// heights, which bounds what one request can make the engine evaluate.
inline constexpr u32 kMaxRegionTiles = 16;

/// What an author's region is: its size and its tiling. The unedited surface is the stack's
/// generator.
struct RegionDescription {
    /// Distinguishes this preview's tile identities from a runtime terrain's
    /// (`TileLayout::terrain`).
    u32 terrain = 0;
    /// Tiles along each edge, 1 to `kMaxRegionTiles`.
    u32 tiles = 2;
    /// Metres along each edge. Sample spacing is `extent / (tiles * kTileQuads)`.
    f32 extent = 128.0F;
};

/// The layout a region's tiles use. Heights are quantised over [-512, 1536] metres.
[[nodiscard]] TileLayout region_layout(const RegionDescription& region) noexcept;

/// Refuses a region with no tiles, too many, or a non-finite or non-positive extent.
[[nodiscard]] Status validate_region(const RegionDescription& region) noexcept;

/// A region evaluated, stitched and measured.
struct RegionSnapshot {
    /// Samples along an edge: `tiles * kTileQuads + 1`.
    u32 edge = 0;
    /// `edge * edge` stored heights, row-major in z then x, quantised against `region_layout()`.
    Array<u16> heights;
    /// `(edge - 1)^2` material texels, one per quad, row-major in z then x.
    Array<MaterialTexel> texels;
    /// `(edge - 1)^2` flags, one per quad: one where the surface is cut away.
    Array<u8> holes;
    /// Triangles `mesh_tile()` emitted for rendering over every tile, and the quads it left open.
    u32 rendered_triangles = 0;
    u32 rendered_hole_quads = 0;
    /// Collision samples `build_collision()` marked `physics::kHeightFieldHole`, over every tile.
    u32 collision_holes = 0;

    explicit RegionSnapshot(Allocator& allocator) noexcept
        : heights(allocator), texels(allocator), holes(allocator) {}
};

/// Evaluate every tile of the region from the stack, then mesh and collide each one. The stack's
/// layout must be `region_layout(region)`.
[[nodiscard]] Expected<RegionSnapshot, Error> evaluate_region(
    Allocator& allocator, const ModifierStack& stack, const RegionDescription& region) noexcept;

/// The stack's surface height at a position, from the lattice sample nearest to it. What a flatten
/// brush levels toward: the ground under its first dab as the modifiers beneath it leave it.
[[nodiscard]] Expected<f32, Error> region_height_at(Allocator& allocator,
                                                    const ModifierStack& stack,
                                                    const RegionDescription& region, f64 x,
                                                    f64 z) noexcept;

}  // namespace cy::terrain
