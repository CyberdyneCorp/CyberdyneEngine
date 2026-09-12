#pragma once
// Where a generated point set becomes a representation. M10 task 4.3, and the second target of
// src/pcg/CMakeLists.txt.
//
// `procedural-content-generation` — "Output adapters": "Generators SHALL produce results through
// output adapters, and the adapter SHALL determine the representation ... Foliage -> compact
// instance populations in clusters (see `foliage`); Terrain -> stamps and layers in the
// non-destructive modifier stack (see `terrain`)", and "**A procedural result SHALL NOT be an
// entity by default.** A generator producing ten million trees SHALL produce a foliage population,
// not ten million spawn operations."
//
// ================================================================================================
// WHY THESE THREE ARE HERE AND NOT IN `cy::pcg`
// ================================================================================================
//
// Each of them names a representation module — `cy::foliage` for the population, `cy::terrain` for
// the modifier stack and for the surface a placement is queried against — and the generator must
// not. `src/pcg/CMakeLists.txt` says it at length: an adapter is an extension point, so a cook, a
// dedicated server or a unit test that compiles a graph would otherwise link a renderer-facing
// vegetation module it has no use for. `cy::terrain-cook` and `cy::foliage-render` are split off
// their own modules for the same reason.
//
// THE FOLIAGE ADAPTER IS WHERE "TEN MILLION TREES ARE A POPULATION" ACTUALLY HAPPENS. It takes a
// whole region's points in one call and produces ONE `foliage::FoliageCluster`.
//
// Nothing here creates an entity, and `cy::pcg` — the GENERATOR — cannot name one at all: it does
// not link `cy::ecs`, which is the strongest form of "a procedural result SHALL NOT be an entity by
// default" available. This target is weaker and honestly so: it links `cy::foliage`, which links
// `cy::ecs` for its promotion path, so `Entity` is reachable from here through a dependency.
// `foliage`'s own promotion BINDS an entity the caller created rather than creating one, and the
// only thing this file calls is `ClusterBuilder`.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>
#include <cy/pcg/adapters.h>
#include <cy/pcg/execute.h>
#include <cy/terrain/surface.h>

namespace cy::terrain {
/// The non-destructive modifier stack, by forward declaration: `TerrainStampAdapter` holds a
/// reference to one and nothing in this header reads its fields, so the stack's own header belongs
/// to the translation unit that appends to it rather than to everything that includes this.
class ModifierStack;
}  // namespace cy::terrain

namespace cy::pcg {

/// A generated point set becomes a foliage cluster.
///
/// The species is chosen by an attribute column where the graph wrote one and is the adapter's
/// declared default otherwise: which species a rule selects is the GRAPH's decision, and an adapter
/// that decided it would be the placement rule living in the representation layer.
class FoliageOutputAdapter final : public OutputAdapter {
public:
    FoliageOutputAdapter(Allocator& allocator, foliage::ClusterStore& store,
                         const foliage::ClusterPolicy& policy, foliage::SpeciesId species,
                         u64 seed) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "pcg.foliage"; }
    [[nodiscard]] OutputTarget target() const noexcept override { return OutputTarget::Foliage; }

    [[nodiscard]] Status emit(const EmitContext& context, const PointSet& points) noexcept override;
    [[nodiscard]] Status demote(const EmitContext& context) noexcept override;

    /// The column a point's species comes from, when the graph writes one. Zero means the adapter's
    /// declared default for every point.
    void set_species_attribute(AttributeId attribute) noexcept { species_attribute_ = attribute; }

    [[nodiscard]] u64 instances() const noexcept { return instances_; }
    [[nodiscard]] u64 clusters() const noexcept { return clusters_; }
    [[nodiscard]] const foliage::ClusterBuildReport& last_report() const noexcept {
        return last_report_;
    }

private:
    Allocator* allocator_;
    foliage::ClusterStore* store_;
    foliage::ClusterPolicy policy_;
    foliage::SpeciesId species_;
    AttributeId species_attribute_;
    u64 seed_ = 0;
    u64 instances_ = 0;
    u64 clusters_ = 0;
    foliage::ClusterBuildReport last_report_;
};

/// A generated point set becomes terrain modifiers: one `Crater` stamp per point.
///
/// The specification's row is "Terrain -> stamps and layers in the non-destructive modifier stack",
/// and `terrain::Modifier` is that stack's unit. The adapter appends rather than edits, because the
/// stack is non-destructive by construction and a generator that removed someone else's modifier
/// would be editing an author's work.
class TerrainStampAdapter final : public OutputAdapter {
public:
    TerrainStampAdapter(terrain::ModifierStack& stack, f32 radius_metres, f32 amplitude,
                        u8 layer) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "pcg.terrain-stamp"; }
    [[nodiscard]] OutputTarget target() const noexcept override { return OutputTarget::Terrain; }

    [[nodiscard]] Status emit(const EmitContext& context, const PointSet& points) noexcept override;

    [[nodiscard]] u64 stamps() const noexcept { return stamps_; }

private:
    terrain::ModifierStack* stack_;
    f32 radius_ = 0.0F;
    f32 amplitude_ = 0.0F;
    u8 layer_ = 0;
    u64 stamps_ = 0;
};

/// The generator's geometric queries, answered by the terrain representation.
///
/// `procedural-content-generation` — "Spatial queries": "Build-time generation SHALL NOT require a
/// running physics world in order to perform geometric queries; canonical geometric representations
/// SHALL be used." `terrain::TerrainQuery` is that representation and names no physics type either,
/// so the whole path from a placement rule to a surface is physics-free by construction.
///
/// A HOLE IS NOT A SURFACE. `surface_batch()` reports false where the terrain reports a hole or
/// fails to resolve, which is what stops a tree being planted in a cave mouth — and it is the
/// terrain module's own distinction rather than a threshold invented here.
class TerrainSpatialQuery final : public SpatialQuery {
public:
    explicit TerrainSpatialQuery(const terrain::TerrainQuery& query) noexcept : query_(&query) {}

    void height_batch(Span<const f64> x, Span<const f64> z, Span<f32> out) const noexcept override;
    void slope_batch(Span<const f64> x, Span<const f64> z, Span<f32> out) const noexcept override;
    void surface_batch(Span<const f64> x, Span<const f64> z, Span<u8> out) const noexcept override;

private:
    const terrain::TerrainQuery* query_;
};

}  // namespace cy::pcg
