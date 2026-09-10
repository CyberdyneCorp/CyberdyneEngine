#include <cy/animation/evaluate.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

constexpr PoseValue kNoPose = graph::pose::kNoPoseValue;
constexpr u16 kNoTransition = 0xFFFFU;

[[nodiscard]] f32 parameter_of(Span<const f32> parameters, u16 index) noexcept {
    return index < parameters.size() ? parameters[index] : 0.0F;
}

/// The additive composition `animation-and-skinning` asks for: a delta from a reference pose,
/// applied over an absolute one at a weight.
[[nodiscard]] Transform compose_additive(const Transform& base, const Transform& delta,
                                         f32 weight) noexcept {
    Transform out = base;
    out.translation = base.translation + (delta.translation * weight);
    out.rotation = normalize(slerp(Quat::identity(), delta.rotation, weight) * base.rotation);
    const Vec3 one{1.0F, 1.0F, 1.0F};
    out.scale = cwise_mul(base.scale, one + ((delta.scale - one) * weight));
    return out;
}

}  // namespace

// --- AnimationRig -------------------------------------------------------------------------------

AnimationRig::AnimationRig(Allocator& allocator) noexcept
    : clips_(allocator), times_(allocator), mask_weights_(allocator), chains_(allocator) {}

Status AnimationRig::bind(const Skeleton& skeleton, const PoseProgram& program,
                          Span<const Clip* const> clips) noexcept {
    if (!skeleton.finalized()) {
        return fail(ErrorCode::InvalidArgument,
                    "a skeleton is finalized before a program is bound to it");
    }
    if (program.joint_count() > skeleton.joint_count()) {
        return fail(ErrorCode::InvalidArgument,
                    "the compiled program addresses more joints than the skeleton has");
    }
    if (clips.size() < program.clips().size()) {
        return fail(ErrorCode::InvalidArgument,
                    "the clip table is shorter than the program's; every clip the program names "
                    "must have an entry, null included");
    }
    clips_.clear();
    if (Status appended = clips_.append(clips.subspan(0, program.clips().size())); !appended) {
        return appended;
    }

    // One entry per DISTINCT time parameter. Two clip instructions sharing a parameter play in
    // lockstep by construction, and advancing it twice would double their speed.
    times_.clear();
    for (const PoseInstruction& instruction : program.code()) {
        if (instruction.op != PoseOp::SampleClip) {
            continue;
        }
        bool seen = false;
        for (const TimeParameter& entry : times_) {
            seen = seen || entry.parameter == instruction.time_param;
        }
        if (seen) {
            continue;
        }
        TimeParameter entry;
        entry.parameter = instruction.time_param;
        entry.clip = instruction.clip;
        const Clip* clip = instruction.clip < clips_.size() ? clips_[instruction.clip] : nullptr;
        entry.duration = clip != nullptr ? clip->duration() : 1.0F;
        if (Status pushed = times_.push_back(entry); !pushed) {
            return pushed;
        }
    }

    joints_ = program.joint_count();
    const usize weights = static_cast<usize>(program.masks().size()) * joints_;
    if (Status sized = mask_weights_.resize(weights); !sized) {
        return sized;
    }
    for (f32& weight : mask_weights_) {
        weight = 1.0F;
    }
    if (Status sized = chains_.resize(0); !sized) {
        return sized;
    }

    skeleton_ = &skeleton;
    program_ = &program;
    return ok();
}

Status AnimationRig::set_mask_weight(u16 mask, u16 joint, f32 weight) noexcept {
    const usize index = (static_cast<usize>(mask) * joints_) + joint;
    if (index >= mask_weights_.size()) {
        return fail(ErrorCode::OutOfRange, "no such mask or joint");
    }
    mask_weights_[index] = math::clamp(weight, 0.0F, 1.0F);
    return ok();
}

f32 AnimationRig::mask_weight(u16 mask, u16 joint) const noexcept {
    const usize index = (static_cast<usize>(mask) * joints_) + joint;
    return index < mask_weights_.size() ? mask_weights_[index] : 1.0F;
}

Status AnimationRig::set_ik_chain(u16 index, const IkChain& chain) noexcept {
    if (index >= chains_.size()) {
        if (Status sized = chains_.resize(static_cast<usize>(index) + 1); !sized) {
            return sized;
        }
    }
    chains_[index] = chain;
    return ok();
}

const IkChain* AnimationRig::ik_chain(u16 index) const noexcept {
    if (index >= chains_.size() || !chains_[index].valid()) {
        return nullptr;
    }
    return &chains_[index];
}

// --- AnimationInstance --------------------------------------------------------------------------

AnimationInstance::AnimationInstance(Allocator& allocator) noexcept
    : parameters_(allocator), cursors_(allocator), previous_(allocator) {}

Status AnimationInstance::prepare(const AnimationRig& rig) noexcept {
    if (!rig.bound()) {
        return fail(ErrorCode::InvalidArgument, "the rig has no program bound");
    }
    const PoseProgram& program = rig.program();
    if (Status sized = parameters_.resize(program.parameters().size()); !sized) {
        return sized;
    }
    for (f32& value : parameters_) {
        value = 0.0F;
    }
    if (Status sized = previous_.resize(rig.time_parameters().size()); !sized) {
        return sized;
    }
    for (f32& value : previous_) {
        value = 0.0F;
    }
    cursors_.clear();
    for (usize index = 0; index < program.clips().size(); ++index) {
        Expected<ClipCursor*, Error> slot = cursors_.emplace_back(cursors_.allocator());
        if (!slot) {
            return Status{make_unexpected(slot.error())};
        }
        const Clip* clip = rig.clips()[index];
        if (clip != nullptr) {
            if (Status reset = (*slot)->reset(clip->track_count()); !reset) {
                return reset;
            }
        }
    }
    machine_ = graph::pose::PoseInstance{};
    machine_.state = program.entry_state();
    clear_root_motion();
    return ok();
}

Status AnimationInstance::set_parameter(const AnimationRig& rig, Name parameter,
                                        f32 value) noexcept {
    const Span<const Name> names = rig.program().parameters();
    for (usize index = 0; index < names.size(); ++index) {
        if (names[index] == parameter && index < parameters_.size()) {
            parameters_[index] = value;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "the program declares no such parameter");
}

f32 AnimationInstance::parameter(const AnimationRig& rig, Name parameter) const noexcept {
    const Span<const Name> names = rig.program().parameters();
    for (usize index = 0; index < names.size(); ++index) {
        if (names[index] == parameter && index < parameters_.size()) {
            return parameters_[index];
        }
    }
    return 0.0F;
}

ClipCursor& AnimationInstance::cursor(u16 clip) noexcept {
    const usize index = clip < cursors_.size() ? clip : 0;
    return cursors_[index];
}

void AnimationInstance::clear_root_motion() noexcept {
    root_ = RootDelta{};
    travelled_ = Vec3{0.0F, 0.0F, 0.0F};
}

void AnimationInstance::accept_root_motion(const RootDelta& delta) noexcept {
    root_ = delta;
    travelled_ = travelled_ + delta.translation;
}

// --- PoseScratch --------------------------------------------------------------------------------

PoseScratch::PoseScratch(Allocator& allocator) noexcept : storage_(allocator) {}

Status PoseScratch::prepare(const AnimationRig& rig) noexcept {
    if (!rig.bound()) {
        return fail(ErrorCode::InvalidArgument, "the rig has no program bound");
    }
    joints_ = rig.skeleton().joint_count();
    slots_ = static_cast<u32>(rig.program().code().size()) + 1;
    return storage_.resize(static_cast<usize>(joints_) * slots_);
}

Span<Transform> PoseScratch::slot(PoseValue value) noexcept {
    if (value >= slots_) {
        return {};
    }
    return {storage_.data() + (static_cast<usize>(value) * joints_), joints_};
}

// --- The evaluator ------------------------------------------------------------------------------

namespace {

/// Copy the joints of `mask` from one pose buffer to another. A free function rather than a method
/// because it needs nothing an evaluator holds.
void copy_masked(Span<Transform> out, Span<const Transform> source,
                 const JointMask& mask) noexcept {
    const usize count = source.size() < out.size() ? source.size() : out.size();
    for (usize index = 0; index < count; ++index) {
        if (mask.test(static_cast<u32>(index))) {
            out[index] = source[index];
        }
    }
}

/// One instance's walk over the program. Lazy: an instruction whose consumers read no joint is not
/// visited, and neither is anything beneath it.
struct Evaluator {
    const AnimationRig* rig = nullptr;
    AnimationInstance* instance = nullptr;
    PoseScratch* scratch = nullptr;
    EvaluationStats* stats = nullptr;
    JointMask lod;
    bool modifiers = true;

    [[nodiscard]] Span<const f32> parameters() const noexcept { return instance->parameters(); }
    [[nodiscard]] const PoseProgram& program() const noexcept { return rig->program(); }

    [[nodiscard]] Status run(PoseValue value) noexcept;

    void seed_reference(Span<Transform> out, const JointMask& mask) const noexcept;
    [[nodiscard]] Status sample_clip(const PoseInstruction& instruction, Span<Transform> out,
                                     const JointMask& required) const noexcept;
    [[nodiscard]] Status combine(const PoseInstruction& instruction, PoseValue value,
                                 const JointMask& required) noexcept;
    [[nodiscard]] Status apply_ik(const PoseInstruction& instruction, PoseValue value,
                                  const JointMask& required) noexcept;
};

void Evaluator::seed_reference(Span<Transform> out, const JointMask& mask) const noexcept {
    const Span<const Joint> joints = rig->skeleton().joints();
    for (usize index = 0; index < joints.size() && index < out.size(); ++index) {
        if (mask.test(static_cast<u32>(index))) {
            out[index] = joints[index].bind_local;
        }
    }
}

Status Evaluator::sample_clip(const PoseInstruction& instruction, Span<Transform> out,
                              const JointMask& required) const noexcept {
    seed_reference(out, required);
    const Clip* clip =
        instruction.clip < rig->clips().size() ? rig->clips()[instruction.clip] : nullptr;
    if (clip == nullptr) {
        // A clip that failed to load leaves the reference pose rather than failing the frame.
        return ok();
    }
    const f32 time = parameter_of(parameters(), instruction.time_param);
    SampleStats sampled;
    if (Status ran = clip->sample(time, required, instance->cursor(instruction.clip), out, sampled);
        !ran) {
        return ran;
    }
    ++stats->clips_sampled;
    stats->joints_sampled += required.count();
    stats->joints_written += sampled.joints_written;
    stats->tracks_skipped += sampled.tracks_skipped;
    return ok();
}

Status Evaluator::combine(const PoseInstruction& instruction, PoseValue value,
                          const JointMask& required) noexcept {
    const Span<Transform> out = scratch->slot(value);
    // Every blend instruction carries a mask index: `compile_pose` interns the whole-skeleton mask
    // for a node that authored none, so there is no "no mask" case to branch on here.
    const bool masked = instruction.mask < program().masks().size();
    const JointMask mask =
        masked ? program().masks()[instruction.mask].intersected(required) : required;
    const f32 weight =
        instruction.op == PoseOp::BlendMask
            ? 1.0F
            : math::clamp(parameter_of(parameters(), instruction.weight_param), 0.0F, 1.0F);

    if (Status ran = run(instruction.a); !ran) {
        return ran;
    }
    copy_masked(out, scratch->slot(instruction.a), required);
    if (weight <= 0.0F || mask.empty()) {
        // AN UNSELECTED BRANCH IS NOT EVALUATED. A layer at zero weight, or one whose mask no
        // retained joint survives, costs no sample at all — which is what "they SHALL NOT be
        // sampled" asks for.
        ++stats->instructions_skipped;
        return ok();
    }
    if (Status ran = run(instruction.b); !ran) {
        return ran;
    }
    const Span<const Transform> layer = scratch->slot(instruction.b);
    for (u32 joint = 0; joint < layer.size(); ++joint) {
        if (!mask.test(joint)) {
            continue;
        }
        const f32 per_joint =
            masked ? rig->mask_weight(instruction.mask, static_cast<u16>(joint)) : 1.0F;
        const f32 blended = weight * per_joint;
        if (instruction.op == PoseOp::Additive) {
            out[joint] = compose_additive(out[joint], layer[joint], blended);
        } else {
            out[joint] = interpolate(out[joint], layer[joint], blended);
        }
    }
    return ok();
}

Status Evaluator::apply_ik(const PoseInstruction& instruction, PoseValue value,
                           const JointMask& required) noexcept {
    if (Status ran = run(instruction.a); !ran) {
        return ran;
    }
    const Span<Transform> out = scratch->slot(value);
    copy_masked(out, scratch->slot(instruction.a), required);
    if (!modifiers) {
        // "WHEN an instance is at `Simplified` tier or lower THEN IK and secondary motion SHALL be
        // skipped."
        ++stats->instructions_skipped;
        return ok();
    }
    const IkChain* chain = rig->ik_chain(instruction.chain);
    if (chain == nullptr) {
        return ok();
    }
    const Span<const f32> params = parameters();
    const Vec3 target{parameter_of(params, chain->target_param),
                      parameter_of(params, static_cast<u16>(chain->target_param + 1)),
                      parameter_of(params, static_cast<u16>(chain->target_param + 2))};
    const f32 weight = parameter_of(params, chain->weight_param);
    const Expected<bool, Error> solved =
        solve_two_bone(rig->skeleton(), *chain, target, weight, out);
    if (!solved) {
        return Status{make_unexpected(solved.error())};
    }
    ++stats->ik_solved;
    return ok();
}

Status Evaluator::run(PoseValue value) noexcept {
    if (value == kNoPose || value >= program().code().size()) {
        return ok();
    }
    const PoseInstruction& instruction = program().code()[value];
    const JointMask required = instruction.required.intersected(lod);
    if (required.empty()) {
        ++stats->instructions_skipped;
        return ok();
    }
    ++stats->instructions;
    switch (instruction.op) {
        case PoseOp::RefPose:
            seed_reference(scratch->slot(value), required);
            return ok();
        case PoseOp::SampleClip:
            return sample_clip(instruction, scratch->slot(value), required);
        case PoseOp::Blend:
        case PoseOp::BlendMask:
        case PoseOp::Additive:
        case PoseOp::Layer:
            return combine(instruction, value, required);
        case PoseOp::IK:
            return apply_ik(instruction, value, required);
        case PoseOp::Count:
            break;
    }
    return fail(ErrorCode::InvalidArgument, "the program holds an instruction with no operation");
}

}  // namespace

Status evaluate(const AnimationRig& rig, AnimationInstance& instance, u8 bone_lod,
                PoseScratch& scratch, Span<Transform> out_local, EvaluationStats& stats) noexcept {
    if (!rig.bound()) {
        return fail(ErrorCode::InvalidArgument, "the rig has no program bound");
    }
    if (!scratch.ready() || scratch.joints() != rig.skeleton().joint_count()) {
        return fail(ErrorCode::InvalidArgument,
                    "the scratch was prepared for a different rig; call prepare() once per rig");
    }
    const PoseProgram& program = rig.program();
    const graph::pose::PoseInstance& machine = instance.machine();
    if (machine.state >= program.states().size()) {
        return fail(ErrorCode::InvalidArgument, "this instance is in no state of this program");
    }
    if (out_local.size() < rig.skeleton().joint_count()) {
        return fail(ErrorCode::InvalidArgument, "the output pose is smaller than the skeleton");
    }

    Evaluator evaluator;
    evaluator.rig = &rig;
    evaluator.instance = &instance;
    evaluator.scratch = &scratch;
    evaluator.stats = &stats;
    evaluator.lod = rig.skeleton().retained(bone_lod);
    evaluator.modifiers = evaluates_modifiers(instance.tier());

    // Only the joints the root instruction was analysed to write are copied out: a joint no
    // instruction wrote keeps the reference pose the caller seeded, rather than whatever the last
    // instance left in the shared scratch.
    const auto written = [&program, &evaluator](PoseValue value) noexcept {
        return value < program.code().size()
                   ? program.code()[value].required.intersected(evaluator.lod)
                   : JointMask{};
    };

    const PoseValue root = program.states()[machine.state].root;
    if (Status ran = evaluator.run(root); !ran) {
        return ran;
    }
    copy_masked(out_local, scratch.slot(root), written(root));

    if (machine.transition == kNoTransition || machine.target >= program.states().size()) {
        return ok();
    }
    // A blend in flight evaluates the TARGET state's tree and nothing else: no other state of the
    // machine is visited, which is what makes an idle state machine cost one tree.
    const PoseValue target_root = program.states()[machine.target].root;
    if (Status ran = evaluator.run(target_root); !ran) {
        return ran;
    }
    const graph::pose::Transition& running = program.transitions()[machine.transition];
    const f32 weight = running.duration <= 0.0F
                           ? 1.0F
                           : math::clamp(machine.blend_elapsed / running.duration, 0.0F, 1.0F);
    const Span<const Transform> target = scratch.slot(target_root);
    const JointMask blended = written(target_root);
    for (u32 joint = 0; joint < out_local.size() && joint < target.size(); ++joint) {
        if (blended.test(joint)) {
            out_local[joint] = interpolate(out_local[joint], target[joint], weight);
        }
    }
    return ok();
}

// --- advance: the deterministic half
// --------------------------------------------------------------

namespace {

/// The root motion of one subtree over the interval, blended by the same weights the pose tree
/// uses.
struct RootWalker {
    const AnimationRig* rig = nullptr;
    Span<const f32> parameters;
    Span<const f32> before;
    Span<const f32> after;

    [[nodiscard]] RootDelta run(PoseValue value) const noexcept;
};

/// The value a clip's time parameter held before the last advance. `before` is parallel to the
/// rig's time-parameter table, which is what makes this a lookup over a handful of entries rather
/// than a second copy of the parameter array.
[[nodiscard]] f32 time_before(const AnimationRig& rig, u16 parameter,
                              Span<const f32> before) noexcept {
    const Span<const AnimationRig::TimeParameter> table = rig.time_parameters();
    for (usize index = 0; index < table.size() && index < before.size(); ++index) {
        if (table[index].parameter == parameter) {
            return before[index];
        }
    }
    return 0.0F;
}

[[nodiscard]] RootDelta blend_root(const RootDelta& a, const RootDelta& b, f32 weight) noexcept {
    RootDelta out;
    out.translation = a.translation + ((b.translation - a.translation) * weight);
    out.rotation = slerp(a.rotation, b.rotation, weight);
    out.distance = a.distance + ((b.distance - a.distance) * weight);
    out.contacts = weight >= 0.5F ? b.contacts : a.contacts;
    return out;
}

RootDelta RootWalker::run(PoseValue value) const noexcept {
    const PoseProgram& program = rig->program();
    if (value == graph::pose::kNoPoseValue || value >= program.code().size()) {
        return RootDelta{};
    }
    const PoseInstruction& instruction = program.code()[value];
    switch (instruction.op) {
        case PoseOp::RefPose:
            return RootDelta{};
        case PoseOp::SampleClip: {
            const Clip* clip =
                instruction.clip < rig->clips().size() ? rig->clips()[instruction.clip] : nullptr;
            if (clip == nullptr || !clip->has_root_motion()) {
                return RootDelta{};
            }
            return clip->root_delta(time_before(*rig, instruction.time_param, before),
                                    parameter_of(after, instruction.time_param));
        }
        case PoseOp::Blend:
        case PoseOp::Layer:
        case PoseOp::Additive: {
            const f32 weight =
                math::clamp(parameter_of(parameters, instruction.weight_param), 0.0F, 1.0F);
            const RootDelta a = run(instruction.a);
            if (weight <= 0.0F) {
                return a;
            }
            return blend_root(a, run(instruction.b), weight);
        }
        case PoseOp::BlendMask: {
            // The root joint belongs to one side of a mask, not to a fraction of both.
            const JointMask& mask = instruction.mask < program.masks().size()
                                        ? program.masks()[instruction.mask]
                                        : instruction.required;
            const bool from_layer = mask.test(0);
            return run(from_layer ? instruction.b : instruction.a);
        }
        case PoseOp::IK:
            return run(instruction.a);
        case PoseOp::Count:
            break;
    }
    return RootDelta{};
}

/// Emit the events every clip of a state's tree crossed in the interval.
[[nodiscard]] Status emit_state_events(const AnimationRig& rig, AnimationInstance& instance,
                                       PoseValue value, Span<const f32> before,
                                       Span<const f32> after, EventBuffer& buffer) noexcept {
    const PoseProgram& program = rig.program();
    if (value == graph::pose::kNoPoseValue || value >= program.code().size()) {
        return ok();
    }
    const PoseInstruction& instruction = program.code()[value];
    if (instruction.op == PoseOp::SampleClip) {
        const Clip* clip =
            instruction.clip < rig.clips().size() ? rig.clips()[instruction.clip] : nullptr;
        if (clip == nullptr) {
            return ok();
        }
        const f32 from = time_before(rig, instruction.time_param, before);
        const f32 to = parameter_of(after, instruction.time_param);
        return emit_events(*clip, instance.identifier(), from, to, instance.event_policy(), buffer);
    }
    if (Status left = emit_state_events(rig, instance, instruction.a, before, after, buffer);
        !left) {
        return left;
    }
    return emit_state_events(rig, instance, instruction.b, before, after, buffer);
}

/// Restart the clip times of a state that has just been entered.
void reset_state_times(const AnimationRig& rig, AnimationInstance& instance,
                       PoseValue value) noexcept {
    const PoseProgram& program = rig.program();
    if (value == graph::pose::kNoPoseValue || value >= program.code().size()) {
        return;
    }
    const PoseInstruction& instruction = program.code()[value];
    if (instruction.op == PoseOp::SampleClip) {
        const Span<f32> parameters = instance.parameters();
        if (instruction.time_param < parameters.size()) {
            parameters[instruction.time_param] = 0.0F;
        }
        return;
    }
    reset_state_times(rig, instance, instruction.a);
    reset_state_times(rig, instance, instruction.b);
}

}  // namespace

Status advance(const AnimationRig& rig, AnimationInstance& instance, f32 dt,
               EventBuffer* events) noexcept {
    if (!rig.bound()) {
        return fail(ErrorCode::InvalidArgument, "the rig has no program bound");
    }
    const PoseProgram& program = rig.program();
    if (instance.machine().state >= program.states().size()) {
        return fail(ErrorCode::InvalidArgument, "this instance is in no state of this program");
    }
    const Span<const AnimationRig::TimeParameter> times = rig.time_parameters();
    const Span<f32> previous = instance.previous_times();
    if (previous.size() != times.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "the instance was prepared against a different rig");
    }

    const Span<f32> parameters = instance.parameters();
    for (usize index = 0; index < times.size(); ++index) {
        previous[index] = parameter_of(parameters, times[index].parameter);
    }

    // THE CLIP CLOCKS. Advanced by the whole interval whatever the tier evaluates at, so an
    // instance evaluated at 10 Hz has advanced exactly as far as one evaluated at 60 Hz.
    const f32 step = dt * instance.play_rate();
    for (const AnimationRig::TimeParameter& entry : times) {
        if (entry.parameter >= parameters.size()) {
            continue;
        }
        parameters[entry.parameter] += step;
    }

    const u16 state_before = instance.machine().state;
    graph::pose::advance(program, instance.machine(), instance.parameters(), dt);
    const u16 state_after = instance.machine().state;

    // The interval each clip advanced over is [previous, the parameter array as it stands now].
    // The clocks are wrapped at the END of this function, so nothing here has to reconstruct a
    // second copy of them — and `advance()` allocates nothing.
    RootWalker walker;
    walker.rig = &rig;
    walker.parameters = instance.parameters();
    walker.before = previous;
    walker.after = instance.parameters();

    const graph::pose::PoseInstance& machine = instance.machine();
    RootDelta delta = walker.run(program.states()[machine.state].root);
    if (machine.transition != kNoTransition && machine.target < program.states().size()) {
        const graph::pose::Transition& running = program.transitions()[machine.transition];
        const f32 weight = running.duration <= 0.0F
                               ? 1.0F
                               : math::clamp(machine.blend_elapsed / running.duration, 0.0F, 1.0F);
        delta = blend_root(delta, walker.run(program.states()[machine.target].root), weight);
    }
    instance.accept_root_motion(delta);

    if (events != nullptr) {
        if (Status emitted = emit_state_events(rig, instance, program.states()[machine.state].root,
                                               previous, instance.parameters(), *events);
            !emitted) {
            return emitted;
        }
    }

    // The clocks are wrapped AFTER root motion and events have read the unwrapped interval, so a
    // step that crosses the end of a clip is one interval rather than a jump backwards.
    for (const AnimationRig::TimeParameter& entry : times) {
        if (entry.parameter >= parameters.size() || entry.duration <= 0.0F) {
            continue;
        }
        const f32 value = parameters[entry.parameter];
        if (value >= entry.duration || value < 0.0F) {
            parameters[entry.parameter] =
                value - (std::floor(value / entry.duration) * entry.duration);
        }
    }

    if (state_after != state_before) {
        reset_state_times(rig, instance, program.states()[state_after].root);
    }
    return ok();
}

// --- AnimationBatch -----------------------------------------------------------------------------

AnimationBatch::AnimationBatch(Allocator& allocator, const AnimationRig& rig) noexcept
    : rig_(&rig), allocator_(&allocator), instances_(allocator) {}

Expected<u32, Error> AnimationBatch::add() noexcept {
    Expected<AnimationInstance*, Error> slot = instances_.emplace_back(*allocator_);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    if (Status prepared = (*slot)->prepare(*rig_); !prepared) {
        instances_.pop_back();
        return make_unexpected(prepared.error());
    }
    const auto index = static_cast<u32>(instances_.size() - 1);
    (*slot)->set_identifier(index);
    return index;
}

Status AnimationBatch::advance_all(f32 dt, EventBuffer* events) noexcept {
    for (AnimationInstance& instance : instances_) {
        if (Status advanced = advance(*rig_, instance, dt, events); !advanced) {
            return advanced;
        }
    }
    return ok();
}

Status AnimationBatch::evaluate_range(u32 first, u32 count, u8 bone_lod, PoseScratch& scratch,
                                      Span<Transform> poses, EvaluationStats& stats) noexcept {
    if (static_cast<usize>(first) + count > instances_.size()) {
        return fail(ErrorCode::OutOfRange, "the range names instances the batch does not have");
    }
    const usize joints = rig_->skeleton().joint_count();
    if (poses.size() < joints * count) {
        return fail(ErrorCode::InvalidArgument,
                    "the pose buffer is smaller than the range it is asked to hold");
    }
    for (u32 index = 0; index < count; ++index) {
        AnimationInstance& instance = instances_[first + index];
        const Span<Transform> out = poses.subspan(static_cast<usize>(index) * joints, joints);
        rig_->skeleton().reference_pose(out);
        if (!evaluates_pose(instance.tier())) {
            // A baked instance has no skeleton evaluation at all; its root motion still ran in
            // `advance()`.
            continue;
        }
        if (Status evaluated = evaluate(*rig_, instance, bone_lod, scratch, out, stats);
            !evaluated) {
            return evaluated;
        }
    }
    return ok();
}

}  // namespace cy::animation
