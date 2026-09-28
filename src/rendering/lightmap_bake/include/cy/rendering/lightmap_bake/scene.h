// SPDX-License-Identifier: MIT
#pragma once
// What a lightmap bake is given: static meshes with their UV2, their materials, their placements,
// the lights and the sky — and the triangle tracer the path tracer traces them through.
//
// `rendering-global-illumination` — "Lightmap baking": "gather static meshes and their UV2 charts,
// build an acceleration structure, path-trace direct and indirect lighting ... emissive surfaces as
// light sources, transparent and alpha-tested occlusion".
//
// ================================================================================================
// THE TRACER IS A `gi::SceneTracer`, SO THE BAKE IS `gi::PathTracer`'S
// ================================================================================================
//
// The lightmap bake does not have a path tracer of its own. `MeshSceneTracer` is the acceleration
// structure — a static `cy::Bvh` over every placed triangle — behind the same two seams every GI
// consumer holds, `SceneTracer` and `Occluder`, and `gi::PathTracer` traces the bake through it. A
// traced hit resolves its material through the GI scene's surface cards, exactly as the real-time
// path's hits do; `bake.h` builds those cards from the same triangles.
//
// ================================================================================================
// ALPHA-TESTED AND TRANSPARENT OCCLUSION ARE DECIDED AT THE HIT
// ================================================================================================
//
// A triangle whose material has an alpha mask is tested at the hit's UV0: below the cutoff the ray
// passes through, as an alpha-tested leaf lets light through its holes. A material with an opacity
// below one is TRANSPARENT: the ray stops with that probability, decided by a hash of the triangle
// and the ray so the bake stays reproducible. Averaged over a texel's samples that is the
// transmission the surface declares, and it applies to the shadow rays of the direct term too,
// because the direct term's `Occluder` is this same object.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>

namespace cy::rendering::lightmap_bake {

/// A mesh, as the importer cooked it. The spans must outlive the bake.
struct BakeMesh {
    Span<const Vec3> positions;
    /// Per vertex. Empty means the face normal.
    Span<const Vec3> normals;
    /// UV0, for an alpha mask. Empty when no material of this mesh has one.
    Span<const Vec2> uv0;
    /// The lightmap coordinate set, in its own unit square: `MeshData::uv2`.
    Span<const Vec2> uv2;
    Span<const u32> indices;
    /// The share of the UV2 square the charts cover (`Uv2Report::utilisation`), and the square's
    /// texel aspect (`Uv2Report::width / height`). What the atlas sizes the object's rectangle by.
    f32 uv_coverage = 1.0F;
    f32 uv_aspect = 1.0F;
};

/// A cooked mesh's UV2 footprint, for a mesh whose unwrap report is not at hand: the share of the
/// unit square its triangles cover, and the texel aspect (width over height) that makes the mapping
/// isotropic — `sqrt(sum dv^2 / sum du^2)` over every edge, which is one exactly when edges are as
/// long in u as in v on average.
void measure_uv2(Span<const Vec2> uv2, Span<const u32> indices, f32& coverage,
                 f32& aspect) noexcept;

/// A coverage mask sampled at UV0, nearest texel, repeating.
struct AlphaMask {
    u32 width = 0;
    u32 height = 0;
    Span<const f32> alpha;

    [[nodiscard]] f32 sample(Vec2 uv) const noexcept;
};

struct BakeMaterial {
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    /// Radiance the front face emits. An emissive surface is a light source in the bake through
    /// this term and through no placed light.
    Vec3 emission{0.0F, 0.0F, 0.0F};
    /// Below one: transparent, stopping a ray with this probability.
    f32 opacity = 1.0F;
    /// Non-null: alpha-tested at UV0 against `alpha_cutoff`.
    const AlphaMask* mask = nullptr;
    f32 alpha_cutoff = 0.5F;
};

struct BakeInstance {
    u32 mesh = 0;
    u32 material = 0;
    /// Model to world. Rigid plus scale.
    Mat4 transform = Mat4::identity();
    /// The object's lightmap resolution over the level's density.
    f32 resolution_scale = 1.0F;
    /// False: the object occludes and bounces light but owns no lightmap rectangle.
    bool receives_lightmap = true;
    /// The identity a caller maps back to its own object.
    u64 id = 0;
};

/// The level a bake is run over.
struct LightmapScene {
    Span<const BakeMesh> meshes;
    Span<const BakeMaterial> materials;
    Span<const BakeInstance> instances;
    Span<const gi::GiLight> lights;
    gi::SkyTerm sky{};
};

/// One placed triangle, in world space.
struct WorldTriangle {
    Vec3 v0{};
    Vec3 v1{};
    Vec3 v2{};
    /// Unit, from the winding: the front face.
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec2 uv0[3] = {};
    u32 instance = 0;
    u32 material = 0;
};

/// Every placed triangle behind a static BVH, as a `SceneTracer` and an `Occluder`.
class MeshSceneTracer final : public gi::SceneTracer, public gi::Occluder {
public:
    MeshSceneTracer() noexcept = default;
    ~MeshSceneTracer() override = default;

    MeshSceneTracer(const MeshSceneTracer&) = delete;
    MeshSceneTracer(MeshSceneTracer&&) = delete;
    MeshSceneTracer& operator=(const MeshSceneTracer&) = delete;
    MeshSceneTracer& operator=(MeshSceneTracer&&) = delete;

    /// Transform every instance's triangles into the world and build the tree. The scene's
    /// materials must outlive the tracer.
    [[nodiscard]] Status build(const LightmapScene& scene) noexcept;

    /// The nearest surface the ray stops at. The reported normal faces the ray, because a lightmap
    /// scene's surfaces are two-sided to light arriving at either face.
    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             gi::SceneHit& hit) const noexcept override;
    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;

    /// The same query, also naming the triangle and whether the ray met its back face — what the
    /// bake uses to find a texel buried inside geometry.
    [[nodiscard]] bool trace_triangle(Vec3 origin, Vec3 direction, f32 max_distance, f32& t,
                                      u32& triangle, bool& back_face) const noexcept;

    [[nodiscard]] Span<const WorldTriangle> triangles() const noexcept { return triangles_.span(); }
    [[nodiscard]] u64 rays() const noexcept { return rays_; }

private:
    /// Whether a ray stops at this triangle's hit: the alpha test, then the transparency draw.
    [[nodiscard]] bool stops(u32 triangle, f32 u, f32 v, Vec3 origin,
                             Vec3 direction) const noexcept;

    Array<WorldTriangle> triangles_;
    Bvh<u32> tree_;
    Span<const BakeMaterial> materials_;
    mutable u64 rays_ = 0;
};

}  // namespace cy::rendering::lightmap_bake
