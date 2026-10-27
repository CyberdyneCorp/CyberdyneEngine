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
        case PoseOp::BlendSpace:
            return "blend_space";
        case PoseOp::FootPlacement:
            return "foot_placement";
        case PoseOp::Aim:
            return "aim";
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
      blend_spaces_(allocator),
      blend_samples_(allocator),
      blend_triangles_(allocator),
      foot_placements_(allocator),
      aims_(allocator),
      debug_(allocator) {}

// --- Blend-space weights --------------------------------------------------------------------

namespace {

struct Barycentric {
    f32 u = 1.0F;
    f32 v = 0.0F;
    f32 w = 0.0F;
};

[[nodiscard]] f32 dot2(f32 ax, f32 ay, f32 bx, f32 by) noexcept {
    return (ax * bx) + (ay * by);
}

/// The point of triangle `abc` nearest `p`, as barycentric coordinates: Ericson's region test
/// (Real-Time Collision Detection, 5.1.5) in two dimensions. Inside the triangle it is `p` itself.
[[nodiscard]] Barycentric nearest_on_triangle(const BlendSample& a, const BlendSample& b,
                                              const BlendSample& c, f32 px, f32 py) noexcept {
    const f32 abx = b.x - a.x;
    const f32 aby = b.y - a.y;
    const f32 acx = c.x - a.x;
    const f32 acy = c.y - a.y;
    const f32 d1 = dot2(abx, aby, px - a.x, py - a.y);
    const f32 d2 = dot2(acx, acy, px - a.x, py - a.y);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return {1.0F, 0.0F, 0.0F};
    }
    const f32 d3 = dot2(abx, aby, px - b.x, py - b.y);
    const f32 d4 = dot2(acx, acy, px - b.x, py - b.y);
    if (d3 >= 0.0F && d4 <= d3) {
        return {0.0F, 1.0F, 0.0F};
    }
    const f32 vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const f32 v = d1 / (d1 - d3);
        return {1.0F - v, v, 0.0F};
    }
    const f32 d5 = dot2(abx, aby, px - c.x, py - c.y);
    const f32 d6 = dot2(acx, acy, px - c.x, py - c.y);
    if (d6 >= 0.0F && d5 <= d6) {
        return {0.0F, 0.0F, 1.0F};
    }
    const f32 vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const f32 w = d2 / (d2 - d6);
        return {1.0F - w, 0.0F, w};
    }
    const f32 va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0.0F, 1.0F - w, w};
    }
    const f32 denominator = 1.0F / (va + vb + vc);
    const f32 v = vb * denominator;
    const f32 w = vc * denominator;
    return {1.0F - v - w, v, w};
}

void weights_1d(Span<const BlendSample> samples, f32 x, Span<f32> out) noexcept {
    const usize last = samples.size() - 1;
    if (x <= samples[0].x) {
        out[0] = 1.0F;
        return;
    }
    if (x >= samples[last].x) {
        out[last] = 1.0F;
        return;
    }
    for (usize index = 0; index < last; ++index) {
        const f32 low = samples[index].x;
        const f32 high = samples[index + 1].x;
        if (x >= low && x < high) {
            const f32 t = (x - low) / (high - low);
            out[index] = 1.0F - t;
            out[index + 1] = t;
            return;
        }
    }
    out[last] = 1.0F;
}

void weights_2d(Span<const BlendSample> samples, Span<const BlendTriangle> triangles, f32 x, f32 y,
                Span<f32> out) noexcept {
    // The first triangle at the smallest distance: zero inside, so the first triangle containing
    // the point wins, and outside the hull the nearest point of its edge is weighted.
    usize best = 0;
    Barycentric best_weights;
    f32 best_distance = -1.0F;
    for (usize index = 0; index < triangles.size(); ++index) {
        const BlendTriangle& triangle = triangles[index];
        const BlendSample& a = samples[triangle.sample[0]];
        const BlendSample& b = samples[triangle.sample[1]];
        const BlendSample& c = samples[triangle.sample[2]];
        const Barycentric weights = nearest_on_triangle(a, b, c, x, y);
        const f32 nx = (weights.u * a.x) + (weights.v * b.x) + (weights.w * c.x);
        const f32 ny = (weights.u * a.y) + (weights.v * b.y) + (weights.w * c.y);
        const f32 distance = ((nx - x) * (nx - x)) + ((ny - y) * (ny - y));
        if (best_distance < 0.0F || distance < best_distance) {
            best = index;
            best_weights = weights;
            best_distance = distance;
        }
        if (distance == 0.0F) {
            break;
        }
    }
    const f32 u = best_weights.u > 0.0F ? best_weights.u : 0.0F;
    const f32 v = best_weights.v > 0.0F ? best_weights.v : 0.0F;
    const f32 w = best_weights.w > 0.0F ? best_weights.w : 0.0F;
    const f32 total = u + v + w;
    const BlendTriangle& triangle = triangles[best];
    if (!(total > 0.0F)) {
        // A parameter that is not a number lands on a sample rather than weighting none.
        out[triangle.sample[0]] += 1.0F;
        return;
    }
    out[triangle.sample[0]] += u / total;
    out[triangle.sample[1]] += v / total;
    out[triangle.sample[2]] += w / total;
}

}  // namespace

bool blend_weights_at(u8 dimensions, Span<const BlendSample> samples,
                      Span<const BlendTriangle> triangles, f32 x, f32 y, Span<f32> out) noexcept {
    if (samples.empty() || out.size() < samples.size()) {
        return false;
    }
    for (usize index = 0; index < samples.size(); ++index) {
        out[index] = 0.0F;
    }
    if (samples.size() == 1) {
        out[0] = 1.0F;
        return true;
    }
    if (dimensions == 1) {
        weights_1d(samples, x, out);
        return true;
    }
    for (const BlendTriangle& triangle : triangles) {
        for (const u16 vertex : triangle.sample) {
            if (vertex >= samples.size()) {
                return false;
            }
        }
    }
    if (triangles.empty()) {
        return false;
    }
    weights_2d(samples, triangles, x, y, out);
    return true;
}

bool blend_space_weights(const PoseProgram& program, u32 space, Span<const f32> parameters,
                         Span<f32> out) noexcept {
    if (space >= program.blend_spaces().size()) {
        return false;
    }
    const BlendSpace& entry = program.blend_spaces()[space];
    const Span<const BlendSample> samples =
        program.blend_samples().subspan(entry.first_sample, entry.sample_count);
    const Span<const BlendTriangle> triangles =
        program.blend_triangles().subspan(entry.first_triangle, entry.triangle_count);
    const f32 x = entry.x_param < parameters.size() ? parameters[entry.x_param] : 0.0F;
    const f32 y = entry.dimensions == 2 && entry.y_param < parameters.size()
                      ? parameters[entry.y_param]
                      : 0.0F;
    return blend_weights_at(entry.dimensions, samples, triangles, x, y, out);
}

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
    [[nodiscard]] Status blend_space(const PoseInstruction& instruction, Span<f32> out) noexcept;
    [[nodiscard]] Status modify(const PoseInstruction& instruction, Span<f32> out) noexcept;
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
        case PoseOp::IK:
        case PoseOp::FootPlacement:
        case PoseOp::Aim:
            return modify(instruction, out);
        case PoseOp::BlendSpace:
            return blend_space(instruction, out);
        case PoseOp::Count:
            break;
    }
    return ok();
}

Status Evaluator::modify(const PoseInstruction& instruction, Span<f32> out) noexcept {
    if (Status ran = run(instruction.a); !ran) {
        return ran;
    }
    const Span<f32> base = slot(instruction.a);
    for (usize index = 0; index < out.size(); ++index) {
        out[index] = base[index];
    }
    // A foot placement or an aim needs a skeleton and, for feet, the ground: neither is the
    // reference evaluator's, so it passes the pose through. The runtime evaluator solves them.
    if (instruction.op == PoseOp::IK) {
        sampler->solve_ik(instruction.chain, instruction.required, out);
    }
    return ok();
}

Status Evaluator::blend_space(const PoseInstruction& instruction, Span<f32> out) noexcept {
    f32 weights[kMaxBlendSamples] = {};
    if (!blend_space_weights(*program, instruction.clip, parameters,
                             Span<f32>(weights, kMaxBlendSamples))) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this instruction names no blend space", 0});
    }
    for (f32& channel : out) {
        channel = 0.0F;
    }
    const BlendSpace& space = program->blend_spaces()[instruction.clip];
    for (u32 index = 0; index < space.sample_count; ++index) {
        if (weights[index] <= 0.0F) {
            continue;
        }
        const PoseValue value = program->blend_samples()[space.first_sample + index].value;
        if (Status ran = run(value); !ran) {
            return ran;
        }
        add_into(out, slot(value), instruction.required, weights[index], joints);
    }
    return ok();
}

}  // namespace

f32 transition_duration(const PoseProgram& program, const PoseInstance& instance) noexcept {
    if (instance.transition == kRequestedTransition) {
        return instance.requested_duration;
    }
    if (instance.transition >= program.transitions().size()) {
        return 0.0F;
    }
    return program.transitions()[instance.transition].duration;
}

u16 find_state(const PoseProgram& program, Name name) noexcept {
    const Span<const PoseState> states = program.states();
    for (usize index = 0; index < states.size(); ++index) {
        if (states[index].name == name) {
            return static_cast<u16>(index);
        }
    }
    return 0xFFFFU;
}

bool request_state(const PoseProgram& program, PoseInstance& instance, u16 state,
                   f32 seconds) noexcept {
    if (state >= program.states().size()) {
        return false;
    }
    const bool blending = instance.transition != 0xFFFFU;
    if ((!blending && instance.state == state) || (blending && instance.target == state)) {
        return true;
    }
    if (seconds <= 0.0F) {
        instance.state = state;
        instance.target = 0xFFFFU;
        instance.transition = 0xFFFFU;
        instance.blend_elapsed = 0.0F;
        instance.state_time = 0.0F;
        return true;
    }
    instance.target = state;
    instance.transition = kRequestedTransition;
    instance.requested_duration = seconds;
    instance.blend_elapsed = 0.0F;
    return true;
}

void advance(const PoseProgram& program, PoseInstance& instance, Span<const f32> parameters,
             f32 dt) noexcept {
    if (instance.state >= program.states().size()) {
        return;
    }
    instance.state_time += dt;
    if (instance.transition == kRequestedTransition) {
        // A HOST'S BLEND runs to completion; the program is consulted again where it lands.
        instance.blend_elapsed += dt;
        if (instance.blend_elapsed >= instance.requested_duration) {
            instance.state = instance.target;
            instance.target = 0xFFFFU;
            instance.transition = 0xFFFFU;
            instance.blend_elapsed = 0.0F;
            instance.state_time = 0.0F;
        }
        return;
    }
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
    const f32 duration = transition_duration(program, instance);
    const f32 weight = duration <= 0.0F ? 1.0F : instance.blend_elapsed / duration;
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

/// A blend space reads its samples' poses, so they are operands like `a` and `b`: in range, and
/// computed before it.
[[nodiscard]] Status check_blend_space(const PoseInstruction& instruction, usize index,
                                       const PoseProgramParts& parts) noexcept {
    if (instruction.clip >= parts.blend_spaces.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program blends a space it does not declare");
    }
    const BlendSpace& space = parts.blend_spaces[instruction.clip];
    const usize parameters = parts.parameters.size();
    const bool shaped =
        (space.dimensions == 1 || space.dimensions == 2) && space.sample_count != 0 &&
        space.sample_count <= kMaxBlendSamples &&
        static_cast<u64>(space.first_sample) + space.sample_count <= parts.blend_samples.size() &&
        static_cast<u64>(space.first_triangle) + space.triangle_count <=
            parts.blend_triangles.size() &&
        space.x_param < parameters && space.y_param < parameters && space.phase_param < parameters;
    if (!shaped) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program blend space names samples, triangles or parameters it "
                    "does not have");
    }
    for (u32 sample = 0; sample < space.sample_count; ++sample) {
        const PoseValue value = parts.blend_samples[space.first_sample + sample].value;
        if (value == kNoPoseValue || value >= index) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked pose program blend space samples a pose it does not have");
        }
    }
    for (u32 triangle = 0; triangle < space.triangle_count; ++triangle) {
        for (const u16 vertex : parts.blend_triangles[space.first_triangle + triangle].sample) {
            if (vertex >= space.sample_count) {
                return fail(ErrorCode::InvalidArgument,
                            "a cooked pose program blend triangle names a sample it does not "
                            "have");
            }
        }
    }
    if (space.dimensions == 2 && space.sample_count > 1 && space.triangle_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked two-dimensional blend space has samples and no triangles");
    }
    return ok();
}

[[nodiscard]] Status check_modifiers(const PoseProgramParts& parts) noexcept {
    for (const AimRef& aim : parts.aims) {
        bool targets = aim.joint_count != 0 && aim.joint_count <= kMaxAimJoints;
        for (const u16 parameter : aim.target_param) {
            targets = targets && parameter < parts.parameters.size();
        }
        if (!targets) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked aim chain has no joints, too many, or a target it does not "
                        "declare");
        }
    }
    return ok();
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
    if (instruction.op == PoseOp::BlendSpace) {
        return check_blend_space(instruction, index, parts);
    }
    const bool modifies = instruction.op == PoseOp::FootPlacement || instruction.op == PoseOp::Aim;
    if (!modifies) {
        return ok();
    }
    const usize table =
        instruction.op == PoseOp::FootPlacement ? parts.foot_placements.size() : parts.aims.size();
    if (instruction.chain >= table || instruction.weight_param >= parts.parameters.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program modifies a pose with a setup or a weight it does not "
                    "declare");
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

/// Every table of `parts` into `program`, stopping at the first that does not fit.
[[nodiscard]] Status copy_parts(PoseProgram& program, const PoseProgramParts& parts) noexcept {
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
    if (copied) {
        copied = copy_into(PoseProgramAccess::blend_spaces(program), parts.blend_spaces);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::blend_samples(program), parts.blend_samples);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::blend_triangles(program), parts.blend_triangles);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::foot_placements(program), parts.foot_placements);
    }
    if (copied) {
        copied = copy_into(PoseProgramAccess::aims(program), parts.aims);
    }
    return copied;
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
    Status checked = check_machine(parts);
    if (checked) {
        checked = check_modifiers(parts);
    }
    if (!checked) {
        return make_unexpected(checked.error());
    }

    PoseProgram program(allocator);
    Status copied = copy_parts(program, parts);
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
