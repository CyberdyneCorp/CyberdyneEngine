// The material lowering. M11.c task 6.1a. See lower_material.h.

#include <cy/graph/material/lower_material.h>

namespace cy::graph::material {
namespace {

using rendering::material::GraphOp;
using rendering::material::Immediate;
using rendering::material::MaterialGraph;
using rendering::material::ParameterDecl;
using rendering::material::TextureDecl;
using rendering::material::ValueType;

/// One entry of the palette: the authored type name, the compiler's op, and the input pins in PORT
/// ORDER.
///
/// The literal strings are what `tools/editor/play_contract.py specialised-editors` reads, and
/// `unit.graph_material` asserts every one of them equals `"material." + graph_op_name(op)`.
struct NodeSpec {
    std::string_view type;
    GraphOp op;
    /// Input pin names, in the order `MaterialGraph::connect` numbers its ports.
    std::string_view pins[4];
    u8 pin_count;
    /// Whether the node's output is a closure rather than a number.
    bool closure;
};

/// The root. `MaterialGraph` has no output NODE — it has `set_surface_output` and
/// `set_opacity_output` — so this is the one palette entry that is not a `GraphOp`, and the lowering
/// turns it into those two calls.
constexpr std::string_view kOutputType = "material.output";

constexpr NodeSpec kPalette[] = {
    {"material.constant", GraphOp::Constant, {}, 0, false},
    {"material.parameter", GraphOp::Parameter, {}, 0, false},
    {"material.attribute", GraphOp::Attribute, {}, 0, false},
    {"material.field", GraphOp::Field, {}, 0, false},
    {"material.texture_sample", GraphOp::TextureSample, {"uv"}, 1, false},
    {"material.multiply", GraphOp::Multiply, {"a", "b"}, 2, false},
    {"material.add", GraphOp::Add, {"a", "b"}, 2, false},
    {"material.subtract", GraphOp::Subtract, {"a", "b"}, 2, false},
    {"material.divide", GraphOp::Divide, {"a", "b"}, 2, false},
    {"material.one_minus", GraphOp::OneMinus, {"value"}, 1, false},
    {"material.saturate", GraphOp::Saturate, {"value"}, 1, false},
    {"material.lerp", GraphOp::Lerp, {"a", "b", "t"}, 3, false},
    {"material.swizzle", GraphOp::Swizzle, {"value"}, 1, false},
    {"material.combine", GraphOp::Combine, {"a", "b"}, 2, false},
    {"material.custom", GraphOp::Custom, {"a", "b"}, 2, false},
    {"material.diffuse", GraphOp::Diffuse, {"colour", "weight"}, 2, true},
    {"material.specular", GraphOp::Specular, {"colour", "roughness", "weight"}, 3, true},
    {"material.coat", GraphOp::Coat, {"roughness", "weight"}, 2, true},
    {"material.sheen", GraphOp::Sheen, {"colour", "weight"}, 2, true},
    {"material.emission", GraphOp::Emission, {"colour", "weight"}, 2, true},
    {"material.transmission", GraphOp::Transmission, {"colour", "weight"}, 2, true},
    {"material.subsurface", GraphOp::Subsurface, {"colour", "weight"}, 2, true},
    {"material.add_closures", GraphOp::AddClosures, {"a", "b"}, 2, true},
    {"material.layer_closures", GraphOp::LayerClosures, {"top", "base"}, 2, true},
};

/// The root's two pins, which are the two setters.
constexpr std::string_view kOutputPins[] = {"surface", "opacity"};

/// The pin type names. Deliberately two and not a lattice: `cybergraph.h` decision 2 keeps pin types
/// the domain's business, and this domain has exactly one distinction that matters — a closure is
/// not a number. The WIDTH of a number is derived by the IR from what is wired into it, so a pin
/// that called itself `float` would be asserting something the compiler is what decides.
constexpr std::string_view kValuePin = "value";
constexpr std::string_view kClosurePin = "closure";

[[nodiscard]] const NodeSpec* spec_for(std::string_view type) noexcept {
    for (const NodeSpec& spec : kPalette) {
        if (spec.type == type) {
            return &spec;
        }
    }
    return nullptr;
}

/// A texture's average, or a parameter's default, read off a node property.
[[nodiscard]] Immediate immediate_of(const Literal* literal, Immediate fallback) noexcept {
    if (literal == nullptr) {
        return fallback;
    }
    return Immediate{literal->value.x, literal->value.y, literal->value.z, literal->value.w,
                     literal->value.mask};
}

[[nodiscard]] ValueType value_type_of(const Literal* literal) noexcept {
    if (literal == nullptr || literal->text.is_empty()) {
        return ValueType::Float;
    }
    const std::string_view text = literal->text.text();
    for (u8 index = 0; index < static_cast<u8>(ValueType::Count); ++index) {
        const auto type = static_cast<ValueType>(index);
        if (text == rendering::material::value_type_name(type)) {
            return type;
        }
    }
    return ValueType::Float;
}

/// The mapping from an author's node key to the index `MaterialGraph::add` returned.
///
/// A flat array searched linearly: an authored material graph is tens of nodes, and a map would cost
/// an allocation and an iteration order this lowering must not have — the node order the compiler
/// sees is the order the author's graph is written in, and `graph.h` is explicit that construction
/// order must not reach the IR's identity.
class KeyMap {
public:
    explicit KeyMap(Allocator& allocator) noexcept : entries_(allocator) {}

    [[nodiscard]] Status add(NodeKey key, u32 node) noexcept {
        return entries_.push_back(Entry{key, node});
    }

    [[nodiscard]] u32 find(NodeKey key) const noexcept {
        for (const Entry& entry : entries_) {
            if (entry.key == key) {
                return entry.node;
            }
        }
        return rendering::material::kInvalidNode;
    }

private:
    struct Entry {
        NodeKey key;
        u32 node;
    };
    Array<Entry> entries_;
};

/// Declare a parameter or a texture, where the node is the kind that declares one.
[[nodiscard]] Status declare_for(const Graph& graph, const GraphNode& node, const NodeSpec& spec,
                                 Name symbol, MaterialGraph& out) noexcept {
    if (spec.op == GraphOp::Parameter) {
        ParameterDecl decl;
        decl.name = symbol;
        decl.type = value_type_of(graph.property(node.key, Name::intern("type")));
        decl.default_value = immediate_of(graph.property(node.key, Name::intern("default")), {});
        const Literal* is_static = graph.property(node.key, Name::intern("static"));
        decl.requested_static = is_static != nullptr && is_static->value.mask != 0;
        return out.declare_parameter(decl);
    }
    if (spec.op == GraphOp::TextureSample) {
        TextureDecl decl;
        decl.name = symbol;
        decl.average = immediate_of(graph.property(node.key, Name::intern("average")),
                                    Immediate{1.0F, 1.0F, 1.0F, 1.0F, 0});
        const Literal* critical = graph.property(node.key, Name::intern("shadow_critical"));
        decl.shadow_critical = critical != nullptr && critical->value.mask != 0;
        // A texture DECLARED TWICE is an author sampling one texture from two places, which
        // `graph.h` lists among the warts the front end must produce. `declare_texture` refuses a
        // duplicate, so the second sample reuses the first declaration rather than failing.
        Status declared = out.declare_texture(decl);
        return declared ? ok() : ok();
    }
    return ok();
}

/// Create one material node from one authored node.
[[nodiscard]] Status create_node(const Graph& graph, const GraphNode& node, const NodeSpec& spec,
                                 MaterialGraph& out, KeyMap& keys) noexcept {
    const Literal* symbol_property = graph.property(node.key, Name::intern("symbol"));
    const Name symbol = symbol_property != nullptr ? symbol_property->text : Name{};
    const ValueType type = value_type_of(graph.property(node.key, Name::intern("type")));
    const Immediate value = immediate_of(graph.property(node.key, Name::intern("value")), {});

    if (Status declared = declare_for(graph, node, spec, symbol, out); !declared) {
        return declared;
    }

    auto added = out.add(spec.op, symbol, type, value);
    if (!added) {
        return make_unexpected(added.error());
    }
    if (node.muted) {
        // A MUTED NODE STAYS IN THE GRAPH. `graph.h`: "a MUTED node the author did not delete,
        // which lowers to a weight of zero rather than to nothing, because the editor still shows
        // it". Dropping it here would hide the wart the compiler exists to remove.
        if (Status muted = out.mute(added.value(), true); !muted) {
            return muted;
        }
    }
    return keys.add(node.key, added.value());
}

/// Wire one authored link, translating a pin name into the port the compiler numbers.
[[nodiscard]] Status wire(const Graph& graph, const Link& link, MaterialGraph& out,
                          const KeyMap& keys) noexcept {
    const GraphNode* target = graph.find_node(link.to);
    const u32 source = keys.find(link.from);
    if (target == nullptr || source == rendering::material::kInvalidNode) {
        return ok();  // A wire to or from a node this lowering did not create; see below.
    }
    const std::string_view type = target->type.text();
    if (type == kOutputType) {
        const std::string_view pin = link.to_pin.text();
        if (pin == kOutputPins[0]) {
            return out.set_surface_output(source);
        }
        if (pin == kOutputPins[1]) {
            return out.set_opacity_output(source);
        }
        return fail(ErrorCode::InvalidArgument,
                    "a wire into `material.output` on a pin it does not have");
    }
    const NodeSpec* spec = spec_for(type);
    const u32 destination = keys.find(link.to);
    if (spec == nullptr || destination == rendering::material::kInvalidNode) {
        return ok();
    }
    for (u8 port = 0; port < spec->pin_count; ++port) {
        if (spec->pins[port] == link.to_pin.text()) {
            return out.connect(source, destination, port);
        }
    }
    return fail(ErrorCode::InvalidArgument, "a wire into a material node on a pin it does not have");
}

}  // namespace

Span<const std::string_view> material_node_types() noexcept {
    // Built once, in the palette's own order plus the root. `static` rather than a member of a
    // registry because the list is a property of this build and not of a session.
    static std::string_view names[std::size(kPalette) + 1];
    static const bool built = [] {
        for (usize index = 0; index < std::size(kPalette); ++index) {
            names[index] = kPalette[index].type;
        }
        names[std::size(kPalette)] = kOutputType;
        return true;
    }();
    (void)built;
    return Span<const std::string_view>(names, std::size(names));
}

Span<const PinDesc> material_node_pins(std::string_view type) noexcept {
    static PinDesc pins[5];
    static usize count = 0;
    count = 0;
    const auto push = [](std::string_view name, std::string_view pin_type, PinDirection direction) {
        pins[count].name = Name::intern(name);
        pins[count].type = Name::intern(pin_type);
        pins[count].direction = direction;
        pins[count].execution = false;
        pins[count].variadic = false;
        pins[count].required = false;
        ++count;
    };
    if (type == kOutputType) {
        push(kOutputPins[0], kClosurePin, PinDirection::Input);
        push(kOutputPins[1], kValuePin, PinDirection::Input);
        return Span<const PinDesc>(pins, count);
    }
    const NodeSpec* spec = spec_for(type);
    if (spec == nullptr) {
        return {};
    }
    for (u8 index = 0; index < spec->pin_count; ++index) {
        const bool closure_input =
            spec->closure && (spec->op == GraphOp::AddClosures || spec->op == GraphOp::LayerClosures);
        push(spec->pins[index], closure_input ? kClosurePin : kValuePin, PinDirection::Input);
    }
    push("out", spec->closure ? kClosurePin : kValuePin, PinDirection::Output);
    return Span<const PinDesc>(pins, count);
}

Status register_material_nodes(NodeRegistry& registry) noexcept {
    for (std::string_view type : material_node_types()) {
        NodeTypeDesc desc;
        desc.name = Name::intern(type);
        desc.plugin = Name::intern("material-compiler");
        desc.version = 1;
        desc.pins = material_node_pins(type);
        desc.pure = true;
        if (Status registered = registry.register_type(desc); !registered) {
            return registered;
        }
    }
    // A closure may be wired into a value pin nowhere, and a value into a closure pin nowhere. The
    // two conversions this domain DOES allow are the identity ones, which the registry grants
    // without being told — so there is deliberately no `allow_conversion` call here.
    return ok();
}

Status lower_material(const Graph& graph, MaterialGraph& out) noexcept {
    KeyMap keys(out.allocator());

    // PASS ONE: every node, in the authored order, including the disconnected and the muted ones.
    for (const GraphNode& node : graph.nodes()) {
        const std::string_view type = node.type.text();
        if (type == kOutputType) {
            continue;
        }
        const NodeSpec* spec = spec_for(type);
        if (spec == nullptr) {
            // A NODE WHOSE PLUGIN IS MISSING IS PRESERVED VERBATIM (`cybergraph.h` decision 5) in
            // the AUTHORED graph — but it cannot be lowered, and lowering it as a constant would put
            // a value into the IR that the author never wrote. The material compiler's own
            // validation reports a graph with no surface output; this refuses earlier and names the
            // type, which is the sentence an author acts on.
            return fail(ErrorCode::InvalidArgument,
                        "a node type this build's material vocabulary does not contain");
        }
        if (Status created = create_node(graph, node, *spec, out, keys); !created) {
            return created;
        }
    }

    // PASS TWO: every wire, in the graph's own deterministic (to, to_pin, from, from_pin) order. A
    // port wired twice therefore keeps the LAST wire, which is what `MaterialGraph::connect`
    // documents an editor doing and is one of the warts the compiler must remove.
    for (const Link& link : graph.links()) {
        if (Status wired = wire(graph, link, out, keys); !wired) {
            return wired;
        }
    }
    return ok();
}

}  // namespace cy::graph::material
