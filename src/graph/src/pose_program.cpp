// SPDX-License-Identifier: MIT
// The compiled pose program's runtime half: the joint mask, the program's storage, the state
// machine's `advance`, the reference evaluator, and the assembly of a program a cook compiled.
//
// SPLIT FROM lower_pose.cpp SO A RUNTIME LINKS NO COMPILER. `animation-and-skinning`: "Compilation
// SHALL occur at cook time; the runtime SHALL contain no graph compiler." Everything a loaded
// program needs at run time is in this file, and nothing here names `compile_pose` or the node
// registry, so a binary that loads a cooked program and never compiles one does not pull
// lower_pose.cpp's object in at all. src/animation/assets/tests/runtime_only_probe.cpp proves it at
// link time.

#include <cy/graph/lower_pose.h>

#include "pose_program_access.h"

#include <algorithm>
#include <utility>

namespace cy::graph::pose {

// --- The joint mask -------------------------------------------------------------------------

JointMask JointMask::all(u32 joints) noexcept {
    JointMask mask;
    const u32 capped = joints > kMaxJoints ? kMaxJoints : joints;
    for (u32 joint = 0; joint < capped; ++joint) {
        mask.set(joint);
    }
    return mask;
}

JointMask JointMask::range(u32 first, u32 count) noexcept {
    JointMask mask;
    for (u32 joint = first; joint < first + count && joint < kMaxJoints; ++joint) {
        mask.set(joint);
    }
    return mask;
}

void JointMask::set(u32 joint) noexcept {
    if (joint < kMaxJoints) {
        words_[joint / 64U] |= 1ULL << (joint % 64U);
    }
}

bool JointMask::test(u32 joint) const noexcept {
    return joint < kMaxJoints && (words_[joint / 64U] & (1ULL << (joint % 64U))) != 0;
}

bool JointMask::empty() const noexcept {
    return std::ranges::all_of(words_, [](u64 word) noexcept { return word == 0; });
}

u32 JointMask::count() const noexcept {
    u32 total = 0;
    for (u64 word : words_) {
        while (word != 0) {
            total += static_cast<u32>(word & 1ULL);
            word >>= 1U;
        }
    }
    return total;
}

JointMask JointMask::merged(const JointMask& other) const noexcept {
    JointMask result;
    for (u32 index = 0; index < kMaxJoints / 64U; ++index) {
        result.words_[index] = words_[index] | other.words_[index];
    }
    return result;
}

JointMask JointMask::intersected(const JointMask& other) const noexcept {
    JointMask result;
    for (u32 index = 0; index < kMaxJoints / 64U; ++index) {
        result.words_[index] = words_[index] & other.words_[index];
    }
    return result;
}

JointMask JointMask::without(const JointMask& other) const noexcept {
    JointMask result;
    for (u32 index = 0; index < kMaxJoints / 64U; ++index) {
        result.words_[index] = words_[index] & ~other.words_[index];
    }
    return result;
}

bool operator==(const JointMask& a, const JointMask& b) noexcept {
    for (u32 index = 0; index < kMaxJoints / 64U; ++index) {
        if (a.words_[index] != b.words_[index]) {
            return false;
        }
    }
    return true;
}

const char* pose_op_name(PoseOp op) noexcept {
    switch (op) {
        case PoseOp::RefPose:
            return "ref_pose";
        case PoseOp::SampleClip:
            return "sample_clip";
        case PoseOp::Blend:
            return "blend";
        case PoseOp::BlendMask:
            return "blend_mask";
        case PoseOp::Additive:
            return "additive";
        case PoseOp::Layer:
            return "layer";
        case PoseOp::IK:
            return "ik";
        case PoseOp::Count:
            break;
    }
    return "?";
}

PoseProgram::PoseProgram(Allocator& allocator) noexcept
    : code_(allocator),
      states_(allocator),
      transitions_(allocator),
      clips_(allocator),
      masks_(allocator),
      parameters_(allocator),
      sync_(allocator),
      markers_(allocator),
      debug_(allocator) {}

// --- Evaluation -----------------------------------------------------------------------------

namespace {

[[nodiscard]] f32 parameter_of(Span<const f32> parameters, u16 index) noexcept {
    return index < parameters.size() ? parameters[index] : 0.0F;
}

/// Blend `b` into `a` over the joints in `mask`.
void blend_into(Span<f32> a, Span<const f32> b, const JointMask& mask, f32 weight,
                u32 joints) noexcept {
    for (u32 joint = 0; joint < joints; ++joint) {
        if (!mask.test(joint)) {
            continue;
        }
        for (u32 channel = 0; channel < kChannelsPerJoint; ++channel) {
            const usize index = (joint * kChannelsPerJoint) + channel;
            a[index] = a[index] + ((b[index] - a[index]) * weight);
        }
    }
}

void add_into(Span<f32> a, Span<const f32> b, const JointMask& mask, f32 weight,
              u32 joints) noexcept {
    for (u32 joint = 0; joint < joints; ++joint) {
        if (!mask.test(joint)) {
            continue;
        }
        for (u32 channel = 0; channel < kChannelsPerJoint; ++channel) {
            const usize index = (joint * kChannelsPerJoint) + channel;
            a[index] += b[index] * weight;
        }
    }
}

/// One evaluation's scratch: a pose buffer per instruction that is actually visited.
struct Evaluator {
    const PoseProgram* program = nullptr;
    Span<const f32> parameters;
    PoseSampler* sampler = nullptr;
    EvaluationReport* report = nullptr;
    Array<f32>* scratch = nullptr;
    u32 joints = 0;

    [[nodiscard]] Span<f32> slot(PoseValue value) const noexcept {
        const usize stride = static_cast<usize>(joints) * kChannelsPerJoint;
        return {scratch->data() + (static_cast<usize>(value) * stride), stride};
    }

    [[nodiscard]] Status run(PoseValue value) noexcept;
};

Status Evaluator::run(PoseValue value) noexcept {
    if (value == kNoPoseValue || value >= program->code().size()) {
        return ok();
    }
    const PoseInstruction& instruction = program->code()[value];
    // LAZY: an instruction whose consumers read no joint is not evaluated at all, and neither is
    // anything beneath it. `animation-and-skinning`: "lower-body joints of that layer's clips are
    // never read, and they SHALL NOT be sampled."
    if (instruction.required.empty()) {
        return ok();
    }
    ++report->instructions_evaluated;
    const Span<f32> out = slot(value);
    switch (instruction.op) {
        case PoseOp::RefPose:
            sampler->reference(instruction.required, out);
            return ok();
        case PoseOp::SampleClip: {
            if (instruction.clip >= program->clips().size()) {
                return make_unexpected(
                    Error{ErrorCode::InvalidArgument, "this instruction names no clip", 0});
            }
            const ClipRef& clip = program->clips()[instruction.clip];
            sampler->sample(clip, parameter_of(parameters, instruction.time_param),
                            instruction.required, out);
            ++report->clips_sampled;
            report->joints_sampled += instruction.required.count();
            return ok();
        }
        case PoseOp::Blend:
        case PoseOp::BlendMask:
        case PoseOp::Additive:
        case PoseOp::Layer: {
            if (Status ran = run(instruction.a); !ran) {
                return ran;
            }
            if (Status ran = run(instruction.b); !ran) {
                return ran;
            }
            const Span<f32> base = slot(instruction.a);
            for (usize index = 0; index < out.size(); ++index) {
                out[index] = base[index];
            }
            const JointMask mask =
                instruction.op == PoseOp::Blend
                    ? instruction.required
                    : instruction.required.intersected(instruction.mask < program->masks().size()
                                                           ? program->masks()[instruction.mask]
                                                           : JointMask::all(joints));
            const f32 weight = instruction.op == PoseOp::BlendMask
                                   ? 1.0F
                                   : parameter_of(parameters, instruction.weight_param);
            if (instruction.op == PoseOp::Additive) {
                add_into(out, slot(instruction.b), mask, weight, joints);
            } else {
                blend_into(out, slot(instruction.b), mask, weight, joints);
            }
            return ok();
        }
        case PoseOp::IK: {
            if (Status ran = run(instruction.a); !ran) {
                return ran;
            }
            const Span<f32> base = slot(instruction.a);
            for (usize index = 0; index < out.size(); ++index) {
                out[index] = base[index];
            }
            sampler->solve_ik(instruction.chain, instruction.required, out);
            return ok();
        }
        case PoseOp::Count:
            break;
    }
    return ok();
}

}  // namespace

void advance(const PoseProgram& program, PoseInstance& instance, Span<const f32> parameters,
             f32 dt) noexcept {
    if (instance.state >= program.states().size()) {
        return;
    }
    instance.state_time += dt;
    if (instance.transition != 0xFFFFU) {
        // A blend in flight. It completes, or a higher-priority transition interrupts it.
        const Transition& running = program.transitions()[instance.transition];
        instance.blend_elapsed += dt;
        if (instance.blend_elapsed >= running.duration) {
            instance.state = running.target_state;
            instance.target = 0xFFFFU;
            instance.transition = 0xFFFFU;
            instance.blend_elapsed = 0.0F;
            instance.state_time = 0.0F;
            return;
        }
        if (running.interruption == Interruption::None) {
            return;
        }
    }

    // ONLY THE CURRENT STATE'S TRANSITIONS. The state machine is not re-descended from a root.
    const PoseState& state = program.states()[instance.state];
    const Transition* best = nullptr;
    u16 best_index = 0xFFFFU;
    for (u32 index = 0; index < state.transition_count; ++index) {
        const u32 slot = state.first_transition + index;
        const Transition& candidate = program.transitions()[slot];
        if (parameter_of(parameters, candidate.condition_param) == 0.0F) {
            continue;
        }
        if (instance.transition != 0xFFFFU) {
            const Transition& running = program.transitions()[instance.transition];
            const bool allowed =
                running.interruption == Interruption::Any || candidate.priority > running.priority;
            if (!allowed) {
                continue;
            }
        }
        if (best == nullptr || candidate.priority > best->priority) {
            best = &candidate;
            best_index = static_cast<u16>(slot);
        }
    }
    if (best == nullptr) {
        return;
    }
    if (best->duration <= 0.0F) {
        instance.state = best->target_state;
        instance.target = 0xFFFFU;
        instance.transition = 0xFFFFU;
        instance.blend_elapsed = 0.0F;
        instance.state_time = 0.0F;
        return;
    }
    instance.target = best->target_state;
    instance.transition = best_index;
    instance.blend_elapsed = 0.0F;
}

Status evaluate(const PoseProgram& program, const PoseInstance& instance,
                Span<const f32> parameters, PoseSampler& sampler, Span<f32> out,
                EvaluationReport& report) noexcept {
    if (instance.state >= program.states().size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this instance is in no state of this program", 0});
    }
    const u32 joints = program.joint_count();
    const usize stride = static_cast<usize>(joints) * kChannelsPerJoint;
    if (out.size() < stride) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the output pose is smaller than the skeleton", 0});
    }
    Array<f32> scratch(program.allocator());
    if (Status sized = scratch.resize(stride * (program.code().size() + 1)); !sized) {
        return sized;
    }
    for (f32& channel : scratch) {
        channel = 0.0F;
    }

    Evaluator evaluator;
    evaluator.program = &program;
    evaluator.parameters = parameters;
    evaluator.sampler = &sampler;
    evaluator.report = &report;
    evaluator.scratch = &scratch;
    evaluator.joints = joints;

    const PoseValue root = program.states()[instance.state].root;
    if (Status ran = evaluator.run(root); !ran) {
        return ran;
    }
    if (root != kNoPoseValue && root < program.code().size()) {
        const Span<f32> source = evaluator.slot(root);
        for (usize index = 0; index < stride; ++index) {
            out[index] = source[index];
        }
    }
    if (instance.transition == 0xFFFFU || instance.target >= program.states().size()) {
        return ok();
    }
    // A blend in flight evaluates the TARGET state's tree as well, and nothing else.
    const PoseValue target_root = program.states()[instance.target].root;
    if (Status ran = evaluator.run(target_root); !ran) {
        return ran;
    }
    const Transition& running = program.transitions()[instance.transition];
    const f32 weight = running.duration <= 0.0F ? 1.0F : instance.blend_elapsed / running.duration;
    if (target_root != kNoPoseValue && target_root < program.code().size()) {
        blend_into(out, evaluator.slot(target_root), JointMask::all(joints), weight, joints);
    }
    return ok();
}

// --- Assembly from a cooked program ----------------------------------------------------------

namespace {

[[nodiscard]] bool value_ok(PoseValue value, usize code) noexcept {
    return value == kNoPoseValue || value < code;
}

[[nodiscard]] Status check_instruction(const PoseInstruction& instruction, usize index,
                                       const PoseProgramParts& parts) noexcept {
    const usize code = parts.code.size();
    if (instruction.op >= PoseOp::Count) {
        return fail(ErrorCode::InvalidArgument, "a cooked pose program holds an unknown operation");
    }
    // An instruction reads only values computed before it: the compiler emits operands first, and
    // an evaluator that followed a forward or self reference would recurse without end.
    const bool ordered = (instruction.a == kNoPoseValue || instruction.a < index) &&
                         (instruction.b == kNoPoseValue || instruction.b < index);
    if (!value_ok(instruction.a, code) || !value_ok(instruction.b, code) || !ordered) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program instruction reads a value it does not have");
    }
    if (instruction.op == PoseOp::SampleClip &&
        (instruction.clip >= parts.clips.size() ||
         instruction.time_param >= parts.parameters.size())) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program samples a clip or reads a clock it does not declare");
    }
    const bool blends = instruction.op == PoseOp::Blend || instruction.op == PoseOp::BlendMask ||
                        instruction.op == PoseOp::Additive || instruction.op == PoseOp::Layer;
    if (blends && (instruction.mask >= parts.masks.size() ||
                   instruction.weight_param >= parts.parameters.size())) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program blends with a mask or a weight it does not declare");
    }
    return ok();
}

[[nodiscard]] Status check_machine(const PoseProgramParts& parts) noexcept {
    if (parts.states.empty() || parts.entry_state >= parts.states.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program has no states, or enters one it does not have");
    }
    for (const PoseState& state : parts.states) {
        if (!value_ok(state.root, parts.code.size()) ||
            static_cast<u64>(state.first_transition) + state.transition_count >
                parts.transitions.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked pose program state names a tree or transitions it does not have");
        }
    }
    for (const Transition& transition : parts.transitions) {
        if (transition.target_state >= parts.states.size() ||
            transition.condition_param >= parts.parameters.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked pose program transition names a state or a condition it does "
                        "not have");
        }
    }
    return ok();
}

template <class T>
[[nodiscard]] Status copy_into(Array<T>& out, Span<const T> from) noexcept {
    out.clear();
    return out.append(from);
}

}  // namespace

Expected<PoseProgram, Error> assemble_pose_program(Allocator& allocator,
                                                   const PoseProgramParts& parts) noexcept {
    if (parts.joint_count == 0 || parts.joint_count > kMaxJoints) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program addresses no joints, or more than a mask can");
    }
    for (usize index = 0; index < parts.code.size(); ++index) {
        if (Status checked = check_instruction(parts.code[index], index, parts); !checked) {
            return make_unexpected(checked.error());
        }
    }
    if (Status checked = check_machine(parts); !checked) {
        return make_unexpected(checked.error());
    }

    PoseProgram program(allocator);
    Status copied = copy_into(PoseProgramAccess::code(program), parts.code);
    if (copied) {
        copied = copy_into(PoseProgramAccess::states(program), parts.states);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::transitions(program), parts.transitions);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::clips(program), parts.clips);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::masks(program), parts.masks);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::parameters(program), parts.parameters);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::sync_groups(program), parts.sync_groups);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::markers(program), parts.markers);
    }
    if (!copied) {
        return make_unexpected(copied.error());
    }
    PoseProgramAccess::set_name(program, parts.name);
    PoseProgramAccess::set_entry(program, parts.entry_state);
    PoseProgramAccess::set_joints(program, parts.joint_count);
    PoseProgramAccess::set_digest(program, parts.digest);
    return program;
}

}  // namespace cy::graph::pose
