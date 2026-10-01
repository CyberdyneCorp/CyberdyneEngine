// SPDX-License-Identifier: MIT
// physics_fixture.h — a reference physics world bound to an ABI host, shared by the physics, body
// and character suites of src/game_backend/tests. Header-only and in an anonymous namespace: each
// suite is its own executable, so each gets its own copy and nothing is linked twice.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/servers/physics/server.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

using namespace cy;
using namespace cy::physics;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Physics);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// Bit-for-bit equality. The determinism claims are about bytes, not values within a tolerance,
/// so this compares object representations on purpose.
[[maybe_unused]] bool same_bytes(const void* a, const void* b, std::size_t size) noexcept {
    return std::memcmp(a, b, size) == 0;
}

/// Every call forwarded to the reference server, except that `stepping()` can be forced on. The
/// reference backend answers `stepping()` true only inside its own `step()`, where no test code
/// runs; forcing it is how "a query during the step" is reached without contriving a callback into
/// the solver. Generated mechanically from server.h's pure virtuals.
class SteppingServer final : public PhysicsServer {
public:
    PhysicsServer* inner = nullptr;
    bool forced_stepping = false;

    const char* backend_name() const noexcept override { return inner->backend_name(); }
    Status initialize() noexcept override { return inner->initialize(); }
    void shutdown() noexcept override { inner->shutdown(); }
    bool is_null_backend() const noexcept override { return inner->is_null_backend(); }
    Capabilities capabilities() const noexcept override { return inner->capabilities(); }
    bool stepping() const noexcept override { return forced_stepping || inner->stepping(); }
    Expected<WorldHandle, Error> create_world(
        const WorldDescription& description) noexcept override {
        return inner->create_world(description);
    }
    Status destroy_world(WorldHandle world) noexcept override {
        return inner->destroy_world(world);
    }
    Status set_gravity(WorldHandle world, Vec3 gravity) noexcept override {
        return inner->set_gravity(world, gravity);
    }
    Status step(WorldHandle world, const StepInput& input) noexcept override {
        return inner->step(world, input);
    }
    Expected<StepStatistics, Error> statistics(WorldHandle world) const noexcept override {
        return inner->statistics(world);
    }
    Expected<Span<const ContactEvent>, Error> events(WorldHandle world) const noexcept override {
        return inner->events(world);
    }
    Expected<Span<const ConstraintBroken>, Error> broken_constraints(
        WorldHandle world) const noexcept override {
        return inner->broken_constraints(world);
    }
    Expected<MaterialHandle, Error> create_material(
        const MaterialDescription& description) noexcept override {
        return inner->create_material(description);
    }
    Status destroy_material(MaterialHandle material) noexcept override {
        return inner->destroy_material(material);
    }
    Expected<MaterialDescription, Error> material(MaterialHandle material) const noexcept override {
        return inner->material(material);
    }
    Expected<ShapeHandle, Error> create_shape(
        const ShapeDescription& description) noexcept override {
        return inner->create_shape(description);
    }
    Status destroy_shape(ShapeHandle shape) noexcept override {
        return inner->destroy_shape(shape);
    }
    Expected<ShapeStatistics, Error> shape_statistics() const noexcept override {
        return inner->shape_statistics();
    }
    Status update_height_field(ShapeHandle shape, u32 x, u32 z, u32 width, u32 depth,
                               Span<const f32> samples) noexcept override {
        return inner->update_height_field(shape, x, z, width, depth, samples);
    }
    Expected<BodyHandle, Error> create_body(WorldHandle world,
                                            const BodyDescription& description) noexcept override {
        return inner->create_body(world, description);
    }
    Expected<BodyHandle, Error> create_soft_body(
        WorldHandle world, const SoftBodyDescription& description) noexcept override {
        return inner->create_soft_body(world, description);
    }
    Expected<u32, Error> soft_body_vertices(BodyHandle body,
                                            Span<Vec3> out) const noexcept override {
        return inner->soft_body_vertices(body, out);
    }
    Status create_bodies(WorldHandle world, Span<const BodyDescription> descriptions,
                         Span<BodyHandle> out) noexcept override {
        return inner->create_bodies(world, descriptions, out);
    }
    Status destroy_body(BodyHandle body) noexcept override { return inner->destroy_body(body); }
    Status destroy_bodies(Span<const BodyHandle> bodies) noexcept override {
        return inner->destroy_bodies(bodies);
    }
    Expected<BodyState, Error> body_state(BodyHandle body) const noexcept override {
        return inner->body_state(body);
    }
    Expected<MassProperties, Error> mass_properties(BodyHandle body) const noexcept override {
        return inner->mass_properties(body);
    }
    Expected<UserData, Error> body_user_data(BodyHandle body) const noexcept override {
        return inner->body_user_data(body);
    }
    bool body_alive(BodyHandle body) const noexcept override { return inner->body_alive(body); }
    Status set_body_transform(BodyHandle body, const Transform& transform,
                              TeleportMode mode) noexcept override {
        return inner->set_body_transform(body, transform, mode);
    }
    Status set_body_velocity(BodyHandle body, Vec3 linear, Vec3 angular) noexcept override {
        return inner->set_body_velocity(body, linear, angular);
    }
    Status set_body_motion_type(BodyHandle body, MotionType motion) noexcept override {
        return inner->set_body_motion_type(body, motion);
    }
    Status set_body_filter(BodyHandle body, CollisionFilter filter) noexcept override {
        return inner->set_body_filter(body, filter);
    }
    Status set_body_gravity_scale(BodyHandle body, f32 scale) noexcept override {
        return inner->set_body_gravity_scale(body, scale);
    }
    Status set_body_awake(BodyHandle body, bool awake) noexcept override {
        return inner->set_body_awake(body, awake);
    }
    Status set_pair_ignored(BodyHandle a, BodyHandle b, bool ignored) noexcept override {
        return inner->set_pair_ignored(a, b, ignored);
    }
    Status add_force(BodyHandle body, Vec3 force) noexcept override {
        return inner->add_force(body, force);
    }
    Status add_torque(BodyHandle body, Vec3 torque) noexcept override {
        return inner->add_torque(body, torque);
    }
    Status add_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        return inner->add_impulse(body, impulse);
    }
    Status add_impulse_at(BodyHandle body, Vec3 impulse, Vec3 world_point) noexcept override {
        return inner->add_impulse_at(body, impulse, world_point);
    }
    Status add_angular_impulse(BodyHandle body, Vec3 impulse) noexcept override {
        return inner->add_angular_impulse(body, impulse);
    }
    Expected<ConstraintHandle, Error> create_constraint(
        WorldHandle world, const ConstraintDescription& description) noexcept override {
        return inner->create_constraint(world, description);
    }
    Status destroy_constraint(ConstraintHandle constraint) noexcept override {
        return inner->destroy_constraint(constraint);
    }
    Status set_constraint_enabled(ConstraintHandle constraint, bool enabled) noexcept override {
        return inner->set_constraint_enabled(constraint, enabled);
    }
    Status set_constraint_motor(ConstraintHandle constraint,
                                const MotorSettings& motor) noexcept override {
        return inner->set_constraint_motor(constraint, motor);
    }
    Status set_constraint_orientation_motor(
        ConstraintHandle constraint, const OrientationMotorSettings& motor) noexcept override {
        return inner->set_constraint_orientation_motor(constraint, motor);
    }
    Expected<VehicleHandle, Error> create_vehicle(
        WorldHandle world, const VehicleDescription& description) noexcept override {
        return inner->create_vehicle(world, description);
    }
    Status destroy_vehicle(VehicleHandle vehicle) noexcept override {
        return inner->destroy_vehicle(vehicle);
    }
    Status set_vehicle_input(VehicleHandle vehicle, const VehicleInput& input) noexcept override {
        return inner->set_vehicle_input(vehicle, input);
    }
    Expected<u32, Error> vehicle_wheels(VehicleHandle vehicle,
                                        Span<VehicleWheelState> out) const noexcept override {
        return inner->vehicle_wheels(vehicle, out);
    }
    Expected<RayCastHit, Error> raycast(WorldHandle world, const RayCastInput& input,
                                        const QueryFilter& filter) const noexcept override {
        return inner->raycast(world, input, filter);
    }
    Expected<u32, Error> raycast_all(WorldHandle world, const RayCastInput& input,
                                     const QueryFilter& filter,
                                     Span<RayCastHit> out) const noexcept override {
        return inner->raycast_all(world, input, filter, out);
    }
    Expected<ShapeCastHit, Error> shape_cast(WorldHandle world, const ShapeCastInput& input,
                                             const QueryFilter& filter) const noexcept override {
        return inner->shape_cast(world, input, filter);
    }
    Expected<u32, Error> overlap(WorldHandle world, const OverlapInput& input,
                                 const QueryFilter& filter,
                                 Span<OverlapHit> out) const noexcept override {
        return inner->overlap(world, input, filter, out);
    }
    Expected<u32, Error> overlap_point(WorldHandle world, Vec3 point, const QueryFilter& filter,
                                       Span<OverlapHit> out) const noexcept override {
        return inner->overlap_point(world, point, filter, out);
    }
    Expected<ClosestPoint, Error> closest_point(WorldHandle world, const ClosestPointInput& input,
                                                const QueryFilter& filter) const noexcept override {
        return inner->closest_point(world, input, filter);
    }
    DeterminismPolicy determinism_policy() const noexcept override {
        return inner->determinism_policy();
    }
    Status hash_state(WorldHandle world, determinism::StateHashTree& tree) const noexcept override {
        return inner->hash_state(world, tree);
    }
    Status debug_draw(WorldHandle world, DebugDrawFlags flags,
                      DebugDrawSink& sink) const noexcept override {
        return inner->debug_draw(world, flags, sink);
    }
};

/// The entity-to-body map the embedder's bridge would provide.
class Bodies final : public game_backend::EntityBodies {
public:
    struct Pair {
        CyEntity entity = 0;
        BodyHandle body;
    };
    Pair pairs[16] = {};
    u32 count = 0;

    void add(CyEntity entity, BodyHandle body) noexcept { pairs[count++] = Pair{entity, body}; }

    BodyHandle body_of(CyEntity entity) const noexcept override {
        for (u32 index = 0; index < count; ++index) {
            if (pairs[index].entity == entity) {
                return pairs[index].body;
            }
        }
        return BodyHandle{};
    }
};

struct Scene {
    SteppingServer server;
    WorldHandle world;
    Bodies bodies;
    abi::Host host{system_allocator(MemoryDomain::Scripting)};

    Scene() noexcept {
        const Expected<PhysicsServer*, Error> made = reference::create_server(allocator());
        CY_REQUIRE(made.has_value());
        server.inner = *made;
        CY_REQUIRE(server.initialize().has_value());
        WorldDescription description;
        description.name = Name::intern("game-backend-physics");
        description.body_capacity = 64;
        description.body_pair_capacity = 256;
        description.contact_constraint_capacity = 256;
        const Expected<WorldHandle, Error> created = server.create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
    }

    ~Scene() {
        server.shutdown();
        reference::destroy_server(server.inner, allocator());
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// A static box of half-extent `half` at `position`, on `layer`, owned by `entity`.
    BodyHandle box(CyEntity entity, Vec3 position, f32 half = 0.5F, u8 layer = 0) noexcept {
        ShapeDescription shape_description;
        shape_description.type = ShapeType::Box;
        shape_description.half_extents = Vec3{half, half, half};
        const Expected<ShapeHandle, Error> shape = server.create_shape(shape_description);
        CY_REQUIRE(shape.has_value());
        ColliderDescription collider;
        collider.shape = *shape;
        collider.filter.layer = layer;
        BodyDescription description;
        description.motion = MotionType::Static;
        description.transform = Transform::from_translation(position);
        description.colliders = &collider;
        description.collider_count = 1;
        description.user_data = entity;
        const Expected<BodyHandle, Error> body = server.create_body(world, description);
        CY_REQUIRE(body.has_value());
        bodies.add(entity, *body);
        return *body;
    }

    /// A dynamic box like `box`, with gravity off so it stays where a test put it until pushed.
    BodyHandle dynamic_box(CyEntity entity, Vec3 position, f32 half = 0.5F) noexcept {
        ShapeDescription shape_description;
        shape_description.type = ShapeType::Box;
        shape_description.half_extents = Vec3{half, half, half};
        const Expected<ShapeHandle, Error> shape = server.create_shape(shape_description);
        CY_REQUIRE(shape.has_value());
        ColliderDescription collider;
        collider.shape = *shape;
        BodyDescription description;
        description.motion = MotionType::Dynamic;
        description.gravity_scale = 0.0F;
        description.transform = Transform::from_translation(position);
        description.colliders = &collider;
        description.collider_count = 1;
        description.user_data = entity;
        const Expected<BodyHandle, Error> body = server.create_body(world, description);
        CY_REQUIRE(body.has_value());
        bodies.add(entity, *body);
        return *body;
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

}  // namespace
