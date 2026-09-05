// The camera server: pools, evaluation, the impulse bus and the query batch.
// See cy/servers/camera/server.h.

#include <cy/servers/camera/server.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <utility>

namespace cy::camera {
namespace {

/// The pools are touched from one thread — see the class comment — so the thread-safe flag is off:
/// it would buy a mutex on every create and destroy to serve nobody.
constexpr bool kPoolThreadSafe = false;
constexpr u32 kPoolChunk = 32;

/// How much motion reduction takes off the shake that survives the player's own shake setting.
/// `camera-system` requires motion reduction to affect shake, head motion and camera acceleration
/// together; at Seed shake is the only one of the three this module produces, so it is the only one
/// scaled — and the constant is named rather than inlined so the other two join it here.
constexpr f32 kMotionReductionScale = 0.25F;

[[nodiscard]] constexpr u64 mix(u64 value) noexcept {
    u64 result = value;
    result ^= result >> 30U;
    result *= 0xBF58476D1CE4E5B9ULL;
    result ^= result >> 27U;
    result *= 0x94D049BB133111EBULL;
    result ^= result >> 31U;
    return result;
}

}  // namespace

const char* evaluation_mode_name(EvaluationMode mode) noexcept {
    switch (mode) {
        case EvaluationMode::Simulation:
            return "simulation";
        case EvaluationMode::Render:
            return "render";
        case EvaluationMode::Hybrid:
            return "hybrid";
        case EvaluationMode::Count:
            break;
    }
    return "unknown";
}

CameraServer::CameraServer(Allocator& allocator) noexcept
    : allocator_(&allocator),
      registry_(allocator),
      definitions_(MemoryDomain::Engine, "camera.definitions", kPoolChunk, kPoolThreadSafe),
      rigs_(MemoryDomain::Engine, "camera.rigs", kPoolChunk, kPoolThreadSafe),
      stacks_(MemoryDomain::Engine, "camera.stacks", 8, kPoolThreadSafe),
      impulses_(allocator),
      impulse_scratch_(allocator),
      queries_(allocator),
      query_results_(allocator),
      stack_scratch_(allocator) {
    definitions_.set_allocator(allocator);
    rigs_.set_allocator(allocator);
    stacks_.set_allocator(allocator);
}

CameraServer::~CameraServer() {
    shutdown();
}

Status CameraServer::initialize() noexcept {
    if (initialized_) {
        return ok();
    }
    if (Status reserved = impulses_.reserve(config_.impulse_capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = impulse_scratch_.reserve(config_.impulse_capacity); !reserved) {
        return reserved;
    }
    initialized_ = true;
    return ok();
}

void CameraServer::shutdown() noexcept {
    impulses_.clear();
    impulse_scratch_.clear();
    queries_.clear();
    query_results_.clear();
    stack_scratch_.clear();
    initialized_ = false;
}

Status CameraServer::configure(const CameraServerConfig& config) noexcept {
    if (initialized_) {
        return fail(ErrorCode::AlreadyExists,
                    "the camera server is already initialized; configure it before initialize()");
    }
    config_ = config;
    return ok();
}

// --- Definitions --------------------------------------------------------------------------------

Expected<DefinitionHandle, Error> CameraServer::create_definition(
    const RigDefinition& definition) noexcept {
    Expected<RigProgram, Error> compiled = compile(definition, registry_, *allocator_);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    return definitions_.create(definition.name, std::move(*compiled));
}

void CameraServer::destroy_definition(DefinitionHandle handle) noexcept {
    (void)definitions_.destroy(handle);
}

const RigProgram* CameraServer::program(DefinitionHandle handle) const noexcept {
    const DefinitionRecord* record = definitions_.resolve(handle);
    return (record == nullptr) ? nullptr : &record->program;
}

// --- Rigs ---------------------------------------------------------------------------------------

Expected<RigHandle, Error> CameraServer::create_rig(DefinitionHandle definition,
                                                    const RigConfig& config) noexcept {
    const DefinitionRecord* record = definitions_.resolve(definition);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera definition");
    }

    Expected<RigHandle, Error> handle = rigs_.create(*allocator_, definition, config);
    if (!handle) {
        return handle;
    }
    Rig* rig = rigs_.resolve(*handle);
    if (rig == nullptr) {
        // Unreachable: the pool issued this handle one line ago. HANDLED RATHER THAN ASSERTED,
        // because CY_ASSERT is compiled out in Profile and Shipping — an assertion here would leave
        // the two lines below dereferencing a pointer the compiler can see may be null, which is
        // exactly what `-Wnull-dereference` reports in those two configurations.
        (void)rigs_.destroy(*handle);
        return fail(ErrorCode::Internal,
                    "a camera rig handle did not resolve immediately after it "
                    "was issued");
    }
    // Derived from the handle, so two rigs never share a history identity and one rig's identity
    // survives every change to what it is framing.
    rig->history_seed = mix((*handle).bits());
    rig->evaluated.history_id = rig->history_seed;
    return handle;
}

void CameraServer::destroy_rig(RigHandle handle) noexcept {
    (void)rigs_.destroy(handle);
}

bool CameraServer::alive(RigHandle handle) const noexcept {
    return rigs_.resolve(handle) != nullptr;
}

Status CameraServer::set_target(RigHandle handle, const TargetBinding& binding) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    const RigProgram* compiled = program(rig->definition);
    if (compiled == nullptr) {
        return fail(ErrorCode::NotFound, "the rig's definition has been destroyed");
    }
    if (!compiled->has_target) {
        // A binding nothing reads is a configuration mistake that would otherwise be invisible: the
        // camera would simply keep framing nothing and look almost right.
        return fail(ErrorCode::InvalidArgument,
                    "this rig's definition has no target node, so a target binding would never be "
                    "read");
    }
    rig->binding = binding;
    return ok();
}

const TargetBinding* CameraServer::target(RigHandle handle) const noexcept {
    const Rig* rig = rigs_.resolve(handle);
    return (rig == nullptr) ? nullptr : &rig->binding;
}

Status CameraServer::apply_intent(RigHandle handle, const CameraIntent& intent) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    accumulate(rig->intent, intent);
    return ok();
}

Status CameraServer::cut(RigHandle handle, CutReason reason, bool anticipated,
                         f32 lead_seconds) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    rig->pending_cut.reason = reason;
    rig->pending_cut.anticipated = anticipated;
    rig->pending_cut.lead_seconds = lead_seconds;
    rig->cut_requested = true;
    return ok();
}

Status CameraServer::override_pose(RigHandle handle, const Transform& pose) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    rig->pose_override_active = true;
    rig->pose_override = pose;
    return ok();
}

Status CameraServer::clear_pose_override(RigHandle handle) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    rig->pose_override_active = false;
    return ok();
}

const EvaluatedCamera* CameraServer::evaluated(RigHandle handle) const noexcept {
    const Rig* rig = rigs_.resolve(handle);
    return (rig == nullptr) ? nullptr : &rig->evaluated;
}

Span<const RigOpTrace> CameraServer::trace(RigHandle handle) const noexcept {
    const Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return Span<const RigOpTrace>{};
    }
    return rig->trace.span();
}

TargetSample CameraServer::sample_for(const TargetBinding& binding,
                                      Span<const TargetSample> samples) noexcept {
    // A linear scan. A camera has one target and a frame has a handful of cameras, so a map would
    // be a hash lookup and an iteration order to reason about in exchange for nothing.
    for (const TargetSample& sample : samples) {
        if (sample.stable_id == binding.stable_id && sample.stable_id != 0) {
            return sample;
        }
    }
    return TargetSample{};
}

Expected<const EvaluatedCamera*, Error> CameraServer::evaluate(
    RigHandle handle, const EvaluationContext& context) noexcept {
    Rig* rig = rigs_.resolve(handle);
    if (rig == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera rig");
    }
    const DefinitionRecord* record = definitions_.resolve(rig->definition);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "the rig's definition has been destroyed");
    }

    // THE MODE IS CHECKED, NOT RECORDED. A simulation-mode camera evaluated off the fixed tick is
    // not reproducible, and a render-mode camera evaluated on the tick is evaluated at the wrong
    // rate. Both are configuration mistakes that produce a camera that looks nearly right, which is
    // the class of mistake worth an error rather than a comment.
    if (rig->config.mode == EvaluationMode::Simulation && !context.simulation) {
        return fail(ErrorCode::InvalidArgument,
                    "a simulation-mode camera was evaluated outside the fixed tick");
    }
    if (rig->config.mode == EvaluationMode::Render && context.simulation) {
        return fail(ErrorCode::InvalidArgument, "a render-mode camera was evaluated on a tick");
    }

    if (Status evaluated_ok = evaluate_resolved(*rig, record->program, handle, context);
        !evaluated_ok) {
        return make_unexpected(evaluated_ok.error());
    }
    return &rig->evaluated;
}

Status CameraServer::evaluate_resolved(Rig& rig, const RigProgram& compiled, RigHandle handle,
                                       const EvaluationContext& context) noexcept {
    // A CUT IS APPLIED BEFORE THE EVALUATION IT AFFECTS, so the first frame after a cut is already
    // at the new pose rather than one frame of easing away from the old one.
    if (rig.cut_requested) {
        rig.state.reset(rig.evaluated.pose, rig.evaluated.lens);
        ++rig.cut_epoch;
        rig.pending_cut.tick = context.tick;
        rig.evaluated.last_cut = rig.pending_cut;
        rig.cut_requested = false;
    }

    RigEvaluationInput input;
    input.delta_seconds = math::max(context.delta_seconds, 0.0F);
    input.tick = context.tick;
    input.binding = rig.binding;
    input.sample = sample_for(rig.binding, context.targets);
    input.intent = rig.intent;
    input.query_results = query_results_.span();
    input.queries = compiled.emits_queries ? &queries_ : nullptr;
    input.rig_bits = handle.bits();

    // EVERY PLAYER SETTING THIS MODULE APPLIES IS READ HERE, ONCE. See the class comment: a rig
    // node that multiplied by a sensitivity again would be the duplication the requirement forbids.
    input.shake_scale =
        settings_.shake_scale * (settings_.reduce_motion ? kMotionReductionScale : 1.0F);
    input.fov_override_radians = settings_.field_of_view_override_radians;
    input.collision_strength = settings_.collision_strength;
    input.impulses = impulses_for(rig.evaluated.pose.translation);

    RigFrame frame;
    frame.position = rig.state.primed ? rig.state.position.value() : rig.evaluated.pose.translation;
    frame.rotation = rig.state.primed ? rig.state.rotation.value() : rig.evaluated.pose.rotation;
    frame.anchor = rig.state.anchor.value();
    frame.lens = rig.evaluated.lens;

    Array<RigOpTrace>* trace = config_.collect_trace ? &rig.trace : nullptr;
    if (Status executed = cy::camera::evaluate(compiled, registry_, rig.state, input, frame, trace);
        !executed) {
        return executed;
    }

    // The shake is composed over the evaluated base rather than baked into it, so a diagnostic can
    // subtract it and the base pose stays the thing the rig produced.
    Transform pose;
    pose.translation = frame.position + frame.shake_offset;
    pose.rotation = frame.rotation * frame.shake_rotation;
    pose.scale = Vec3{1.0F, 1.0F, 1.0F};

    rig.evaluated.pose_overridden = rig.pose_override_active;
    if (rig.pose_override_active) {
        pose = rig.pose_override;
    }

    const f32 delta = input.delta_seconds;
    rig.evaluated.velocity =
        (rig.state.has_previous && delta > 0.0F)
            ? ((pose.translation - rig.state.previous_position) * (1.0F / delta))
            : Vec3{0.0F, 0.0F, 0.0F};
    rig.state.previous_position = pose.translation;
    rig.state.has_previous = true;

    rig.evaluated.pose = pose;
    rig.evaluated.lens = frame.lens;
    if (settings_.field_of_view_override_radians > 0.0F) {
        // The accessibility override replaces the evaluated field of view. Written into both halves
        // of the lens so that a physical lens reports the focal length that matches what is being
        // rendered rather than the one the rig computed.
        rig.evaluated.lens.gameplay.vertical_fov_radians = settings_.field_of_view_override_radians;
        rig.evaluated.lens.physical.focal_length_mm = focal_length_from_fov(
            settings_.field_of_view_override_radians, rig.evaluated.lens.physical.sensor_height_mm);
        rig.evaluated.lens.kind = LensKind::Gameplay;
    }

    // VIEW AIM IS DERIVED AND CONTROL AIM IS CARRIED. See camera.h: a camera that computed the
    // control aim would make the camera authoritative by the back door.
    rig.evaluated.aim.view_aim = pose.forward();
    if (rig.intent.control_aim_set) {
        rig.evaluated.aim.control_aim = rig.intent.control_aim;
    }

    rig.evaluated.cut_epoch = rig.cut_epoch;
    rig.evaluated.history_id = rig.history_seed;
    rig.evaluated.listener =
        derive_listener(rig.evaluated, context.controlled, context.controlled_velocity,
                        rig.config.listener_policy, rig.config.listener_blend);
    rig.evaluated.streaming = derive_streaming_source(
        rig.evaluated, context.aspect, rig.config.importance, rig.config.prediction_seconds);

    // Intent is consumed by the evaluation it was accumulated for. Leaving it would apply one
    // frame's look delta on every subsequent frame, which is a camera that keeps turning after the
    // stick is released.
    rig.intent = CameraIntent{};
    return ok();
}

// --- The impulse bus
// ------------------------------------------------------------------------------

Status CameraServer::emit_impulse(const CameraImpulse& impulse) noexcept {
    if (impulses_.size() >= config_.impulse_capacity) {
        // Refused rather than silently dropped or grown without bound: an impulse bus that is full
        // means something is emitting far more than it should, and a diagnostic is more useful than
        // a quietly louder camera.
        return fail(ErrorCode::OutOfRange, "the camera impulse bus is full");
    }
    return impulses_.push_back(LiveImpulse{impulse, 0.0F});
}

void CameraServer::advance_impulses(f32 delta_seconds) noexcept {
    const f32 delta = math::max(delta_seconds, 0.0F);
    usize write = 0;
    for (usize read = 0; read < impulses_.size(); ++read) {
        impulses_[read].elapsed += delta;
        const CameraImpulse& impulse = impulses_[read].impulse;
        if (impulse.duration_seconds > 0.0F &&
            impulses_[read].elapsed >= impulse.duration_seconds) {
            continue;
        }
        if (write != read) {
            impulses_[write] = impulses_[read];
        }
        ++write;
    }
    while (impulses_.size() > write) {
        impulses_.pop_back();
    }
}

Span<const CameraImpulse> CameraServer::impulses_for(Vec3 camera_position) noexcept {
    impulse_scratch_.clear();
    for (const LiveImpulse& live : impulses_) {
        const CameraImpulse& source = live.impulse;

        // The envelope: linear decay over the impulse's own duration. A zero duration is a
        // single-frame impulse at full strength.
        const f32 envelope =
            (source.duration_seconds > 0.0F)
                ? math::clamp(1.0F - (live.elapsed / source.duration_seconds), 0.0F, 1.0F)
                : 1.0F;

        // EACH CAMERA ATTENUATES BY ITS OWN DISTANCE AND OCCLUSION, which is why the impulse
        // carries a world position rather than an amplitude: "**WHEN** an explosion emits an
        // impulse
        // **THEN** each local camera SHALL respond according to its own distance and occlusion."
        f32 spatial = 1.0F;
        if (source.radius > 0.0F) {
            const f32 distance = length(camera_position - source.world_position);
            spatial = math::clamp(1.0F - (distance / source.radius), 0.0F, 1.0F);
        }
        const f32 strength =
            source.strength * envelope * spatial * math::clamp(1.0F - source.occlusion, 0.0F, 1.0F);
        if (strength <= 0.0F) {
            continue;
        }
        CameraImpulse attenuated = source;
        attenuated.strength = strength;
        (void)impulse_scratch_.push_back(attenuated);
    }
    return impulse_scratch_.span();
}

// --- Stacks
// ---------------------------------------------------------------------------------------

Expected<StackHandle, Error> CameraServer::create_stack() noexcept {
    return stacks_.create(*allocator_);
}

void CameraServer::destroy_stack(StackHandle handle) noexcept {
    (void)stacks_.destroy(handle);
}

CameraStack* CameraServer::stack(StackHandle handle) noexcept {
    return stacks_.resolve(handle);
}

const CameraStack* CameraServer::stack(StackHandle handle) const noexcept {
    return stacks_.resolve(handle);
}

Status CameraServer::evaluate_stack(StackHandle handle, const EvaluationContext& context,
                                    EvaluatedCamera& out,
                                    Array<StackContribution>* report) noexcept {
    CameraStack* target_stack = stacks_.resolve(handle);
    if (target_stack == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera stack");
    }

    // Advanced BEFORE the rigs are evaluated, so an entry that finished blending out this frame is
    // gone before anything is evaluated for it — otherwise a released cinematic would be evaluated
    // one frame past its own removal.
    target_stack->advance(context.delta_seconds);
    if (target_stack->empty()) {
        return fail(ErrorCode::NotFound, "the camera stack is empty");
    }

    stack_scratch_.clear();
    if (Status reserved = stack_scratch_.reserve(target_stack->size()); !reserved) {
        return reserved;
    }
    for (usize i = 0; i < target_stack->size(); ++i) {
        const Expected<const EvaluatedCamera*, Error> evaluated_entry =
            evaluate(target_stack->entry_at(i).rig, context);
        if (!evaluated_entry) {
            return make_unexpected(evaluated_entry.error());
        }
        if (Status pushed = stack_scratch_.push_back(**evaluated_entry); !pushed) {
            return pushed;
        }
    }
    return target_stack->blend(stack_scratch_.span(), out, report);
}

// --- The query batch
// --------------------------------------------------------------------------------

void CameraServer::begin_frame() noexcept {
    queries_.clear();
}

Status CameraServer::submit_query_results(Span<const CameraQueryResult> results) noexcept {
    query_results_.clear();
    if (Status reserved = query_results_.reserve(results.size()); !reserved) {
        return reserved;
    }
    for (const CameraQueryResult& result : results) {
        if (Status pushed = query_results_.push_back(result); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::camera
