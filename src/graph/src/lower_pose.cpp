// The animation IR's compiler and its dependency analysis. Task 2.4. The runtime half — the
// program's storage, the state machine and the reference evaluator — is pose_program.cpp, split
// out so a runtime that loads a cooked program links no compiler (issue #76).
//
// See lower_pose.h for why animation keeps its own IR rather than lowering through the shared
// expression core, and for what makes the program lazy.

#include <cy/graph/lower_pose.h>

#include "blend_triangulation.h"
#include "pose_program_access.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace cy::graph::pose {

// --- Compilation ------------------------------------------------------------------------------

namespace {

[[nodiscard]] PinDesc pin(const char* name, const char* type, PinDirection direction) noexcept {
    PinDesc desc;
    desc.name = Name::intern(name);
    desc.type = Name::intern(type);
    desc.direction = direction;
    return desc;
}

[[nodiscard]] Status register_node(NodeRegistry& registry, const char* type,
                                   Span<const PinDesc> pins) noexcept {
    NodeTypeDesc desc;
    desc.name = Name::intern(type);
    desc.plugin = Name::intern("cy.graph.pose");
    desc.pins = pins;
    desc.pure = true;
    return registry.register_type(desc);
}

struct Compilation {
    const Graph* graph = nullptr;
    PoseProgram* program = nullptr;
    DiagnosticSink* sink = nullptr;
};

[[nodiscard]] u16 intern_parameter(PoseProgram& program, Name name) noexcept {
    Array<Name>& parameters = PoseProgramAccess::parameters(program);
    for (usize index = 0; index < parameters.size(); ++index) {
        if (parameters[index] == name) {
            return static_cast<u16>(index);
        }
    }
    if (!parameters.push_back(name)) {
        return 0;
    }
    return static_cast<u16>(parameters.size() - 1);
}

[[nodiscard]] NodeKey source_of(const Graph& graph, NodeKey node, const char* pin_name) noexcept {
    const Name pin = Name::intern(pin_name);
    NodeKey source = kInvalidNodeKey;
    for (const Link& link : graph.links()) {
        if (link.to == node && link.to_pin == pin) {
            source = link.from;
        }
    }
    return source;
}

[[nodiscard]] Expected<PoseValue, Error> lower_value(Compilation& state, NodeKey node) noexcept;

/// The clock a clip node owns when it names none: `clock.<node key>`, which no other node shares.
[[nodiscard]] Name own_clock(NodeKey node) noexcept {
    char name[40] = {};
    (void)std::snprintf(name, sizeof(name), "clock.%llu", static_cast<unsigned long long>(node));
    return Name::intern(name);
}

/// The `pose.clip` branch of `lower_value`, lifted out because it is the only one that mints a
/// `ClipRef` and the only one with a table to deduplicate against — the rest of that function is a
/// chain of one-line assignments, and reading them together made the chain hard to see.
[[nodiscard]] Status lower_clip(Compilation& state, NodeKey node,
                                PoseInstruction& instruction) noexcept {
    PoseProgram& program = *state.program;
    instruction.op = PoseOp::SampleClip;
    const Literal* clip = state.graph->property(node, Name::intern("clip"));
    const Literal* duration = state.graph->property(node, Name::intern("duration"));
    ClipRef reference;
    reference.name = clip != nullptr ? clip->text : Name{};
    reference.duration = duration != nullptr ? duration->value.x : 1.0F;
    // `animation-and-skinning`'s node table: "Clip | Plays a clip with speed and LOOP CONTROL". A
    // `ClipRef` is the whole of what a compiled program says about a clip, so if the loop mode is
    // not authored here the runtime has nowhere else to read it from — and the difference is not
    // cosmetic: a death or a hit reaction that wraps returns the character to the moment it
    // started. Absent, the property means the usual case, a looping locomotion clip, which is what
    // this field already defaulted to.
    const Literal* loop = state.graph->property(node, Name::intern("loop"));
    reference.looping = loop == nullptr || loop->value.mask != 0;

    Array<ClipRef>& clips = PoseProgramAccess::clips(program);
    instruction.clip = static_cast<u16>(clips.size());
    for (usize index = 0; index < clips.size(); ++index) {
        if (clips[index].name != reference.name) {
            continue;
        }
        instruction.clip = static_cast<u16>(index);
        // ONE `ClipRef` PER CLIP NAME, so two nodes naming one clip have to agree about how it
        // plays. Keeping the first silently would make the answer depend on the order states
        // happened to be numbered in, which is a property of the node keys and not of anything the
        // author decided.
        if (clips[index].looping != reference.looping ||
            clips[index].duration != reference.duration) {
            Diagnostic conflict;
            conflict.severity = Severity::Warning;
            conflict.node = node;
            conflict.detail = reference.name;
            conflict.message =
                "this clip is already referenced with a different duration or loop mode, and a "
                "compiled program carries one reference per clip: the first one is what it keeps";
            state.sink->report(conflict);
        }
    }
    if (instruction.clip == clips.size()) {
        if (Status pushed = clips.push_back(reference); !pushed) {
            return pushed;
        }
    }
    // A CLIP WITH NO NAMED CLOCK HAS A CLOCK OF ITS OWN. Every clip instruction's time parameter is
    // a clock the runtime advances (`AnimationRig::time_parameters`), and two clips reading one
    // parameter read one clock: an unnamed clock interned as the empty name made every clip that
    // left the property blank — what a node fresh from an editor's palette is — share a single one.
    const Literal* time = state.graph->property(node, Name::intern("time_parameter"));
    instruction.time_param = intern_parameter(
        program, time != nullptr && !time->text.is_empty() ? time->text : own_clock(node));
    return ok();
}

[[nodiscard]] Expected<PoseValue, Error> lower_input(Compilation& state, NodeKey node,
                                                     const char* pin_name) noexcept {
    const NodeKey source = source_of(*state.graph, node, pin_name);
    if (source == kInvalidNodeKey) {
        return kNoPoseValue;
    }
    return lower_value(state, source);
}

[[nodiscard]] u16 mask_index(PoseProgram& program, const Graph& graph, NodeKey node) noexcept {
    const Literal* first = graph.property(node, Name::intern("mask_first"));
    const Literal* count = graph.property(node, Name::intern("mask_count"));
    JointMask mask = JointMask::all(program.joint_count());
    if (first != nullptr && count != nullptr) {
        mask = JointMask::range(first->value.mask, count->value.mask);
    }
    Array<JointMask>& masks = PoseProgramAccess::masks(program);
    for (usize index = 0; index < masks.size(); ++index) {
        if (masks[index] == mask) {
            return static_cast<u16>(index);
        }
    }
    if (!masks.push_back(mask)) {
        return 0;
    }
    return static_cast<u16>(masks.size() - 1);
}

// --- Blend spaces and modifiers -----------------------------------------------------------

void report_error(Compilation& state, NodeKey node, const char* code, const char* message,
                  Name detail = Name{}) noexcept {
    Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.node = node;
    diagnostic.message = message;
    diagnostic.detail = detail;
    state.sink->report(diagnostic);
}

[[nodiscard]] f32 number_or(const Graph& graph, NodeKey node, const char* name,
                            f32 fallback) noexcept {
    const Literal* literal = graph.property(node, Name::intern(name));
    return literal != nullptr ? literal->value.x : fallback;
}

[[nodiscard]] Name name_of(const Graph& graph, NodeKey node, const char* name) noexcept {
    const Literal* literal = graph.property(node, Name::intern(name));
    return literal != nullptr ? literal->text : Name{};
}

/// `prefix.x`, `prefix.y` or `prefix.z`.
[[nodiscard]] Name component(Name prefix, char axis) noexcept {
    char text[96] = {};
    (void)std::snprintf(text, sizeof(text), "%.*s.%c", static_cast<int>(prefix.text().size()),
                        prefix.text().data(), axis);
    return Name::intern(text);
}

/// The sample pins a blend space reads, `s0` to `s15`.
[[nodiscard]] const char* sample_pin(u32 index) noexcept {
    static constexpr const char* kPins[kMaxBlendSamples] = {"s0",  "s1",  "s2",  "s3", "s4",  "s5",
                                                            "s6",  "s7",  "s8",  "s9", "s10", "s11",
                                                            "s12", "s13", "s14", "s15"};
    return kPins[index];
}

/// The wired samples of a blend-space node, in pin order, each lowered and placed. False, with a
/// diagnostic, when a wired sample has no position or two samples share one.
[[nodiscard]] Expected<bool, Error> gather_samples(Compilation& state, NodeKey node, bool planar,
                                                   Array<BlendSample>& samples) noexcept {
    for (u32 index = 0; index < kMaxBlendSamples; ++index) {
        if (source_of(*state.graph, node, sample_pin(index)) == kInvalidNodeKey) {
            continue;
        }
        const Literal* position = state.graph->property(node, Name::intern(sample_pin(index)));
        if (position == nullptr) {
            report_error(state, node, "pose.blend_space.position",
                         "a wired blend-space sample needs its position: the property named as "
                         "its pin",
                         Name::intern(sample_pin(index)));
            return false;
        }
        auto value = lower_input(state, node, sample_pin(index));
        if (!value) {
            return make_unexpected(value.error());
        }
        if (value.value() == kNoPoseValue) {
            return false;
        }
        BlendSample sample;
        sample.x = position->value.x;
        sample.y = planar ? position->value.y : 0.0F;
        sample.value = value.value();
        for (const BlendSample& other : samples) {
            if (other.x == sample.x && other.y == sample.y) {
                report_error(state, node, "pose.blend_space.duplicate",
                             "two blend-space samples sit at one position, so neither could be "
                             "weighted alone",
                             Name::intern(sample_pin(index)));
                return false;
            }
        }
        if (Status pushed = samples.push_back(sample); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (samples.empty()) {
        report_error(state, node, "pose.blend_space.empty",
                     "a blend space needs at least one wired sample");
        return false;
    }
    return true;
}

/// `pose.blend1d` and `pose.blend2d`. The samples are stored sorted along x in one dimension and
/// triangulated in two, so the runtime finds its interval or its triangle and builds nothing.
[[nodiscard]] Status lower_blend_space(Compilation& state, NodeKey node, bool planar,
                                       PoseInstruction& instruction) noexcept {
    PoseProgram& program = *state.program;
    Array<BlendSample> samples(state.graph->allocator());
    Expected<bool, Error> gathered = gather_samples(state, node, planar, samples);
    if (!gathered) {
        return Status{make_unexpected(gathered.error())};
    }
    if (!gathered.value()) {
        instruction.op = PoseOp::RefPose;
        return ok();
    }
    if (!planar) {
        std::ranges::sort(
            samples, [](const BlendSample& a, const BlendSample& b) noexcept { return a.x < b.x; });
    }

    BlendSpace space;
    space.dimensions = planar ? 2 : 1;
    space.origin = node;
    const Literal* sync = state.graph->property(node, Name::intern("sync"));
    space.sync = sync != nullptr && sync->value.mask != 0;
    space.x_param = intern_parameter(program, name_of(*state.graph, node, "x_parameter"));
    space.y_param = planar ? intern_parameter(program, name_of(*state.graph, node, "y_parameter"))
                           : space.x_param;
    space.phase_param = space.x_param;
    if (space.sync) {
        char phase[40] = {};
        (void)std::snprintf(phase, sizeof(phase), "phase.%llu",
                            static_cast<unsigned long long>(node));
        space.phase_param = intern_parameter(program, Name::intern(phase));
    }

    Array<BlendSample>& stored = PoseProgramAccess::blend_samples(program);
    Array<BlendTriangle>& triangles = PoseProgramAccess::blend_triangles(program);
    space.first_sample = static_cast<u32>(stored.size());
    space.sample_count = static_cast<u32>(samples.size());
    space.first_triangle = static_cast<u32>(triangles.size());
    if (planar && samples.size() > 1) {
        if (Status triangulated = triangulate_blend_space(samples.span(), triangles);
            !triangulated) {
            report_error(state, node, "pose.blend_space.degenerate",
                         "a two-dimensional blend space needs three samples that are not all on "
                         "one line");
            instruction.op = PoseOp::RefPose;
            return ok();
        }
    }
    space.triangle_count = static_cast<u32>(triangles.size()) - space.first_triangle;
    if (Status appended = stored.append(samples.span()); !appended) {
        return appended;
    }
    Array<BlendSpace>& spaces = PoseProgramAccess::blend_spaces(program);
    instruction.clip = static_cast<u16>(spaces.size());
    return spaces.push_back(space);
}

/// `pose.foot_ik`: the legs named on the node, resolved against a skeleton when a rig is bound.
[[nodiscard]] Status lower_foot_placement(Compilation& state, NodeKey node,
                                          PoseInstruction& instruction) noexcept {
    const Graph& graph = *state.graph;
    FootPlacementRef foot;
    foot.origin = node;
    foot.pelvis = name_of(graph, node, "pelvis");
    static constexpr const char* kLegs[2][3] = {{"hip_l", "knee_l", "ankle_l"},
                                                {"hip_r", "knee_r", "ankle_r"}};
    bool named = !foot.pelvis.is_empty();
    for (u32 leg = 0; leg < 2; ++leg) {
        for (u32 joint = 0; joint < 3; ++joint) {
            foot.leg[leg][joint] = name_of(graph, node, kLegs[leg][joint]);
            named = named && !foot.leg[leg][joint].is_empty();
        }
    }
    if (!named) {
        report_error(state, node, "pose.foot_ik.joints",
                     "foot placement names its pelvis and both legs' hip, knee and ankle");
        instruction.op = PoseOp::RefPose;
        return ok();
    }
    foot.ankle_height = number_or(graph, node, "ankle_height", foot.ankle_height);
    foot.probe_above = number_or(graph, node, "probe_above", foot.probe_above);
    foot.probe_below = number_or(graph, node, "probe_below", foot.probe_below);
    foot.max_pelvis_drop = number_or(graph, node, "max_pelvis_drop", foot.max_pelvis_drop);
    foot.max_foot_raise = number_or(graph, node, "max_foot_raise", foot.max_foot_raise);
    foot.smoothing = number_or(graph, node, "smoothing", foot.smoothing);
    foot.max_tilt = number_or(graph, node, "max_tilt", foot.max_tilt);
    if (const Literal* pole = graph.property(node, Name::intern("pole")); pole != nullptr) {
        foot.pole[0] = pole->value.x;
        foot.pole[1] = pole->value.y;
        foot.pole[2] = pole->value.z;
    }
    Array<FootPlacementRef>& table = PoseProgramAccess::foot_placements(*state.program);
    instruction.op = PoseOp::FootPlacement;
    instruction.chain = static_cast<u16>(table.size());
    return table.push_back(foot);
}

/// `pose.aim`: `chain` is the joints from the chain's root to the one that points, separated by
/// commas; `target_parameter` names the point, read as `<name>.x`, `.y` and `.z`.
[[nodiscard]] Status lower_aim(Compilation& state, NodeKey node,
                               PoseInstruction& instruction) noexcept {
    const Graph& graph = *state.graph;
    AimRef aim;
    aim.origin = node;
    const std::string_view chain = name_of(graph, node, "chain").text();
    usize begin = 0;
    while (begin < chain.size()) {
        usize end = chain.find(',', begin);
        end = end == std::string_view::npos ? chain.size() : end;
        std::string_view joint = chain.substr(begin, end - begin);
        while (!joint.empty() && joint.front() == ' ') {
            joint.remove_prefix(1);
        }
        while (!joint.empty() && joint.back() == ' ') {
            joint.remove_suffix(1);
        }
        if (!joint.empty()) {
            if (aim.joint_count == kMaxAimJoints) {
                report_error(state, node, "pose.aim.chain",
                             "an aim chain turns at most four joints");
                instruction.op = PoseOp::RefPose;
                return ok();
            }
            aim.joints[aim.joint_count++] = Name::intern(joint);
        }
        begin = end + 1;
    }
    const Name target = name_of(graph, node, "target_parameter");
    if (aim.joint_count == 0 || target.is_empty()) {
        report_error(state, node, "pose.aim.setup",
                     "an aim names the joints of its chain and the parameter its target is in");
        instruction.op = PoseOp::RefPose;
        return ok();
    }
    if (const Literal* axis = graph.property(node, Name::intern("axis")); axis != nullptr) {
        aim.axis[0] = axis->value.x;
        aim.axis[1] = axis->value.y;
        aim.axis[2] = axis->value.z;
    }
    aim.max_angle = number_or(graph, node, "max_angle", aim.max_angle);
    aim.target_param[0] = intern_parameter(*state.program, component(target, 'x'));
    aim.target_param[1] = intern_parameter(*state.program, component(target, 'y'));
    aim.target_param[2] = intern_parameter(*state.program, component(target, 'z'));
    Array<AimRef>& table = PoseProgramAccess::aims(*state.program);
    instruction.op = PoseOp::Aim;
    instruction.chain = static_cast<u16>(table.size());
    return table.push_back(aim);
}

/// `pose.blend`, `pose.blend_mask`, `pose.additive` and `pose.layer`: two operands, a mask and a
/// weight.
[[nodiscard]] Status lower_binary(Compilation& state, NodeKey node, std::string_view type,
                                  PoseInstruction& instruction) noexcept {
    PoseProgram& program = *state.program;
    instruction.op = PoseOp::Layer;
    if (type == "pose.blend") {
        instruction.op = PoseOp::Blend;
    } else if (type == "pose.blend_mask") {
        instruction.op = PoseOp::BlendMask;
    } else if (type == "pose.additive") {
        instruction.op = PoseOp::Additive;
    }
    auto a = lower_input(state, node, "a");
    if (!a) {
        return Status{make_unexpected(a.error())};
    }
    auto b = lower_input(state, node, "b");
    if (!b) {
        return Status{make_unexpected(b.error())};
    }
    instruction.a = a.value();
    instruction.b = b.value();
    instruction.mask = mask_index(program, *state.graph, node);
    instruction.weight_param =
        intern_parameter(program, name_of(*state.graph, node, "weight_parameter"));
    return ok();
}

/// `pose.ik`, `pose.foot_ik` and `pose.aim`: one operand modified. A foot placement or an aim that
/// was refused leaves the instruction a `RefPose`, which `lower_value` emits nothing for.
[[nodiscard]] Status lower_modifier(Compilation& state, NodeKey node, std::string_view type,
                                    PoseInstruction& instruction) noexcept {
    auto a = lower_input(state, node, "a");
    if (!a) {
        return Status{make_unexpected(a.error())};
    }
    instruction.a = a.value();
    if (type == "pose.ik") {
        instruction.op = PoseOp::IK;
        const Literal* chain = state.graph->property(node, Name::intern("chain"));
        instruction.chain = chain != nullptr ? static_cast<u16>(chain->value.mask) : 0;
        return ok();
    }
    instruction.weight_param =
        intern_parameter(*state.program, name_of(*state.graph, node, "weight_parameter"));
    return type == "pose.foot_ik" ? lower_foot_placement(state, node, instruction)
                                  : lower_aim(state, node, instruction);
}

[[nodiscard]] bool is_binary(std::string_view type) noexcept {
    return type == "pose.blend" || type == "pose.blend_mask" || type == "pose.additive" ||
           type == "pose.layer";
}

[[nodiscard]] bool is_modifier(std::string_view type) noexcept {
    return type == "pose.ik" || type == "pose.foot_ik" || type == "pose.aim";
}

/// Fill `instruction` for the node of type `type`. The op stays `RefPose` for a node that produces
/// nothing — a refused blend space, foot placement or aim — and `Count` for one that is not an
/// animation node at all.
[[nodiscard]] Status lower_instruction(Compilation& state, NodeKey node, std::string_view type,
                                       PoseInstruction& instruction) noexcept {
    if (type == "pose.ref") {
        instruction.op = PoseOp::RefPose;
        return ok();
    }
    if (type == "pose.clip") {
        return lower_clip(state, node, instruction);
    }
    if (is_binary(type)) {
        return lower_binary(state, node, type, instruction);
    }
    if (type == "pose.blend1d" || type == "pose.blend2d") {
        instruction.op = PoseOp::BlendSpace;
        return lower_blend_space(state, node, type == "pose.blend2d", instruction);
    }
    if (is_modifier(type)) {
        return lower_modifier(state, node, type, instruction);
    }
    instruction.op = PoseOp::Count;
    return ok();
}

Expected<PoseValue, Error> lower_value(Compilation& state, NodeKey node) noexcept {
    const GraphNode* authored = state.graph->find_node(node);
    if (authored == nullptr) {
        return kNoPoseValue;
    }
    PoseProgram& program = *state.program;
    PoseInstruction instruction;
    instruction.origin = node;
    if (Status lowered = lower_instruction(state, node, authored->type.text(), instruction);
        !lowered) {
        return make_unexpected(lowered.error());
    }
    if (instruction.op == PoseOp::Count) {
        Diagnostic diagnostic;
        diagnostic.node = node;
        diagnostic.detail = authored->type;
        diagnostic.message = "this node is not an animation node and cannot produce a pose";
        state.sink->report(diagnostic);
        return kNoPoseValue;
    }
    if (instruction.op == PoseOp::RefPose && authored->type != Name::intern("pose.ref")) {
        // A blend space, foot placement or aim that was refused, its diagnostic reported.
        return kNoPoseValue;
    }

    Array<PoseInstruction>& code = PoseProgramAccess::code(program);
    const auto value = static_cast<PoseValue>(code.size());
    if (Status pushed = code.push_back(instruction); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status recorded = PoseProgramAccess::debug(program).record(value, node); !recorded) {
        return make_unexpected(recorded.error());
    }
    return value;
}

/// Every sample may carry weight, so every sample of a blend space is read wherever it is.
void require_samples(PoseProgram& program, const PoseInstruction& instruction) noexcept {
    Array<PoseInstruction>& code = PoseProgramAccess::code(program);
    const BlendSpace& space = program.blend_spaces()[instruction.clip];
    for (u32 sample = 0; sample < space.sample_count; ++sample) {
        const PoseValue value = program.blend_samples()[space.first_sample + sample].value;
        if (value < code.size()) {
            code[value].required = code[value].required.merged(instruction.required);
        }
    }
}

/// POSE DEPENDENCY ANALYSIS. Push each state root's mask down through the tree; an instruction
/// nothing reads keeps an empty mask and the evaluator skips it, and a masked layer's operand is
/// narrowed to the joints that layer actually contributes.
void analyse_dependencies(PoseProgram& program) noexcept {
    Array<PoseInstruction>& code = PoseProgramAccess::code(program);
    for (PoseInstruction& instruction : code) {
        instruction.required = JointMask{};
    }
    const JointMask everything = JointMask::all(program.joint_count());
    for (const PoseState& state : program.states()) {
        if (state.root != kNoPoseValue && state.root < code.size()) {
            code[state.root].required = code[state.root].required.merged(everything);
        }
    }
    // The instructions are in post-order — an operand is always emitted before its consumer — so
    // one backwards sweep propagates every mask.
    for (usize index = code.size(); index > 0; --index) {
        PoseInstruction& instruction = code[index - 1];
        if (instruction.required.empty()) {
            continue;
        }
        if (instruction.op == PoseOp::BlendSpace &&
            instruction.clip < program.blend_spaces().size()) {
            require_samples(program, instruction);
            continue;
        }
        const JointMask layer_mask = instruction.mask < program.masks().size()
                                         ? program.masks()[instruction.mask]
                                         : everything;
        if (instruction.a != kNoPoseValue && instruction.a < code.size()) {
            code[instruction.a].required =
                code[instruction.a].required.merged(instruction.required);
        }
        if (instruction.b == kNoPoseValue || instruction.b >= code.size()) {
            continue;
        }
        const bool masked = instruction.op == PoseOp::BlendMask || instruction.op == PoseOp::Layer;
        const JointMask contributed =
            masked ? instruction.required.intersected(layer_mask) : instruction.required;
        code[instruction.b].required = code[instruction.b].required.merged(contributed);
    }
}

/// The blend spaces, foot placements and aims. A program with none hashes exactly as it did before
/// they existed, so a cook keyed on the digest of an older program still matches.
[[nodiscard]] u64 hash_modifiers(u64 digest, const PoseProgram& program) noexcept {
    for (const BlendSpace& space : program.blend_spaces()) {
        digest = hash_u64(digest, (static_cast<u64>(space.dimensions) << 8U) |
                                      static_cast<u64>(space.sync ? 1U : 0U));
        digest = hash_u64(digest, (static_cast<u64>(space.x_param) << 32U) |
                                      (static_cast<u64>(space.y_param) << 16U) | space.phase_param);
        digest =
            hash_u64(digest, (static_cast<u64>(space.first_sample) << 32U) | space.sample_count);
        digest = hash_u64(digest,
                          (static_cast<u64>(space.first_triangle) << 32U) | space.triangle_count);
    }
    for (const BlendSample& sample : program.blend_samples()) {
        digest = hash_bytes(digest, &sample.x, sizeof(sample.x));
        digest = hash_bytes(digest, &sample.y, sizeof(sample.y));
        digest = hash_u64(digest, sample.value);
    }
    for (const BlendTriangle& triangle : program.blend_triangles()) {
        digest = hash_u64(digest, (static_cast<u64>(triangle.sample[0]) << 32U) |
                                      (static_cast<u64>(triangle.sample[1]) << 16U) |
                                      triangle.sample[2]);
    }
    for (const FootPlacementRef& foot : program.foot_placements()) {
        digest = hash_text(digest, foot.pelvis.text());
        for (const auto& leg : foot.leg) {
            for (const Name& joint : leg) {
                digest = hash_text(digest, joint.text());
            }
        }
        const f32 settings[] = {foot.ankle_height,    foot.probe_above,    foot.probe_below,
                                foot.max_pelvis_drop, foot.max_foot_raise, foot.smoothing,
                                foot.max_tilt,        foot.pole[0],        foot.pole[1],
                                foot.pole[2]};
        digest = hash_bytes(digest, settings, sizeof(settings));
    }
    for (const AimRef& aim : program.aims()) {
        for (u32 joint = 0; joint < aim.joint_count; ++joint) {
            digest = hash_text(digest, aim.joints[joint].text());
        }
        const f32 settings[] = {aim.axis[0], aim.axis[1], aim.axis[2], aim.max_angle};
        digest = hash_bytes(digest, settings, sizeof(settings));
        digest = hash_u64(digest, (static_cast<u64>(aim.target_param[0]) << 32U) |
                                      (static_cast<u64>(aim.target_param[1]) << 16U) |
                                      aim.target_param[2]);
    }
    return digest;
}

void finish_digest(PoseProgram& program) noexcept {
    u64 digest = hash_u64(kHashSeed, program.code().size());
    for (const PoseInstruction& instruction : program.code()) {
        digest = hash_u64(digest, static_cast<u64>(instruction.op));
        digest = hash_u64(digest, (static_cast<u64>(instruction.a) << 16U) | instruction.b);
        digest = hash_u64(digest, (static_cast<u64>(instruction.clip) << 16U) | instruction.mask);
        for (u32 word = 0; word < kMaxJoints / 64U; ++word) {
            digest = hash_u64(digest, instruction.required.word(word));
        }
    }
    for (const PoseState& state : program.states()) {
        digest = hash_text(digest, state.name.text());
        digest = hash_u64(digest, state.root);
    }
    for (const Transition& transition : program.transitions()) {
        digest = hash_u64(digest, transition.target_state);
        digest = hash_bytes(digest, &transition.duration, sizeof(transition.duration));
        // THE WHOLE RULE, not only where the transition goes and how long it takes. Which parameter
        // opens it, what it outranks and whether it can be interrupted are what `advance` reads
        // every frame, so two programs that differ in them are two different programs.
        digest = hash_u64(digest, transition.condition_param);
        digest = hash_u64(digest, (static_cast<u64>(transition.priority) << 8U) |
                                      static_cast<u64>(transition.interruption));
    }
    // THE CLIPS AND THE PARAMETERS ARE PART OF THE PROGRAM'S MEANING. An instruction records the
    // INDEX of the clip it samples and the index of the parameter it reads, and an index says
    // nothing about which clip or which parameter: without these two loops a program that walks and
    // a program that sprints hash the same, and a cook keyed on the digest would serve one where
    // the other was asked for.
    for (const ClipRef& clip : program.clips()) {
        digest = hash_text(digest, clip.name.text());
        digest = hash_bytes(digest, &clip.duration, sizeof(clip.duration));
        digest = hash_u64(digest, clip.looping ? 1U : 0U);
    }
    for (const Name& parameter : program.parameters()) {
        digest = hash_text(digest, parameter.text());
    }
    digest = hash_modifiers(digest, program);
    PoseProgramAccess::set_digest(program, digest);
}

}  // namespace

Status register_pose_nodes(NodeRegistry& registry) noexcept {
    const PinDesc leaf[] = {pin("pose", "pose", PinDirection::Output)};
    for (const char* type : {"pose.ref", "pose.clip"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(leaf, 1)); !added) {
            return added;
        }
    }
    const PinDesc binary[] = {pin("a", "pose", PinDirection::Input),
                              pin("b", "pose", PinDirection::Input),
                              pin("pose", "pose", PinDirection::Output)};
    for (const char* type : {"pose.blend", "pose.blend_mask", "pose.additive", "pose.layer"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(binary, 3)); !added) {
            return added;
        }
    }
    const PinDesc unary[] = {pin("a", "pose", PinDirection::Input),
                             pin("pose", "pose", PinDirection::Output)};
    for (const char* type : {"pose.ik", "pose.foot_ik", "pose.aim"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(unary, 2)); !added) {
            return added;
        }
    }
    // A blend space: a pose input per sample, `s0` to `s15`, each placed by the property of the
    // same name. Unwired pins are not samples.
    PinDesc space[kMaxBlendSamples + 1];
    for (u32 index = 0; index < kMaxBlendSamples; ++index) {
        space[index] = pin(sample_pin(index), "pose", PinDirection::Input);
    }
    space[kMaxBlendSamples] = pin("pose", "pose", PinDirection::Output);
    for (const char* type : {"pose.blend1d", "pose.blend2d"}) {
        if (Status added =
                register_node(registry, type, Span<const PinDesc>(space, kMaxBlendSamples + 1));
            !added) {
            return added;
        }
    }
    // A state's `state` output is what a transition's `from` and `to` are wired to: the state's
    // identity, not a pose. Declared so a graph that draws its transitions validates — an editor's
    // canvas wires only an output into an input of the same type — and the compiler reads the
    // transition's input links whichever pin of the state they leave.
    const PinDesc state_pins[] = {pin("pose", "pose", PinDirection::Input),
                                  pin("state", "state", PinDirection::Output)};
    if (Status added = register_node(registry, "pose.state", Span<const PinDesc>(state_pins, 2));
        !added) {
        return added;
    }
    const PinDesc transition_pins[] = {pin("from", "state", PinDirection::Input),
                                       pin("to", "state", PinDirection::Input)};
    return register_node(registry, "pose.transition", Span<const PinDesc>(transition_pins, 2));
}

Expected<PoseProgram, Error> compile_pose(const Graph& graph, const NodeRegistry& /*registry*/,
                                          u32 joint_count, DiagnosticSink& sink) noexcept {
    PoseProgram program(graph.allocator());
    PoseProgramAccess::set_name(program, graph.name());
    PoseProgramAccess::set_joints(program, joint_count > kMaxJoints ? kMaxJoints : joint_count);

    Compilation state;
    state.graph = &graph;
    state.program = &program;
    state.sink = &sink;

    // States first, in key order, so a program's state numbering is a function of the graph and not
    // of the order the author happened to add nodes in.
    const Name state_type = Name::intern("pose.state");
    Array<NodeKey> state_keys(graph.allocator());
    for (const GraphNode& node : graph.nodes()) {
        if (node.type != state_type) {
            continue;
        }
        if (Status pushed = state_keys.push_back(node.key); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (usize outer = 1; outer < state_keys.size(); ++outer) {
        for (usize inner = outer; inner > 0 && state_keys[inner - 1] > state_keys[inner]; --inner) {
            const NodeKey swap = state_keys[inner - 1];
            state_keys[inner - 1] = state_keys[inner];
            state_keys[inner] = swap;
        }
    }
    for (const NodeKey key : state_keys) {
        auto root = lower_input(state, key, "pose");
        if (!root) {
            return make_unexpected(root.error());
        }
        PoseState entry;
        const Literal* named = graph.property(key, Name::intern("name"));
        entry.name = named != nullptr ? named->text : Name{};
        entry.root = root.value();
        entry.origin = key;
        if (Status pushed = PoseProgramAccess::states(program).push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    // Transitions, grouped by source state so that `advance` reads one contiguous run.
    for (usize index = 0; index < state_keys.size(); ++index) {
        PoseState& entry = PoseProgramAccess::states(program)[index];
        entry.first_transition = static_cast<u32>(PoseProgramAccess::transitions(program).size());
        for (const GraphNode& node : graph.nodes()) {
            if (node.type != Name::intern("pose.transition")) {
                continue;
            }
            if (source_of(graph, node.key, "from") != state_keys[index]) {
                continue;
            }
            const NodeKey to = source_of(graph, node.key, "to");
            Transition transition;
            transition.origin = node.key;
            for (usize target = 0; target < state_keys.size(); ++target) {
                if (state_keys[target] == to) {
                    transition.target_state = static_cast<u16>(target);
                }
            }
            const Literal* condition = graph.property(node.key, Name::intern("condition"));
            transition.condition_param =
                intern_parameter(program, condition != nullptr ? condition->text : Name{});
            const Literal* duration = graph.property(node.key, Name::intern("duration"));
            transition.duration = duration != nullptr ? duration->value.x : 0.0F;
            const Literal* priority = graph.property(node.key, Name::intern("priority"));
            transition.priority = priority != nullptr ? static_cast<u16>(priority->value.mask) : 0;
            const Literal* interruption = graph.property(node.key, Name::intern("interruption"));
            if (interruption != nullptr) {
                const std::string_view rule = interruption->text.text();
                transition.interruption = Interruption::None;
                if (rule == "any") {
                    transition.interruption = Interruption::Any;
                } else if (rule == "higher_priority") {
                    transition.interruption = Interruption::HigherPriority;
                }
            }
            if (Status pushed = PoseProgramAccess::transitions(program).push_back(transition);
                !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        entry.transition_count = static_cast<u32>(PoseProgramAccess::transitions(program).size()) -
                                 entry.first_transition;
    }

    if (program.states().empty()) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "an animation graph needs at least one state", 0});
    }
    PoseProgramAccess::set_entry(program, 0);
    analyse_dependencies(program);
    finish_digest(program);
    return program;
}

}  // namespace cy::graph::pose
