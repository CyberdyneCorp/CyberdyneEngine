// The animation IR's compiler and its dependency analysis. Task 2.4. The runtime half — the
// program's storage, the state machine and the reference evaluator — is pose_program.cpp, split
// out so a runtime that loads a cooked program links no compiler (issue #76).
//
// See lower_pose.h for why animation keeps its own IR rather than lowering through the shared
// expression core, and for what makes the program lazy.

#include <cy/graph/lower_pose.h>

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
        if (Status lowered = lower_clip(state, node, instruction); !lowered) {
            return make_unexpected(lowered.error());
        }
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
