#pragma once
// Apply WaterSystem's authoritative buoyancy calculation through the engine physics interface.

#include <cy/core/memory/array.h>
#include <cy/servers/physics/server.h>
#include <cy/water/system.h>

namespace cy::physics::buoyancy {

class Driver {
public:
    Driver(PhysicsServer& physics, water::WaterSystem& water, Allocator& allocator) noexcept
        : physics_(physics), water_(water), samples_(allocator) {}

    /// Call before the fixed physics step. Hull offsets are body-local; origin maps the physics
    /// world's local coordinates into WaterSystem's absolute coordinates after an origin rebase.
    /// The result is returned for diagnostics; the same force and torque are applied to the body.
    [[nodiscard]] Expected<water::BuoyancyResult, Error> apply(
        BodyHandle body, Span<const water::BuoyancySample> hull,
        const water::BuoyancyParams& params, world::WorldVec3d origin = {}) noexcept;

private:
    PhysicsServer& physics_;
    water::WaterSystem& water_;
    Array<water::BuoyancySample> samples_;
};

}  // namespace cy::physics::buoyancy
