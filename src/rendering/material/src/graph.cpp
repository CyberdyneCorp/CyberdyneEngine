// The node-graph front-end. M7 task 6.1. See graph.h for why it is shaped like an editor.

#include <cy/rendering/material/graph.h>

#include <utility>

namespace cy::rendering::material {
namespace {

struct PortSpec {
    /// How many value inputs the node has, before the weight.
    u8 inputs;
    /// True when the last port is a closure weight, defaulting to one.
    bool weighted;
};

[[nodiscard]] PortSpec ports_of(GraphOp op) noexcept {
    switch (op) {
        case GraphOp::Constant:
        case GraphOp::Parameter:
        case GraphOp::Attribute:
        case GraphOp::Field:
            return {0, false};
        case GraphOp::TextureSample:
        case GraphOp::OneMinus:
        case GraphOp::Saturate:
        case GraphOp::Swizzle:
            return {1, false};
        case GraphOp::Multiply:
        case GraphOp::Add:
        case GraphOp::Subtract:
        case GraphOp::Divide:
        case GraphOp::Combine:
        case GraphOp::AddClosures:
        case GraphOp::LayerClosures:
        case GraphOp::Custom:
            return {2, false};
        case GraphOp::Lerp:
            return {3, false};
        case GraphOp::Diffuse:
        case GraphOp::Coat:
        case GraphOp::Sheen:
        case GraphOp::Emission:
        case GraphOp::Transmission:
        case GraphOp::Subsurface:
            return {1, true};
        case GraphOp::Specular:
            return {2, true};
        case GraphOp::Count:
            break;
    }
    return {0, false};
}

[[nodiscard]] Op ir_op(GraphOp op) noexcept {
    switch (op) {
        case GraphOp::Multiply:
            return Op::Mul;
        case GraphOp::Add:
            return Op::Add;
        case GraphOp::Subtract:
            return Op::Sub;
        case GraphOp::Divide:
            return Op::Div;
        case GraphOp::OneMinus:
            return Op::OneMinus;
        case GraphOp::Saturate:
            return Op::Saturate;
        case GraphOp::Lerp:
            return Op::Lerp;
        case GraphOp::Swizzle:
            return Op::Swizzle;
        case GraphOp::Combine:
            return Op::Combine;
        case GraphOp::Custom:
            return Op::Custom;
        case GraphOp::Diffuse:
            return Op::Diffuse;
        case GraphOp::Specular:
            return Op::Specular;
        case GraphOp::Coat:
            return Op::Coat;
        case GraphOp::Sheen:
            return Op::Sheen;
        case GraphOp::Emission:
            return Op::Emission;
        case GraphOp::Transmission:
            return Op::Transmission;
        case GraphOp::Subsurface:
            return Op::Subsurface;
        case GraphOp::AddClosures:
            return Op::ClosureAdd;
        case GraphOp::LayerClosures:
            return Op::ClosureLayer;
        default:
            return Op::Constant;
    }
}

/// Lowering state: the map from graph node to IR value, and the builder they go into.
struct Lowering {
    const MaterialGraph* graph = nullptr;
    Builder* builder = nullptr;
    Array<NodeId>* mapped = nullptr;
};

[[nodiscard]] Expected<NodeId, Error> default_for(Builder& builder, ValueType type) noexcept {
    // An unwired port is a zero of its type. An editor draws it as an empty socket with a value
    // beside it; what matters here is that it is a VALUE — a lowering that failed on an unwired
    // port could not lower a graph under construction, which is every graph an author is editing.
    return builder.constant(type, Immediate{});
}

[[nodiscard]] Expected<NodeId, Error> lower_leaf(const GraphNode& node, Builder& builder,
                                                 Span<const NodeId> operands) noexcept {
    switch (node.op) {
        case GraphOp::Constant:
            return builder.constant(node.type, node.value);
        case GraphOp::Parameter:
            return builder.parameter(node.symbol);
        case GraphOp::Attribute:
            return builder.attribute(node.symbol, node.type);
        case GraphOp::Field:
            return builder.field(node.symbol, node.type);
        case GraphOp::TextureSample:
            return builder.texture_sample(node.symbol, operands[0]);
        default:
            break;
    }
    return kInvalidNode;
}

[[nodiscard]] Expected<NodeId, Error> lower_node(const Lowering& state,
                                                 const GraphNode& node) noexcept {
    Builder& builder = *state.builder;
    const PortSpec ports = ports_of(node.op);

    NodeId operands[MaterialGraph::kMaxPorts] = {};
    for (u8 port = 0; port < ports.inputs; ++port) {
        const u32 wired = state.graph->input(node.id, port);
        if (wired != kInvalidNode && (*state.mapped)[wired] != kInvalidNode) {
            operands[port] = (*state.mapped)[wired];
            continue;
        }
        // The type an unwired port defaults to. A closure's colour is a float3 and everything else
        // is a float, which is enough for a graph under construction to still compile.
        const bool colour =
            port == 0 && op_is_leaf_closure(ir_op(node.op)) && node.op != GraphOp::Coat;
        auto fallback = default_for(builder, colour ? ValueType::Vec3 : ValueType::Float);
        if (!fallback) {
            return fallback;
        }
        operands[port] = fallback.value();
    }

    if (ports.inputs == 0 || node.op == GraphOp::TextureSample) {
        return lower_leaf(node, builder, Span<const NodeId>(operands, ports.inputs));
    }

    auto made = builder.make(ir_op(node.op), node.type, node.symbol, node.value,
                             Span<const NodeId>(operands, ports.inputs));
    if (!made || !ports.weighted) {
        return made;
    }

    // THE WEIGHT PORT. An editor puts one on every closure whether the author touched it or not, so
    // this is emitted unconditionally — a weight of one for an untouched port, of zero for a muted
    // node. Closure simplification removes both, which is what makes the graph and the text
    // definition one material rather than two.
    const u32 wired = state.graph->input(node.id, ports.inputs);
    NodeId weight = kInvalidNode;
    if (!node.muted && wired != kInvalidNode && (*state.mapped)[wired] != kInvalidNode) {
        weight = (*state.mapped)[wired];
    } else {
        auto literal = builder.constant_float(node.muted ? 0.0F : 1.0F);
        if (!literal) {
            return literal;
        }
        weight = literal.value();
    }
    const NodeId scaled[] = {made.value(), weight};
    return builder.make(Op::ClosureScale, ValueType::Closure, Name{}, Immediate{},
                        Span<const NodeId>(scaled, 2));
}

}  // namespace

const char* graph_op_name(GraphOp op) noexcept {
    switch (op) {
        case GraphOp::Constant:
            return "constant";
        case GraphOp::Parameter:
            return "parameter";
        case GraphOp::Attribute:
            return "attribute";
        case GraphOp::Field:
            return "field";
        case GraphOp::TextureSample:
            return "texture_sample";
        case GraphOp::Multiply:
            return "multiply";
        case GraphOp::Add:
            return "add";
        case GraphOp::Subtract:
            return "subtract";
        case GraphOp::Divide:
            return "divide";
        case GraphOp::OneMinus:
            return "one_minus";
        case GraphOp::Saturate:
            return "saturate";
        case GraphOp::Lerp:
            return "lerp";
        case GraphOp::Swizzle:
            return "swizzle";
        case GraphOp::Combine:
            return "combine";
        case GraphOp::Custom:
            return "custom";
        case GraphOp::Diffuse:
            return "diffuse";
        case GraphOp::Specular:
            return "specular";
        case GraphOp::Coat:
            return "coat";
        case GraphOp::Sheen:
            return "sheen";
        case GraphOp::Emission:
            return "emission";
        case GraphOp::Transmission:
            return "transmission";
        case GraphOp::Subsurface:
            return "subsurface";
        case GraphOp::AddClosures:
            return "add_closures";
        case GraphOp::LayerClosures:
            return "layer_closures";
        case GraphOp::Count:
            break;
    }
    return "?";
}

MaterialGraph::MaterialGraph(Allocator& allocator, Name material_name) noexcept
    : name_(material_name),
      nodes_(allocator),
      links_(allocator),
      parameters_(allocator),
      textures_(allocator) {}

Status MaterialGraph::declare_parameter(const ParameterDecl& decl) noexcept {
    return parameters_.push_back(decl);
}

Status MaterialGraph::declare_texture(const TextureDecl& decl) noexcept {
    return textures_.push_back(decl);
}

Expected<u32, Error> MaterialGraph::add(GraphOp op, Name symbol, ValueType type,
                                        const Immediate& value) noexcept {
    GraphNode node;
    node.id = static_cast<u32>(nodes_.size());
    node.op = op;
    node.symbol = symbol;
    node.type = type;
    node.value = value;
    if (Status pushed = nodes_.push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }
    for (u8 port = 0; port < kMaxPorts; ++port) {
        if (Status pushed = links_.push_back(kInvalidNode); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return node.id;
}

Status MaterialGraph::connect(u32 from, u32 to, u8 port) noexcept {
    if (from >= nodes_.size() || to >= nodes_.size() || port >= kMaxPorts) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node or port", 0});
    }
    if (from >= to) {
        // The graph is acyclic and stored in dependency order, which is what an editor's own
        // topological sort produces and what makes lowering one pass.
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a wire must run from an earlier node to a later one", 0});
    }
    links_[(to * kMaxPorts) + port] = from;
    return ok();
}

Status MaterialGraph::mute(u32 node, bool muted) noexcept {
    if (node >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node", 0});
    }
    nodes_[node].muted = muted;
    return ok();
}

Status MaterialGraph::annotate(u32 node, NodeFlags flags) noexcept {
    if (node >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node", 0});
    }
    nodes_[node].flags = nodes_[node].flags | flags;
    return ok();
}

Status MaterialGraph::set_surface_output(u32 node) noexcept {
    if (node >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node", 0});
    }
    surface_ = node;
    return ok();
}

Status MaterialGraph::set_opacity_output(u32 node) noexcept {
    if (node >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node", 0});
    }
    opacity_ = node;
    return ok();
}

u32 MaterialGraph::input(u32 node, u8 port) const noexcept {
    if (node >= nodes_.size() || port >= kMaxPorts) {
        return kInvalidNode;
    }
    return links_[(node * kMaxPorts) + port];
}

Expected<Module, Error> lower_graph(const MaterialGraph& graph, Allocator& allocator,
                                    const PassSwitches& switches) noexcept {
    Builder builder(allocator, graph.name());
    builder.set_policy(builder_policy(switches));
    for (const ParameterDecl& decl : graph.parameters()) {
        if (Status declared = builder.declare_parameter(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }
    for (const TextureDecl& decl : graph.textures()) {
        if (Status declared = builder.declare_texture(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }

    Array<NodeId> mapped(allocator);
    if (Status sized = mapped.resize(graph.nodes().size()); !sized) {
        return make_unexpected(sized.error());
    }
    Lowering state;
    state.graph = &graph;
    state.builder = &builder;
    state.mapped = &mapped;

    for (const GraphNode& node : graph.nodes()) {
        auto lowered = lower_node(state, node);
        if (!lowered) {
            return make_unexpected(lowered.error());
        }
        mapped[node.id] = lowered.value();
        if (lowered.value() == kInvalidNode) {
            continue;
        }
        // Provenance and annotations travel to the value, not into it.
        if (Status recorded = builder.add_origin(lowered.value(), node.id); !recorded) {
            return make_unexpected(recorded.error());
        }
        if (Status annotated = builder.annotate(lowered.value(), node.flags); !annotated) {
            return make_unexpected(annotated.error());
        }
    }

    if (graph.surface_output() != kInvalidNode) {
        if (Status set = builder.set_surface(mapped[graph.surface_output()]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (graph.opacity_output() != kInvalidNode) {
        if (Status set = builder.set_opacity(mapped[graph.opacity_output()]); !set) {
            return make_unexpected(set.error());
        }
    }
    return builder.finish();
}

}  // namespace cy::rendering::material
