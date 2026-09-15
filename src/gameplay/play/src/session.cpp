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

    // THE REFUSAL, AND IT IS THE FIRST THING THAT HAPPENS. M11.b task 0.5:
    // *"A mode that is selected and not implemented must refuse by name. A `RemoteDevice` request
    // that quietly runs `InEditor` is a green test over a feature that does not exist."*
    //
    // Before the snapshot, before the world, before the solver — a session that built a world and
    // then discovered it had no transport would have to tear one down, and a tear-down is exactly
    // the path along which a fallback gets written. The message is the availability's own `reason`,
    // which names the mode, and the `due` rung rides in `system_code`'s place as part of the
    // message rather than being dropped: see mode.cpp for why both are string literals.
    const PlayModeAvailability availability =
        availability_of(configuration.mode, configuration.support);
    if (!availability.available) {
        return fail(ErrorCode::Unavailable, availability.reason);
    }

    // AND A SECOND REFUSAL, ABOUT A DIFFERENT QUESTION. `availability_of` answers the editor's —
    // *may I offer this mode* — and the answer for `SeparateProcess` is yes whenever this build can
    // launch a runtime host. This answers the session's: *is this process the one that mode's
    // runtime lives in*.
    //
    // A session is the runtime world of the process it is in; it does not launch anything and
    // cannot become a second process by being told it is one. So a `PlaySession` built in the
    // process that did the launching, and labelled `SeparateProcess`, is the in-editor world under
    // another name — which is exactly what M11.b shipped, and exactly what let a suite "drive three
    // modes" by building three sessions in one process and comparing them.
    //
    // `cy::gameplay::ProcessPlayDriver` is the editor's side of the mode and the launched
    // `cy_play_runtime_host` is where its session lives, with `hosted_runtime_process` set.
    if (configuration.mode == PlayMode::SeparateProcess &&
        !configuration.support.hosted_runtime_process) {
        return fail(ErrorCode::Unavailable,
                    "separate-process: a play session in the process that did the launching is the "
                    "in-editor world under another name — drive this mode through "
                    "cy::gameplay::ProcessPlayDriver, whose runtime is the launched process");
    }

    report_ = PlayReport{};
    report_.mode = configuration.mode;
    configuration_ = configuration;
    mode_ = configuration.mode;
    frame_rate_ = configuration.frame_rate;
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
    return advance_one();
}

Status PlaySession::advance_one() noexcept {
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

// --- What a live edit needs ----------------------------------------------------------------------
//
// See session.h: these two are exposed so that `cy::gameplay::live` does not have to reimplement
// "build this node's physics from the authored world" and then disagree with this file about it.

PlaySession::Simulated* PlaySession::simulated_for(u64 identity) noexcept {
    for (Simulated& entry : simulated_) {
        if (entry.identity == identity) {
            return &entry;
        }
    }
    return nullptr;
}

Status PlaySession::detach_physics(ecs::Entity entity) noexcept {
    // Removed in the reverse of the order they were added, and a component the entity does not have
    // is not an error: a node may carry a collider and no body, or neither.
    //
    // `report_.colliders` is decremented HERE rather than adjusted by the caller, because
    // `attach_physics` is what increments it and the two belong together — a counter maintained at
    // one end and corrected at the other is how a report drifts.
    if (world_->has(entity, components_.collider) && report_.colliders > 0) {
        --report_.colliders;
    }
    const ecs::ComponentTypeId ids[4] = {components_.rigid_body, components_.static_body,
                                         components_.kinematic_body, components_.collider};
    for (const ecs::ComponentTypeId id : ids) {
        if (!world_->has(entity, id)) {
            continue;
        }
        if (Status removed = world_->remove(entity, id); !removed) {
            return removed;
        }
    }
    return ok();
}

Status PlaySession::reinitialize_physics(u64 identity) noexcept {
    if (state_ == PlayState::Editing || !world_) {
        return fail(ErrorCode::Unavailable,
                    "play: a component can only be reinitialised inside a session");
    }
    Simulated* entry = simulated_for(identity);
    if (entry == nullptr) {
        // Refused rather than succeeding over nothing: a reinitialise that found no node and
        // returned ok() would report a policy applied that was not.
        return fail(ErrorCode::NotFound,
                    "play: no live node in this session carries that authored identity");
    }
    if (Status detached = detach_physics(entry->entity); !detached) {
        return detached;
    }
    if (Status attached = attach_physics(authored_->nodes()[entry->node], entry->entity);
        !attached) {
        return attached;
    }
    // The bridge notices the components on its next sync; ask for one now so a caller that reads
    // `report().bodies` immediately after sees the world it just asked for.
    if (Status synced = bridge_->sync(); !synced) {
        return synced;
    }
    report_.bodies = bridge_->tracked_bodies();
    return ok();
}

Status PlaySession::recreate_entity(u64 identity) noexcept {
    if (state_ == PlayState::Editing || !world_) {
        return fail(ErrorCode::Unavailable,
                    "play: an entity can only be recreated inside a session");
    }
    Simulated* entry = simulated_for(identity);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound,
                    "play: no live node in this session carries that authored identity");
    }
    const u32 node_index = entry->node;
    const Transform before = entry->before;

    if (Status detached = detach_physics(entry->entity); !detached) {
        return detached;
    }
    if (Status destroyed = tree_->destroy_node(tree_->node(entry->entity)); !destroyed) {
        return destroyed;
    }
    // Drop the stale row and build a new one, rather than patching the entity in place: the entry
    // carries the PRE-PLAY placement that `stop()` restores, and reusing it is how the identity
    // survives an operation that replaces everything else about the node.
    const auto position = static_cast<usize>(entry - simulated_.data());
    simulated_.erase(position);

    if (Status spawned = spawn_authored(authored_->nodes()[node_index], node_index); !spawned) {
        return spawned;
    }
    Simulated& fresh = simulated_[simulated_.size() - 1];
    fresh.before = before;  // the restore target is the pre-play placement, not the current one
    if (Status attached = attach_physics(authored_->nodes()[node_index], fresh.entity); !attached) {
        return attached;
    }
    if (Status propagated =
            scene::propagate(*tree_, scene::PropagationPhase::Simulation, nullptr, nullptr);
        !propagated) {
        return propagated;
    }
    if (Status synced = bridge_->sync(); !synced) {
        return synced;
    }
    report_.bodies = bridge_->tracked_bodies();
    return ok();
}

std::string_view PlaySession::restore_target() const noexcept {
    return {snapshot_.data(), snapshot_.size()};
}

Status PlaySession::set_restore_target(std::string_view text) noexcept {
    if (state_ == PlayState::Editing) {
        return fail(ErrorCode::Unavailable, "play: there is no restore target outside a session");
    }
    if (text.empty()) {
        // An empty restore target would make `stop()` write an empty world over the document,
        // which is the loss this whole mechanism exists to prevent.
        return fail(ErrorCode::InvalidArgument,
                    "play: the restore target is the authored world's text and cannot be empty");
    }
    snapshot_.clear();
    if (Status appended = snapshot_.append(Span<const char>(text.data(), text.size())); !appended) {
        return appended;
    }
    report_.restored_length_before = snapshot_.size();
    return ok();
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

PlayModeCapabilities PlaySession::capabilities() const noexcept {
    return capabilities_of(mode_);
}

u32 PlaySession::ticks_per_frame() const noexcept {
    if (state_ == PlayState::Editing) {
        return 0;
    }
    // ticks per frame = (tick rate) / (frame rate), both exact rationals, so the division is one
    // cross-multiplication and nothing rounds until the end. At 60/1 ticks against 30/1 frames this
    // is 2, and at 60/1 against 60/1 it is 1 — the case everything else in this engine assumes and
    // which is therefore the one a bug here would hide behind.
    const determinism::TickRate ticks = clock_.rate();
    const determinism::TickRate frames = frame_rate_;
    if (frames.numerator == 0 || ticks.denominator == 0) {
        return 1;
    }
    const u64 numerator = static_cast<u64>(ticks.numerator) * frames.denominator;
    const u64 denominator = static_cast<u64>(ticks.denominator) * frames.numerator;
    if (denominator == 0) {
        return 1;
    }
    const u64 per_frame = numerator / denominator;
    // At least one: a frame rate above the tick rate is a legitimate configuration and a step that
    // ran zero ticks would be a button that does nothing.
    return per_frame == 0 ? 1U : static_cast<u32>(per_frame);
}

// --- Stepping ------------------------------------------------------------------------------------
//
// `live-editing`: *"Play mode SHALL support pause, single frame step, and single simulation tick
// step in every mode where the runtime permits."* M11.b's delta makes "where the runtime permits" a
// QUERY rather than something a caller discovers by trying, so both entry points ask
// `capabilities_of` and refuse by name when the answer is no. A capability that was silently
// ignored would be the same defect as a mode that silently fell back.

Status PlaySession::step_tick() noexcept {
    if (state_ != PlayState::Paused) {
        return fail(ErrorCode::Unavailable,
                    "play: a step is only meaningful while paused — a step into a running "
                    "simulation is one extra tick, not a step");
    }
    if (!capabilities_of(mode_).step_tick) {
        return fail(ErrorCode::Unsupported,
                    "play: this mode does not support a single simulation tick step");
    }
    if (Status advanced = advance_one(); !advanced) {
        return advanced;
    }
    ++report_.stepped_ticks;
    return ok();
}

Status PlaySession::step_frame() noexcept {
    if (state_ != PlayState::Paused) {
        return fail(ErrorCode::Unavailable,
                    "play: a step is only meaningful while paused — a step into a running "
                    "simulation is one extra frame, not a step");
    }
    if (!capabilities_of(mode_).step_frame) {
        // The one capability that differs by mode, and it differs by transport: a remote runtime's
        // frames arrive encoded and are not individually addressable. See capabilities_of().
        return fail(ErrorCode::Unsupported, "play: this mode does not support a single frame step");
    }
    const u32 per_frame = ticks_per_frame();
    for (u32 index = 0; index < per_frame; ++index) {
        if (Status advanced = advance_one(); !advanced) {
            return advanced;
        }
        ++report_.stepped_ticks;
    }
    ++report_.stepped_frames;
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
