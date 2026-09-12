#pragma once
// The terrain QUERY INTERFACE, and the representations that answer it. M10 task 2.1.
//
// `terrain` — "Three representations are admitted — heightfield, mesh, and signed distance field —
// with only the first required, so that no consumer is permitted to assume terrain is a heightmap
// and caves become an implementation rather than a rewrite."
//
// ================================================================================================
// WHY THE PRIMITIVE IS A COLUMN AND NOT A HEIGHT
// ================================================================================================
//
// The requirement's own scenario is the design constraint: a consumer asking for the surface at a
// position "SHALL use the terrain query interface, which SHALL be able to answer for any
// representation, INCLUDING ONE WITH MULTIPLE SURFACES ABOVE A POINT".
//
// A `height_at(x, z)` returning one float cannot answer that, and every consumer written against it
// would have to be rewritten the day an arch exists. So the primitive here is `column()`, which
// writes every surface in the column highest-first, and `sample()` is the one-surface convenience
// spelled in terms of it. A heightfield answers with one entry; a mesh cliff answers with two; an
// SDF cave will answer with as many as it has.
//
// `SurfaceSource` is the seam a representation plugs into, and it is an abstract class for the same
// reason `world::Partitioner` is: the replaceable thing is named once, and everything above it is
// written against the interface. `HeightfieldSource` is the required one. `MeshSource` is built
// here because "a world MAY combine them: a heightfield surface with mesh cliffs" is a claim about
// composition that a second source is the only honest way to check. The SDF is NOT built, and the
// place that says so is `Representation::SignedDistanceField` plus deform.h's refusal of a
// structural deformation — a `NotImplemented` naming the missing representation, rather than a
// silent success that carves nothing.
//
// ================================================================================================
// A QUERY NEVER BLOCKS AND NEVER FAULTS
// ================================================================================================
//
// "A query in a region whose terrain is not resident SHALL return the COARSEST RESIDENT ANSWER with
// a RESOLUTION INDICATOR, and SHALL NOT block." `SurfaceSample::level` and `sample_metres` are that
// indicator, `resolved` is false where nothing was resident at all, and no path in this file
// touches a streaming system: the store answers with what it has, which is the same discipline
// `environment::FieldStore::sample()` follows.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// The three representations the interface admits. `terrain`'s own table, in its order.
enum class Representation : u8 {
    /// The common ground surface. Required.
    Heightfield = 0,
    /// Cliffs, overhangs, arches, authored formations.
    Mesh,
    /// Caves, tunnels, runtime excavation. Planned; see the header note.
    SignedDistanceField,
};

[[nodiscard]] const char* representation_name(Representation representation) noexcept;

/// One surface in a column, and how good the answer is.
struct SurfaceSample {
    /// Metres. Meaningless when `resolved` is false.
    f32 height = 0.0F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    f32 slope_degrees = 0.0F;
    /// The surface's second derivative, positive on a ridge and negative in a hollow. A material
    /// rule input in its own right — `terrain` names curvature beside slope and altitude.
    f32 curvature = 0.0F;
    /// The dominant material layer and the biome at this position.
    u8 layer = 0;
    u8 biome = 0;
    /// True where the surface is deliberately absent. A hole is NOT a surface: `resolved` is false
    /// beside it, so a consumer that only checks `resolved` cannot place an object on a cave mouth.
    bool hole = false;
    bool resolved = false;
    /// The resolution indicator the specification requires of an answer from unstreamed terrain.
    u8 level = 0;
    f32 sample_metres = 0.0F;
    Representation from = Representation::Heightfield;
};

/// The largest column a query will report. Four surfaces is a terrace, a bridge deck, its underside
/// and the ground; a column with more is reported truncated rather than growing an allocation into
/// a query that must not allocate.
inline constexpr u32 kMaxColumnSurfaces = 4;

/// A representation that can answer for a column. See the header note.
class SurfaceSource {
public:
    SurfaceSource() = default;
    SurfaceSource(const SurfaceSource&) = delete;
    SurfaceSource& operator=(const SurfaceSource&) = delete;
    virtual ~SurfaceSource() = default;

    [[nodiscard]] virtual Representation representation() const noexcept = 0;

    /// Every surface this representation has in the column at (x, z), HIGHEST FIRST, written into
    /// `out`. Returns how many were written; never blocks, never allocates, never faults.
    [[nodiscard]] virtual u32 column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept = 0;
};

/// Runtime height change, as the query path sees it. Implemented by deform.h's `TerrainDeltaStore`.
///
/// It is an interface rather than a direct dependency so that surface.h does not have to know how a
/// delta is stored, replicated or persisted — and so `cooked terrain + terrain delta = current
/// terrain` is one addition in one place rather than a rule every consumer applies for itself.
class HeightDeltaSource {
public:
    HeightDeltaSource() = default;
    HeightDeltaSource(const HeightDeltaSource&) = delete;
    HeightDeltaSource& operator=(const HeightDeltaSource&) = delete;
    virtual ~HeightDeltaSource() = default;

    /// Metres to add to one cooked sample. Zero where nothing was deformed.
    [[nodiscard]] virtual f32 height_delta(const TileCoord& coord, u32 i, u32 j) const noexcept = 0;
    /// Whether a runtime edit opened a hole in this quad.
    [[nodiscard]] virtual bool hole_delta(const TileCoord& coord, u32 qi,
                                          u32 qj) const noexcept = 0;
};

/// The required representation: the cooked heightfield, plus the runtime delta over it.
class HeightfieldSource final : public SurfaceSource {
public:
    HeightfieldSource(const TerrainStore& store, const HeightDeltaSource* delta) noexcept
        : store_(&store), delta_(delta) {}

    [[nodiscard]] Representation representation() const noexcept override {
        return Representation::Heightfield;
    }
    [[nodiscard]] u32 column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept override;

    /// The finest resident level's answer at a position, whatever the delta says about holes. Used
    /// by collision and meshing, which work per tile rather than per column.
    [[nodiscard]] f32 sample_height(const TileCoord& coord, u32 i, u32 j) const noexcept;

private:
    const TerrainStore* store_;
    const HeightDeltaSource* delta_;
};

/// A triangle of the mesh representation. f64 in the horizontal axes for the reason every other
/// absolute position in this module is f64, and f32 in height because a world is thin vertically.
struct WorldTriangle {
    TerrainPoint a;
    TerrainPoint b;
    TerrainPoint c;
    f32 height_a = 0.0F;
    f32 height_b = 0.0F;
    f32 height_c = 0.0F;
    u8 layer = 0;
    u8 biome = 0;
};

/// An authored triangle surface: cliffs, overhangs and arches. `terrain`'s `Mesh` representation.
///
/// Deliberately small. It exists so that "a world MAY combine them" and "including one with
/// multiple surfaces above a point" are properties this module can be MEASURED against rather than
/// claims about an interface nothing has ever implemented twice.
class MeshSource final : public SurfaceSource {
public:
    explicit MeshSource(Allocator& allocator) noexcept;

    [[nodiscard]] Representation representation() const noexcept override {
        return Representation::Mesh;
    }
    [[nodiscard]] u32 column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept override;

    /// Add one triangle, in absolute world coordinates. `layer` and `biome` are what a query
    /// reports where this triangle answers.
    [[nodiscard]] Status add_triangle(const WorldTriangle& triangle) noexcept;
    [[nodiscard]] usize triangle_count() const noexcept { return triangles_.size(); }

private:
    Array<WorldTriangle> triangles_;
};

/// What a ray found.
struct RayHit {
    SurfaceSample surface;
    /// Distance along the ray, in metres.
    f32 distance = 0.0F;
    bool hit = false;
};

/// The interface every consumer of terrain uses — gameplay, AI, VFX, audio, foliage placement.
///
/// `terrain` — "Queries SHALL be answerable from the terrain representation WITHOUT A PHYSICS
/// QUERY, and SHALL be batchable." Neither this class nor anything it calls names a physics type.
class TerrainQuery {
public:
    explicit TerrainQuery(Allocator& allocator) noexcept;

    /// Sources are consulted in the order they were added and their answers merged highest-first.
    /// The heightfield is a source like any other: nothing here privileges it, which is what stops
    /// "the ground" from meaning "the heightmap" inside this class.
    [[nodiscard]] Status add_source(const SurfaceSource& source) noexcept;
    [[nodiscard]] usize source_count() const noexcept { return sources_.size(); }

    /// Every surface in the column, highest first. The primitive; everything below is spelled in
    /// terms of it.
    [[nodiscard]] u32 column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept;

    /// The topmost surface. What "place an object on the ground" means when there is one ground.
    [[nodiscard]] SurfaceSample sample(f64 x, f64 z) const noexcept;

    /// The first surface at or below `from_height`, and the first at or above it. This is the
    /// vertical cast the specification asks for, and it is the pair of calls that makes a bridge
    /// usable: a character under an arch wants the surface below it, not the arch.
    [[nodiscard]] SurfaceSample below(f64 x, f64 z, f32 from_height) const noexcept;
    [[nodiscard]] SurfaceSample above(f64 x, f64 z, f32 from_height) const noexcept;

    /// March a ray against the surface. Deterministic: a fixed step derived from the finest
    /// resident sample spacing, then a fixed number of bisections, so two machines agree.
    [[nodiscard]] RayHit raycast(f64 origin_x, f32 origin_height, f64 origin_z, Vec3 direction,
                                 f32 max_distance) const noexcept;

    /// "Queries ... SHALL be batchable." One traversal of the source list for the whole batch
    /// rather than one per position, which is the per-query overhead the requirement is about.
    [[nodiscard]] Status sample_many(Span<const TerrainPoint> positions,
                                     Span<SurfaceSample> out) const noexcept;

private:
    Array<const SurfaceSource*> sources_;
};

}  // namespace cy::terrain
