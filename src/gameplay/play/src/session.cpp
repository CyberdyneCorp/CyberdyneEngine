// Play mode. See cy/gameplay/play/session.h for the argument, especially the three mechanisms that
// make "stop restores exactly" a measurement rather than a claim.

#include <cy/gameplay/play/session.h>
#include <cy/scene/node.h>
#include <cy/scene/propagation.h>

#include <cstdio>

namespace cy::gameplay {
namespace {

namespace ser = scene::serialization;

/// The type names the editor's `scene.add-body` writes, and the field names inside them.
///
/// **THE GOLDEN NAMES.** `editor/crates/cy-editor-services/src/bodies.rs` writes exactly these, and
/// `editor/crates/cy-editor-services/tests/a_body_is_a_transaction.rs` holds the same strings in
/// Rust. A spelling that drifted on one side would be a body the runtime silently does not
/// simulate — the failure this milestone exists to end, in a new place — so it is pinned from both
/// ends rather than remembered.
constexpr std::string_view kRigidBody = "RigidBody";
constexpr std::string_view kStaticBody = "StaticBody";
constexpr std::string_view kKinematicBody = "KinematicBody";
constexpr std::string_view kCollider = "Collider";

constexpr std::string_view kFieldMass = "mass";
constexpr std::string_view kFieldGravityScale = "gravity_scale";
constexpr std::string_view kFieldShape = "shape";
constexpr std::string_view kFieldExtent = "extent";
constexpr std::string_view kFieldRadius = "radius";
constexpr std::string_view kFieldHeight = "height";

/// The declared type of a given name, or null.
[[nodiscard]] const ser::WorldTypeDecl* type_named(const ser::World& world,
                                                   std::string_view name) noexcept {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) == name) {
            return &declared;
        }
    }
    return nullptr;
}

/// One field of one component, found by the NAME the file gave it.
[[nodiscard]] const ser::WorldValue* field_named(const ser::World& world,
                                                 const ser::WorldTypeDecl& declared,
                                                 const ser::WorldComponent& component,
                                                 std::string_view name) noexcept {
    for (const ser::WorldFieldDecl& field : declared.fields()) {
        if (world.text(field.name) != name) {
            continue;
        }
        if (const ser::WorldField* held = component.find(field.file_field); held != nullptr) {
            return &held->value;
        }
        return nullptr;
    }
    return nullptr;
}

[[nodiscard]] f32 float_of(const ser::WorldValue* value, f32 fallback) noexcept {
    if (value == nullptr) {
        return fallback;
    }
    switch (value->kind) {
        case ser::WorldValueKind::Float:
            return value->lanes[0];
        case ser::WorldValueKind::Double:
            return static_cast<f32>(value->real);
        case ser::WorldValueKind::Int:
            return static_cast<f32>(value->integer);
        default:
            return fallback;
    }
}

[[nodiscard]] Vec3 vec3_of(const ser::WorldValue* value, Vec3 fallback) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Vec3) {
        return fallback;
    }
    return Vec3{value->lanes[0], value->lanes[1], value->lanes[2]};
}

[[nodiscard]] std::string_view text_of(const ser::World& world, const ser::WorldValue* value,
                                       std::string_view fallback) noexcept {
    if (value == nullptr || value->kind != ser::WorldValueKind::Text) {
        return fallback;
    }
    const Span<const u8> bytes = world.blob(*value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The shape a `Collider` component's `shape` field names.
[[nodiscard]] physics::ShapeDescription shape_of(const ser::World& world,
                                                 const ser::WorldTypeDecl& declared,
                                                 const ser::WorldComponent& component) noexcept {
    physics::ShapeDescription shape;
    const std::string_view kind =
        text_of(world, field_named(world, declared, component, kFieldShape), "box");
    const f32 radius = float_of(field_named(world, declared, component, kFieldRadius), 0.5F);
    const f32 height = float_of(field_named(world, declared, component, kFieldHeight), 1.0F);
    const Vec3 extent =
        vec3_of(field_named(world, declared, component, kFieldExtent), Vec3{0.5F, 0.5F, 0.5F});

    if (kind == "sphere") {
        shape.type = physics::ShapeType::Sphere;
        shape.radius = radius;
    } else if (kind == "capsule") {
        shape.type = physics::ShapeType::Capsule;
        shape.radius = radius;
        // The authored `height` is the TOTAL height, which is the number an artist has; the shape's
        // `half_height` is the cylindrical section alone. The two differ by exactly one radius and
        // the wrong one produces a character that sinks into the floor — shapes.h says so, and this
        // is the one place the conversion happens.
        shape.half_height = (height * 0.5F) - radius;
    } else if (kind == "cylinder") {
        shape.type = physics::ShapeType::Cylinder;
        shape.radius = radius;
        shape.half_height = height * 0.5F;
    } else {
        shape.type = physics::ShapeType::Box;
        shape.half_extents = extent;
    }
    return shape;
}

}  // namespace

const char* play_state_name(PlayState state) noexcept {
    switch (state) {
        case PlayState::Editing:
            return "editing";
        case PlayState::Playing:
            return "playing";
        case PlayState::Paused:
            return "paused";
    }
    return "unknown";
}

Expected<PlayState, Error> play_state_of(std::string_view name) noexcept {
    if (name == "editing") {
        return PlayState::Editing;
    }
    if (name == "playing") {
        return PlayState::Playing;
    }
    if (name == "paused") {
        return PlayState::Paused;
    }
    return make_unexpected(
        fail(ErrorCode::InvalidArgument, "play: the states are editing, playing and paused")
            .error());
}

PlaySession::PlaySession(Allocator& allocator, ser::World& authored) noexcept
    : authored_(&authored),
      allocator_(&allocator),
      simulated_(allocator),
      snapshot_(allocator),
      rewritten_(allocator) {}

PlaySession::~PlaySession() {
    // Not `stop()`: a destructor that wrote into the authored world would put a restore on a path
    // the caller did not ask for, and the world may already be gone. What the destructor owes is
    // the simulation's own teardown, in the order task 4.4 is about.
    release();
}

// --- Entering ------------------------------------------------------------------------------------

Status PlaySession::enter(const PlayConfiguration& configuration) noexcept {
    if (state_ != PlayState::Editing) {
        return fail(ErrorCode::AlreadyExists, "play: a session is already running");
    }
    if (configuration.physics == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "play: a session needs a physics server; which backend is the host's decision");
    }
    report_ = PlayReport{};
    schema_ = configuration.schema;

    if (Status built = build(configuration); !built) {
        // A session that failed halfway is residue of exactly the kind task 5.2 is about.
        release();
        simulated_.clear();
        snapshot_.clear();
        return built;
    }
    state_ = PlayState::Playing;
    return ok();
}

Status PlaySession::build(const PlayConfiguration& configuration) noexcept {
    // 1. THE SNAPSHOT, FIRST AND BEFORE ANYTHING ELSE TOUCHES THE WORLD.
    snapshot_.clear();
    if (Status written = ser::write_world(*authored_, snapshot_); !written) {
        return written;
    }
    report_.restored_length_before = snapshot_.size();

    // 2. The simulation's own world and the scene over it.
    Expected<UniquePtr<ecs::World>, Error> world =
        make_unique<ecs::World>(*allocator_, *allocator_);
    if (!world) {
        return make_unexpected(world.error());
    }
    world_ = std::move(*world);
    if (Status started = world_->initialize(); !started) {
        return started;
    }
    Expected<UniquePtr<scene::SceneTree>, Error> tree =
        make_unique<scene::SceneTree>(*allocator_, *world_);
    if (!tree) {
        return make_unexpected(tree.error());
    }
    tree_ = std::move(*tree);
    if (Status started = tree_->initialize(); !started) {
        return started;
    }
    Expected<UniquePtr<SpawnService>, Error> spawns =
        make_unique<SpawnService>(*allocator_, *tree_, *allocator_);
    if (!spawns) {
        return make_unexpected(spawns.error());
    }
    spawns_ = std::move(*spawns);

    const Expected<physics::PhysicsComponents, Error> components =
        physics::PhysicsComponents::register_all(*world_);
    if (!components) {
        return make_unexpected(components.error());
    }
    components_ = *components;

    // 3. The solver's world. Created here and destroyed by `release()`, because a session owns its
    //    world even though it does not own the server.
    server_ = configuration.physics;
    physics::WorldDescription description;
    description.name = Name::intern("play");
    description.gravity = configuration.gravity;
    description.body_capacity = configuration.body_capacity;
    description.body_pair_capacity = configuration.body_capacity * 4;
    description.contact_constraint_capacity = configuration.body_capacity * 4;
    const Expected<physics::WorldHandle, Error> created = server_->create_world(description);
    if (!created) {
        return make_unexpected(created.error());
    }
    physics_world_ = *created;

    // 4. The clock. `FixedStep`, because a play session's tick is driven by the host's loop and a
    //    realtime accumulator here would step a variable number of times per call.
    determinism::ClockConfig clock;
    clock.rate = configuration.rate;
    clock.mode = determinism::TickMode::FixedStep;
    if (Status configured = clock_.configure(clock); !configured) {
        return configured;
    }

    // 5. One entity per live authored node, then the parents, then the physics.
    simulated_.clear();
    const Array<ser::WorldNode>& nodes = authored_->nodes();
    for (usize index = 0; index < nodes.size(); ++index) {
        if (!nodes[index].live) {
            continue;
        }
        if (Status spawned = spawn_authored(nodes[index], static_cast<u32>(index)); !spawned) {
            return spawned;
        }
    }
    for (const Simulated& entry : simulated_) {
        const u32 parent = nodes[entry.node].parent;
        if (parent == ser::WorldNode::kNoParent) {
            continue;
        }
        const ecs::Entity above = entity_for(nodes[parent].identity);
        if (!above.valid()) {
            continue;
        }
        const scene::Node child = tree_->node(entry.entity);
        if (Status attached = child.set_parent(tree_->node(above)); !attached) {
            return attached;
        }
    }
    for (const Simulated& entry : simulated_) {
        if (Status attached = attach_physics(nodes[entry.node], entry.entity); !attached) {
            return attached;
        }
    }
    report_.entities = static_cast<u32>(simulated_.size());

    // Propagate once before the first step, so a parented body is created at its WORLD placement
    // rather than at its parent-relative one.
    if (Status propagated =
            scene::propagate(*tree_, scene::PropagationPhase::Simulation, nullptr, nullptr);
        !propagated) {
        return propagated;
    }

    // 6. The bridge, and the schedule that runs it in the `Physics` stage.
    Expected<UniquePtr<physics::PhysicsBridge>, Error> bridge = make_unique<physics::PhysicsBridge>(
        *allocator_, *allocator_, *tree_, components_, *server_, physics_world_);
    if (!bridge) {
        return make_unexpected(bridge.error());
    }
    bridge_ = std::move(*bridge);

    Expected<UniquePtr<ecs::Schedule>, Error> schedule =
        make_unique<ecs::Schedule>(*allocator_, *world_);
    if (!schedule) {
        return make_unexpected(schedule.error());
    }
    schedule_ = std::move(*schedule);
    if (const Expected<ecs::SystemId, Error> installed = bridge_->install(*schedule_, clock_);
        !installed) {
        return make_unexpected(installed.error());
    }
    if (Status built = schedule_->build(); !built) {
        return built;
    }

    // The first sync, so `report().bodies` is right before the first tick — which is what a caller
    // reports back to an editor that has just pressed play.
    if (Status synced = bridge_->sync(); !synced) {
        return synced;
    }
    report_.bodies = bridge_->tracked_bodies();
    return ok();
}

Status PlaySession::spawn_authored(const ser::WorldNode& node, u32 index) noexcept {
    Transform placement;
    // A node with no `Transform` the engine resolved sits at the origin, which is where an
    // unplaced node is. Not an error: an authored node is allowed to be a container.
    (void)ser::transform_of(*authored_, node, placement);

    // The name is derived from the ordinal rather than read from the file, because a node's NAME is
    // an authoring component this build does not resolve and the scene tree needs something unique.
    // Nothing downstream reads it: what identifies a simulated node is its authored identity, in
    // `simulated_`.
    char label[32] = {};
    (void)std::snprintf(label, sizeof(label), "node%llu",
                        static_cast<unsigned long long>(node.ordinal));

    SpawnRequest request;
    request.name = Name::intern(label);
    request.placement = placement;
    request.policy = SpawnPolicy::ExactPosition;
    const Expected<SpawnResult, Error> spawned = spawns_->spawn(request, clock_.tick());
    if (!spawned) {
        return make_unexpected(spawned.error());
    }

    Simulated entry;
    entry.identity = node.identity;
    entry.node = index;
    entry.entity = spawned->entity;
    entry.before = placement;
    return simulated_.push_back(entry);
}

Status PlaySession::attach_physics(const ser::WorldNode& node, ecs::Entity entity) noexcept {
    const ser::WorldTypeDecl* collider_type = type_named(*authored_, kCollider);
    if (collider_type != nullptr) {
        if (const ser::WorldComponent* component = node.find(collider_type->file_type);
            component != nullptr) {
            physics::Collider collider;
            collider.shape = shape_of(*authored_, *collider_type, *component);
            if (Status added = world_->add(entity, components_.collider, &collider); !added) {
                return added;
            }
            ++report_.colliders;
        }
    }

    struct Kind {
        std::string_view name;
        u8 motion;  // 0 dynamic, 1 static, 2 kinematic
    };
    const Kind kinds[3] = {{kRigidBody, 0}, {kStaticBody, 1}, {kKinematicBody, 2}};
    for (const Kind& kind : kinds) {
        const ser::WorldTypeDecl* declared = type_named(*authored_, kind.name);
        if (declared == nullptr) {
            continue;
        }
        const ser::WorldComponent* component = node.find(declared->file_type);
        if (component == nullptr) {
            continue;
        }
        if (kind.motion == 0) {
            physics::RigidBody body;
            body.mass = float_of(field_named(*authored_, *declared, *component, kFieldMass), 0.0F);
            body.gravity_scale =
                float_of(field_named(*authored_, *declared, *component, kFieldGravityScale), 1.0F);
            if (Status added = world_->add(entity, components_.rigid_body, &body); !added) {
                return added;
            }
        } else if (kind.motion == 1) {
            const physics::StaticBody body;
            if (Status added = world_->add(entity, components_.static_body, &body); !added) {
                return added;
            }
        } else {
            const physics::KinematicBody body;
            if (Status added = world_->add(entity, components_.kinematic_body, &body); !added) {
                return added;
            }
        }
        // The first body component wins, by the same precedence `PhysicsComponents` applies: an
        // entity with two is an authoring mistake, and answering it deterministically is what keeps
        // the mistake reproducible.
        return ok();
    }
    return ok();
}

// --- Running -------------------------------------------------------------------------------------

Status PlaySession::tick() noexcept {
    if (state_ != PlayState::Playing) {
        // A host's loop calls this every frame; asking it to check first would put the state
        // machine in every caller.
        return ok();
    }
    clock_.advance();
    if (Status ran = schedule_->run_serial(ecs::Stage::Physics); !ran) {
        return ran;
    }
    if (Status reported = bridge_->last_error(); !reported) {
        return reported;
    }
    if (Status propagated =
            scene::propagate(*tree_, scene::PropagationPhase::Simulation, nullptr, nullptr);
        !propagated) {
        return propagated;
    }
    report_.ticks = clock_.tick();
    return publish_placements();
}

Status PlaySession::publish_placements() noexcept {
    // THE ONE WRITE INTO THE AUTHORED WORLD, and mechanism 1 of task 5.2. Everything else in this
    // file reads it.
    for (const Simulated& entry : simulated_) {
        const auto* world_transform =
            world_->get<scene::WorldTransform>(entry.entity, tree_->components().world_transform);
        if (world_transform == nullptr) {
            continue;
        }
        ser::WorldNode& node = authored_->nodes()[entry.node];
        if (!ser::set_transform(*authored_, node, world_transform->value)) {
            continue;
        }
        ++report_.placements_published;
    }
    return ok();
}

Status PlaySession::pause() noexcept {
    if (state_ != PlayState::Playing) {
        return fail(ErrorCode::Unavailable, "play: nothing is playing");
    }
    state_ = PlayState::Paused;
    return ok();
}

Status PlaySession::resume() noexcept {
    if (state_ != PlayState::Paused) {
        return fail(ErrorCode::Unavailable, "play: nothing is paused");
    }
    state_ = PlayState::Playing;
    return ok();
}

// --- Stopping ------------------------------------------------------------------------------------

Status PlaySession::stop() noexcept {
    if (state_ == PlayState::Editing) {
        return ok();  // idempotent: a stop after a stop is not an error worth refusing
    }

    // MECHANISM 2: the value that was there, written back into the same field of the same
    // component.
    report_.restored = 0;
    for (const Simulated& entry : simulated_) {
        ser::WorldNode& node = authored_->nodes()[entry.node];
        if (ser::set_transform(*authored_, node, entry.before)) {
            ++report_.restored;
        }
    }

    // MECHANISM 3: the bytes, compared.
    rewritten_.clear();
    Status written = ser::write_world(*authored_, rewritten_);
    report_.restored_length_after = rewritten_.size();
    report_.restored_exactly = false;
    report_.restored_difference = 0;
    if (written) {
        const usize shortest =
            snapshot_.size() < rewritten_.size() ? snapshot_.size() : rewritten_.size();
        usize offset = 0;
        while (offset < shortest && snapshot_[offset] == rewritten_[offset]) {
            ++offset;
        }
        report_.restored_difference = offset;
        report_.restored_exactly = snapshot_.size() == rewritten_.size() && offset == shortest;
    }

    if (!report_.restored_exactly) {
        // The world is put back WHOLE from the snapshot. A caller that sees `restored_exactly ==
        // false` has found a defect in this file and still has a correct world, which is the right
        // way round: an editor that lost a designer's work because a restore was partial would be
        // worse than one that reported an internal error.
        ser::World fresh(*allocator_);
        const std::string_view text(snapshot_.data(), snapshot_.size());
        if (ser::read_world(text, authored_->path(), fresh)) {
            if (schema_ != nullptr) {
                (void)ser::resolve_against(fresh, *schema_);
            }
            *authored_ = std::move(fresh);
            report_.total_restore = true;
        }
    }

    release();
    simulated_.clear();
    state_ = PlayState::Editing;
    return ok();
}

void PlaySession::release() noexcept {
    // THE ORDER IS TASK 4.4's. The bridge lets its bodies and shapes go before the world they live
    // in is destroyed, and the world before the server the host still owns. The schedule goes first
    // because it holds a system whose `user` pointer is the bridge.
    schedule_.reset();
    if (bridge_) {
        bridge_->teardown();
        bridge_.reset();
    }
    if (server_ != nullptr && !physics_world_.is_null()) {
        (void)server_->destroy_world(physics_world_);
    }
    physics_world_ = physics::WorldHandle();
    server_ = nullptr;
    spawns_.reset();
    tree_.reset();
    world_.reset();
}

ecs::Entity PlaySession::entity_for(u64 identity) const noexcept {
    for (const Simulated& entry : simulated_) {
        if (entry.identity == identity) {
            return entry.entity;
        }
    }
    return ecs::Entity{};
}

}  // namespace cy::gameplay
