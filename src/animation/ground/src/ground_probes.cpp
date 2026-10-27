// SPDX-License-Identifier: MIT
#include <cy/animation/ground_probes.h>

namespace cy::animation {

namespace {
/// Hits read per probe: the character's own capsule and a foot's collider are the ones skipped,
/// and two more leave room for a trigger it stands in.
constexpr u32 kHitsPerProbe = 4;
}  // namespace

PhysicsGroundProbe::PhysicsGroundProbe(const physics::PhysicsServer& server,
                                       physics::WorldHandle world,
                                       const physics::CollisionFilter& filter) noexcept
    : server_(&server), world_(world), filter_(filter) {}

GroundHit PhysicsGroundProbe::cast_down(const GroundQuery& query) const noexcept {
    physics::RayCastInput input;
    input.origin = query.origin;
    input.direction = Vec3{0.0F, -1.0F, 0.0F};
    input.max_distance = query.length;
    physics::QueryFilter filter;
    filter.filter = filter_;
    physics::RayCastHit hits[kHitsPerProbe];
    const Expected<u32, Error> found =
        server_->raycast_all(world_, input, filter, Span<physics::RayCastHit>(hits, kHitsPerProbe));
    GroundHit ground;
    if (!found) {
        return ground;
    }
    for (u32 index = 0; index < *found && index < kHitsPerProbe; ++index) {
        const physics::RayCastHit& hit = hits[index];
        if (hit.trigger || (query.owner != 0 && hit.user_data == query.owner)) {
            continue;
        }
        ground.hit = true;
        ground.point = hit.position;
        ground.normal = hit.normal;
        return ground;
    }
    return ground;
}

TerrainGroundProbe::TerrainGroundProbe(const terrain::TerrainQuery& terrain) noexcept
    : terrain_(&terrain) {}

GroundHit TerrainGroundProbe::cast_down(const GroundQuery& query) const noexcept {
    const terrain::SurfaceSample sample = terrain_->below(
        static_cast<f64>(query.origin.x), static_cast<f64>(query.origin.z), query.origin.y);
    GroundHit ground;
    if (!sample.resolved || sample.hole || query.origin.y - sample.height > query.length) {
        return ground;
    }
    ground.hit = true;
    ground.point = Vec3{query.origin.x, sample.height, query.origin.z};
    ground.normal = sample.normal;
    return ground;
}

}  // namespace cy::animation
