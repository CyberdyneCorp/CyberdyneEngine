#pragma once
// Geometry adapters: what each geometry source hands the acceleration structure. Task 9.4.
//
// `ray-tracing-infrastructure` — "Geometry adapters": every geometry source SHALL supply an adapter
// producing the representation the acceleration structure requires, and the table of six sources in
// that requirement is reproduced here as `GeometrySource` and `adapter_for()`.
//
// ================================================================================================
// AN ADAPTER IS A POLICY, NOT A CONVERTER
// ================================================================================================
//
// The interesting content of an adapter is not "turn a mesh into triangles" — every source is
// triangles by the time it reaches a structure. It is two declarations that the service cannot
// derive and the consumer must not guess:
//
//   * how the structure is MAINTAINED when the geometry moves (rebuild, refit, or instance-only),
//     which is what decides whether an animated character costs a build every frame;
//   * what the representation is NOT, which is the virtual-geometry row. A streaming cluster
//     hierarchy cannot feed a structure, so a proxy at a declared detail level is used and the
//     difference from the rasterised surface is a NUMBER this header carries rather than a caveat
//     in a document. `ProxyPolicy::error_metres` is that number and it reaches the diagnostics.
//
// ================================================================================================
// WHY THE PROXY ERROR IS DECLARED AND NOT MEASURED
// ================================================================================================
//
// The cooker chooses a proxy level; the runtime cannot re-derive what it cost without the source
// mesh it deliberately does not have. So the level and its error travel together, and a consumer
// that cares — the GI resolve, when it decides how much to trust a hardware hit near a silhouette —
// reads `BuildInput::declared_error_metres` off the hit's geometry rather than assuming zero.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::rt {

/// The six rows of `ray-tracing-infrastructure`'s adapter table.
enum class GeometrySource : u8 {
    StaticMesh = 0,
    SkinnedMesh,
    VirtualGeometry,
    Terrain,
    MeshParticles,
    Procedural,
    Count,
};

inline constexpr u32 kGeometrySourceCount = static_cast<u32>(GeometrySource::Count);

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* geometry_source_name(GeometrySource source) noexcept;

/// How a structure is kept in step with geometry that moves.
enum class MaintenancePolicy : u8 {
    /// The geometry never changes in its own space. A moving instance is a top-level transform and
    /// costs nothing below it.
    Static = 0,
    /// Vertex positions change every frame but the topology does not: refit, and rebuild only when
    /// refit quality has degraded past `Adapter::refit_quality_floor`.
    Refit,
    /// Topology itself changes. Nothing can be refit; the structure is rebuilt or dropped.
    Rebuild,
    /// The instance references another geometry's structure and owns none of its own.
    InstanceOnly,
};

/// A triangle list in the geometry's own space, plus the material each triangle resolves to.
///
/// Non-owning: the caller keeps the arrays alive until `AccelerationService::declare_geometry`
/// returns, which copies what it needs.
///
/// `triangle_materials` is EMPTY for the ordinary single-material mesh, and a hit then resolves to
/// the INSTANCE's material. That is the whole rule and there is only one: a geometry does not own a
/// material — the GPU scene instance does, and it is the instance's identifier the material table
/// is indexed by. A geometry-level default beside the instance's would be a second identity to keep
/// in step, and the one place it would disagree is a mesh shared between two instances with
/// different materials, which is the ordinary case rather than an exotic one.
struct TriangleGeometry {
    Span<const Vec3> positions;
    Span<const u32> indices;
    Span<const u32> triangle_materials;

    [[nodiscard]] usize triangle_count() const noexcept { return indices.size() / 3; }
};

/// Virtual geometry's proxy policy: the cooker and runtime decision, with its trade-off stated.
///
/// `ray-tracing-infrastructure`: "The proxy detail level for virtual geometry SHALL be a cooker and
/// runtime policy with a stated memory and accuracy trade-off, because ray traced results will not
/// match the rasterised surface exactly."
struct ProxyPolicy {
    /// The level of the cluster hierarchy the proxy is taken from. 0 is the finest.
    u32 detail_level = 2;
    /// The world-space error the proxy declares against the rasterised surface, in metres.
    f32 error_metres = 0.05F;
    /// The proxy's triangle count as a fraction of the source's. The memory half of the trade-off.
    f32 triangle_fraction = 0.12F;
};

/// What an adapter produced for one geometry.
struct BuildInput {
    GeometrySource source = GeometrySource::StaticMesh;
    MaintenancePolicy maintenance = MaintenancePolicy::Static;
    TriangleGeometry triangles;
    /// Virtual geometry: the proxy level chosen, and the error it declares. Zero for every other
    /// source, which is what makes "how wrong is this hit" one question with one answer.
    u32 detail_level = 0;
    f32 declared_error_metres = 0.0F;
    /// Procedural geometry supplies bounds and an intersection shader rather than triangles. The
    /// shader is named by id: this layer never holds a shader object.
    bool procedural = false;
    Aabb procedural_bounds{};
    u32 intersection_shader_id = 0;
};

/// The static half of an adapter: what a source declares before any particular geometry exists.
struct Adapter {
    GeometrySource source = GeometrySource::StaticMesh;
    MaintenancePolicy maintenance = MaintenancePolicy::Static;
    /// Below this ratio of refit bounds growth, a refit is no longer good enough and the structure
    /// is rebuilt. Only meaningful for `MaintenancePolicy::Refit`.
    f32 refit_quality_floor = 0.5F;
    /// True when the source cannot feed a structure directly and a proxy stands in for it.
    bool uses_proxy = false;
};

/// The adapter table of `ray-tracing-infrastructure`, as data.
[[nodiscard]] Adapter adapter_for(GeometrySource source) noexcept;

// --- The six adapters -----------------------------------------------------------------------
//
// Each returns the `BuildInput` its row of the table describes. They are free functions rather than
// virtual methods because an adapter has no state: it is the row plus the geometry it was handed.

[[nodiscard]] BuildInput adapt_static_mesh(const TriangleGeometry& triangles, u32 lod) noexcept;

[[nodiscard]] BuildInput adapt_skinned_mesh(const TriangleGeometry& posed) noexcept;

/// The proxy row. `triangles` is the proxy the cooker produced, not the source mesh, and `policy`
/// travels into `declared_error_metres` so a consumer can see how wrong the hit may be.
[[nodiscard]] BuildInput adapt_virtual_geometry(const TriangleGeometry& proxy,
                                                const ProxyPolicy& policy) noexcept;

[[nodiscard]] BuildInput adapt_terrain(const TriangleGeometry& heightfield) noexcept;

/// Mesh particles hold no structure of their own: they are instances of the source mesh's.
[[nodiscard]] BuildInput adapt_mesh_particles() noexcept;

[[nodiscard]] BuildInput adapt_procedural(const Aabb& bounds, u32 intersection_shader_id) noexcept;

}  // namespace cy::rendering::rt
