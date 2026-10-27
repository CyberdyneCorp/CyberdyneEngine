// SPDX-License-Identifier: MIT
#pragma once
// The two grounds a host hands foot placement: a physics world and a terrain. Issue #92.
//
// Both are const and thread-safe because their queries are: `PhysicsServer`'s queries are
// (`physics` — "Parallel queries"), and `TerrainQuery` neither blocks nor allocates nor mutates.
// That is what lets `AnimationSystem` call them from every job worker of `Stage::Animation` at
// once.

#include <cy/animation/foot_placement.h>
#include <cy/servers/physics/server.h>
#include <cy/terrain/surface.h>

namespace cy::animation {

/// Ground from a physics world: the first solid surface a ray straight down meets, skipping the
/// asking character's own bodies (`GroundQuery::owner` against each hit's user data, which is the
/// entity) and trigger volumes.
class PhysicsGroundProbe final : public GroundProbe {
public:
    /// `server` and `world` are borrowed. `filter` says which layers are ground.
    PhysicsGroundProbe(const physics::PhysicsServer& server, physics::WorldHandle world,
                       const physics::CollisionFilter& filter = {}) noexcept;
    [[nodiscard]] GroundHit cast_down(const GroundQuery& query) const noexcept override;

private:
    const physics::PhysicsServer* server_;
    physics::WorldHandle world_;
    physics::CollisionFilter filter_;
};

/// Ground from a terrain: the first surface at or below the ray's origin, within its length. A
/// hole is not ground.
class TerrainGroundProbe final : public GroundProbe {
public:
    explicit TerrainGroundProbe(const terrain::TerrainQuery& terrain) noexcept;
    [[nodiscard]] GroundHit cast_down(const GroundQuery& query) const noexcept override;

private:
    const terrain::TerrainQuery* terrain_;
};

}  // namespace cy::animation
