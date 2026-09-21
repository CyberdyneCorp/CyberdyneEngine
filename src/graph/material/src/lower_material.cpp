// The material lowering. M11.c task 6.1a. See lower_material.h.

#include <cy/graph/material/lower_material.h>

#include <iterator>

namespace cy::graph::material {
namespace {

// QUALIFIED RATHER THAN IMPORTED, for one specific reason: `cy::graph::Immediate` and
// `cy::rendering::material::Immediate` are two structurally identical types in two namespaces that
// are both visible here, and a `using` that pulled the second into this one would make every
// mention of the name ambiguous — or, worse, silently pick the enclosing namespace's.
using rendering::material::GraphOp;
using rendering::material::MaterialGraph;
using MaterialImmediate = rendering::material::Immediate;
using MaterialValueType = rendering::material::ValueType;

/// One entry of the palette: the authored type name, the compiler's op, and the input pins in PORT
/// ORDER.
///
/// The literal strings are what `tools/editor/play_contract.py specialised-editors` reads, and
/// `unit.graph_material` asserts every one of them equals `"material." + graph_op_name(op)`.
struct NodeSpec {
    NodeTypeId identity;
    std::string_view type;
    GraphOp op;
    /// Input pin names, in the order `MaterialGraph::connect` numbers its ports.
    std::string_view pins[4];
    u8 pin_count;
    /// Whether the node's output is a closure rather than a number.
    bool closure;
};

/// The root. `MaterialGraph` has no output NODE — it has `set_surface_output` and
/// `set_opacity_output` — so this is the one palette entry that is not a `GraphOp`, and the
/// lowering turns it into those two calls.
constexpr std::string_view kOutputType = "material.output";

constexpr NodeSpec kPalette[] = {
    {1, "material.constant", GraphOp::Constant, {}, 0, false},
    {2, "material.parameter", GraphOp::Parameter, {}, 0, false},
    {3, "material.attribute", GraphOp::Attribute, {}, 0, false},
    {4, "material.field", GraphOp::Field, {}, 0, false},
    {5, "material.texture_sample", GraphOp::TextureSample, {"uv"}, 1, false},
    {6, "material.multiply", GraphOp::Multiply, {"a", "b"}, 2, false},
    {7, "material.add", GraphOp::Add, {"a", "b"}, 2, false},
    {8, "material.subtract", GraphOp::Subtract, {"a", "b"}, 2, false},
    {9, "material.divide", GraphOp::Divide, {"a", "b"}, 2, false},
    {10, "material.one_minus", GraphOp::OneMinus, {"value"}, 1, false},
    {11, "material.saturate", GraphOp::Saturate, {"value"}, 1, false},
    {12, "material.lerp", GraphOp::Lerp, {"a", "b", "t"}, 3, false},
    {13, "material.swizzle", GraphOp::Swizzle, {"value"}, 1, false},
    {14, "material.combine", GraphOp::Combine, {"a", "b"}, 2, false},
    {15, "material.custom", GraphOp::Custom, {"a", "b"}, 2, false},
    {16, "material.diffuse", GraphOp::Diffuse, {"colour", "weight"}, 2, true},
    {17, "material.specular", GraphOp::Specular, {"colour", "roughness", "weight"}, 3, true},
    {18, "material.coat", GraphOp::Coat, {"roughness", "weight"}, 2, true},
    {19, "material.sheen", GraphOp::Sheen, {"colour", "weight"}, 2, true},
    {20, "material.emission", GraphOp::Emission, {"colour", "weight"}, 2, true},
    {21, "material.transmission", GraphOp::Transmission, {"colour", "weight"}, 2, true},
    {22, "material.subsurface", GraphOp::Subsurface, {"colour", "weight"}, 2, true},
    {23, "material.add_closures", GraphOp::AddClosures, {"a", "b"}, 2, true},
    {24, "material.layer_closures", GraphOp::LayerClosures, {"top", "base"}, 2, true},
};

constexpr NodeTypeId kOutputIdentity = 25;

/// The root's two pins, which are the two setters.
constexpr std::string_view kOutputPins[] = {"surface", "opacity"};

/// The pin type names. Deliberately two and not a lattice: `cybergraph.h` decision 2 keeps pin
/// types the domain's business, and this domain has exactly one distinction that matters — a
/// closure is not a number. The WIDTH of a number is derived by the IR from what is wired into it,
/// so a pin that called itself `float` would be asserting something the compiler is what decides.
constexpr std::string_view kValuePin = "value";
constexpr std::string_view kClosurePin = "closure";

/// One authored annotation: the boolean property that carries it, and the flag it sets.
struct FlagSpec {
    std::string_view property;
    rendering::material::NodeFlags flag;
};

enum class PropertyKind : u8 { Text, Bool, Scalar, Vector, Enumeration, Asset };

struct PropertySpec {
    u32 identity;
    std::string_view name;
    PropertyKind kind;
    std::string_view fallback;
    std::string_view constraint;
    std::string_view tooltip;
};

constexpr PropertySpec kConstantProperties[] = {
    {1, "type", PropertyKind::Enumeration, "float", "float|vec2|vec3|vec4", "Value type"},
    {2, "value", PropertyKind::Vector, "0", "", "Constant value"},
};
constexpr PropertySpec kParameterProperties[] = {
    {1, "symbol", PropertyKind::Text, "parameter", "identifier", "Shader parameter name"},
    {2, "type", PropertyKind::Enumeration, "float", "float|vec2|vec3|vec4", "Value type"},
    {3, "default", PropertyKind::Vector, "0", "", "Default runtime value"},
    {4, "static", PropertyKind::Bool, "false", "", "Request static specialisation"},
};
constexpr PropertySpec kNamedProperties[] = {
    {1, "symbol", PropertyKind::Text, "", "identifier", "Engine attribute or field name"},
    {2, "type", PropertyKind::Enumeration, "float", "float|vec2|vec3|vec4", "Value type"},
};
constexpr PropertySpec kTextureProperties[] = {
    {1, "symbol", PropertyKind::Text, "texture", "identifier", "Bindless texture slot name"},
    {2, "texture", PropertyKind::Asset, "", "texture", "Project texture asset"},
    {3, "type", PropertyKind::Enumeration, "vec4", "float|vec2|vec3|vec4", "Sample value type"},
    {4, "average", PropertyKind::Vector, "1,1,1,1", "", "Fallback and analysis average"},
    {5, "shadow_critical", PropertyKind::Bool, "false", "", "Retain in shadow derivations"},
    {6, "microdetail", PropertyKind::Bool, "false", "", "Mark as microdetail"},
};
constexpr PropertySpec kTypedProperties[] = {
    {1, "type", PropertyKind::Enumeration, "float", "float|vec2|vec3|vec4", "Result value type"},
};

[[nodiscard]] Span<const PropertySpec> properties_for(std::string_view type) noexcept {
    if (type == "material.constant") {
        return {kConstantProperties, std::size(kConstantProperties)};
    }
    if (type == "material.parameter") {
        return {kParameterProperties, std::size(kParameterProperties)};
    }
    if (type == "material.attribute" || type == "material.field" || type == "material.custom") {
        return {kNamedProperties, std::size(kNamedProperties)};
    }
    if (type == "material.texture_sample") {
        return {kTextureProperties, std::size(kTextureProperties)};
    }
    if (type == "material.swizzle" || type == "material.combine") {
        return {kTypedProperties, std::size(kTypedProperties)};
    }
    return {};
}

constexpr FlagSpec kFlags[] = {
    {"base_reflectance", rendering::material::NodeFlags::BaseReflectance},
    {"opacity_critical", rendering::material::NodeFlags::OpacityCritical},
    {"microdetail", rendering::material::NodeFlags::Microdetail},
};

[[nodiscard]] const NodeSpec* spec_for(std::string_view type) noexcept {
    for (const NodeSpec& spec : kPalette) {
        if (spec.type == type) {
            return &spec;
        }
    }
    return nullptr;
}

/// A texture's average, or a parameter's default, read off a node property.
[[nodiscard]] MaterialImmediate immediate_of(const Literal* literal,
                                             MaterialImmediate fallback) noexcept {
    if (literal == nullptr) {
        return fallback;
    }
    return MaterialImmediate{literal->value.x, literal->value.y, literal->value.z, literal->value.w,
                             literal->value.mask};
}

[[nodiscard]] MaterialValueType value_type_of(const Literal* literal) noexcept {
    if (literal == nullptr || literal->text.is_empty()) {
        return MaterialValueType::Float;
    }
    const std::string_view text = literal->text.text();
    for (u8 index = 0; index < static_cast<u8>(MaterialValueType::Count); ++index) {
        const auto type = static_cast<MaterialValueType>(index);
        if (text == rendering::material::value_type_name(type)) {
            return type;
        }
    }
    return MaterialValueType::Float;
}

/// The mapping from an author's node key to the index `MaterialGraph::add` returned.
///
/// A flat array searched linearly: an authored material graph is tens of nodes, and a map would
/// cost an allocation and an iteration order this lowering must not have — the node order the
/// compiler sees is the order the author's graph is written in, and `graph.h` is explicit that
/// construction order must not reach the IR's identity.
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
        // The same argument as for a texture below: an author reads one parameter from three parts
        // of a graph, and that is three NODES and one declaration.
        for (const rendering::material::ParameterDecl& declared : out.parameters()) {
            if (declared.name == symbol) {
                return ok();
            }
        }
        rendering::material::ParameterDecl decl;
        decl.name = symbol;
        decl.type = value_type_of(graph.property(node.key, Name::intern("type")));
        decl.default_value = immediate_of(graph.property(node.key, Name::intern("default")), {});
        const Literal* is_static = graph.property(node.key, Name::intern("static"));
        decl.requested_static = is_static != nullptr && is_static->value.mask != 0;
        return out.declare_parameter(decl);
    }
    if (spec.op == GraphOp::TextureSample) {
        rendering::material::TextureDecl decl;
        decl.name = symbol;
        decl.average = immediate_of(graph.property(node.key, Name::intern("average")),
                                    MaterialImmediate{1.0F, 1.0F, 1.0F, 1.0F, 0});
        const Literal* critical = graph.property(node.key, Name::intern("shadow_critical"));
        decl.shadow_critical = critical != nullptr && critical->value.mask != 0;
        // A TEXTURE SAMPLED TWICE IS ONE TEXTURE. `graph.h` lists "one texture sampled from two
        // separate nodes, because two parts of a graph each dragged the texture in" among the warts
        // an editor produces, and it is a wart in the NODES rather than in the DECLARATIONS: the
        // declaration list is what the material's parameter block and its bindless slots are built
        // from, so declaring `base_color_map` twice would give the material two slots for one
        // texture. `MaterialGraph::declare_texture` appends unconditionally, so the check is here.
        for (const rendering::material::TextureDecl& declared : out.textures()) {
            if (declared.name == symbol) {
                return ok();
            }
        }
        return out.declare_texture(decl);
    }
    return ok();
}

/// Create one material node from one authored node.
[[nodiscard]] Status create_node(const Graph& graph, const GraphNode& node, const NodeSpec& spec,
                                 MaterialGraph& out, KeyMap& keys) noexcept {
    const Literal* symbol_property = graph.property(node.key, Name::intern("symbol"));
    const Name symbol = symbol_property != nullptr ? symbol_property->text : Name{};
    const MaterialValueType type = value_type_of(graph.property(node.key, Name::intern("type")));
    const MaterialImmediate value =
        immediate_of(graph.property(node.key, Name::intern("value")), {});

    if (Status declared = declare_for(graph, node, spec, symbol, out); !declared) {
        return declared;
    }

    auto added = out.add(spec.op, symbol, type, value);
    if (!added) {
        return make_unexpected(added.error());
    }
    // THE AUTHOR'S OWN ANNOTATIONS. `ir.h` calls these "what an author said about a node" and
    // records that none of them reaches a content hash — so they change which program a derivation
    // keeps and never what the primary program means. A graph that could not carry them would make
    // `material.output`'s far-field and shadow derivations unauthorable from the editor.
    rendering::material::NodeFlags flags = rendering::material::NodeFlags::None;
    for (const FlagSpec& flag : kFlags) {
        const Literal* property = graph.property(node.key, Name::intern(flag.property));
        if (property != nullptr && property->value.mask != 0) {
            flags = flags | flag.flag;
        }
    }
    if (flags != rendering::material::NodeFlags::None) {
        if (Status annotated = out.annotate(added.value(), flags); !annotated) {
            return annotated;
        }
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
    return fail(ErrorCode::InvalidArgument,
                "a wire into a material node on a pin it does not have");
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
    return {names, std::size(names)};
}

NodeTypeId material_node_type_id(std::string_view type) noexcept {
    if (type == kOutputType) {
        return kOutputIdentity;
    }
    const NodeSpec* spec = spec_for(type);
    return spec != nullptr ? spec->identity : kInvalidNodeTypeId;
}

Status encode_material_catalogue(Array<u8>& out) noexcept {
    const auto u8_value = [&](u8 value) { return out.push_back(value); };
    const auto u32_value = [&](u32 value) -> Status {
        for (usize byte = 0; byte < 4; ++byte) {
            if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return ok();
    };
    const auto text = [&](std::string_view value) -> Status {
        if (Status length = u32_value(static_cast<u32>(value.size())); !length) {
            return length;
        }
        return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
    };

    out.clear();
    if (Status status = u32_value(1); !status) {
        return status;  // schema
    }
    if (Status status = u32_value(2); !status) {
        return status;  // catalogue version
    }
    const auto types = material_node_types();
    if (Status status = u32_value(static_cast<u32>(types.size())); !status) {
        return status;
    }
    for (const std::string_view type : types) {
        if (Status status = u32_value(material_node_type_id(type)); !status) {
            return status;
        }
        if (Status status = u32_value(1); !status) {
            return status;  // node schema version
        }
        if (Status status = text(type); !status) {
            return status;
        }
        PinDesc storage[kMaxPins];
        const auto pins = material_node_pins(type, storage);
        if (Status status = u32_value(static_cast<u32>(pins.size())); !status) {
            return status;
        }
        for (const PinDesc& pin : pins) {
            if (Status status = u32_value(pin.identity); !status) {
                return status;
            }
            if (Status status = u8_value(static_cast<u8>(pin.direction)); !status) {
                return status;
            }
            if (Status status = text(pin.name.text()); !status) {
                return status;
            }
            if (Status status = text(pin.type.text()); !status) {
                return status;
            }
        }
        const Span<const PropertySpec> properties = properties_for(type);
        if (Status status = u32_value(static_cast<u32>(properties.size())); !status) {
            return status;
        }
        for (const PropertySpec& property : properties) {
            if (Status status = u32_value(property.identity); !status) {
                return status;
            }
            if (Status status = u8_value(static_cast<u8>(property.kind)); !status) {
                return status;
            }
            if (Status status = text(property.name); !status) {
                return status;
            }
            if (Status status = text(property.fallback); !status) {
                return status;
            }
            if (Status status = text(property.constraint); !status) {
                return status;
            }
            if (Status status = text(property.tooltip); !status) {
                return status;
            }
        }
    }
    return ok();
}

PinId material_node_pin_id(std::string_view type, std::string_view pin,
                           PinDirection direction) noexcept {
    PinDesc storage[kMaxPins];
    const Span<const PinDesc> pins = material_node_pins(type, storage);
    for (usize index = 0; index < pins.size(); ++index) {
        if (pins[index].name.text() == pin && pins[index].direction == direction) {
            return static_cast<PinId>(index + 1);
        }
    }
    return kInvalidPinId;
}

Span<const PinDesc> material_node_pins(std::string_view type, PinDesc storage[kMaxPins]) noexcept {
    usize count = 0;
    PinDesc* pins = storage;
    const auto push = [&](std::string_view name, std::string_view pin_type,
                          PinDirection direction) {
        pins[count].name = Name::intern(name);
        pins[count].identity = static_cast<PinId>(count + 1);
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
        return {storage, count};
    }
    const NodeSpec* spec = spec_for(type);
    if (spec == nullptr) {
        return {};
    }
    for (u8 index = 0; index < spec->pin_count; ++index) {
        const bool closure_input = spec->closure && (spec->op == GraphOp::AddClosures ||
                                                     spec->op == GraphOp::LayerClosures);
        push(spec->pins[index], closure_input ? kClosurePin : kValuePin, PinDirection::Input);
    }
    push("out", spec->closure ? kClosurePin : kValuePin, PinDirection::Output);
    return {storage, count};
}

Status register_material_nodes(NodeRegistry& registry) noexcept {
    for (std::string_view type : material_node_types()) {
        PinDesc storage[kMaxPins];
        NodeTypeDesc desc;
        desc.identity = material_node_type_id(type);
        desc.name = Name::intern(type);
        desc.plugin = Name::intern("material-compiler");
        desc.version = 1;
        desc.pins = material_node_pins(type, storage);
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
            // the AUTHORED graph — but it cannot be lowered, and lowering it as a constant would
            // put a value into the IR that the author never wrote. The material compiler's own
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
