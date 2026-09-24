// SPDX-License-Identifier: MIT
#include <cy/core/memory/system_allocator.h>
#include <cy/physics/buoyancy/driver.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>

#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

#include <cmath>

using namespace cy;
using namespace cy::physics;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Physics);
}

const world::PartitionConfig& partition() noexcept {
    static const world::PartitionConfig config = [] {
        world::PartitionConfig value;
        value.partition = 1;
        value.base_cell_size = 128.0f;
        return value;
    }();
    return config;
}

f64 deep_bed(void*, f64, f64) noexcept {
    return -20.0;
}

struct Fixture {
    explicit Fixture(bool use_jolt = false) noexcept : jolt_backend(use_jolt) {
#if defined(CY_PHYSICS)
        const auto made = use_jolt ? jolt::create_server(allocator(), nullptr)
                                   : reference::create_server(allocator());
#else
        const auto made = reference::create_server(allocator());
#endif
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        WorldDescription description;
        description.name = Name::intern("buoyancy-physics-test");
        description.body_capacity = 64;
        description.body_pair_capacity = 256;
        description.contact_constraint_capacity = 256;
        const auto created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
        CY_REQUIRE(server->set_gravity(world, Vec3{}).has_value());
    }

    ~Fixture() {
        if (server == nullptr) {
            return;
        }
        (void)server->destroy_world(world);
        server->shutdown();
#if defined(CY_PHYSICS)
        if (jolt_backend) {
            jolt::destroy_server(server, allocator());
            return;
        }
#endif
        reference::destroy_server(server, allocator());
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    BodyHandle body(Vec3 position, Vec3 half_extents,
                    MotionType motion = MotionType::Dynamic) const {
        ShapeDescription shape;
        shape.type = ShapeType::Box;
        shape.half_extents = half_extents;
        const auto created_shape = server->create_shape(shape);
        CY_REQUIRE(created_shape.has_value());
        ColliderDescription collider;
        collider.shape = *created_shape;
        BodyDescription description;
        description.motion = motion;
        description.transform = Transform::from_translation(position);
        description.mass = 100.0f;
        description.colliders = &collider;
        description.collider_count = 1;
        const auto created = server->create_body(world, description);
        CY_REQUIRE(created.has_value());
        CY_REQUIRE(server->destroy_shape(*created_shape).has_value());
        return *created;
    }

    void step(u64 tick) const {
        StepInput input;
        input.tick = tick;
        CY_REQUIRE(server->step(world, input).has_value());
    }

    PhysicsServer* server = nullptr;
    WorldHandle world;
    bool jolt_backend = false;
};

water::WaterBodyDesc pool() noexcept {
    water::WaterBodyDesc description;
    description.name = "buoyancy.pool";
    description.type = water::WaterBodyType::Pool;
    description.backend = water::WaterBackend::Flat;
    description.mean_level = 0.0;
    description.bounds = water::WaterBounds{1990.0, -20.0, 1990.0, 2010.0, 10.0, 2010.0};
    description.density = 1000.0f;
    return description;
}

}  // namespace

CY_TEST_CASE("buoyancy applies authoritative lift through the reference physics interface") {
    Fixture fixture;
    CY_CHECK(fixture.server->capabilities().buoyancy);
    water::WaterSystem water(allocator(), partition());
    CY_REQUIRE(water.registry().add(pool()).has_value());
    water.set_bed_source(&deep_bed, nullptr);
    const BodyHandle body = fixture.body(Vec3{0.0f, -0.5f, 0.0f}, Vec3{1.0f, 0.5f, 1.0f});
    buoyancy::Driver driver(*fixture.server, water, allocator());
    const water::BuoyancySample sample{Vec3{}, 1.0f};
    const auto applied =
        driver.apply(body, Span<const water::BuoyancySample>(&sample, 1), water::BuoyancyParams{},
                     world::WorldVec3d{2000.0, 0.0, 2000.0});
    CY_REQUIRE(applied.has_value());
    CY_CHECK_GT(applied->displacement_force.y, 1000.0f);
    fixture.step(1);
    CY_CHECK_GT(fixture.server->body_state(body)->linear_velocity.y, 0.1f);
    const BodyHandle fixed =
        fixture.body(Vec3{0.0f, -0.5f, 0.0f}, Vec3{1.0f, 0.5f, 1.0f}, MotionType::Kinematic);
    CY_CHECK_EQ(driver
                    .apply(fixed, Span<const water::BuoyancySample>(&sample, 1),
                           water::BuoyancyParams{}, world::WorldVec3d{2000.0, 0.0, 2000.0})
                    .error()
                    .code,
                ErrorCode::InvalidArgument);
}

#if defined(CY_PHYSICS)
CY_TEST_CASE("a floating physics body pitches under multi-point authoritative swell") {
    Fixture fixture(true);
    CY_CHECK(fixture.server->capabilities().buoyancy);
    water::WaterSystem water(allocator(), partition());
    water::WaterBodyDesc ocean;
    ocean.name = "buoyancy.ocean";
    ocean.type = water::WaterBodyType::Ocean;
    ocean.backend = water::WaterBackend::Spectral;
    ocean.mean_level = 0.0;
    ocean.bounds = water::WaterBounds{-100.0, -20.0, -100.0, 100.0, 10.0, 100.0};
    ocean.density = 1025.0f;
    const auto added = water.registry().add(ocean);
    CY_REQUIRE(added.has_value());
    water::OceanParams swell;
    swell.shortest_wavelength = 30.0f;
    swell.longest_wavelength = 60.0f;
    swell.cascades = 1;
    swell.trains_per_cascade = 1;
    swell.spread_degrees = 0.0f;
    swell.wind_speed_mps = 12.0f;
    CY_REQUIRE(water.set_ocean(*added, swell, 314).has_value());
    water.set_bed_source(&deep_bed, nullptr);

    const BodyHandle vessel = fixture.body(Vec3{0.0f, -0.5f, 0.0f}, Vec3{30.0f, 0.5f, 1.0f});
    const water::BuoyancySample hull[2] = {{Vec3{-30.0f, 0.0f, 0.0f}, 4.0f},
                                           {Vec3{30.0f, 0.0f, 0.0f}, 4.0f}};
    water::BuoyancyState state;
    state.position = world::WorldVec3d{0.0, -0.5, 0.0};
    f32 greatest = 0.0f;
    f64 best_time = 0.0;
    for (u32 index = 0; index < 24; ++index) {
        const f64 time = static_cast<f64>(index) * 0.4;
        water.set_time(time);
        const auto result = water.buoyancy(state, hull, {});
        CY_REQUIRE(result.has_value());
        if (std::fabs(result->torque.z) > greatest) {
            greatest = std::fabs(result->torque.z);
            best_time = time;
        }
    }
    CY_REQUIRE(greatest > 1000.0f);
    water.set_time(best_time);
    buoyancy::Driver driver(*fixture.server, water, allocator());
    const auto applied = driver.apply(vessel, hull, {});
    CY_REQUIRE(applied.has_value());
    CY_CHECK_GT(std::fabs(applied->torque.z), 1000.0f);
    fixture.step(1);
    const f32 angular = fixture.server->body_state(vessel)->angular_velocity.z;
    CY_CHECK_GT(std::fabs(angular), 0.01f);
    CY_CHECK_GT(angular * applied->torque.z, 0.0f);
}
#endif
