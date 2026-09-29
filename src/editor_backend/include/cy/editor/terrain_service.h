// SPDX-License-Identifier: MIT
#pragma once
// The engine side of the editor's terrain tools: `terrain.evaluate`. Issue #29, "Terrain (finish)".
//
// The editor records each sculpt, paint and hole stroke as a modifier in its document and sends the
// whole ordered stack after every change — a stroke, an undo, a redo, an enable, a reorder. This
// evaluates it with `cy::terrain` (the modifier stack, meshing and collision), answers with the
// heights, texels and holes the engine computed, and keeps the region for the viewport host to
// draw. Undo therefore needs no inverse here: the editor's history restores the stack, and the same
// stack evaluates to the same bytes.
//
// It also owns the navigation stale flag. Every modifier that appears, disappears or changes marks
// its reach dirty on a `terrain::TerrainNavigation`; the regions accumulate until navigation is
// rebaked, which is issue #28's and not this service's.
//
// REQUEST (schema 1), little-endian:
//   u32 format = 1, u128 terrain, u32 tiles, f32 extent, f32 base_height, u32 modifier_count, then
//   per modifier: u128 identity, u8 op (raise, lower, smooth, flatten, paint, hole), u8 enabled,
//   u8 layer (0 is the base; the editor's first layer is 1), f32 radius (metres), f32 strength,
//   f32 falloff, u32 dab_count, and per dab f32 x, f32 z (normalised over the extent), f32
//   pressure.
// REPLY:
//   u32 format = 1, u64 generation, u32 edge, f32 extent, f32 height_min, f32 height_max,
//   u32 rendered_triangles, u32 rendered_hole_quads, u32 collision_holes, u32 stale_count,
//   stale_count x (f32 min_x, f32 min_z, f32 max_x, f32 max_z) in metres, edge*edge u16 heights,
//   (edge-1)^2 x (u8 layer[4], u8 weight[4]) texels, (edge-1)^2 u8 holes; rows run along x.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/terrain/collision.h>
#include <cy/terrain/region.h>

namespace cy::editor {

/// The request and reply format `terrain.evaluate` speaks.
inline constexpr u32 kTerrainEvaluateFormat = 1;
/// The most modifiers and dabs one request may carry.
inline constexpr u32 kMaxTerrainModifiers = 4096;
inline constexpr u32 kMaxTerrainDabs = 1U << 20U;

/// One session's terrain preview: the last evaluated region and the navigation it made stale.
class TerrainPreview {
public:
    explicit TerrainPreview(Allocator& allocator) noexcept;

    TerrainPreview(const TerrainPreview&) = delete;
    TerrainPreview& operator=(const TerrainPreview&) = delete;

    /// Decode, evaluate and encode one `terrain.evaluate`. On a refusal nothing changes and the
    /// error names the problem.
    [[nodiscard]] Status evaluate(Span<const u8> request, Array<u8>& reply) noexcept;

    /// The last evaluated region, or null before the first. For the viewport host.
    [[nodiscard]] const terrain::RegionSnapshot* snapshot() const noexcept {
        return has_snapshot_ ? &snapshot_ : nullptr;
    }
    [[nodiscard]] const terrain::RegionDescription& region() const noexcept { return region_; }
    /// Increments on every successful evaluation, so a host knows when to rebuild what it draws.
    [[nodiscard]] u64 generation() const noexcept { return generation_; }
    /// Regions whose navigation is stale, in metres over the region.
    [[nodiscard]] Span<const terrain::TerrainBounds> stale_navigation() const noexcept {
        return navigation_.dirty();
    }

private:
    struct Seen {
        u64 identity_low = 0;
        u64 identity_high = 0;
        u64 digest = 0;
        terrain::TerrainBounds reach;
    };

    [[nodiscard]] Status mark_changes(Span<const Seen> current) noexcept;

    Allocator* allocator_;
    terrain::RegionDescription region_;
    terrain::RegionSnapshot snapshot_;
    terrain::TerrainNavigation navigation_;
    Array<Seen> seen_;
    u64 terrain_low_ = 0;
    u64 terrain_high_ = 0;
    u64 generation_ = 0;
    bool has_snapshot_ = false;
};

}  // namespace cy::editor
