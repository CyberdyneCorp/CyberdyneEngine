// graph -> typed VFX IR -> optimisation -> Slang. M8.c tasks 2.1 and 2.2.
//
// The two passes in this file that the shared expression core cannot make, because both are
// statements about the ORDERED WRITE LIST rather than about the expression DAG:
//
//   ATTRIBUTE LIVENESS  an attribute written by some stage and read by none — including the render
//                       stage — is not allocated and its write is dropped.
//   KERNEL FUSION       Initialise and Update share one dispatch over the newly spawned particles:
//                       the update's reads of an attribute the initialise just wrote are replaced
//                       by the initialise's own SSA value, so the initial values never round-trip
//                       through memory and the spawn path issues one dispatch instead of two.

#include <cy/vfx/compile.h>

#include "access.h"
#include "emit_slang.h"

#include <cy/core/memory/array.h>
#include <cy/graph/expr.h>
#include <cy/graph/passes.h>

#include <utility>

namespace cy::vfx {

using graph::Builder;
using graph::Diagnostic;
using graph::GraphNode;
using graph::hash_bytes;
using graph::hash_text;
using graph::hash_u64;
using graph::kHashSeed;
using graph::kInvalidNodeKey;
using graph::Link;
using graph::Literal;
using graph::NodeKey;
using graph::OptimiseReport;
using graph::Severity;

namespace {

/// The node types the lowering recognises, resolved once so the hot path compares two `Name`s
/// rather than two strings.
struct NodeNames {
    Name constant;
    Name parameter;
    Name attribute;
    Name input;
    Name random;
    Name sample;
    Name curve;
    Name noise;
    Name set_attribute;
    Name kill_if;
    Name spawn_count;
    Name emit_event;
    Name pin_value;
    Name pin_out;
    Name prop_attribute;
    Name prop_parameter;
    Name prop_value;
    Name prop_interface;
    Name prop_field;
    Name prop_input;
    Name prop_curve;
    Name prop_channel;

    NodeNames() noexcept
        : constant(Name::intern("vfx.constant")),
          parameter(Name::intern("vfx.parameter")),
          attribute(Name::intern("vfx.attribute")),
          input(Name::intern("vfx.input")),
          random(Name::intern("vfx.random")),
          sample(Name::intern("vfx.sample")),
          curve(Name::intern("vfx.curve")),
          noise(Name::intern("vfx.noise")),
          set_attribute(Name::intern("vfx.set_attribute")),
          kill_if(Name::intern("vfx.kill_if")),
          spawn_count(Name::intern("vfx.spawn_count")),
          emit_event(Name::intern("vfx.emit_event")),
          pin_value(Name::intern(prop::kValue)),
          pin_out(Name::intern("out")),
          prop_attribute(Name::intern(prop::kAttribute)),
          prop_parameter(Name::intern(prop::kParameter)),
          prop_value(Name::intern(prop::kValue)),
          prop_interface(Name::intern(prop::kInterface)),
          prop_field(Name::intern(prop::kField)),
          prop_input(Name::intern(prop::kInput)),
          prop_curve(Name::intern(prop::kCurve)),
          prop_channel(Name::intern(prop::kChannel)) {}
};

const NodeNames& names() noexcept {
    static const NodeNames instance;
    return instance;
}

/// Which arithmetic op a node type lowers to, by the suffix after `vfx.`.
struct ArithmeticRow {
    const char* suffix;
    OpId op;
};

constexpr ArithmeticRow kArithmetic[] = {
    {"add", Add},
    {"sub", Sub},
    {"mul", Mul},
    {"div", Div},
    {"min", Min},
    {"max", Max},
    {"dot", Dot},
    {"cross", Cross},
    {"length", Length},
    {"normalize", Normalize},
    {"sin", Sin},
    {"cos", Cos},
    {"pow", Pow},
    {"saturate", Saturate},
    {"lerp", Lerp},
    {"select", Select},
    {"less", Less},
    {"greater", Greater},
    {"make_float3", MakeVec3},
    {"make_float4", MakeVec4},
};

[[nodiscard]] OpId arithmetic_op(Name type) noexcept {
    const std::string_view text = type.text();
    if (text.size() < 5 || !text.starts_with("vfx.")) {
        return kInvalidOp;
    }
    const std::string_view suffix = text.substr(4);
    for (const ArithmeticRow& row : kArithmetic) {
        if (suffix == row.suffix) {
            return row.op;
        }
    }
    return kInvalidOp;
}

/// The input pins of an arithmetic node, in the order the op takes its operands. Spelled once
/// because the node library declares the same order and the two must not drift.
[[nodiscard]] Span<const char* const> operand_pins(OpId op) noexcept {
    static constexpr const char* kUnary[] = {"x"};
    static constexpr const char* kBinary[] = {"a", "b"};
    static constexpr const char* kTernary[] = {"a", "b", "t"};
    static constexpr const char* kSelect[] = {"condition", "a", "b"};
    static constexpr const char* kMake3[] = {"x", "y", "z"};
    static constexpr const char* kMake4[] = {"x", "y", "z", "w"};
    switch (op) {
        case Length:
        case Normalize:
        case Sin:
        case Cos:
        case Saturate:
        case LogicalNot:
            return {kUnary, 1};
        case Lerp:
            return {kTernary, 3};
        case Select:
            return {kSelect, 3};
        case MakeVec3:
            return {kMake3, 3};
        case MakeVec4:
            return {kMake4, 4};
        default:
            break;
    }
    return {kBinary, 2};
}

void report(DiagnosticSink& sink, NodeKey node, Name pin, const char* message,
            Name detail = Name{}) noexcept {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.node = node;
    diagnostic.pin = pin;
    diagnostic.message = message;
    diagnostic.detail = detail;
    sink.report(diagnostic);
}

/// A value already lowered, keyed by the authoring node it came from.
struct Memo {
    NodeKey key = kInvalidNodeKey;
    NodeId value = kInvalidNode;
};

/// One raise the lowering found, before its root slot is assigned.
struct PendingEvent {
    Name channel;
    NodeId value = kInvalidNode;
    NodeKey origin = kInvalidNodeKey;
};

/// An attribute whose value is live in registers because the fused initialise wrote it.
struct PendingWrite {
    Name attribute;
    NodeId value = kInvalidNode;
    TypeId type = Float;
    NodeKey origin = kInvalidNodeKey;
};

/// Everything one kernel's lowering needs. One structure rather than eleven arguments, because the
/// recursive value lowering threads all of it.
struct Lowering {
    Builder* builder = nullptr;
    const VfxSystemAsset* asset = nullptr;
    const Emitter* emitter = nullptr;
    const DataInterfaceRegistry* interfaces = nullptr;
    const CompileOptions* options = nullptr;
    DiagnosticSink* sink = nullptr;
    AttributeLayout* layout = nullptr;
    EmitterReport* report = nullptr;
    Array<Memo>* memo = nullptr;
    Array<PendingWrite>* pending = nullptr;
    Array<PendingEvent>* raises = nullptr;
    /// True while lowering a stage that is fused onto an earlier one: an attribute read then
    /// resolves to the pending value rather than to a fresh `Attribute` op.
    bool substitute_pending = false;
};

[[nodiscard]] NodeId memo_find(const Array<Memo>& memo, NodeKey key) noexcept {
    for (const Memo& entry : memo) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return kInvalidNode;
}

[[nodiscard]] const PendingWrite* pending_find(const Array<PendingWrite>& pending,
                                               Name attribute) noexcept {
    // BACKWARDS: the last write to an attribute is the one a later read sees. That is the ordered
    // write list doing the one thing an expression DAG cannot.
    for (usize index = pending.size(); index > 0; --index) {
        if (pending[index - 1].attribute == attribute) {
            return &pending[index - 1];
        }
    }
    return nullptr;
}

[[nodiscard]] TypeId literal_type(const Literal& literal) noexcept {
    const TypeId type = vfx_type_from_name(literal.type);
    return type == kInvalidType ? static_cast<TypeId>(Float) : type;
}

[[nodiscard]] Immediate literal_value(const Literal& literal) noexcept {
    return literal.value;
}

/// The single wire arriving at one input pin, or `kInvalidNodeKey`.
[[nodiscard]] Expected<NodeKey, Error> wired_source(const Graph& graph, NodeKey node, Name pin,
                                                    Array<Link>& scratch) noexcept {
    scratch.clear();
    if (Status collected = graph.inputs_of(node, pin, scratch); !collected) {
        return make_unexpected(collected.error());
    }
    if (scratch.empty()) {
        return kInvalidNodeKey;
    }
    return scratch[scratch.size() - 1].from;
}

[[nodiscard]] Expected<NodeId, Error> lower_value(Lowering& state, const Graph& graph,
                                                  NodeKey key) noexcept;

/// Lower whatever feeds `pin`: a wire if there is one, otherwise the node's own property of the
/// same name, otherwise a node- and pin-precise diagnostic.
[[nodiscard]] Expected<NodeId, Error> lower_pin(Lowering& state, const Graph& graph, NodeKey node,
                                                const char* pin_name) noexcept {
    const Name pin = Name::intern(pin_name);
    Array<Link> scratch(state.layout->allocator());
    auto source = wired_source(graph, node, pin, scratch);
    if (!source) {
        return make_unexpected(source.error());
    }
    if (source.value() != kInvalidNodeKey) {
        return lower_value(state, graph, source.value());
    }
    if (const Literal* literal = graph.property(node, pin); literal != nullptr) {
        return state.builder->make(Constant, literal_type(*literal), Name{},
                                   literal_value(*literal), {});
    }
    report(*state.sink, node, pin,
           "vfx: this input has neither a wire nor a value — a required input must have one");
    return fail(ErrorCode::InvalidArgument, "vfx: an unwired required input");
}

[[nodiscard]] Expected<NodeId, Error> lower_leaf(Lowering& state, const Graph& graph,
                                                 const GraphNode& node) noexcept {
    const NodeNames& n = names();
    Builder& builder = *state.builder;

    if (node.type == n.constant) {
        const Literal* literal = graph.property(node.key, n.prop_value);
        if (literal == nullptr) {
            report(*state.sink, node.key, n.prop_value, "vfx: a constant node has no value");
            return fail(ErrorCode::InvalidArgument, "vfx: a constant with no value");
        }
        return builder.make(Constant, literal_type(*literal), Name{}, literal_value(*literal), {});
    }

    if (node.type == n.parameter) {
        const Literal* named = graph.property(node.key, n.prop_parameter);
        if (named == nullptr || named->text.is_empty()) {
            report(*state.sink, node.key, n.prop_parameter, "vfx: a parameter node names nothing");
            return fail(ErrorCode::InvalidArgument, "vfx: a parameter with no name");
        }
        const ParameterDecl* decl = state.asset->find_parameter(named->text);
        if (decl == nullptr) {
            report(*state.sink, node.key, n.prop_parameter,
                   "vfx: this parameter is not declared by the system", named->text);
            return fail(ErrorCode::NotFound, "vfx: an undeclared parameter");
        }
        const TypeId type = vfx_type_from_name(decl->type);
        if (!decl->exposed && state.options->fold_parameters) {
            // CONSTANT FOLDING OF A PARAMETER KNOWN AT COOK TIME. `vfx-system`: "the value SHALL be
            // folded into the generated code rather than read from a buffer".
            ++state.report->folded_parameters;
            const Immediate value{decl->value[0], decl->value[1], decl->value[2], decl->value[3],
                                  0};
            return builder.make(Constant, type, Name{}, value, {});
        }
        return builder.make(Parameter, type, decl->name, Immediate{}, {});
    }

    if (node.type == n.attribute) {
        const Literal* named = graph.property(node.key, n.prop_attribute);
        if (named == nullptr || named->text.is_empty()) {
            report(*state.sink, node.key, n.prop_attribute, "vfx: an attribute node names nothing");
            return fail(ErrorCode::InvalidArgument, "vfx: an attribute with no name");
        }
        TypeId type = Float;
        if (const AttributeDecl* decl = state.emitter->find_attribute(named->text);
            decl != nullptr) {
            const TypeId declared = vfx_type_from_name(decl->type);
            type = declared == kInvalidType ? static_cast<TypeId>(Float) : declared;
        }
        if (state.substitute_pending) {
            if (const PendingWrite* live = pending_find(*state.pending, named->text);
                live != nullptr) {
                // KERNEL FUSION. The value is already in a register, so the read never happens and
                // the initialise's store never happens either.
                return live->value;
            }
        }
        if (Status touched = state.layout->touch(named->text, type, true, false); !touched) {
            report(*state.sink, node.key, n.prop_attribute,
                   "vfx: this emitter references more attributes than a kernel has write roots",
                   named->text);
            return make_unexpected(touched.error());
        }
        return builder.make(Attribute, type, named->text, Immediate{}, {});
    }

    if (node.type == n.input) {
        const Literal* named = graph.property(node.key, n.prop_input);
        const Name symbol = named != nullptr && !named->text.is_empty()
                                ? named->text
                                : Name::intern(input::kDeltaTime);
        return builder.make(EmitterInput, Float, symbol, Immediate{}, {});
    }

    if (node.type == n.random) {
        // The stream index is the authoring node's own key, so two `vfx.random` nodes draw from
        // two streams and one node draws the same value for one particle every time it is
        // evaluated. That is what makes the statistical tests reproducible at a fixed seed.
        Immediate stream;
        stream.mask = static_cast<u32>(node.key);
        return builder.make(Random, Float, Name{}, stream, {});
    }

    report(*state.sink, node.key, Name{}, "vfx: this node type has no lowering", node.type);
    return fail(ErrorCode::Unsupported, "vfx: an unlowerable node type");
}

[[nodiscard]] Expected<NodeId, Error> lower_sample(Lowering& state, const Graph& graph,
                                                   const GraphNode& node) noexcept {
    const NodeNames& n = names();
    const Literal* interface_name = graph.property(node.key, n.prop_interface);
    const Literal* field_name = graph.property(node.key, n.prop_field);
    if (interface_name == nullptr || field_name == nullptr) {
        report(*state.sink, node.key, n.prop_interface,
               "vfx: a sample node names an interface and a field");
        return fail(ErrorCode::InvalidArgument, "vfx: a sample with no interface");
    }
    const DataInterface* interface = state.interfaces->find(interface_name->text);
    if (interface == nullptr) {
        report(*state.sink, node.key, n.prop_interface,
               "vfx: no data interface of that name is registered", interface_name->text);
        return fail(ErrorCode::NotFound, "vfx: an unregistered data interface");
    }
    const InterfaceField* field = interface->find_field(field_name->text);
    if (field == nullptr) {
        report(*state.sink, node.key, n.prop_field, "vfx: that data interface has no such field",
               field_name->text);
        return fail(ErrorCode::NotFound, "vfx: an unknown interface field");
    }
    // THE COOK-TIME GATE. `vfx-system`: "WHEN an effect declares CPU simulation and uses a GPU-only
    // data interface THEN cooking SHALL FAIL with a diagnostic naming the interface."
    if (state.emitter->path() == SimulationPath::CpuRequired && !interface->cpu_available()) {
        report(*state.sink, node.key, n.prop_interface,
               "vfx: this effect declares the CPU path and this data interface is not available on "
               "it",
               interface->name());
        return fail(ErrorCode::Unsupported, "vfx: a GPU-only interface in a CPU-path effect");
    }
    if (state.emitter->path() != SimulationPath::CpuRequired && !interface->gpu_available()) {
        report(*state.sink, node.key, n.prop_interface,
               "vfx: this data interface is available only on the CPU path, and this effect does "
               "not declare it",
               interface->name());
        return fail(ErrorCode::Unsupported, "vfx: a CPU-only interface in a GPU-path effect");
    }

    state.report->sample_cost_weight += interface_cost_weight(interface->cost());

    auto argument = lower_pin(state, graph, node.key, "x");
    if (!argument) {
        return argument;
    }
    // `<interface>.<field>` in one symbol, so the emitter spells one name and the executor keys one
    // lookup. Built here rather than at every use site.
    char joined[192] = {};
    usize length = 0;
    const auto append = [&joined, &length](std::string_view text) noexcept {
        for (const char character : text) {
            if (length + 1 < sizeof(joined)) {
                joined[length++] = character;
            }
        }
    };
    append(interface->name().text());
    append(".");
    append(field->name);
    const NodeId operands[1] = {argument.value()};
    return state.builder->make(Sample, field->type, Name::intern({joined, length}), Immediate{},
                               {operands, 1});
}

Expected<NodeId, Error> lower_value(Lowering& state, const Graph& graph, NodeKey key) noexcept {
    if (const NodeId cached = memo_find(*state.memo, key); cached != kInvalidNode) {
        return cached;
    }
    const GraphNode* node = graph.find_node(key);
    if (node == nullptr) {
        report(*state.sink, key, Name{},
               "vfx: a wire arrives from a node that is not in the graph");
        return fail(ErrorCode::NotFound, "vfx: a dangling wire");
    }
    if (node->muted) {
        // A MUTED NODE CONTRIBUTES NOTHING, and in this domain "nothing" is zero: an author who
        // mutes a force expects the force to stop, not the graph to stop compiling.
        return state.builder->make(Constant, Float, Name{}, Immediate{}, {});
    }

    const NodeNames& n = names();
    Expected<NodeId, Error> produced = fail(ErrorCode::Internal, "vfx: unlowered");

    if (node->type == n.sample) {
        produced = lower_sample(state, graph, *node);
    } else if (node->type == n.curve || node->type == n.noise) {
        auto argument = lower_pin(state, graph, key, "x");
        if (!argument) {
            return argument;
        }
        const Literal* named = graph.property(key, n.prop_curve);
        const NodeId operands[1] = {argument.value()};
        produced = state.builder->make(node->type == n.curve ? Curve : Noise, Float,
                                       named != nullptr ? named->text : Name{}, Immediate{},
                                       {operands, 1});
    } else if (const OpId op = arithmetic_op(node->type); op != kInvalidOp) {
        const Span<const char* const> pins = operand_pins(op);
        NodeId operands[graph::kMaxOperands] = {};
        u32 count = 0;
        for (const char* pin : pins) {
            auto lowered = lower_pin(state, graph, key, pin);
            if (!lowered) {
                return lowered;
            }
            operands[count++] = lowered.value();
        }
        produced = state.builder->make(op, kInvalidType, Name{}, Immediate{}, {operands, count});
        if (!produced) {
            report(*state.sink, key, Name{}, "vfx: this operation's operand types do not combine",
                   node->type);
            return produced;
        }
    } else {
        produced = lower_leaf(state, graph, *node);
    }

    if (!produced) {
        return produced;
    }
    // PROVENANCE. `Builder::add_origin` takes a u32; a `NodeKey` is allocated monotonically from
    // one, so the low word identifies the node for every graph an editor can hold.
    if (Status attributed = state.builder->add_origin(produced.value(), static_cast<u32>(key));
        !attributed) {
        return make_unexpected(attributed.error());
    }
    Memo entry{key, produced.value()};
    if (Status remembered = state.memo->push_back(entry); !remembered) {
        return make_unexpected(remembered.error());
    }
    return produced;
}

/// What one stage's graph contributed to a kernel under construction.
struct StageLowering {
    NodeId kill = kInvalidNode;
    NodeId spawn = kInvalidNode;
};

/// Lower every write node of one stage graph into the builder, appending to `pending`.
[[nodiscard]] Status lower_stage(Lowering& state, const Graph& graph, Stage stage,
                                 StageLowering& out) noexcept {
    const NodeNames& n = names();
    for (const GraphNode& node : graph.nodes()) {
        if (node.muted) {
            continue;
        }
        const bool is_write = node.type == n.set_attribute;
        const bool is_kill = node.type == n.kill_if;
        const bool is_spawn = node.type == n.spawn_count;
        const bool is_event = node.type == n.emit_event;
        if (!is_write && !is_kill && !is_spawn && !is_event) {
            continue;
        }
        auto value = lower_pin(state, graph, node.key, prop::kValue);
        if (!value) {
            return make_unexpected(value.error());
        }
        if (is_kill) {
            out.kill = value.value();
            continue;
        }
        if (is_event) {
            const Literal* channel = graph.property(node.key, n.prop_channel);
            if (channel == nullptr || channel->text.is_empty()) {
                report(*state.sink, node.key, n.prop_channel,
                       "vfx: an event raise names no channel");
                return fail(ErrorCode::InvalidArgument, "vfx: a raise with no channel");
            }
            if (state.asset->find_channel(channel->text) == nullptr) {
                report(
                    *state.sink, node.key, n.prop_channel,
                    "vfx: this system declares no event channel of that name — a channel carries "
                    "both of its bounds and one that is not declared has neither",
                    channel->text);
                return fail(ErrorCode::NotFound, "vfx: an undeclared event channel");
            }
            PendingEvent raise;
            raise.channel = channel->text;
            raise.value = value.value();
            raise.origin = node.key;
            return state.raises->push_back(raise);
        }
        if (is_spawn) {
            if (stage != Stage::Spawn) {
                report(*state.sink, node.key, Name{},
                       "vfx: a spawn count belongs to the Spawn stage");
                return fail(ErrorCode::InvalidArgument, "vfx: a spawn count outside Spawn");
            }
            out.spawn = value.value();
            continue;
        }
        const Literal* named = graph.property(node.key, n.prop_attribute);
        if (named == nullptr || named->text.is_empty()) {
            report(*state.sink, node.key, n.prop_attribute,
                   "vfx: a set-attribute node names no attribute");
            return fail(ErrorCode::InvalidArgument, "vfx: a write with no attribute");
        }
        const TypeId type = state.builder->node(value.value()).type;
        if (Status touched = state.layout->touch(named->text, type, false, true); !touched) {
            report(*state.sink, node.key, n.prop_attribute,
                   "vfx: this emitter writes more attributes than a kernel has write roots",
                   named->text);
            return touched;
        }
        PendingWrite write;
        write.attribute = named->text;
        write.value = value.value();
        write.type = type;
        write.origin = node.key;
        if (Status pushed = state.pending->push_back(write); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The last write to each attribute, in first-write order — which is the order an author reads and
/// is stable across a rebuild.
[[nodiscard]] Status collapse_writes(const Array<PendingWrite>& pending,
                                     Array<PendingWrite>& out) noexcept {
    out.clear();
    for (const PendingWrite& write : pending) {
        bool replaced = false;
        for (PendingWrite& existing : out) {
            if (existing.attribute == write.attribute) {
                existing = write;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (Status pushed = out.push_back(write); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// Build the flat program the CPU path steps through. NOT a graph interpreter — see ir.h.
[[nodiscard]] Status build_program(VfxKernel& kernel, const Module& module,
                                   Span<const u32> write_roots, Span<const u32> event_roots,
                                   Allocator& allocator) noexcept {
    Array<NodeId> order(allocator);
    if (Status walked = graph::canonical_order(module, module.roots(), order); !walked) {
        return walked;
    }
    Array<u16> slot_of(allocator);
    if (Status sized = slot_of.resize(module.size()); !sized) {
        return sized;
    }
    for (u16& slot : slot_of) {
        slot = 0xFFFFU;
    }
    if (order.size() > 0xFFFEU) {
        return fail(ErrorCode::OutOfRange,
                    "vfx: a kernel needs more than 65534 slots, which is more than the compiled "
                    "program's operand encoding can address");
    }

    Array<KernelStep>& program = KernelAccess::program(kernel);
    program.clear();
    for (usize index = 0; index < order.size(); ++index) {
        const NodeId id = order[index];
        slot_of[id] = static_cast<u16>(index);
        const graph::Node& node = module.node(id);
        KernelStep step;
        step.op = node.op;
        step.dst = static_cast<u16>(index);
        step.type = node.type;
        step.value = node.value;
        step.symbol = node.symbol;
        const Span<const NodeId> operands = module.operands(id);
        u16* targets[graph::kMaxOperands] = {&step.a, &step.b, &step.c, &step.d};
        for (usize operand = 0; operand < operands.size() && operand < graph::kMaxOperands;
             ++operand) {
            *targets[operand] = slot_of[operands[operand]];
        }
        if (Status pushed = program.push_back(step); !pushed) {
            return pushed;
        }
    }
    KernelAccess::set_slots(kernel, static_cast<u32>(order.size()));

    Array<u16>& write_slots = KernelAccess::write_slots(kernel);
    write_slots.clear();
    for (const u32 root : write_roots) {
        const NodeId id = module.root(root);
        if (Status pushed = write_slots.push_back(id == kInvalidNode ? 0xFFFFU : slot_of[id]);
            !pushed) {
            return pushed;
        }
    }
    Array<u16>& event_slots = KernelAccess::event_slots(kernel);
    event_slots.clear();
    for (const u32 root : event_roots) {
        const NodeId id = module.root(root);
        if (Status pushed = event_slots.push_back(id == kInvalidNode ? 0xFFFFU : slot_of[id]);
            !pushed) {
            return pushed;
        }
    }
    const NodeId kill = module.root(kKillRoot);
    KernelAccess::set_kill_slot(kernel, kill == kInvalidNode ? 0xFFFFU : slot_of[kill]);
    const NodeId spawn = module.root(kSpawnRoot);
    KernelAccess::set_spawn_slot(kernel, spawn == kInvalidNode ? 0xFFFFU : slot_of[spawn]);
    return ok();
}

/// Give each collapsed write a root slot, and record it on the kernel. The slot IS the write's
/// identity through every rebuild the optimiser performs: a rebuild renumbers node ids and carries
/// the roots across.
[[nodiscard]] Status assign_write_roots(Builder& builder, VfxKernel& kernel,
                                        Span<const PendingWrite> collapsed,
                                        Array<u32>& roots) noexcept {
    for (usize index = 0; index < collapsed.size(); ++index) {
        const auto slot = static_cast<u32>(index);
        if (Status set = builder.set_root(slot, collapsed[index].value); !set) {
            return set;
        }
        KernelWrite write;
        write.attribute = collapsed[index].attribute;
        write.root_slot = slot;
        write.origin = collapsed[index].origin;
        if (Status pushed = KernelAccess::writes(kernel).push_back(write); !pushed) {
            return pushed;
        }
        if (Status pushed = roots.push_back(slot); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The same, for the event raises, which take the slots after the writes.
[[nodiscard]] Status assign_event_roots(Builder& builder, VfxKernel& kernel,
                                        Span<const PendingEvent> raises, u32 first_slot,
                                        Array<u32>& roots) noexcept {
    for (usize index = 0; index < raises.size(); ++index) {
        const auto slot = static_cast<u32>(first_slot + index);
        if (Status set = builder.set_root(slot, raises[index].value); !set) {
            return set;
        }
        KernelEvent raise;
        raise.channel = raises[index].channel;
        raise.root_slot = slot;
        raise.origin = raises[index].origin;
        if (Status pushed = KernelAccess::events(kernel).push_back(raise); !pushed) {
            return pushed;
        }
        if (Status pushed = roots.push_back(slot); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// One kernel, from one or two stages. `fuse` names the second stage lowered onto the first.
[[nodiscard]] Expected<VfxKernel, Error> compile_kernel(
    const VfxSystemAsset& asset, const Emitter& emitter, Stage primary, Stage fused,
    const DataInterfaceRegistry& interfaces, const CompileOptions& options, AttributeLayout& layout,
    DiagnosticSink& sink, EmitterReport& report, Allocator& allocator) noexcept {
    // THE MODULE'S NAME IS THE STAGE'S, NOT THE EMITTER'S, and that is load-bearing rather than
    // cosmetic: `Module::digest` closes over the module name, so naming it after the emitter would
    // give two structurally identical kernels two digests — and the scheduler groups by digest, so
    // nothing would ever merge. The emitter's name stays on the KERNEL, where a diagnostic reads
    // it.
    Builder builder(allocator, vfx_domain(), Name::intern(stage_name(primary)));
    builder.set_policy(graph::builder_policy(options.switches));

    Array<Memo> memo(allocator);
    Array<PendingWrite> pending(allocator);
    Array<PendingEvent> raises(allocator);
    Lowering state;
    state.builder = &builder;
    state.asset = &asset;
    state.emitter = &emitter;
    state.interfaces = &interfaces;
    state.options = &options;
    state.sink = &sink;
    state.layout = &layout;
    state.report = &report;
    state.memo = &memo;
    state.pending = &pending;
    state.raises = &raises;

    StageLowering primary_result;
    const Graph* primary_graph = emitter.stage(primary);
    if (primary_graph == nullptr) {
        return fail(ErrorCode::NotFound, "vfx: the stage this kernel is for has no graph");
    }
    if (Status lowered = lower_stage(state, *primary_graph, primary, primary_result); !lowered) {
        return make_unexpected(lowered.error());
    }

    StageLowering fused_result;
    if (fused != Stage::Count) {
        const Graph* fused_graph = emitter.stage(fused);
        if (fused_graph != nullptr) {
            // The memo is cleared because the two stages are two graphs and a `NodeKey` is unique
            // only within one; `pending` is NOT, which is the whole of the fusion.
            memo.clear();
            state.substitute_pending = true;
            if (Status lowered = lower_stage(state, *fused_graph, fused, fused_result); !lowered) {
                return make_unexpected(lowered.error());
            }
            state.substitute_pending = false;
        }
    }

    Array<PendingWrite> collapsed(allocator);
    if (Status folded = collapse_writes(pending, collapsed); !folded) {
        return make_unexpected(folded.error());
    }
    if (collapsed.size() + raises.size() > kMaxKernelWrites) {
        return fail(ErrorCode::OutOfRange,
                    "vfx: a kernel's attribute writes and event raises together need more roots "
                    "than the domain declares");
    }

    VfxKernel kernel(allocator, vfx_domain());
    KernelAccess::set_name(kernel, emitter.name());
    KernelAccess::add_stage(kernel, primary);
    if (fused != Stage::Count && emitter.has_stage(fused)) {
        KernelAccess::add_stage(kernel, fused);
    }

    Array<u32> write_roots(allocator);
    Array<u32> event_roots(allocator);
    if (Status assigned = assign_write_roots(builder, kernel, collapsed.span(), write_roots);
        !assigned) {
        return make_unexpected(assigned.error());
    }
    if (Status assigned = assign_event_roots(builder, kernel, raises.span(),
                                             static_cast<u32>(collapsed.size()), event_roots);
        !assigned) {
        return make_unexpected(assigned.error());
    }

    const NodeId kill = fused_result.kill != kInvalidNode ? fused_result.kill : primary_result.kill;
    if (kill != kInvalidNode) {
        if (Status set = builder.set_root(kKillRoot, kill); !set) {
            return make_unexpected(set.error());
        }
    }
    const NodeId spawn =
        primary_result.spawn != kInvalidNode ? primary_result.spawn : fused_result.spawn;
    if (spawn != kInvalidNode) {
        if (Status set = builder.set_root(kSpawnRoot, spawn); !set) {
            return make_unexpected(set.error());
        }
    }

    auto built = builder.finish();
    if (!built) {
        return make_unexpected(built.error());
    }
    report.nodes_before_optimisation += built.value().size();

    OptimiseReport optimise_report(allocator);
    auto optimised = graph::optimise(built.value(), options.switches, optimise_report);
    if (!optimised) {
        return make_unexpected(optimised.error());
    }
    report.nodes_after_optimisation += optimised.value().size();
    report.folded_constants += optimise_report.folded_constants;
    report.merged_values += optimise_report.merged_values;
    report.dropped_nodes += optimise_report.dropped_nodes;

    const u64 module_digest = optimised.value().digest();
    KernelAccess::set_module(kernel, std::move(optimised.value()));
    if (Status programmed = build_program(kernel, kernel.expressions(), write_roots.span(),
                                          event_roots.span(), allocator);
        !programmed) {
        return make_unexpected(programmed.error());
    }
    u64 digest = hash_u64(kHashSeed, graph::kIrVersion);
    digest = hash_u64(digest, kernel.stages());
    digest = hash_u64(digest, module_digest);
    KernelAccess::set_digest(kernel, digest);
    return kernel;
}

/// The attributes the renderer consumes. An attribute of one of these names is never elided even
/// when no stage reads it, because the renderer is a reader this compiler cannot see.
///
/// Documented rather than inferred: an effect whose render stage READS an attribute marks it read
/// through the ordinary path, and this list is what keeps a sprite's size and colour alive in an
/// emitter that has no render stage at all.
[[nodiscard]] Status renderer_inputs(const Emitter& emitter, Array<Name>& out) noexcept {
    static constexpr const char* kNames[] = {"position", "size", "color", "emission"};
    out.clear();
    for (const char* name : kNames) {
        if (Status pushed = out.push_back(Name::intern(name)); !pushed) {
            return pushed;
        }
    }
    // Whatever the render stage itself writes is an output by construction.
    const Graph* render = emitter.stage(Stage::Render);
    if (render == nullptr) {
        return ok();
    }
    const Name attribute = Name::intern(prop::kAttribute);
    const Name set_attribute = Name::intern("vfx.set_attribute");
    for (const GraphNode& node : render->nodes()) {
        if (node.type != set_attribute) {
            continue;
        }
        if (const Literal* named = render->property(node.key, attribute); named != nullptr) {
            if (Status pushed = out.push_back(named->text); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace

// --- The public surface --------------------------------------------------------------------------

CompiledEmitter::CompiledEmitter(Allocator& allocator) noexcept
    : layout_(allocator), kernels_(allocator), sources_(allocator) {}

const VfxKernel* CompiledEmitter::kernel_for(Stage stage) const noexcept {
    // AN EXACT MATCH WINS. With Initialise and Update fused there are two kernels covering Update —
    // the fused one, which runs over the newly spawned, and the plain one, which runs over the
    // live. Asking for `Update` and being handed the fused kernel would re-initialise every live
    // particle every step, which is the defect this preference exists to make impossible.
    for (const VfxKernel& kernel : kernels_) {
        if (kernel.stages() == stage_bit(stage)) {
            return &kernel;
        }
    }
    for (const VfxKernel& kernel : kernels_) {
        if (kernel.covers(stage)) {
            return &kernel;
        }
    }
    return nullptr;
}

usize CompiledEmitter::source_bytes() const noexcept {
    usize bytes = 0;
    for (const GeneratedSource& source : sources_) {
        bytes += source.text.size();
    }
    return bytes;
}

CompiledSystem::CompiledSystem(Allocator& allocator) noexcept
    : emitters_(allocator), parameters_(allocator), channels_(allocator) {}

const CompiledEmitter* CompiledSystem::find_emitter(Name emitter) const noexcept {
    for (const CompiledEmitter& candidate : emitters_) {
        if (candidate.name() == emitter) {
            return &candidate;
        }
    }
    return nullptr;
}

u32 CompiledSystem::capacity() const noexcept {
    u32 total = 0;
    for (const CompiledEmitter& emitter : emitters_) {
        total += emitter.capacity();
    }
    return total;
}

namespace {

/// Validate every stage graph of one emitter. Node- and pin-precise diagnostics land in `sink`; the
/// return says only whether anything is wrong, because "which node" is the question an author has.
[[nodiscard]] Status validate_emitter(const Emitter& emitter, const NodeRegistry& registry,
                                      DiagnosticSink& sink) noexcept {
    for (u32 which = 0; which < static_cast<u32>(Stage::Count); ++which) {
        const Graph* stage_graph = emitter.stage(static_cast<Stage>(which));
        if (stage_graph == nullptr) {
            continue;
        }
        if (Status valid = graph::validate(*stage_graph, registry, nullptr, sink); !valid) {
            return valid;
        }
    }
    if (sink.errors() != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "vfx: a stage graph did not validate; the sink names the node and the pin");
    }
    return ok();
}

/// Which kernels one emitter compiles to, and which of them carries a fused second stage.
struct KernelPlan {
    Stage primary = Stage::Count;
    Stage fused = Stage::Count;
};

/// THE FUSION DECISION, and the two numbers that make it measurable. Unfused, a particle spawned
/// this step is touched by two dispatches — Initialise then Update — and its initial values
/// round-trip through memory between them. Fused, it is touched by one.
[[nodiscard]] u32 plan_kernels(const Emitter& emitter, const CompileOptions& options,
                               EmitterReport& report, KernelPlan* out) noexcept {
    const bool both = emitter.has_stage(Stage::Initialise) && emitter.has_stage(Stage::Update);
    const bool can_fuse = options.fuse_stages && both;
    report.dispatches_before_fusion = both ? 2U : 1U;
    report.dispatches_after_fusion = can_fuse ? 1U : report.dispatches_before_fusion;

    u32 count = 0;
    for (u32 which = 0; which < static_cast<u32>(Stage::Count); ++which) {
        const auto stage = static_cast<Stage>(which);
        if (!emitter.has_stage(stage)) {
            continue;
        }
        out[count].primary = stage;
        out[count].fused = (can_fuse && stage == Stage::Initialise) ? Stage::Update : Stage::Count;
        ++count;
    }
    return count;
}

/// Cook one emitter: its kernels, its derived layout, its generated Slang and its report.
[[nodiscard]] Expected<CompiledEmitter, Error> compile_emitter(
    const VfxSystemAsset& asset, const Emitter& emitter, const DataInterfaceRegistry& interfaces,
    const CompileOptions& options, DiagnosticSink& sink, EmitterReport& emitter_report,
    Allocator& allocator) noexcept {
    CompiledEmitter compiled(allocator);
    CompiledAccess::set_name(compiled, emitter.name());
    CompiledAccess::set_path(compiled, emitter.path());
    CompiledAccess::set_capacity(compiled, emitter.capacity());
    AttributeLayout& layout = CompiledAccess::layout(compiled);

    KernelPlan plan[static_cast<usize>(Stage::Count)];
    const u32 plan_count = plan_kernels(emitter, options, emitter_report, plan);
    for (u32 which = 0; which < plan_count; ++which) {
        auto kernel = compile_kernel(asset, emitter, plan[which].primary, plan[which].fused,
                                     interfaces, options, layout, sink, emitter_report, allocator);
        if (!kernel) {
            return make_unexpected(kernel.error());
        }
        if (Status pushed = CompiledAccess::kernels(compiled).push_back(std::move(*kernel));
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Array<Name> outputs(allocator);
    if (Status listed = renderer_inputs(emitter, outputs); !listed) {
        return make_unexpected(listed.error());
    }
    if (options.attribute_liveness) {
        layout.eliminate_dead(outputs.span());
    }
    for (const AttributeDecl& decl : emitter.attributes()) {
        if (Status declared = layout.declare(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }
    if (Status resolved = layout.resolve(emitter.capacity()); !resolved) {
        return make_unexpected(resolved.error());
    }
    if (Status emitted = emit_emitter_sources(compiled, asset.parameters()); !emitted) {
        return make_unexpected(emitted.error());
    }

    emitter_report.kernels = static_cast<u32>(compiled.kernels().size());
    emitter_report.bytes_per_particle = layout.bytes_per_particle();
    emitter_report.max_population = layout.max_population(options.memory_budget_bytes);
    emitter_report.attributes_allocated = layout.allocated_attributes();
    emitter_report.attributes_elided = layout.elided_attributes();
    emitter_report.estimated_cost_units =
        static_cast<u64>(emitter_report.sample_cost_weight + emitter_report.kernels) *
        static_cast<u64>(options.reference_population);
    emitter_report.generated_source_bytes = static_cast<u32>(compiled.source_bytes());

    u64 digest = hash_u64(kHashSeed, layout.digest());
    for (const VfxKernel& kernel : compiled.kernels()) {
        digest = hash_u64(digest, kernel.digest());
    }
    CompiledAccess::set_digest(compiled, digest);
    return compiled;
}

/// The half of the cook key that is not an emitter's digest: the asset's identity, the interface
/// registry's version set, the options, the parameters and the channels.
[[nodiscard]] u64 declaration_key(const VfxSystemAsset& asset,
                                  const DataInterfaceRegistry& interfaces,
                                  const CompileOptions& options) noexcept {
    u64 key = hash_u64(kHashSeed, graph::kIrVersion);
    key = hash_text(key, asset.name().text());
    key = hash_u64(key, interfaces.digest());
    key = hash_u64(key, options.fuse_stages ? 1U : 0U);
    key = hash_u64(key, options.fold_parameters ? 1U : 0U);
    key = hash_u64(key, options.attribute_liveness ? 1U : 0U);
    for (const ParameterDecl& parameter : asset.parameters()) {
        key = hash_text(key, parameter.name.text());
        key = hash_bytes(key, parameter.value, sizeof(parameter.value));
        key = hash_u64(key, parameter.exposed ? 1U : 0U);
    }
    for (const EventChannelDecl& channel : asset.channels()) {
        key = hash_text(key, channel.name.text());
        key = hash_u64(key, channel.max_events_per_frame);
        key = hash_u64(key, channel.max_chain_depth);
        key = hash_u64(key, channel.readback ? 1U : 0U);
    }
    return key;
}

}  // namespace

Expected<CompiledSystem, Error> compile_system(const VfxSystemAsset& asset,
                                               const NodeRegistry& registry,
                                               const DataInterfaceRegistry& interfaces,
                                               const CompileOptions& options, DiagnosticSink& sink,
                                               CompileReport& report) noexcept {
    Allocator& allocator = asset.allocator();
    CompiledSystem system(allocator);
    CompiledAccess::set_name(system, asset.name());
    CompiledAccess::set_importance(system, asset.importance());
    CompiledAccess::set_scalability(system, asset.scalability());
    report.bisection_build = report.bisection_build || !options.switches.all_enabled();

    for (const ParameterDecl& parameter : asset.parameters()) {
        if (Status pushed = CompiledAccess::parameters(system).push_back(parameter); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (const EventChannelDecl& channel : asset.channels()) {
        if (Status pushed = CompiledAccess::channels(system).push_back(channel); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    u64 cook_key = declaration_key(asset, interfaces, options);
    for (const Emitter& emitter : asset.emitters()) {
        // EVERY STAGE GRAPH IS VALIDATED BEFORE IT IS LOWERED, so a wire whose types do not convert
        // is a node- and pin-precise diagnostic from the authoring layer rather than a type error
        // from the builder with no node in it.
        if (Status valid = validate_emitter(emitter, registry, sink); !valid) {
            return make_unexpected(valid.error());
        }

        EmitterReport emitter_report;
        emitter_report.emitter = emitter.name();
        emitter_report.path = emitter.path();
        auto compiled =
            compile_emitter(asset, emitter, interfaces, options, sink, emitter_report, allocator);
        if (!compiled) {
            return make_unexpected(compiled.error());
        }
        cook_key = hash_u64(cook_key, compiled->digest());

        report.kernels += emitter_report.kernels;
        report.total_bytes_per_particle += emitter_report.bytes_per_particle;
        if (Status pushed = report.emitters.push_back(emitter_report); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = CompiledAccess::emitters(system).push_back(std::move(*compiled));
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    CompiledAccess::set_cook_key(system, cook_key);
    return system;
}

}  // namespace cy::vfx
