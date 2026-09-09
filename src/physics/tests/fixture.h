#pragma once
// A world, a scene over it, a physics server and a bridge between them. Shared by the three suites.
//
// NOTHING HERE IS A MOCK. The world is the ECS's, the tree is the scene layer's, the server is the
// reference backend and the bridge is the thing under test — which is what makes these cases claims
// about the engine rather than about a fixture. The reference backend is the default because it is
// the one that exists in every configuration (`-D CY_PHYSICS=OFF` included); the teardown suite
// runs against Jolt as well, where this build has it, because M5.5's defect was Jolt's.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/physics/bridge.h>
#include <cy/scene/propagation.h>
#include <cy/scene/tree.h>
#include <cy/servers/physics/reference/server.h>

#include <utility>

namespace cy::physics::test {

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// The reference backend, initialised, with one world in it. Destroyed in the order the backend
/// requires: the world, then the shutdown, then the server object.
class Backend {
public:
    Backend() noexcept {
        const Expected<PhysicsServer*, Error> made = reference::create_server(allocator());
        if (!made) {
            return;
        }
        server_ = *made;
        if (!server_->initialize()) {
            return;
        }
        WorldDescription description;
        description.name = Name::intern("physics-bridge-test");
        description.body_capacity = 512;
        description.body_pair_capacity = 2048;
        description.contact_constraint_capacity = 2048;
        const Expected<WorldHandle, Error> created = server_->create_world(description);
        if (!created) {
            return;
        }
        world_ = *created;
        ready_ = true;
    }

    ~Backend() { release(); }

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    void release() noexcept {
        if (server_ == nullptr) {
            return;
        }
        if (!world_.is_null()) {
            (void)server_->destroy_world(world_);
            world_ = WorldHandle();
        }
        server_->shutdown();
        reference::destroy_server(server_, allocator());
        server_ = nullptr;
        ready_ = false;
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] PhysicsServer& server() const noexcept { return *server_; }
    [[nodiscard]] WorldHandle world() const noexcept { return world_; }

private:
    PhysicsServer* server_ = nullptr;
    WorldHandle world_;
    bool ready_ = false;
};

/// The ECS world, the scene tree over it, the registered components and the bridge.
struct Fixture {
    Fixture() noexcept : world(allocator()), tree(world) {
        started =
            world.initialize().has_value() && tree.initialize().has_value() && backend.ready();
        if (!started) {
            return;
        }
        const Expected<PhysicsComponents, Error> registered =
            PhysicsComponents::register_all(world);
        started = registered.has_value();
        if (!started) {
            return;
        }
        components = *registered;
        determinism::ClockConfig config;
        config.mode = determinism::TickMode::FixedStep;
        started = clock.configure(config).has_value();
        if (!started) {
            return;
        }
        Expected<UniquePtr<PhysicsBridge>, Error> made = make_unique<PhysicsBridge>(
            allocator(), allocator(), tree, components, backend.server(), backend.world());
        started = made.has_value();
        if (started) {
            bridge = std::move(*made);
        }
    }

    /// A node with a placement, so the bridge has somewhere to publish to. Every entity a body is
    /// put on in these suites is a real scene node, because that is what one is in a `.cyworld`.
    [[nodiscard]] ecs::Entity node(const char* name, Vec3 position) noexcept {
        Expected<scene::Node, Error> made =
            tree.create_node(Name::intern(name), tree.root(), Name());
        if (!made) {
            return ecs::Entity{};
        }
        const ecs::Entity entity = made->entity();
        scene::LocalTransform local;
        local.value = Transform::from_translation(position);
        if (!world.set(entity, tree.components().local_transform, local)) {
            return ecs::Entity{};
        }
        if (!scene::mark_transform_changed(tree, entity)) {
            return ecs::Entity{};
        }
        return entity;
    }

    /// A unit box collider on `entity`.
    [[nodiscard]] bool box(ecs::Entity entity, Vec3 half_extents) noexcept {
        Collider collider;
        collider.shape.type = ShapeType::Box;
        collider.shape.half_extents = half_extents;
        return world.add(entity, components.collider, &collider).has_value();
    }

    [[nodiscard]] bool dynamic(ecs::Entity entity) noexcept {
        RigidBody body;
        body.allow_sleeping = false;  // a test that fell asleep would measure the sleep threshold
        return world.add(entity, components.rigid_body, &body).has_value();
    }

    [[nodiscard]] bool immovable(ecs::Entity entity) noexcept {
        StaticBody body;
        return world.add(entity, components.static_body, &body).has_value();
    }

    [[nodiscard]] Vec3 position_of(ecs::Entity entity) const noexcept {
        const auto* local =
            world.get<scene::LocalTransform>(entity, tree.components().local_transform);
        return local == nullptr ? Vec3{} : local->value.translation;
    }

    /// Advance the clock and the bridge `ticks` times, which is what a host's fixed step does.
    [[nodiscard]] Status run(u32 ticks) noexcept {
        for (u32 tick = 0; tick < ticks; ++tick) {
            clock.advance();
            if (Status advanced = bridge->advance(clock); !advanced) {
                return advanced;
            }
        }
        return ok();
    }

    ecs::World world;
    scene::SceneTree tree;
    Backend backend;
    PhysicsComponents components;
    UniquePtr<PhysicsBridge> bridge;
    determinism::SimulationClock clock;
    bool started = false;
};

}  // namespace cy::physics::test
