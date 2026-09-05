#pragma once
// The shared fixture for the physics suites: a reference-backed server, one world, and the
// handful of shapes every case needs. Section 4.2.
//
// WHY THE FIXTURE IS OVER `PhysicsServer&` AND NOT OVER `ReferenceServer`. Every case here is a
// case about the INTERFACE, and `physics` requires that "no gameplay code, component definition, or
// scene asset SHALL require changes" when the backend changes. A fixture typed on the concrete
// backend would let a case reach past the interface without anybody noticing; typed on the
// interface, the same cases are what src/backends/physics-jolt/tests/ runs against Jolt.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/servers/physics/server.h>
#include <cy/test/test.h>

namespace cy::physics::test {

inline Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Physics);
}

/// A server, a world and the shapes a case builds a scene out of.
///
/// The world is sized for tens of bodies rather than the default thousand: the reference backend's
/// broad phase is O(n^2) and the event buffer is reserved at creation, so the default would have
/// every case paying for a capacity nothing uses.
struct Fixture {
    PhysicsServer* server = nullptr;
    WorldHandle world;

    Fixture() noexcept {
        const Expected<PhysicsServer*, Error> made = reference::create_server(allocator());
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        WorldDescription description;
        description.name = Name::intern("test-world");
        description.body_capacity = 64;
        description.body_pair_capacity = 256;
        description.contact_constraint_capacity = 256;
        const Expected<WorldHandle, Error> created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
    }

    ~Fixture() {
        if (server != nullptr) {
            server->shutdown();
            reference::destroy_server(server, allocator());
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] ShapeHandle box(Vec3 half_extents) const noexcept {
        ShapeDescription description;
        description.type = ShapeType::Box;
        description.half_extents = half_extents;
        const Expected<ShapeHandle, Error> shape = server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        return *shape;
    }

    [[nodiscard]] ShapeHandle sphere(f32 radius) const noexcept {
        ShapeDescription description;
        description.type = ShapeType::Sphere;
        description.radius = radius;
        const Expected<ShapeHandle, Error> shape = server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        return *shape;
    }

    /// A ground half-space through the origin with the given normal. Exact in the reference
    /// backend, which is what makes the slope cases mean something.
    [[nodiscard]] ShapeHandle ground_plane(Vec3 normal = Vec3{0.0f, 1.0f, 0.0f}) const noexcept {
        ShapeDescription description;
        description.type = ShapeType::Plane;
        description.plane = Plane::from_point_normal(Vec3{}, normalize(normal));
        const Expected<ShapeHandle, Error> shape = server->create_shape(description);
        CY_REQUIRE(shape.has_value());
        return *shape;
    }

    /// A body with one collider. The overwhelmingly common shape of a test scene.
    [[nodiscard]] BodyHandle body(ShapeHandle shape, MotionType motion, Vec3 position,
                                  UserData user_data = 0, bool trigger = false) const noexcept {
        ColliderDescription collider;
        collider.shape = shape;
        collider.is_trigger = trigger;
        BodyDescription description;
        description.motion = motion;
        description.transform = Transform::from_translation(position);
        description.colliders = &collider;
        description.collider_count = 1;
        description.user_data = user_data;
        // Off by default in the fixture: a case that wants to watch a body come to rest asks for
        // it, and a case that does not should not have its body silently stop integrating halfway
        // through.
        description.allow_sleeping = false;
        const Expected<BodyHandle, Error> made = server->create_body(world, description);
        CY_REQUIRE(made.has_value());
        return *made;
    }

    Status step(u64 tick, f32 delta = 1.0f / 60.0f) const noexcept {
        StepInput input;
        input.delta_seconds = delta;
        input.tick = tick;
        return server->step(world, input);
    }

    [[nodiscard]] Vec3 position_of(BodyHandle body) const noexcept {
        const Expected<BodyState, Error> state = server->body_state(body);
        CY_REQUIRE(state.has_value());
        return state->transform.translation;
    }
};

}  // namespace cy::physics::test
