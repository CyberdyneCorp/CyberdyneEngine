// The live edit compiler. See cy/gameplay/live/compiler.h for the requirement it discharges and for
// why "without a restart" is two tick numbers rather than a boolean.

#include <cy/gameplay/live/compiler.h>
#include <cy/physics/bridge.h>
#include <cy/scene/components.h>
#include <cy/servers/physics/components.h>

#include <cstddef>
#include <cstring>

namespace cy::gameplay::live {
namespace {

namespace ser = scene::serialization;

/// The type names a play session reads out of a `.cyworld`'s own type section.
///
/// **The same golden names `src/gameplay/play/src/session.cpp` pins**, and pinned here for the same
/// reason: `editor/crates/cy-editor-services/src/bodies.rs` writes them, a Rust test holds them,
/// and a spelling that drifted on one side would be a live edit that silently reaches nothing. They
/// are repeated rather than shared because sharing them would put a header of this module on the
/// include path of a module that must not depend on it — and because two lists that disagree fail
/// the tests on both sides rather than neither.
constexpr std::string_view kTransform = "Transform";
constexpr std::string_view kRigidBody = "RigidBody";
constexpr std::string_view kCollider = "Collider";
constexpr std::string_view kWorldType = "World";

constexpr std::string_view kFieldTranslation = "translation";
constexpr std::string_view kFieldRotation = "rotation";
constexpr std::string_view kFieldScale = "scale";
constexpr std::string_view kFieldMass = "mass";
constexpr std::string_view kFieldGravityScale = "gravity_scale";
constexpr std::string_view kFieldShape = "shape";
constexpr std::string_view kFieldExtent = "extent";
constexpr std::string_view kFieldRadius = "radius";
constexpr std::string_view kFieldHeight = "height";
constexpr std::string_view kFieldGravity = "gravity";

/// Byte offsets inside `cy::scene::LocalTransform`, composed rather than written with a nested
/// `offsetof`: `offsetof` on a nested member is conditionally supported, and composing two
/// well-defined ones is not.
constexpr u32 kLocalTransformValue = static_cast<u32>(offsetof(scene::LocalTransform, value));
constexpr u32 kTransformTranslation = static_cast<u32>(offsetof(Transform, translation));
constexpr u32 kTransformRotation = static_cast<u32>(offsetof(Transform, rotation));
constexpr u32 kTransformScale = static_cast<u32>(offsetof(Transform, scale));

/// How many bytes a kind occupies in a live component.
[[nodiscard]] u32 width_of(LiveFieldKind kind) noexcept {
    switch (kind) {
        case LiveFieldKind::F32:
            return sizeof(f32);
        case LiveFieldKind::Vec3:
            return 3 * sizeof(f32);
        case LiveFieldKind::Quat:
            return 4 * sizeof(f32);
        case LiveFieldKind::U32:
            return sizeof(u32);
        case LiveFieldKind::Bool:
            return sizeof(bool);
        case LiveFieldKind::AssetPath:
        case LiveFieldKind::Structural:
            return 0;
    }
    return 0;
}

[[nodiscard]] f32 float_of(const ser::WorldValue& value) noexcept {
    switch (value.kind) {
        case ser::WorldValueKind::Float:
            return value.lanes[0];
        case ser::WorldValueKind::Double:
            return static_cast<f32>(value.real);
        case ser::WorldValueKind::Int:
            return static_cast<f32>(value.integer);
        default:
            return 0.0F;
    }
}

/// The declared type of a given name, or null. The same lookup `session.cpp` makes, over the file's
/// own type section rather than through `engine_type`, because physics' components resolve to no
/// reflected type at all.
[[nodiscard]] ser::WorldTypeDecl* type_named(ser::World& world, std::string_view name) noexcept {
    for (ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) == name) {
            return &declared;
        }
    }
    return nullptr;
}

[[nodiscard]] const ser::WorldFieldDecl* field_named(const ser::World& world,
                                                     const ser::WorldTypeDecl& declared,
                                                     std::string_view name) noexcept {
    for (const ser::WorldFieldDecl& field : declared.fields()) {
        if (world.text(field.name) == name) {
            return &field;
        }
    }
    return nullptr;
}

[[nodiscard]] ser::WorldNode* node_with_identity(ser::World& world, u64 identity) noexcept {
    for (ser::WorldNode& node : world.nodes()) {
        if (node.live && node.identity == identity) {
            return &node;
        }
    }
    return nullptr;
}

/// Write one field's value into one world's document. The same operation is performed on the live
/// document and on the restore target, which is why it is a function of a `World&`.
[[nodiscard]] Status set_field(ser::World& world, u64 identity, std::string_view type,
                               std::string_view field, const ser::WorldValue& value) noexcept {
    ser::WorldNode* node = node_with_identity(world, identity);
    if (node == nullptr) {
        return fail(ErrorCode::NotFound, "live edit: no live node with that identity in the world");
    }
    ser::WorldTypeDecl* declared = type_named(world, type);
    if (declared == nullptr) {
        return fail(ErrorCode::NotFound,
                    "live edit: the world declares no type of that name in its own type section");
    }
    const ser::WorldFieldDecl* field_decl = field_named(world, *declared, field);
    if (field_decl == nullptr) {
        return fail(ErrorCode::NotFound, "live edit: that type declares no field of that name");
    }
    ser::WorldComponent* component = node->find(declared->file_type);
    if (component == nullptr) {
        // A live edit adds no component. Adding one is a structural authoring change and belongs in
        // a transaction, which is a different path with its own undo entry.
        return fail(ErrorCode::NotFound,
                    "live edit: the node carries no component of that type — adding one is a "
                    "transaction, not a live edit");
    }
    if (ser::WorldField* held = component->find(field_decl->file_field); held != nullptr) {
        held->value = value;
        return ok();
    }
    ser::WorldField fresh;
    fresh.file_field = field_decl->file_field;
    fresh.engine_field = field_decl->engine_field;
    fresh.value = value;
    return component->fields().push_back(fresh);
}

/// The shapes of the fields a play session creates. See `play_session_field_shapes`.
///
/// EVERY ONE OF THEM IS STRONGER THAN `Immediate` BY DECLARATION, and that is a finding rather than
/// a choice: `cy::physics::PhysicsBridge` creates a solver body from the node's `WorldTransform`
/// and from the body and collider components AT CREATION, and thereafter *writes* `LocalTransform`
/// back every step (bridge.cpp's own access declaration lists `local_transform` under WRITES and
/// `world_transform` under READS). So an immediate write into any of these is overwritten on the
/// next tick, or never read at all. `declare_engine_policies` says so per field.
constexpr LiveFieldBinding kPlayShapes[] = {
    {kTransform,
     kFieldTranslation,
     ecs::kInvalidComponent,
     kLocalTransformValue + kTransformTranslation,
     LiveFieldKind::Vec3,
     {}},
    {kTransform,
     kFieldRotation,
     ecs::kInvalidComponent,
     kLocalTransformValue + kTransformRotation,
     LiveFieldKind::Quat,
     {}},
    {kTransform,
     kFieldScale,
     ecs::kInvalidComponent,
     kLocalTransformValue + kTransformScale,
     LiveFieldKind::Vec3,
     {}},
    {kRigidBody,
     kFieldMass,
     ecs::kInvalidComponent,
     static_cast<u32>(offsetof(physics::RigidBody, mass)),
     LiveFieldKind::F32,
     {}},
    {kRigidBody,
     kFieldGravityScale,
     ecs::kInvalidComponent,
     static_cast<u32>(offsetof(physics::RigidBody, gravity_scale)),
     LiveFieldKind::F32,
     {}},
    // The four collider fields are STRUCTURAL: the server cooks `shape`, `extent`, `radius` and
    // `height` into a shape handle when the body is created, and the component's own bytes are a
    // description the bridge has already consumed.
    {kCollider, kFieldShape, ecs::kInvalidComponent, 0, LiveFieldKind::Structural, {}},
    {kCollider, kFieldExtent, ecs::kInvalidComponent, 0, LiveFieldKind::Structural, {}},
    {kCollider, kFieldRadius, ecs::kInvalidComponent, 0, LiveFieldKind::Structural, {}},
    {kCollider, kFieldHeight, ecs::kInvalidComponent, 0, LiveFieldKind::Structural, {}},
    // The world's own gravity. No component, because there is no entity it lives on: it is an
    // argument to the solver world the session creates, which is exactly why it restarts.
    {kWorldType, kFieldGravity, ecs::kInvalidComponent, 0, LiveFieldKind::Vec3, {}},
};

}  // namespace

// --- The default host ----------------------------------------------------------------------------

Status LiveEditHost::reinitialize(PlaySession& session, u64 identity,
                                  std::string_view type) noexcept {
    // The components a play session itself creates are the ones it can rebuild. Anything else is a
    // component some other code added to the session's world, and only that code knows how to build
    // it again — so it is refused by name rather than silently skipped.
    if (type == kRigidBody || type == kCollider) {
        return session.reinitialize_physics(identity);
    }
    return fail(ErrorCode::Unsupported,
                "live edit: this host cannot rebuild a component of that type — a project "
                "component needs a LiveEditHost that knows how to build it");
}

Status LiveEditHost::rebind_asset(PlaySession& session, u64 identity, std::string_view type,
                                  std::string_view field, std::string_view path) noexcept {
    (void)session;
    (void)identity;
    (void)type;
    (void)field;
    (void)path;
    // Refused rather than returning success. An engine with no asset system wired to this session
    // cannot reload an asset, and a `ReloadAsset` that quietly did nothing would be a policy that
    // reports applied over a rebind that did not happen.
    return fail(ErrorCode::NotImplemented,
                "live edit: no asset rebinder is installed on this host, so a reload-asset policy "
                "cannot be honoured");
}

// --- The compiler --------------------------------------------------------------------------------

LiveEditCompiler::LiveEditCompiler(Allocator& allocator,
                                   const LiveEditPolicyTable& policies) noexcept
    : policies_(&policies),
      bindings_(allocator),
      names_(allocator),
      captured_(allocator),
      scratch_(allocator) {}

std::string_view LiveEditCompiler::text(u32 offset, u32 length) const noexcept {
    if (static_cast<usize>(offset) + length > names_.size()) {
        return {};
    }
    return {names_.data() + offset, length};
}

const LiveEditCompiler::Bound* LiveEditCompiler::lookup(std::string_view type,
                                                        std::string_view field) const noexcept {
    for (const Bound& bound : bindings_) {
        if (text(bound.type_offset, bound.type_length) == type &&
            text(bound.field_offset, bound.field_length) == field) {
            return &bound;
        }
    }
    return nullptr;
}

Status LiveEditCompiler::bind(const LiveFieldBinding& binding) noexcept {
    if (binding.type.empty() || binding.field.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "live edit: a binding names a component type and a field, and neither may be "
                    "empty");
    }
    for (Bound& bound : bindings_) {
        if (text(bound.type_offset, bound.type_length) == binding.type &&
            text(bound.field_offset, bound.field_length) == binding.field) {
            bound.component = binding.component;
            bound.offset = binding.offset;
            bound.kind = binding.kind;
            bound.nature = binding.nature;
            return ok();
        }
    }

    Bound bound;
    bound.component = binding.component;
    bound.offset = binding.offset;
    bound.kind = binding.kind;
    bound.nature = binding.nature;
    bound.type_offset = static_cast<u32>(names_.size());
    bound.type_length = static_cast<u32>(binding.type.size());
    if (Status appended = names_.append(Span<const char>(binding.type.data(), binding.type.size()));
        !appended) {
        return appended;
    }
    bound.field_offset = static_cast<u32>(names_.size());
    bound.field_length = static_cast<u32>(binding.field.size());
    if (Status appended =
            names_.append(Span<const char>(binding.field.data(), binding.field.size()));
        !appended) {
        return appended;
    }
    return bindings_.push_back(bound);
}

Span<const LiveFieldBinding> play_session_field_shapes() noexcept {
    return {kPlayShapes, sizeof(kPlayShapes) / sizeof(kPlayShapes[0])};
}

Status LiveEditCompiler::bind_play_session(PlaySession& session) noexcept {
    scene::SceneTree* tree = session.tree();
    physics::PhysicsBridge* bridge = session.bridge();
    if (tree == nullptr || bridge == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "live edit: the engine's own fields can only be bound against a session that "
                    "has entered play, because the component identifiers are that world's");
    }
    const scene::SceneComponents& scene_ids = tree->components();
    const physics::PhysicsComponents& physics_ids = bridge->components();

    for (const LiveFieldBinding& shape : play_session_field_shapes()) {
        LiveFieldBinding binding = shape;
        if (shape.type == kTransform) {
            binding.component = scene_ids.local_transform;
        } else if (shape.type == kRigidBody) {
            binding.component = physics_ids.rigid_body;
        } else if (shape.type == kCollider) {
            binding.component = physics_ids.collider;
        }
        // `World`-scoped fields keep `kInvalidComponent`: there is no entity a world's gravity
        // lives on, which is the whole reason its policy is a restart.
        if (Status bound = bind(binding); !bound) {
            return bound;
        }
    }
    return ok();
}

const LiveFieldBinding* LiveEditCompiler::binding_for(std::string_view type,
                                                      std::string_view field) const noexcept {
    // Returned through the shapes table rather than as a pointer into `bindings_`, which holds
    // offsets rather than views. The caller wants the SHAPE of the field, and there is exactly one
    // shape per (type, field) in the engine's own table.
    if (lookup(type, field) == nullptr) {
        return nullptr;
    }
    for (const LiveFieldBinding& shape : play_session_field_shapes()) {
        if (shape.type == type && shape.field == field) {
            return &shape;
        }
    }
    return nullptr;
}

LiveEditDecision LiveEditCompiler::announce(const AuthoringChange& change) const noexcept {
    const Bound* bound = lookup(change.type, change.field);
    if (bound == nullptr) {
        LiveEditDecision decision;
        decision.policy = LiveEditPolicy::Unsupported;
        decision.declared = false;
        decision.reason =
            "no live binding for this field: nothing in this build knows where it lives in a "
            "running world, so the change applies on the next run";
        return decision;
    }
    return policies_->policy_for(change.type, change.field, bound->nature);
}

Status LiveEditCompiler::record_authoring(PlaySession& session, ser::World& authored,
                                          const AuthoringChange& change) noexcept {
    if (change.scope == ChangeScope::World) {
        // A world-scoped property is not in the document — it is an argument to the session — so
        // there is nothing to record and nothing to restore.
        return ok();
    }
    if (change.value.kind == ser::WorldValueKind::Text ||
        change.value.kind == ser::WorldValueKind::Bytes) {
        // A `Text` or `Bytes` value is a SLICE of one world's blob pool. Copying the slice into
        // another world would point at whatever happens to be at that offset there, so it is
        // refused by name rather than producing a value that reads as something else.
        return fail(ErrorCode::Unsupported,
                    "live edit: a text or bytes value is a slice of one world's blob pool and "
                    "cannot be copied between worlds; intern it into the target world first");
    }

    if (Status written =
            set_field(authored, change.node_identity, change.type, change.field, change.value);
        !written) {
        return written;
    }

    // AND INTO THE RESTORE TARGET. See `PlaySession::restore_target`: `stop()` puts the document
    // back to the bytes the target holds, so an authoring edit that reached only the live document
    // would be silently discarded the moment the designer left play.
    const std::string_view target = session.restore_target();
    if (target.empty()) {
        return fail(ErrorCode::Internal, "live edit: the session has no restore target");
    }
    ser::World restore(authored.allocator());
    if (const Expected<ser::WorldReadReport, Error> read =
            ser::read_world(target, authored.path(), restore);
        !read) {
        return make_unexpected(read.error());
    }
    if (Status written =
            set_field(restore, change.node_identity, change.type, change.field, change.value);
        !written) {
        return written;
    }
    scratch_.clear();
    if (Status out = ser::write_world(restore, scratch_); !out) {
        return out;
    }
    return session.set_restore_target(std::string_view(scratch_.data(), scratch_.size()));
}
/// Write one value into a live component. `Immediate`'s whole body.
Status LiveEditCompiler::write_live(PlaySession& session, const Bound& bound,
                                    const AuthoringChange& change) noexcept {
    if (bound.kind == LiveFieldKind::Structural || bound.kind == LiveFieldKind::AssetPath) {
        // THE REFUSAL THAT MAKES `Structural` WORTH HAVING. A field whose authored value the engine
        // cooks into something else has no in-place write, so a policy that asked for one is a
        // declaration mistake and is reported as one rather than writing bytes that mean nothing.
        return fail(ErrorCode::Unsupported,
                    "live edit: this field has no in-place representation in a running world, so "
                    "an immediate policy cannot be honoured — declare it reinitialize-component, "
                    "recreate-entity or reload-asset");
    }
    if (bound.component == ecs::kInvalidComponent) {
        return fail(ErrorCode::Unsupported,
                    "live edit: this field lives on no component, so there is nothing running to "
                    "write it into");
    }
    ecs::World* world = session.world();
    if (world == nullptr) {
        return fail(ErrorCode::Unavailable, "live edit: the session is not running");
    }
    const ecs::Entity entity = session.entity_for(change.node_identity);
    if (!entity.valid()) {
        return fail(ErrorCode::NotFound,
                    "live edit: no entity in this session is simulating that authored identity");
    }
    // `get_mut` rather than `get`: it stamps the chunk's version for this component, which is what
    // makes a downstream change filter fire on a live edit exactly as it does on a simulated write.
    auto* bytes = static_cast<u8*>(world->get_mut(entity, bound.component));
    if (bytes == nullptr) {
        return fail(ErrorCode::NotFound,
                    "live edit: the entity does not carry the component this field lives in");
    }

    u8* target = bytes + bound.offset;
    switch (bound.kind) {
        case LiveFieldKind::F32: {
            const f32 value = float_of(change.value);
            std::memcpy(target, &value, sizeof(value));
            return ok();
        }
        case LiveFieldKind::Vec3: {
            const f32 lanes[3] = {change.value.lanes[0], change.value.lanes[1],
                                  change.value.lanes[2]};
            std::memcpy(target, lanes, sizeof(lanes));
            return ok();
        }
        case LiveFieldKind::Quat: {
            const f32 lanes[4] = {change.value.lanes[0], change.value.lanes[1],
                                  change.value.lanes[2], change.value.lanes[3]};
            std::memcpy(target, lanes, sizeof(lanes));
            return ok();
        }
        case LiveFieldKind::U32: {
            const auto value = static_cast<u32>(change.value.integer);
            std::memcpy(target, &value, sizeof(value));
            return ok();
        }
        case LiveFieldKind::Bool: {
            const bool value = change.value.integer != 0;
            std::memcpy(target, &value, sizeof(value));
            return ok();
        }
        case LiveFieldKind::AssetPath:
        case LiveFieldKind::Structural:
            break;
    }
    return fail(ErrorCode::Internal, "live edit: unreachable field kind");
}

Status LiveEditCompiler::capture_runtime_state(PlaySession& session, u64 identity) noexcept {
    captured_.clear();
    ecs::World* world = session.world();
    const ecs::Entity entity = session.entity_for(identity);
    if (world == nullptr || !entity.valid()) {
        return ok();  // nothing running to carry; the rebuild reports the loss
    }
    for (usize index = 0; index < bindings_.size(); ++index) {
        const Bound& bound = bindings_[index];
        const bool owned_by_the_simulation =
            bound.nature.persistence == reflect::PersistenceKind::RuntimeState ||
            bound.nature.persistence == reflect::PersistenceKind::PersistentState;
        if (!owned_by_the_simulation || bound.nature.transient) {
            continue;
        }
        const u32 width = width_of(bound.kind);
        if (width == 0 || width > sizeof(Captured::bytes)) {
            continue;
        }
        const auto* bytes = static_cast<const u8*>(world->get(entity, bound.component));
        if (bytes == nullptr) {
            continue;
        }
        Captured captured;
        captured.binding = index;
        captured.length = width;
        std::memcpy(captured.bytes, bytes + bound.offset, width);
        if (Status kept = captured_.push_back(captured); !kept) {
            return kept;
        }
    }
    return ok();
}

Status LiveEditCompiler::restore_runtime_state(PlaySession& session, u64 identity,
                                               LiveEditOutcome& outcome) noexcept {
    ecs::World* world = session.world();
    const ecs::Entity entity = session.entity_for(identity);
    for (const Captured& captured : captured_) {
        const Bound& bound = bindings_[captured.binding];
        u8* bytes = (world == nullptr || !entity.valid())
                        ? nullptr
                        : static_cast<u8*>(world->get_mut(entity, bound.component));
        if (bytes == nullptr) {
            // REPORTED RATHER THAN DROPPED. `live-editing`: *"Where a change cannot preserve
            // runtime state … the loss SHALL be reported and, by configuration, require
            // confirmation rather than occurring silently."* The count is the report; the
            // confirmation is the caller's, which is why this returns ok() and does not decide for
            // it.
            ++outcome.runtime_state_lost;
            continue;
        }
        std::memcpy(bytes + bound.offset, captured.bytes, captured.length);
        ++outcome.runtime_state_preserved;
    }
    captured_.clear();
    return ok();
}

Expected<LiveEditOutcome, Error> LiveEditCompiler::apply(PlaySession& session, ser::World& authored,
                                                         LiveEditHost& host,
                                                         const AuthoringChange& change) noexcept {
    LiveEditOutcome outcome;
    const LiveEditDecision decision = announce(change);
    outcome.policy = decision.policy;
    outcome.declared = decision.declared;
    outcome.reason = decision.reason;
    outcome.tick_before = session.clock().tick();
    outcome.tick_after = outcome.tick_before;

    if (session.state() == PlayState::Editing) {
        return fail(ErrorCode::Unavailable,
                    "live edit: there is no running world to apply a change to");
    }
    if (decision.policy == LiveEditPolicy::Unsupported) {
        // Refused, and the reason is the announcement's own — so a caller that ignored `announce`
        // and called `apply` is told the same thing it would have been told beforehand.
        return fail(ErrorCode::Unsupported, decision.reason);
    }

    // THE AUTHORING HALF FIRST, AND BEFORE ANY POLICY RUNS. A rebuild reads the document, so a
    // document that had not been written yet would rebuild the old value; and a restart restores
    // the target, so a target that had not been written yet would discard the edit.
    if (Status recorded = record_authoring(session, authored, change); !recorded) {
        return make_unexpected(recorded.error());
    }

    Status performed = ok();
    switch (decision.policy) {
        case LiveEditPolicy::Immediate:
            performed = apply_immediate(session, change, outcome);
            break;
        case LiveEditPolicy::ReinitializeComponent:
            performed = apply_reinitialize(session, host, change, outcome);
            break;
        case LiveEditPolicy::RecreateEntity:
            performed = apply_recreate(session, change, outcome);
            break;
        case LiveEditPolicy::ReloadAsset:
            performed = apply_reload_asset(session, authored, host, change, outcome);
            break;
        case LiveEditPolicy::RestartWorld:
            performed = apply_restart(session, change, outcome);
            break;
        case LiveEditPolicy::Unsupported:
            performed = fail(ErrorCode::Unsupported, decision.reason);
            break;
    }
    if (!performed) {
        return make_unexpected(performed.error());
    }

    outcome.applied = true;
    outcome.tick_after = session.clock().tick();
    return outcome;
}

// --- One policy each -----------------------------------------------------------------------------
//
// Split out of `apply` for the reason the header gives: six independent operations that happen to
// share a decision. Each is responsible for its own capture, its own refusal and its own count, and
// none of them decides WHICH policy applies — that is `announce`'s, once, for both callers.

Status LiveEditCompiler::apply_immediate(PlaySession& session, const AuthoringChange& change,
                                         LiveEditOutcome& outcome) noexcept {
    const Bound* bound = lookup(change.type, change.field);
    if (bound == nullptr) {
        return fail(ErrorCode::NotFound, "live edit: no binding for this field");
    }
    if (Status written = write_live(session, *bound, change); !written) {
        return written;
    }
    ++outcome.fields_written;
    return ok();
}

Status LiveEditCompiler::apply_reinitialize(PlaySession& session, LiveEditHost& host,
                                            const AuthoringChange& change,
                                            LiveEditOutcome& outcome) noexcept {
    if (Status captured = capture_runtime_state(session, change.node_identity); !captured) {
        return captured;
    }
    if (Status rebuilt = host.reinitialize(session, change.node_identity, change.type); !rebuilt) {
        captured_.clear();
        return rebuilt;
    }
    if (Status restored = restore_runtime_state(session, change.node_identity, outcome);
        !restored) {
        return restored;
    }
    ++outcome.components_reinitialized;
    return ok();
}

Status LiveEditCompiler::apply_recreate(PlaySession& session, const AuthoringChange& change,
                                        LiveEditOutcome& outcome) noexcept {
    if (Status captured = capture_runtime_state(session, change.node_identity); !captured) {
        return captured;
    }
    if (Status recreated = session.recreate_entity(change.node_identity); !recreated) {
        captured_.clear();
        return recreated;
    }
    if (Status restored = restore_runtime_state(session, change.node_identity, outcome);
        !restored) {
        return restored;
    }
    ++outcome.entities_recreated;
    return ok();
}

Status LiveEditCompiler::apply_reload_asset(PlaySession& session, ser::World& authored,
                                            LiveEditHost& host, const AuthoringChange& change,
                                            LiveEditOutcome& outcome) noexcept {
    const Span<const u8> bytes = authored.blob(change.value);
    const std::string_view path(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (Status rebound =
            host.rebind_asset(session, change.node_identity, change.type, change.field, path);
        !rebound) {
        return rebound;
    }
    ++outcome.assets_rebound;
    return ok();
}

Status LiveEditCompiler::apply_restart(PlaySession& session, const AuthoringChange& change,
                                       LiveEditOutcome& outcome) noexcept {
    // The whole session, ended and begun again. `stop()` restores the document to the restore
    // target — which `record_authoring` has already written this change into — and `enter()` builds
    // a new simulation from it.
    PlayConfiguration configuration = session.configuration();
    if (change.scope == ChangeScope::World && change.field == kFieldGravity) {
        // The one world-scoped property this build knows. A second would be a second case here and
        // a second entry in `kPlayShapes`, which is the shape a reviewer can count.
        configuration.gravity =
            Vec3{change.value.lanes[0], change.value.lanes[1], change.value.lanes[2]};
    }
    if (Status stopped = session.stop(); !stopped) {
        return stopped;
    }
    if (Status entered = session.enter(configuration); !entered) {
        return entered;
    }

    // EVERY BINDING IS DROPPED AND THE ENGINE'S ARE TAKEN AGAIN, because a component identifier is
    // a WORLD's and the world is a new one. Keeping a caller's binding across a restart would leave
    // a stale `ComponentTypeId` pointing at whichever column that number names in the new world — a
    // write into the wrong bytes rather than a diagnosable failure. Dropping it instead makes the
    // next change to that field announce `Unsupported` with "no live binding for this field", which
    // is a caller that has to re-bind rather than a caller that has corrupted a component.
    bindings_.clear();
    names_.clear();
    captured_.clear();
    if (Status rebound = bind_play_session(session); !rebound) {
        return rebound;
    }
    ++outcome.worlds_restarted;
    return ok();
}

// --- The engine's own declarations ---------------------------------------------------------------

Status declare_engine_policies(LiveEditPolicyTable& table) noexcept {
    struct Declaration {
        std::string_view type;
        std::string_view field;
        LiveEditPolicy policy;
    };

    // EVERY ONE OF THESE IS A FACT ABOUT WHAT THE ENGINE DOES WITH THE VALUE, WHICH IS WHY IT IS
    // DECLARED AND NOT DERIVED. All eleven fields are classified `Authoring`, so the derived
    // default for every one of them is `Immediate` — and `Immediate` would be wrong for all eleven,
    // because `cy::physics::PhysicsBridge` consumes each at body creation and thereafter writes
    // `LocalTransform` back every step. A derived policy would apply the edit and the next tick
    // would undo it, which looks exactly like a live edit that works.
    const Declaration declarations[] = {
        // The bridge creates the solver body at the node's `WorldTransform` and writes
        // `LocalTransform` back from the simulation every step. Moving an authored node during play
        // therefore means putting the body somewhere else, which is a recreate.
        {kTransform, kFieldTranslation, LiveEditPolicy::RecreateEntity},
        {kTransform, kFieldRotation, LiveEditPolicy::RecreateEntity},
        {kTransform, kFieldScale, LiveEditPolicy::RecreateEntity},
        // Mass and gravity scale are baked into the body description at creation.
        {kRigidBody, kFieldMass, LiveEditPolicy::ReinitializeComponent},
        {kRigidBody, kFieldGravityScale, LiveEditPolicy::ReinitializeComponent},
        // The collider's four fields are cooked into a shape handle at creation.
        {kCollider, kFieldShape, LiveEditPolicy::ReinitializeComponent},
        {kCollider, kFieldExtent, LiveEditPolicy::ReinitializeComponent},
        {kCollider, kFieldRadius, LiveEditPolicy::ReinitializeComponent},
        {kCollider, kFieldHeight, LiveEditPolicy::ReinitializeComponent},
        // The solver world is created with its gravity, and no running object holds it.
        {kWorldType, kFieldGravity, LiveEditPolicy::RestartWorld},
    };

    for (const Declaration& declaration : declarations) {
        if (Status declared =
                table.declare(declaration.type, declaration.field, declaration.policy);
            !declared) {
            return declared;
        }
    }
    return ok();
}

}  // namespace cy::gameplay::live
