// The representation adapters. See include/cy/pcg/foliage_adapter.h.

#include <cy/pcg/foliage_adapter.h>

#include <cy/terrain/stack.h>

#include <cmath>
#include <utility>

namespace cy::pcg {

FoliageOutputAdapter::FoliageOutputAdapter(Allocator& allocator, foliage::ClusterStore& store,
                                           const foliage::ClusterPolicy& policy,
                                           foliage::SpeciesId species, u64 seed) noexcept
    : allocator_(&allocator), store_(&store), policy_(policy), species_(species), seed_(seed) {}

Status FoliageOutputAdapter::emit(const EmitContext& context, const PointSet& points) noexcept {
    if (points.empty()) {
        return ok();
    }
    // ONE CLUSTER PER REGION, built from the whole region's points in one call. This is the
    // mechanical content of "ten million trees are a population": the loop below adds records to a
    // builder, and nothing in this function or anything it calls creates an engine object per tree.
    const foliage::ClusterCoord coord{context.region.x, context.region.z, context.region.level};
    foliage::ClusterBounds bounds;
    bounds.min_x = context.origin_x;
    bounds.min_z = context.origin_z;
    bounds.max_x = context.origin_x + context.region_metres;
    bounds.max_z = context.origin_z + context.region_metres;
    bounds.min_y = 0.0F;
    bounds.max_y = 0.0F;
    for (usize index = 0; index < points.size(); ++index) {
        bounds.min_y = index == 0
                           ? points.y(index)
                           : (points.y(index) < bounds.min_y ? points.y(index) : bounds.min_y);
        bounds.max_y = index == 0
                           ? points.y(index)
                           : (points.y(index) > bounds.max_y ? points.y(index) : bounds.max_y);
    }
    // A zero-height cluster would quantise every instance's y to one value; a metre of headroom
    // costs nothing and keeps the encode meaningful for a flat region.
    bounds.max_y += 1.0F;

    // The GENERATOR VERSION participates in the cluster's identity, which is `foliage`'s own rule:
    // a cluster generated under version 3 has a different identity from the same square under
    // version 4, so a cache reports a miss rather than serving the old forest.
    const foliage::ClusterId id =
        foliage::cluster_identity(context.seed, coord, context.generator_version);
    foliage::ClusterBuilder builder(*allocator_, policy_, id, coord, bounds);
    for (usize index = 0; index < points.size(); ++index) {
        const world::WorldVec3d position{context.origin_x + static_cast<f64>(points.x(index)),
                                         static_cast<f64>(points.y(index)),
                                         context.origin_z + static_cast<f64>(points.z(index))};
        foliage::SpeciesId species = species_;
        if (species_attribute_.is_valid()) {
            // Which species a rule selects is the GRAPH's decision. An adapter that decided it
            // would be a placement rule living in the representation layer.
            const u64 chosen = points.get_u64(species_attribute_, index);
            species = chosen != 0 ? foliage::SpeciesId{chosen} : species_;
        }
        // Yaw and scale from the point's own identity rather than from a draw: the identity is
        // already a hash of (seed, node, region, slot), so a per-instance variation derived from it
        // is reproducible without the adapter holding a stream of its own — and an adapter with a
        // stream would be a second source of procedural variation outside the graph.
        const u64 identity = points.identity(index);
        const f32 yaw = static_cast<f32>(identity & 0xFFFFULL) * (6.2831853F / 65536.0F);
        const f32 scale = 0.75F + static_cast<f32>((identity >> 16U) & 0xFFULL) / 1024.0F;
        const u8 variation = static_cast<u8>((identity >> 24U) & 0xFFULL);
        if (Status added = builder.add(species, position, yaw, scale, variation, 0, 0.0F, 0.0F,
                                       foliage::InstanceFlags{});
            !added) {
            return added;
        }
    }
    Expected<foliage::FoliageCluster, Error> cluster = builder.finish();
    if (!cluster) {
        return Status{make_unexpected(cluster.error())};
    }
    last_report_ = builder.report();
    instances_ += cluster->size();
    ++clusters_;
    return store_->insert(std::move(*cluster));
}

Status FoliageOutputAdapter::demote(const EmitContext& context) noexcept {
    const foliage::ClusterCoord coord{context.region.x, context.region.z, context.region.level};
    const foliage::ClusterId id =
        foliage::cluster_identity(context.seed, coord, context.generator_version);
    // The cluster goes and the region's macro state — which `pcg::GenerationWorld::demote()` has
    // already updated from the detail — stays. A demotion that dropped both would lose what
    // happened in the region, which is the round trip the specification names.
    (void)store_->evict(id);
    return ok();
}

// --- The terrain stamp adapter -------------------------------------------------------------

TerrainStampAdapter::TerrainStampAdapter(terrain::ModifierStack& stack, f32 radius_metres,
                                         f32 amplitude, u8 layer) noexcept
    : stack_(&stack), radius_(radius_metres), amplitude_(amplitude), layer_(layer) {}

Status TerrainStampAdapter::emit(const EmitContext& context, const PointSet& points) noexcept {
    for (usize index = 0; index < points.size(); ++index) {
        const f64 wx = context.origin_x + static_cast<f64>(points.x(index));
        const f64 wz = context.origin_z + static_cast<f64>(points.z(index));
        terrain::Modifier modifier;
        modifier.kind = terrain::ModifierKind::Crater;
        modifier.name = context.generator_name;
        modifier.bounds =
            terrain::TerrainBounds{wx - static_cast<f64>(radius_), wz - static_cast<f64>(radius_),
                                   wx + static_cast<f64>(radius_), wz + static_cast<f64>(radius_)};
        // THE DECLARED RADIUS, and it is zero on purpose: the stamp's own bounds already cover
        // everything it touches, so a second radius would widen terrain's dirty set past what the
        // modifier reads. `terrain`'s `dirty_tiles()` is bounds-plus-radius exactly.
        modifier.radius = 0.0F;
        modifier.amplitude = amplitude_;
        modifier.period = radius_ * 2.0F;
        modifier.layer = layer_;
        if (Expected<u32, Error> added = stack_->add(modifier); !added) {
            return Status{make_unexpected(added.error())};
        }
        ++stamps_;
    }
    return ok();
}

// --- The terrain-backed spatial query ------------------------------------------------------

void TerrainSpatialQuery::height_batch(Span<const f64> x, Span<const f64> z,
                                       Span<f32> out) const noexcept {
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        const terrain::SurfaceSample sample = query_->sample(x[index], z[index]);
        out[index] = sample.resolved ? sample.height : 0.0F;
    }
}

void TerrainSpatialQuery::slope_batch(Span<const f64> x, Span<const f64> z,
                                      Span<f32> out) const noexcept {
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        const terrain::SurfaceSample sample = query_->sample(x[index], z[index]);
        // Radians, because that is what `SpatialQuery` declares. `terrain` answers in degrees, and
        // converting here rather than changing either interface keeps each module's own unit its
        // own — a mismatch that is converted at the seam is a mismatch nobody has to remember.
        out[index] = sample.resolved ? sample.slope_degrees * (3.14159265F / 180.0F) : 0.0F;
    }
}

void TerrainSpatialQuery::surface_batch(Span<const f64> x, Span<const f64> z,
                                        Span<u8> out) const noexcept {
    for (usize index = 0; index < out.size() && index < x.size() && index < z.size(); ++index) {
        const terrain::SurfaceSample sample = query_->sample(x[index], z[index]);
        // A HOLE IS NOT A SURFACE, and it is terrain's own distinction: `resolved` is false beside
        // a hole, so this is one test rather than two and it cannot drift from terrain's meaning.
        out[index] = (sample.resolved && !sample.hole) ? 1 : 0;
    }
}

}  // namespace cy::pcg
