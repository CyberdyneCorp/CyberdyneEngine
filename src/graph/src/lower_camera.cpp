// The camera rig domain and its compiled program. Task 2.4.
//
// See lower_camera.h for why this consumer does lower through the shared expression core and which
// three of the four extensions it is the first user of.

#include <cy/graph/lower_camera.h>

#include <cmath>
#include <utility>

namespace cy::graph::camera {

/// Write access to a `CameraRigProgram`.
class RigProgramAccess {
public:
    [[nodiscard]] static Array<RigStep>& steps(CameraRigProgram& program) noexcept {
        return program.steps_;
    }
    [[nodiscard]] static DebugMap& debug(CameraRigProgram& program) noexcept {
        return program.debug_;
    }
    static void set_name(CameraRigProgram& program, Name name) noexcept { program.name_ = name; }
    static void set_slots(CameraRigProgram& program, u32 slots) noexcept { program.slots_ = slots; }
    static void set_phases(CameraRigProgram& program, u32 phases) noexcept {
        program.phases_ = phases;
    }
    static void set_roots(CameraRigProgram& program, u16 pose, u16 lens, u16 state) noexcept {
        program.pose_slot_ = pose;
        program.lens_slot_ = lens;
        program.state_slot_ = state;
    }
    static void set_digests(CameraRigProgram& program, u64 digest, u64 ir) noexcept {
        program.digest_ = digest;
        program.ir_digest_ = ir;
    }
};

namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

// --- The domain (E1, E2, E3, E4) ---------------------------------------------------------------
//
// Identity is TEXT for every row here. The material anchor is the only domain in the tree that pins
// a numeric identity, and it does so to stay compatible with cooked data that already exists;
// a domain being introduced has none, so E2's rule applies without an exception.

constexpr TypeDesc kTypes[] = {
    {"scalar", 1, true, 0, false},     {"vector", 3, true, 0, false},
    {"quaternion", 4, true, 0, false}, {"transform", 4, true, 0, false},
    {"lens", 1, true, 0, false},
};
static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == static_cast<usize>(RigTypeCount));

consteval OpDesc rig_op(const char* name, u32 arity, bool commutative = false, bool leaf = false) {
    OpDesc desc;
    desc.name = name;
    desc.arity = arity;
    desc.commutative = commutative;
    desc.inline_leaf = leaf;
    desc.uniform_leaf = leaf;
    desc.merge = leaf ? MergeClass::Leaf : MergeClass::Expression;
    desc.category = leaf ? OpCategory::Leaf : OpCategory::Arithmetic;
    return desc;
}

consteval OpDesc varying_leaf(const char* name) {
    OpDesc desc = rig_op(name, 0, false, true);
    desc.varying = true;
    desc.uniform_leaf = false;
    return desc;
}

consteval OpDesc boundary_op(const char* name, u32 arity) {
    OpDesc desc = rig_op(name, arity);
    // E4. A node of this op ends one dispatch: its value arrives from a batched external query.
    desc.phase_boundary = true;
    desc.category = OpCategory::Sample;
    desc.merge = MergeClass::Sample;
    desc.varying = true;
    return desc;
}

constexpr OpDesc kOps[] = {
    rig_op("const", 0, false, true),
    rig_op("param", 0, false, true),
    varying_leaf("input"),
    rig_op("add", 2, true),
    rig_op("sub", 2),
    rig_op("mul", 2, true),
    rig_op("scale", 2),
    rig_op("lerp", 3),
    rig_op("smooth_half_life", 4),
    rig_op("smooth_damped", 4),
    rig_op("shake", 2),
    boundary_op("collide", 2),
    rig_op("lens_of", 1),
    rig_op("compose", 2),
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<usize>(RigOpCount));

constexpr RootDecl kRoots[] = {
    {"pose", kInvalidType, false},
    {"lens", Lens, false},
    // THE SMOOTHING STATE, ON ITS WAY OUT. A camera's per-frame state is carried in as an input and
    // out as a root — the shape probe P12 demonstrated, and the reason `Module`'s roots had to stop
    // being two fixed slots.
    {"state", kInvalidType, false},
};

class RigDomain final : public Domain {
public:
    [[nodiscard]] std::string_view domain_name() const noexcept override { return "camera.rig"; }
    [[nodiscard]] Span<const TypeDesc> types() const noexcept override {
        return {kTypes, static_cast<usize>(RigTypeCount)};
    }
    [[nodiscard]] Span<const OpDesc> ops() const noexcept override {
        return {kOps, static_cast<usize>(RigOpCount)};
    }
    [[nodiscard]] Span<const RootDecl> roots() const noexcept override { return {kRoots, 3}; }

    [[nodiscard]] Expected<TypeId, Error> result_type(
        const TypeQuery& query) const noexcept override {
        switch (query.op) {
            case Constant:
            case Parameter:
            case Input:
                return query.hint == kInvalidType ? static_cast<TypeId>(Scalar) : query.hint;
            case Collide:
                return Vector;
            case LensOf:
                return Lens;
            case Compose:
                return Transform;
            case Scale:
            case Lerp:
            case SmoothHalfLife:
            case SmoothDamped:
            case Shake:
                return query.operands[0];
            default:
                break;
        }
        if (query.operands.size() < 2) {
            return make_unexpected(invalid("this rig operation takes two operands"));
        }
        // A scalar promotes against a vector; anything else must agree.
        if (query.operands[0] == query.operands[1]) {
            return query.operands[0];
        }
        if (query.operands[0] == Scalar) {
            return query.operands[1];
        }
        if (query.operands[1] == Scalar) {
            return query.operands[0];
        }
        return make_unexpected(invalid("these rig operand types do not combine"));
    }

    [[nodiscard]] OpId constant_op() const noexcept override { return Constant; }
};

}  // namespace

const Domain& rig_domain() noexcept {
    static const RigDomain kDomain;
    return kDomain;
}

CameraRigProgram::CameraRigProgram(Allocator& allocator) noexcept
    : steps_(allocator), debug_(allocator) {}

// --- Evaluation -----------------------------------------------------------------------------

namespace {

struct Slot {
    f32 v[4] = {};
};

[[nodiscard]] f32 named_value(Span<const Name> names, Span<const f32> values,
                              Name wanted) noexcept {
    for (usize index = 0; index < names.size() && index < values.size(); ++index) {
        if (names[index] == wanted) {
            return values[index];
        }
    }
    return 0.0F;
}

void run_step(const RigStep& step, Span<Slot> slots, const RigInputs& inputs,
              const RigInstance& instance) noexcept {
    Slot& dst = slots[step.dst];
    const Slot a = step.a != 0xFFFFU ? slots[step.a] : Slot{};
    const Slot b = step.b != 0xFFFFU ? slots[step.b] : Slot{};
    const Slot c = step.c != 0xFFFFU ? slots[step.c] : Slot{};
    switch (step.op) {
        case Constant:
            dst.v[0] = step.value.x;
            dst.v[1] = step.value.y;
            dst.v[2] = step.value.z;
            dst.v[3] = step.value.w;
            return;
        case Parameter: {
            const f32 value =
                named_value(inputs.parameter_names, inputs.parameter_values, step.symbol);
            dst.v[0] = value;
            dst.v[1] = value;
            dst.v[2] = value;
            return;
        }
        case Input: {
            if (step.symbol.text() == "state") {
                // The smoothing state on its way IN. On the frame after a cut it is not primed, and
                // the rig starts from the desired value rather than easing towards it from nowhere.
                for (u32 channel = 0; channel < 4U; ++channel) {
                    dst.v[channel] = instance.primed ? instance.state[channel] : 0.0F;
                }
                return;
            }
            const f32 value = named_value(inputs.input_names, inputs.input_values, step.symbol);
            dst.v[0] = value;
            dst.v[1] = value;
            dst.v[2] = value;
            return;
        }
        case Add:
        case Sub:
        case Mul:
        case Scale:
            for (u32 channel = 0; channel < 4U; ++channel) {
                const f32 right = step.op == Scale ? b.v[0] : b.v[channel];
                f32 result = a.v[channel] * right;
                if (step.op == Add) {
                    result = a.v[channel] + right;
                } else if (step.op == Sub) {
                    result = a.v[channel] - right;
                }
                dst.v[channel] = result;
            }
            return;
        case Lerp:
            for (u32 channel = 0; channel < 4U; ++channel) {
                dst.v[channel] = a.v[channel] + ((b.v[channel] - a.v[channel]) * c.v[0]);
            }
            return;
        case SmoothHalfLife: {
            // FRAME-RATE INDEPENDENT, IN PHYSICALLY MEANINGFUL TERMS. `camera-system` requires the
            // half-life spelling precisely so that a rig tuned at 30 frames a second behaves the
            // same at 144.
            const f32 half_life = c.v[0] <= 0.0F ? 0.0001F : c.v[0];
            const f32 alpha = 1.0F - std::pow(0.5F, inputs.dt / half_life);
            for (u32 channel = 0; channel < 4U; ++channel) {
                dst.v[channel] = a.v[channel] + ((b.v[channel] - a.v[channel]) * alpha);
            }
            return;
        }
        case SmoothDamped: {
            const f32 frequency = c.v[0] <= 0.0F ? 0.0001F : c.v[0];
            const f32 alpha = 1.0F - std::exp(-frequency * inputs.dt);
            for (u32 channel = 0; channel < 4U; ++channel) {
                dst.v[channel] = a.v[channel] + ((b.v[channel] - a.v[channel]) * alpha);
            }
            return;
        }
        case Shake:
            for (u32 channel = 0; channel < 4U; ++channel) {
                dst.v[channel] = a.v[channel] + (b.v[channel] * b.v[3]);
            }
            return;
        case LensOf:
            dst.v[0] = a.v[0];
            return;
        case Compose:
            dst = a;
            dst.v[3] = b.v[0];
            return;
        default:
            // A `Collide` is filled in by the batch between the phases, and there is nothing else.
            return;
    }
}

}  // namespace

Status evaluate_rig(const CameraRigProgram& program, const RigInputs& inputs, RigInstance& instance,
                    RigQueryBatch& batch, RigOutput& out) noexcept {
    Array<Slot> slots(program.allocator());
    if (Status sized = slots.resize(program.slot_count()); !sized) {
        return sized;
    }
    Array<RigQuery> queries(program.allocator());
    Array<RigQueryResult> results(program.allocator());

    // ONE PASS PER PHASE. A boundary node's own value arrives from the batch, so it belongs to the
    // phase AFTER its operands: entering phase p, every boundary step of phase p is asked for at
    // once and answered in ONE call, whatever the rig's node count. `camera-system` forbids
    // "scattered synchronous casts from individual rig nodes", and this is how a pure expression
    // DAG says so.
    for (u32 phase = 0; phase < program.phase_count(); ++phase) {
        queries.clear();
        for (const RigStep& step : program.steps()) {
            if (step.phase != phase || step.op != Collide) {
                continue;
            }
            RigQuery query;
            for (u32 channel = 0; channel < 3U; ++channel) {
                query.origin[channel] = step.a != 0xFFFFU ? slots[step.a].v[channel] : 0.0F;
                query.target[channel] = step.b != 0xFFFFU ? slots[step.b].v[channel] : 0.0F;
            }
            query.step = step.dst;
            if (Status pushed = queries.push_back(query); !pushed) {
                return pushed;
            }
        }
        if (!queries.empty()) {
            results.clear();
            if (Status sized = results.resize(queries.size()); !sized) {
                return sized;
            }
            batch.resolve(queries.span(), results.span());
            for (usize index = 0; index < queries.size(); ++index) {
                Slot& slot = slots[queries[index].step];
                for (u32 channel = 0; channel < 3U; ++channel) {
                    slot.v[channel] = results[index].hit[channel];
                }
            }
        }
        for (const RigStep& step : program.steps()) {
            if (step.phase != phase || step.op == Collide) {
                continue;
            }
            run_step(step, slots.span(), inputs, instance);
        }
    }

    if (program.pose_slot() != 0xFFFFU) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            out.position[channel] = slots[program.pose_slot()].v[channel];
        }
    }
    if (program.lens_slot() != 0xFFFFU) {
        out.focal_length = slots[program.lens_slot()].v[0];
    }
    if (program.state_slot() != 0xFFFFU) {
        for (u32 channel = 0; channel < 4U; ++channel) {
            out.state[channel] = slots[program.state_slot()].v[channel];
            instance.state[channel] = out.state[channel];
        }
        instance.primed = true;
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
    desc.plugin = Name::intern("cy.graph.camera");
    desc.pins = pins;
    desc.pure = true;
    return registry.register_type(desc);
}

struct RigLowering {
    const Graph* graph = nullptr;
    Builder* builder = nullptr;
    DiagnosticSink* sink = nullptr;
    Array<NodeKey>* stack = nullptr;
    Array<NodeKey>* keys = nullptr;
    Array<NodeId>* values = nullptr;
};

[[nodiscard]] NodeKey source_of(const Graph& graph, NodeKey node, const char* pin_name) noexcept {
    const Name wanted = Name::intern(pin_name);
    NodeKey source = kInvalidNodeKey;
    for (const Link& link : graph.links()) {
        if (link.to == node && link.to_pin == wanted) {
            source = link.from;
        }
    }
    return source;
}

struct RigNodeSpec {
    const char* type;
    OpId op;
    const char* inputs[4];
};

constexpr RigNodeSpec kSpecs[] = {
    {"camera.constant", Constant, {nullptr, nullptr, nullptr, nullptr}},
    {"camera.parameter", Parameter, {nullptr, nullptr, nullptr, nullptr}},
    {"camera.input", Input, {nullptr, nullptr, nullptr, nullptr}},
    {"camera.add", Add, {"a", "b", nullptr, nullptr}},
    {"camera.sub", Sub, {"a", "b", nullptr, nullptr}},
    {"camera.mul", Mul, {"a", "b", nullptr, nullptr}},
    {"camera.scale", Scale, {"a", "b", nullptr, nullptr}},
    {"camera.lerp", Lerp, {"a", "b", "t", nullptr}},
    {"camera.smooth_half_life", SmoothHalfLife, {"previous", "desired", "half_life", "dt"}},
    {"camera.smooth_damped", SmoothDamped, {"previous", "desired", "frequency", "dt"}},
    {"camera.shake", Shake, {"a", "impulse", nullptr, nullptr}},
    {"camera.collide", Collide, {"origin", "target", nullptr, nullptr}},
    {"camera.lens", LensOf, {"focal_length", nullptr, nullptr, nullptr}},
    {"camera.compose", Compose, {"position", "focal_length", nullptr, nullptr}},
};

[[nodiscard]] const RigNodeSpec* spec_of(Name type) noexcept {
    for (const RigNodeSpec& spec : kSpecs) {
        if (type.text() == spec.type) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] Expected<NodeId, Error> lower_node(RigLowering& state, NodeKey node) noexcept;

[[nodiscard]] Expected<NodeId, Error> lower_operand(RigLowering& state, NodeKey node,
                                                    const char* pin_name) noexcept {
    const NodeKey source = source_of(*state.graph, node, pin_name);
    if (source != kInvalidNodeKey) {
        return lower_node(state, source);
    }
    // An unwired operand is a zero of its type, so a rig under construction still compiles.
    return state.builder->make(Constant, Scalar, Name{}, Immediate{}, {});
}

Expected<NodeId, Error> lower_node(RigLowering& state, NodeKey node) noexcept {
    for (usize index = 0; index < state.keys->size(); ++index) {
        if ((*state.keys)[index] == node) {
            return (*state.values)[index];
        }
    }
    for (const NodeKey visiting : *state.stack) {
        if (visiting == node) {
            // A cycle. The shared core cannot hold one by construction — an operand must already
            // exist — so this is a diagnostic and not a silent stack overflow.
            Diagnostic diagnostic;
            diagnostic.node = node;
            diagnostic.message =
                "this rig node takes part in a cycle, and a rig program is a pure expression: an "
                "operand must already exist";
            state.sink->report(diagnostic);
            return make_unexpected(invalid("a rig graph must be acyclic"));
        }
    }
    const GraphNode* authored = state.graph->find_node(node);
    const RigNodeSpec* spec = authored == nullptr ? nullptr : spec_of(authored->type);
    if (spec == nullptr) {
        Diagnostic diagnostic;
        diagnostic.node = node;
        diagnostic.detail = authored != nullptr ? authored->type : Name{};
        diagnostic.message = "this node is not a camera rig node";
        state.sink->report(diagnostic);
        return make_unexpected(invalid("a rig graph holds rig nodes"));
    }
    if (Status pushed = state.stack->push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }

    NodeId operands[kMaxOperands] = {};
    u32 count = 0;
    for (const char* input : spec->inputs) {
        if (input == nullptr) {
            break;
        }
        auto operand = lower_operand(state, node, input);
        if (!operand) {
            return operand;
        }
        operands[count++] = operand.value();
    }

    const Literal* symbol = state.graph->property(node, Name::intern("name"));
    const Literal* value = state.graph->property(node, Name::intern("value"));
    const Literal* typed = state.graph->property(node, Name::intern("type"));
    TypeId hint = kInvalidType;
    if (typed != nullptr) {
        const std::string_view named = typed->text.text();
        hint = Scalar;
        if (named == "vector") {
            hint = Vector;
        } else if (named == "quaternion") {
            hint = Quaternion;
        } else if (named == "transform") {
            hint = Transform;
        } else if (named == "lens") {
            hint = Lens;
        }
    } else if (spec->op == Constant || spec->op == Parameter || spec->op == Input) {
        hint = Scalar;
    }
    auto made = state.builder->make(spec->op, hint, symbol != nullptr ? symbol->text : Name{},
                                    value != nullptr ? value->value : Immediate{},
                                    Span<const NodeId>(operands, count));
    if (!made) {
        return made;
    }
    if (Status recorded = state.builder->add_origin(made.value(), static_cast<u32>(node));
        !recorded) {
        return make_unexpected(recorded.error());
    }
    state.stack->pop_back();
    if (Status pushed = state.keys->push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = state.values->push_back(made.value()); !pushed) {
        return make_unexpected(pushed.error());
    }
    return made.value();
}

}  // namespace

Status register_camera_nodes(NodeRegistry& registry) noexcept {
    const PinDesc leaf[] = {pin("value", "scalar", PinDirection::Output)};
    for (const char* type : {"camera.constant", "camera.parameter", "camera.input"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(leaf, 1)); !added) {
            return added;
        }
    }
    const PinDesc binary[] = {pin("a", "scalar", PinDirection::Input),
                              pin("b", "scalar", PinDirection::Input),
                              pin("value", "scalar", PinDirection::Output)};
    for (const char* type : {"camera.add", "camera.sub", "camera.mul", "camera.scale"}) {
        if (Status added = register_node(registry, type, Span<const PinDesc>(binary, 3)); !added) {
            return added;
        }
    }
    const PinDesc lerp[] = {
        pin("a", "scalar", PinDirection::Input), pin("b", "scalar", PinDirection::Input),
        pin("t", "scalar", PinDirection::Input), pin("value", "scalar", PinDirection::Output)};
    if (Status added = register_node(registry, "camera.lerp", Span<const PinDesc>(lerp, 4));
        !added) {
        return added;
    }
    const PinDesc smooth[] = {pin("previous", "scalar", PinDirection::Input),
                              pin("desired", "scalar", PinDirection::Input),
                              pin("half_life", "scalar", PinDirection::Input),
                              pin("dt", "scalar", PinDirection::Input)};
    PinDesc smooth_full[5] = {smooth[0], smooth[1], smooth[2], smooth[3],
                              pin("value", "scalar", PinDirection::Output)};
    if (Status added =
            register_node(registry, "camera.smooth_half_life", Span<const PinDesc>(smooth_full, 5));
        !added) {
        return added;
    }
    smooth_full[2] = pin("frequency", "scalar", PinDirection::Input);
    if (Status added =
            register_node(registry, "camera.smooth_damped", Span<const PinDesc>(smooth_full, 5));
        !added) {
        return added;
    }
    const PinDesc shake[] = {pin("a", "scalar", PinDirection::Input),
                             pin("impulse", "scalar", PinDirection::Input),
                             pin("value", "scalar", PinDirection::Output)};
    if (Status added = register_node(registry, "camera.shake", Span<const PinDesc>(shake, 3));
        !added) {
        return added;
    }
    const PinDesc collide[] = {pin("origin", "vector", PinDirection::Input),
                               pin("target", "vector", PinDirection::Input),
                               pin("value", "vector", PinDirection::Output)};
    if (Status added = register_node(registry, "camera.collide", Span<const PinDesc>(collide, 3));
        !added) {
        return added;
    }
    const PinDesc lens[] = {pin("focal_length", "scalar", PinDirection::Input),
                            pin("value", "lens", PinDirection::Output)};
    if (Status added = register_node(registry, "camera.lens", Span<const PinDesc>(lens, 2));
        !added) {
        return added;
    }
    const PinDesc compose[] = {pin("position", "vector", PinDirection::Input),
                               pin("focal_length", "scalar", PinDirection::Input),
                               pin("value", "transform", PinDirection::Output)};
    if (Status added = register_node(registry, "camera.compose", Span<const PinDesc>(compose, 3));
        !added) {
        return added;
    }
    const PinDesc output[] = {pin("pose", "transform", PinDirection::Input),
                              pin("lens", "lens", PinDirection::Input),
                              pin("state", "vector", PinDirection::Input)};
    return register_node(registry, "camera.output", Span<const PinDesc>(output, 3));
}

Expected<CameraRigProgram, Error> compile_rig(const Graph& graph, const NodeRegistry& /*registry*/,
                                              DiagnosticSink& sink) noexcept {
    Allocator& allocator = graph.allocator();
    NodeKey output = kInvalidNodeKey;
    for (const GraphNode& node : graph.nodes()) {
        if (node.type == Name::intern("camera.output")) {
            output = node.key;
        }
    }
    if (output == kInvalidNodeKey) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "a rig graph needs a `camera.output` node", 0});
    }

    Builder builder(allocator, rig_domain(), graph.name());
    Array<NodeKey> stack(allocator);
    Array<NodeKey> keys(allocator);
    Array<NodeId> values(allocator);
    RigLowering state;
    state.graph = &graph;
    state.builder = &builder;
    state.sink = &sink;
    state.stack = &stack;
    state.keys = &keys;
    state.values = &values;

    const char* root_pins[] = {"pose", "lens", "state"};
    NodeId roots[3] = {kInvalidNode, kInvalidNode, kInvalidNode};
    for (u32 slot = 0; slot < 3U; ++slot) {
        const NodeKey source = source_of(graph, output, root_pins[slot]);
        if (source == kInvalidNodeKey) {
            continue;
        }
        auto lowered = lower_node(state, source);
        if (!lowered) {
            return make_unexpected(lowered.error());
        }
        roots[slot] = lowered.value();
    }
    for (u32 slot = 0; slot < 3U; ++slot) {
        if (roots[slot] == kInvalidNode) {
            continue;
        }
        if (Status set = builder.set_root(slot, roots[slot]); !set) {
            return make_unexpected(set.error());
        }
    }
    auto module = builder.finish();
    if (!module) {
        return make_unexpected(module.error());
    }

    OptimiseReport report(allocator);
    const PassSwitches switches;
    auto optimised = optimise(module.value(), switches, report);
    if (!optimised) {
        return make_unexpected(optimised.error());
    }

    // FLATTENED INTO A LINEAR STEP LIST. `camera-system`: "no per-node allocation or virtual
    // dispatch". Canonical order is what makes the flattening a function of content alone.
    Array<NodeId> order(allocator);
    if (Status walked = canonical_order(optimised.value(), optimised.value().roots(), order);
        !walked) {
        return make_unexpected(walked.error());
    }
    Array<u16> slot_of(allocator);
    if (Status sized = slot_of.resize(optimised.value().size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u16& slot : slot_of) {
        slot = 0xFFFFU;
    }

    CameraRigProgram program(allocator);
    RigProgramAccess::set_name(program, graph.name());
    u16 next_slot = 0;
    for (const NodeId id : order) {
        const Node& node = optimised.value().node(id);
        RigStep step;
        step.op = node.op;
        step.value = node.value;
        step.symbol = node.symbol;
        step.phase = static_cast<u8>(optimised.value().phase_of(id));
        const Span<const NodeId> operands = optimised.value().operands(id);
        if (!operands.empty()) {
            step.a = slot_of[operands[0]];
        }
        if (operands.size() > 1) {
            step.b = slot_of[operands[1]];
        }
        if (operands.size() > 2) {
            step.c = slot_of[operands[2]];
        }
        step.dst = next_slot;
        slot_of[id] = next_slot++;
        const Span<const u32> origins = optimised.value().origins(id);
        step.origin = origins.empty() ? kInvalidNodeKey : static_cast<NodeKey>(origins[0]);
        if (Status pushed = RigProgramAccess::steps(program).push_back(step); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status recorded = RigProgramAccess::debug(program).record(step.dst, step.origin);
            !recorded) {
            return make_unexpected(recorded.error());
        }
    }
    RigProgramAccess::set_slots(program, next_slot);
    RigProgramAccess::set_phases(program, optimised.value().phase_count());
    const auto slot_for = [&](u32 root) noexcept {
        const NodeId id = optimised.value().root(root);
        return id == kInvalidNode ? static_cast<u16>(0xFFFFU) : slot_of[id];
    };
    RigProgramAccess::set_roots(program, slot_for(kPoseRoot), slot_for(kLensRoot),
                                slot_for(kStateRoot));

    u64 digest = hash_u64(kHashSeed, program.steps().size());
    for (const RigStep& step : program.steps()) {
        digest = hash_u64(digest, step.op);
        digest = hash_u64(
            digest, (static_cast<u64>(step.a) << 32U) | (static_cast<u64>(step.b) << 16U) | step.c);
        digest = hash_bytes(digest, &step.value, sizeof(step.value));
        digest = hash_text(digest, step.symbol.text());
    }
    RigProgramAccess::set_digests(program, digest, optimised.value().digest());
    return program;
}

}  // namespace cy::graph::camera
