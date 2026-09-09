// The material anchor's authoring front end, and the tree's own reference material. Task 2.3.
//
// `src/rendering/material/graph.h` explains why this shape is deliberate and must not be tidied:
// "a graph front-end written to look like the IR would meet [the criterion] by construction and
// prove nothing". Every wart is one an editor produces.

#include "material_anchor.h"

namespace cy::graph::anchor {
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

[[nodiscard]] OpId ir_op(GraphOp op) noexcept {
    switch (op) {
        case GraphOp::Multiply:
            return OpMul;
        case GraphOp::Add:
            return OpAdd;
        case GraphOp::Subtract:
            return OpSub;
        case GraphOp::Divide:
            return OpDiv;
        case GraphOp::OneMinus:
            return OpOneMinus;
        case GraphOp::Saturate:
            return OpSaturate;
        case GraphOp::Lerp:
            return OpLerp;
        case GraphOp::Swizzle:
            return OpSwizzle;
        case GraphOp::Combine:
            return OpCombine;
        case GraphOp::Custom:
            return OpCustom;
        case GraphOp::Diffuse:
            return OpDiffuse;
        case GraphOp::Specular:
            return OpSpecular;
        case GraphOp::Coat:
            return OpCoat;
        case GraphOp::Sheen:
            return OpSheen;
        case GraphOp::Emission:
            return OpEmission;
        case GraphOp::Transmission:
            return OpTransmission;
        case GraphOp::Subsurface:
            return OpSubsurface;
        case GraphOp::AddClosures:
            return OpClosureAdd;
        case GraphOp::LayerClosures:
            return OpClosureLayer;
        default:
            return OpConstant;
    }
}

[[nodiscard]] bool is_leaf_closure(GraphOp op) noexcept {
    const OpId lowered = ir_op(op);
    return lowered >= OpDiffuse && lowered <= OpEmission;
}

struct Lowering {
    const MaterialGraph* graph = nullptr;
    Builder* builder = nullptr;
    Array<NodeId>* mapped = nullptr;
};

[[nodiscard]] Expected<NodeId, Error> constant_of(Builder& builder, TypeId type,
                                                  const Immediate& value) noexcept {
    return builder.make(OpConstant, type, Name{}, value, {});
}

[[nodiscard]] Expected<NodeId, Error> lower_leaf(const GraphNode& node, Builder& builder,
                                                 Span<const NodeId> operands) noexcept {
    switch (node.op) {
        case GraphOp::Constant:
            return constant_of(builder, node.type, node.value);
        case GraphOp::Parameter: {
            const Decl* decl = builder.find_decl(ParameterDecl, node.symbol);
            if (decl == nullptr) {
                return make_unexpected(
                    Error{ErrorCode::NotFound,
                          "a parameter must be declared before it is used, so that a misspelt name "
                          "is a diagnostic rather than a silent zero",
                          0});
            }
            return builder.make(OpParameter, decl->type, node.symbol, Immediate{}, {});
        }
        case GraphOp::Attribute:
            return builder.make(OpAttribute, node.type, node.symbol, Immediate{}, {});
        case GraphOp::Field:
            return builder.make(OpField, node.type, node.symbol, Immediate{}, {});
        case GraphOp::TextureSample:
            if (builder.find_decl(TextureDecl, node.symbol) == nullptr) {
                return make_unexpected(Error{ErrorCode::NotFound,
                                             "a texture must be declared before it is sampled", 0});
            }
            return builder.make(OpTextureSample, Vec4, node.symbol, Immediate{}, operands);
        default:
            break;
    }
    return kInvalidNode;
}

[[nodiscard]] Expected<NodeId, Error> lower_weight(const Lowering& state, const GraphNode& node,
                                                   const PortSpec& ports, NodeId value) noexcept {
    // THE WEIGHT PORT. An editor puts one on every closure whether the author touched it or not, so
    // this is emitted unconditionally — a weight of one for an untouched port, of zero for a muted
    // node. Closure simplification removes both, which is what makes the graph and a hand-written
    // definition one material rather than two.
    Builder& builder = *state.builder;
    const u32 wired = state.graph->input(node.id, ports.inputs);
    NodeId weight = kInvalidNode;
    if (!node.muted && wired != kInvalidNode && (*state.mapped)[wired] != kInvalidNode) {
        weight = (*state.mapped)[wired];
    } else {
        auto literal = constant_of(builder, Float, Immediate::scalar(node.muted ? 0.0F : 1.0F));
        if (!literal) {
            return literal;
        }
        weight = literal.value();
    }
    const NodeId scaled[] = {value, weight};
    return builder.make(OpClosureScale, Closure, Name{}, Immediate{},
                        Span<const NodeId>(scaled, 2));
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
        // An unwired port is a zero of its type. A lowering that failed on one could not lower a
        // graph under construction, which is every graph an author is editing.
        const bool colour = port == 0 && is_leaf_closure(node.op) && node.op != GraphOp::Coat;
        auto fallback = constant_of(builder, colour ? Vec3 : Float, Immediate{});
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
    return lower_weight(state, node, ports, made.value());
}

[[nodiscard]] Immediate vec3(f32 x, f32 y, f32 z) noexcept {
    return Immediate{x, y, z, 0.0F, 0};
}

/// Declare the parameters and textures, in one order.
[[nodiscard]] bool declare_common(MaterialGraph& graph) noexcept {
    const Decl declarations[] = {
        {Name::intern("base_color"), Vec3, vec3(0.82F, 0.78F, 0.74F), ParameterDecl, 0},
        {Name::intern("metallic"), Float, Immediate::scalar(1.0F), ParameterDecl, 0},
        {Name::intern("roughness"), Float, Immediate::scalar(0.35F), ParameterDecl, 0},
        {Name::intern("emissive"), Vec3, vec3(1.0F, 0.4F, 0.1F), ParameterDecl, 0},
        {Name::intern("base_color_map"), Vec4, Immediate{0.5F, 0.5F, 0.5F, 1.0F, 0}, TextureDecl,
         0},
        {Name::intern("grime_map"), Vec4, Immediate{0.35F, 0.35F, 0.35F, 1.0F, 0}, TextureDecl, 0},
    };
    for (const Decl& decl : declarations) {
        if (!graph.declare(decl)) {
            return false;
        }
    }
    return true;
}

}  // namespace

MaterialGraph::MaterialGraph(Allocator& allocator, Name material_name) noexcept
    : name_(material_name), nodes_(allocator), links_(allocator), decls_(allocator) {}

Status MaterialGraph::declare(const Decl& decl) noexcept {
    return decls_.push_back(decl);
}

Expected<u32, Error> MaterialGraph::add(GraphOp op, Name symbol, TypeId type,
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

Status MaterialGraph::annotate(u32 node, u32 flags) noexcept {
    if (node >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "no such node", 0});
    }
    nodes_[node].flags |= flags;
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
    Builder builder(allocator, material_domain(), graph.name());
    builder.set_policy(builder_policy(switches));
    for (const Decl& decl : graph.decls()) {
        if (Status declared = builder.declare(decl); !declared) {
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
        if (Status set = builder.set_root(kSurfaceRoot, mapped[graph.surface_output()]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (graph.opacity_output() != kInvalidNode) {
        if (Status set = builder.set_root(kOpacityRoot, mapped[graph.opacity_output()]); !set) {
            return make_unexpected(set.error());
        }
    }
    return builder.finish();
}

bool build_reference_graph(MaterialGraph& graph, ReferenceIds& ids) noexcept {
    if (!declare_common(graph)) {
        return false;
    }
    const u8 components[] = {0, 1, 2};
    Immediate swizzle_xyz;
    swizzle_xyz.mask = swizzle_mask(Span<const u8>(components, 3));
    const u8 first_component[] = {0};
    Immediate swizzle_x;
    swizzle_x.mask = swizzle_mask(Span<const u8>(first_component, 1));

    bool good = true;
    const auto add = [&graph, &good](GraphOp op, Name symbol = Name{}, TypeId type = Float,
                                     const Immediate& value = Immediate{}) noexcept {
        auto added = graph.add(op, symbol, type, value);
        good = good && added.has_value();
        return added ? added.value() : 0U;
    };
    const auto wire = [&graph, &good](u32 from, u32 to, u8 port) noexcept {
        good = good && graph.connect(from, to, port).has_value();
    };

    ids.uv = add(GraphOp::Attribute, Name::intern("uv0"), Vec2);
    ids.albedo_sample = add(GraphOp::TextureSample, Name::intern("base_color_map"), Vec4);
    wire(ids.uv, ids.albedo_sample, 0);
    const u32 albedo_swizzle = add(GraphOp::Swizzle, Name{}, kInvalidType, swizzle_xyz);
    wire(ids.albedo_sample, albedo_swizzle, 0);
    const u32 base_color = add(GraphOp::Parameter, Name::intern("base_color"), Vec3);
    // The wire order an editor produces: the parameter landed on port 0 and the sample on port 1,
    // where a hand-written definition writes `sample * base_color`. Canonical commutative ordering
    // is what makes these one value.
    const u32 albedo = add(GraphOp::Multiply);
    wire(base_color, albedo, 0);
    wire(albedo_swizzle, albedo, 1);

    const u32 metallic = add(GraphOp::Parameter, Name::intern("metallic"), Float);
    const u32 one_minus = add(GraphOp::OneMinus);
    wire(metallic, one_minus, 0);

    ids.grime_sample = add(GraphOp::TextureSample, Name::intern("grime_map"), Vec4);
    wire(ids.uv, ids.grime_sample, 0);
    good = good && graph.annotate(ids.grime_sample, kFlagMicrodetail).has_value();
    const u32 grime = add(GraphOp::Swizzle, Name{}, kInvalidType, swizzle_x);
    wire(ids.grime_sample, grime, 0);

    const u32 worn = add(GraphOp::Multiply);
    wire(albedo, worn, 0);
    wire(one_minus, worn, 1);
    const u32 worn_scaled = add(GraphOp::Multiply);
    wire(worn, worn_scaled, 0);
    wire(grime, worn_scaled, 1);

    // A tint node the author dragged in and left at one. An editor produces these constantly; a
    // person writing the material does not write `* 1.0`. Constant folding is what removes it.
    ids.tint_constant = add(GraphOp::Constant, Name{}, Float, Immediate::scalar(1.0F));
    const u32 tinted = add(GraphOp::Multiply);
    wire(worn_scaled, tinted, 0);
    wire(ids.tint_constant, tinted, 1);

    const u32 diffuse = add(GraphOp::Diffuse);
    wire(tinted, diffuse, 0);

    // The same texture, dragged in a second time by the part of the graph that feeds specular.
    const u32 second_sample = add(GraphOp::TextureSample, Name::intern("base_color_map"), Vec4);
    wire(ids.uv, second_sample, 0);
    const u32 second_swizzle = add(GraphOp::Swizzle, Name{}, kInvalidType, swizzle_xyz);
    wire(second_sample, second_swizzle, 0);
    const u32 second_albedo = add(GraphOp::Multiply);
    wire(base_color, second_albedo, 0);
    wire(second_swizzle, second_albedo, 1);

    const u32 roughness = add(GraphOp::Parameter, Name::intern("roughness"), Float);
    const u32 specular = add(GraphOp::Specular);
    wire(second_albedo, specular, 0);
    wire(roughness, specular, 1);

    const u32 sum = add(GraphOp::AddClosures);
    wire(diffuse, sum, 0);
    wire(specular, sum, 1);

    const u32 emissive = add(GraphOp::Parameter, Name::intern("emissive"), Vec3);
    ids.emission = add(GraphOp::Emission);
    wire(emissive, ids.emission, 0);
    good = good && graph.mute(ids.emission, true).has_value();

    const u32 outer_sum = add(GraphOp::AddClosures);
    wire(sum, outer_sum, 0);
    wire(ids.emission, outer_sum, 1);

    const u32 opacity = add(GraphOp::Constant, Name{}, Float, Immediate::scalar(1.0F));

    // Left over from an experiment, wired to nothing.
    ids.orphan_constant = add(GraphOp::Constant, Name{}, Float, Immediate::scalar(2.0F));
    ids.orphan = add(GraphOp::Multiply);
    wire(roughness, ids.orphan, 0);
    wire(ids.orphan_constant, ids.orphan, 1);

    good = good && graph.set_surface_output(outer_sum).has_value();
    good = good && graph.set_opacity_output(opacity).has_value();
    return good;
}

}  // namespace cy::graph::anchor
