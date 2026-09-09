// The animation IR: the compiler, the dependency analysis, and the lazy evaluator. Task 2.4.
//
// See lower_pose.h for why animation keeps its own IR rather than lowering through the shared
// expression core, and for what makes the program lazy.

#include <cy/graph/lower_pose.h>

#include <algorithm>
#include <utility>

namespace cy::graph::pose {

/// Write access to a `PoseProgram`, which has none in public: a compiled program is shared by every
/// character at run time.
class PoseProgramAccess {
public:
    [[nodiscard]] static Array<PoseInstruction>& code(PoseProgram& program) noexcept {
        return program.code_;
    }
    [[nodiscard]] static Array<PoseState>& states(PoseProgram& program) noexcept {
        return program.states_;
    }
    [[nodiscard]] static Array<Transition>& transitions(PoseProgram& program) noexcept {
        return program.transitions_;
    }
    [[nodiscard]] static Array<ClipRef>& clips(PoseProgram& program) noexcept {
        return program.clips_;
    }
    [[nodiscard]] static Array<JointMask>& masks(PoseProgram& program) noexcept {
        return program.masks_;
    }
    [[nodiscard]] static Array<Name>& parameters(PoseProgram& program) noexcept {
        return program.parameters_;
    }
    [[nodiscard]] static Array<SyncGroup>& sync_groups(PoseProgram& program) noexcept {
        return program.sync_;
    }
    [[nodiscard]] static Array<SyncMarker>& markers(PoseProgram& program) noexcept {
        return program.markers_;
    }
    [[nodiscard]] static DebugMap& debug(PoseProgram& program) noexcept { return program.debug_; }
    static void set_name(PoseProgram& program, Name name) noexcept { program.name_ = name; }
    static void set_entry(PoseProgram& program, u16 state) noexcept {
        program.entry_state_ = state;
    }
    static void set_joints(PoseProgram& program, u32 joints) noexcept { program.joints_ = joints; }
    static void set_digest(PoseProgram& program, u64 digest) noexcept { program.digest_ = digest; }
};

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

Expected<PoseValue, Error> lower_value(Compilation& state, NodeKey node) noexcept {
    const GraphNode* authored = state.graph->find_node(node);
    if (authored == nullptr) {
        return kNoPoseValue;
    }
    PoseProgram& program = *state.program;
    PoseInstruction instruction;
    instruction.origin = node;
    const std::string_view type = authored->type.text();

    if (type == "pose.ref") {
        instruction.op = PoseOp::RefPose;
    } else if (type == "pose.clip") {
        instruction.op = PoseOp::SampleClip;
        const Literal* clip = state.graph->property(node, Name::intern("clip"));
        const Literal* duration = state.graph->property(node, Name::intern("duration"));
        ClipRef reference;
        reference.name = clip != nullptr ? clip->text : Name{};
        reference.duration = duration != nullptr ? duration->value.x : 1.0F;
        Array<ClipRef>& clips = PoseProgramAccess::clips(program);
        instruction.clip = static_cast<u16>(clips.size());
        for (usize index = 0; index < clips.size(); ++index) {
            if (clips[index].name == reference.name) {
                instruction.clip = static_cast<u16>(index);
            }
        }
        if (instruction.clip == clips.size()) {
            if (Status pushed = clips.push_back(reference); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        const Literal* time = state.graph->property(node, Name::intern("time_parameter"));
        instruction.time_param = intern_parameter(program, time != nullptr ? time->text : Name{});
    } else if (type == "pose.blend" || type == "pose.blend_mask" || type == "pose.additive" ||
               type == "pose.layer") {
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
            return a;
        }
        auto b = lower_input(state, node, "b");
        if (!b) {
            return b;
        }
        instruction.a = a.value();
        instruction.b = b.value();
        instruction.mask = mask_index(program, *state.graph, node);
        const Literal* weight = state.graph->property(node, Name::intern("weight_parameter"));
        instruction.weight_param =
            intern_parameter(program, weight != nullptr ? weight->text : Name{});
    } else if (type == "pose.ik") {
        instruction.op = PoseOp::IK;
        auto a = lower_input(state, node, "a");
        if (!a) {
            return a;
        }
        instruction.a = a.value();
        const Literal* chain = state.graph->property(node, Name::intern("chain"));
        instruction.chain = chain != nullptr ? static_cast<u16>(chain->value.mask) : 0;
    } else {
        Diagnostic diagnostic;
        diagnostic.node = node;
        diagnostic.detail = authored->type;
        diagnostic.message = "this node is not an animation node and cannot produce a pose";
        state.sink->report(diagnostic);
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
    }
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
    if (Status added = register_node(registry, "pose.ik", Span<const PinDesc>(unary, 2)); !added) {
        return added;
    }
    const PinDesc state_pins[] = {pin("pose", "pose", PinDirection::Input)};
    if (Status added = register_node(registry, "pose.state", Span<const PinDesc>(state_pins, 1));
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
