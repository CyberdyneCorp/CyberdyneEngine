#pragma once
// The physics ECS bridge: components become bodies, the stepper runs in the `Physics` stage, and
// what the solver produced is written back to `cy::scene::LocalTransform`. M8.a tasks 4.1 and 4.4.
//
// `physics` — "Fixed-step integration": physics "SHALL step exactly once per simulation tick at the
// fixed timestep, within the `Physics` stage, and SHALL never step during a variable-rate frame.
// Transforms written by physics SHALL be published to `LocalTransform`/`WorldTransform` after the
// step".
//
// ================================================================================================
// THIS MODULE IS samples/04-character's HOST, MOVED — NOT REDESIGNED
// ================================================================================================
//
// design.md §3: "`samples/04-character` does that work in its own host, in C++, which is why its
// `game.cpp` is larger than its Swift. That sample is the specification of what the bridge must do,
// written out longhand — read it before designing the module, because the behaviour is already
// settled and only its home is not."
//
// It was read. What that host does, in `build_level()` and in step 7 of its tick, is exactly four
// things, and this class does the same four in the same order:
//
//   1. walk the entities that carry a body component and a placement;
//   2. `create_shape` per collider, then `create_body` with `user_data = entity.bits()`;
//   3. one `step` per simulation tick, and one only;
//   4. put the result back where the rest of the engine reads a placement.
//
// The four differences, each of which is the sample being a sample:
//
//   * the sample reads its shapes out of a Swift contract's components and this reads them out of
//     `cy::physics::Collider`, which is the component `physics` specifies;
//   * the sample calls `PhysicsServer::step` directly and this drives `PhysicsStepper`, so the
//     interpolation pair `physics` requires exists and the "exactly once per tick" property is the
//     stepper's rather than the caller's discipline;
//   * the sample never destroys a body until it shuts down, because a level does not change. Play
//     mode does: this class has a removal sweep and a teardown, and `test_teardown.cpp` is about
//     precisely that;
//   * the sample publishes into its own report and this publishes into `LocalTransform`, which is
//     the layer-4 half `cy/servers/physics/stepper.h` inverted the publication for.
//
// ================================================================================================
// WHAT THIS DOES NOT DO, DELIBERATELY
// ================================================================================================
//
// It does not own the backend: it is handed a `PhysicsServer&` and a `WorldHandle` and creates
// neither. `samples/04-character` picks Jolt or the reference backend from a command-line flag and
// a game host will pick from a project setting; a bridge that picked would be layer 4 deciding
// policy, and `-D CY_PHYSICS=OFF` would then change what this module *is* rather than which backend
// it was given.
//
// It does not create the `CharacterBody`'s controller. `cy::physics::CharacterController` holds a
// pointer to the server and is neither trivially copyable nor safe in chunk storage — the component
// is a description plus a handle, and who owns the controller object is a gameplay question this
// module cannot answer. `character_body` is registered, and reported as not yet created, rather
// than silently ignored.
//
// It does not map `Joint`. Neither backend maps constraints (`Capabilities::constraints == false`),
// so a bridge that created them would fail at every world with a diagnostic about a component the
// author was entitled to add. Joints are counted in `BridgeStatistics::joints_deferred` so the
// number is visible rather than absent.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/physics/components.h>
#include <cy/scene/components.h>
#include <cy/scene/propagation.h>
#include <cy/scene/tree.h>
#include <cy/servers/physics/server.h>
#include <cy/servers/physics/stepper.h>

namespace cy::physics {

/// What the bridge has done, for a test, a panel and a report.
struct BridgeStatistics {
    /// Bodies created from components, over the bridge's life.
    u64 bodies_created = 0;
    /// Bodies destroyed because their component or their entity went away.
    u64 bodies_destroyed = 0;
    /// Shapes created from `Collider` and `Trigger` descriptions.
    u64 shapes_created = 0;
    /// `LocalTransform` writes the stepper's publication produced.
    u64 transforms_written = 0;
    /// Publications naming an entity the world no longer has. Should be zero; it is counted rather
    /// than asserted, because it is the number that would move if the removal sweep were wrong.
    u64 orphan_publications = 0;
    /// Bodies the server refused, with the reason in `last_error()`. **One entity's refusal does
    /// not fail the world**: an author who adds a `RigidBody` and has not yet added a `Collider`
    /// has a dynamic body with no volume, which `validate()` rightly refuses — and an editor that
    /// stopped simulating everything else because of it would be unusable. The count is the visible
    /// number, and it is what a test asserts on.
    u64 bodies_refused = 0;
    /// `LocalTransform` writes that also marked the node's transform dirty, so propagation derives
    /// `WorldTransform` from what physics produced. Lower than `transforms_written` exactly when a
    /// body's entity is not a scene node.
    u64 transforms_marked = 0;
    /// `Joint` components seen and not created. See the header note.
    u64 joints_deferred = 0;
    /// `CharacterBody` components seen and not created. See the header note.
    u64 characters_deferred = 0;
    /// Steps run. A test asserts this against the clock's tick count, which is how "exactly once
    /// per simulation tick" is checked rather than assumed.
    u64 steps = 0;
};

/// One ECS world's physics: the components, the bodies behind them, and the step.
///
/// Not thread-safe, and not meant to be: it runs in the `Physics` stage, on the simulation thread,
/// at the quiesced commit boundary — the same statement `PhysicsStepper` makes about itself.
class PhysicsBridge final : public TransformSink {
public:
    /// `tree` is the scene over the ECS world; `physics_world` is the solver's, and the caller owns
    /// it, the tree and `server`. The bridge outliving any of them is the defect
    /// `test_teardown.cpp` is about, so `teardown()` is idempotent and the destructor calls it.
    ///
    /// **THE SCENE TREE RATHER THAN JUST ITS COMPONENT IDS**, which is what
    /// `cy::rendering::SnapshotExtractor` takes. Because writing `LocalTransform` is only half of
    /// publishing a placement: the other half is telling the scene layer that an authored transform
    /// changed, so `propagate()` derives `WorldTransform` from it — and that is
    /// `scene::mark_transform_changed`, which takes a tree. A bridge that wrote the component and
    /// not the dirty bit would move a body whose world placement never changed, which is a stale
    /// render and a stale query in the same tick.
    PhysicsBridge(Allocator& allocator, scene::SceneTree& tree, const PhysicsComponents& components,
                  PhysicsServer& server, WorldHandle physics_world) noexcept;

    ~PhysicsBridge() override;

    PhysicsBridge(const PhysicsBridge&) = delete;
    PhysicsBridge& operator=(const PhysicsBridge&) = delete;
    PhysicsBridge(PhysicsBridge&&) = delete;
    PhysicsBridge& operator=(PhysicsBridge&&) = delete;

    /// Create a body for every entity that has one authored and does not have one yet, and destroy
    /// the body of every entity that has lost its component or its life.
    ///
    /// Idempotent and cheap when nothing changed: it walks the tracked set and the authored set,
    /// and touches the server only where the two disagree.
    [[nodiscard]] Status sync() noexcept;

    /// ONE fixed step, at `clock`'s rate, for `clock`'s tick, publishing into `LocalTransform`.
    ///
    /// The clock is `const` for the reason `PhysicsStepper::tick` gives: advancing it is the
    /// runtime's, and a stepper that advanced it could step twice for one tick.
    [[nodiscard]] Status step(const determinism::SimulationClock& clock) noexcept;

    /// `sync()` then `step()`. What the `Physics` stage's system runs, and what a host that has no
    /// schedule calls directly.
    [[nodiscard]] Status advance(const determinism::SimulationClock& clock) noexcept;

    /// Register the bridge as a system in `Stage::Physics`.
    ///
    /// This is the requirement's "within the `Physics` stage" made structural rather than stated:
    /// the body is registered into that stage and no other, and `Schedule::run(Stage::Frame, …)`
    /// therefore cannot step physics however wrong the caller's loop is.
    ///
    /// `clock` must outlive the schedule. The system body cannot be handed one — `SystemContext`
    /// carries a world, a command buffer and the job context, and `simulation-and-determinism`
    /// would have something to say about a fifth member that was a clock — so the bridge holds the
    /// pointer and the stage's system reads it back.
    [[nodiscard]] Expected<ecs::SystemId, Error> install(
        ecs::Schedule& schedule, const determinism::SimulationClock& clock) noexcept;

    /// What the last stage run reported, since a system body cannot return a `Status`.
    ///
    /// Cleared by the next successful `advance()`. A host that runs the stage and never reads this
    /// would miss a body that failed to be created, which is why the sample's own loop checks it.
    [[nodiscard]] const Status& last_error() const noexcept { return last_error_; }

    /// Destroy every body and shape this bridge created, and forget them. Idempotent.
    ///
    /// **THE ORDER IS THE WHOLE POINT AND IT IS M5.5's DEFECT.** That milestone's gate found Jolt
    /// destroying its job free list underneath a worker still releasing a job — one run in forty,
    /// as a fault with no physics call on the stack. The shape is a teardown that races something
    /// still running. So: the bodies go first, then the shapes, then the tracking, and the caller
    /// destroys the physics world and the server after this returns. A bridge that let its
    /// destructor run after the server's would call `destroy_body` on freed memory, which is why
    /// this is a named method a host calls at a moment it chose rather than only a destructor.
    void teardown() noexcept;

    [[nodiscard]] const BridgeStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] u32 tracked_bodies() const noexcept { return static_cast<u32>(tracked_.size()); }
    [[nodiscard]] PhysicsStepper& stepper() noexcept { return stepper_; }
    [[nodiscard]] const PhysicsStepper& stepper() const noexcept { return stepper_; }

    /// The body an entity's components produced, or a null handle.
    [[nodiscard]] BodyHandle body_of(ecs::Entity entity) const noexcept;

    // --- TransformSink -------------------------------------------------------------------------

    /// Write one stepped body's transform into its entity's `LocalTransform`.
    ///
    /// `teleported` is carried by the stepper's interpolation record rather than acted on here: a
    /// teleport suppresses *interpolation*, which is a presentation decision, and the authored
    /// placement is the same value either way.
    void publish(BodyHandle body, UserData user_data, const Transform& transform,
                 bool teleported) noexcept override;

private:
    /// One entity's physics, as this bridge knows it.
    struct Tracked {
        ecs::Entity entity;
        BodyHandle body;
        /// The component that produced the body, so the sweep can notice it being removed. A body
        /// whose entity swapped `RigidBody` for `StaticBody` is destroyed and recreated, which is
        /// what the solver requires and what the author asked for.
        ComponentTypeId source = kInvalidComponent;
        /// The shapes created for this entity's colliders, destroyed with it.
        u32 first_shape = 0;
        u32 shape_count = 0;
    };

    [[nodiscard]] Status create_for(ecs::Entity entity, ComponentTypeId source) noexcept;
    [[nodiscard]] Status collect_colliders(ecs::Entity entity, Array<ColliderDescription>& out,
                                           u32& first_shape) noexcept;
    [[nodiscard]] Status sweep_removed() noexcept;
    void release(Tracked& tracked) noexcept;

    /// One entity waiting for a body, and the component that asked for it.
    struct Pending {
        ecs::Entity entity;
        ComponentTypeId source = kInvalidComponent;
    };

    /// The placement a body is created at: the entity's world placement when propagation has
    /// produced one, and its authored placement otherwise.
    [[nodiscard]] Transform placement_of(ecs::Entity entity) const noexcept;

    ecs::World* world_;
    scene::SceneTree* tree_;
    scene::SceneComponents scene_;
    PhysicsComponents components_;
    PhysicsServer* server_;
    WorldHandle physics_world_;
    PhysicsStepper stepper_;
    const determinism::SimulationClock* clock_ = nullptr;

    /// entity bits -> index into `tracked_`.
    HashMap<u64, u32> index_;
    Array<Tracked> tracked_;
    /// Every shape the bridge created, in creation order. A `Tracked` names a range of it.
    Array<ShapeHandle> shapes_;
    /// Scratch for the sweep, a member so a tick allocates nothing.
    Array<u32> doomed_;
    Array<ColliderDescription> colliders_;
    /// The entities `sync()` found with a body component and no body yet, collected before anything
    /// is created. Creating inside the query's own walk would write components under the iteration
    /// that produced them.
    Array<Pending> pending_;

    BridgeStatistics statistics_;
    Status last_error_ = ok();
    bool torn_down_ = false;
};

}  // namespace cy::physics
