// SPDX-License-Identifier: MIT
#pragma once
// A triangle mesh as the GI scene holds it: surface cards over its faces, and a signed distance
// grid in its own space for the field to composite. Issue #35, stage 4.
//
// ================================================================================================
// WHY THIS EXISTS
// ================================================================================================
//
// Every GI suite and sample before stage 4 built its scene by hand — a room's cards on a lattice,
// a box's field in closed form (`box_distance`). A frame draws meshes, and the per-pixel resolve's
// first real scene (the beauty shot) is a dozen of them: boxes, cylinders, a sphere, a ground
// plane. So the two things the GI scene needs of any mesh are built here, from its triangles, the
// way a cook would: cards at a stated spacing (`append_mesh_cards`), and a dense signed distance
// grid
// (`MeshDistanceField`) — the nearest triangle for the distance, the parity of a ray's crossings
// for the sign, so a closed mesh is negative inside and an open one (a ground plane) negative
// below it.
//
// CARDS BY CELL, NOT BY TRIANGLE. `lightmap_bake` cuts every triangle n by n, n from its longest
// edge, and that is right for a lightmap's even tessellation and wrong for a primitive's: a
// cylinder's side is a fan of slivers four metres long and eight centimetres wide, and a card per
// sub-triangle of each is thousands of cards a column. Here each triangle is cut finely and the
// pieces are pooled per world cell of the spacing and per facing (the dominant axis of the normal,
// signed), one card a pool: the card density is the surface's, whatever the triangles were.
//
// Brute force, by design: every grid sample against every triangle. A cook runs it once per asset
// and the field composites the result by transform; it is not on any frame's path.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

struct MeshCardSettings {
    /// The edge of the world cell a card stands for.
    f32 spacing_metres = 0.5F;
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    Vec3 emission{0.0F, 0.0F, 0.0F};
    f32 roughness = 0.8F;
    u32 instance_id = 0;
    u32 material_id = 0;
    /// Cards on both faces: a mesh that is seen from either side — a wall of one quad. A closed
    /// mesh is seen from outside only, and its inner cards would be light nothing reaches.
    bool two_sided = false;
    /// Cards only where the surface is inside `region`: a ground plane a hundred metres across
    /// whose GI matters where the camera looks.
    bool clip_to_region = false;
    Aabb region{};
};

/// Cards over the triangles of `positions` / `indices`, transformed by `transform` (model to
/// world): one a world cell of `spacing_metres` and facing, at the area-weighted mean of the
/// surface in it, facing its mean winding normal, its area the surface's. Degenerate triangles are
/// skipped.
[[nodiscard]] Status append_mesh_cards(Span<const Vec3> positions, Span<const u32> indices,
                                       const Mat4& transform, const MeshCardSettings& settings,
                                       Array<Surfel>& out) noexcept;

struct MeshFieldSettings {
    /// Samples along each axis of the grid.
    u32 dimension = 32;
    /// How far beyond the mesh's bounds the grid reaches, in metres: a field that ends at the
    /// surface cannot tell a ray how far it is from it.
    f32 margin_metres = 0.5F;
};

/// A mesh's signed distance grid in its own space: `AssetDistanceField`'s cooked form.
class MeshDistanceField {
public:
    MeshDistanceField() noexcept = default;

    [[nodiscard]] Status build(Span<const Vec3> positions, Span<const u32> indices,
                               const MeshFieldSettings& settings) noexcept;
    /// The grid as the field places it. Valid while this object lives.
    [[nodiscard]] AssetDistanceField asset() const noexcept;
    [[nodiscard]] const Aabb& bounds() const noexcept { return bounds_; }

private:
    u32 dimension_ = 0;
    Aabb bounds_{};
    Array<f32> distances_;
};

}  // namespace cy::rendering::gi
