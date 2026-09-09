// CyberGraph: the registry, the graph, the semantic digest and validation. Task 2.1.
//
// See cybergraph.h for the five decisions this file implements. The two that shape it most are that
// a node's identity is an author-owned key rather than an index, and that layout is a side table
// which the semantic digest does not close over.

#include <cy/graph/cybergraph.h>

#include <algorithm>
#include <utility>

namespace cy::graph {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

/// Order two names by their TEXT, never by `Name::index()` — interning order is not stable across
/// runs, and everything deterministic in this file depends on that distinction.
[[nodiscard]] bool text_before(Name a, Name b) noexcept {
    return a.text() < b.text();
}

/// An insertion sort over an index array. Small by construction — a node's property list and a
/// graph's link list are both short — and stable, which the deterministic text format needs.
template <typename Less>
void sort_indices(Array<u32>& order, Less&& less) noexcept {
    for (usize outer = 1; outer < order.size(); ++outer) {
        for (usize inner = outer; inner > 0 && less(order[inner], order[inner - 1]); --inner) {
            const u32 swap = order[inner - 1];
            order[inner - 1] = order[inner];
            order[inner] = swap;
        }
    }
}

[[nodiscard]] Status fill_indices(Array<u32>& order, usize count) noexcept {
    order.clear();
    for (usize index = 0; index < count; ++index) {
        if (Status pushed = order.push_back(static_cast<u32>(index)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] u64 hash_literal(u64 seed, const Literal& literal) noexcept {
    u64 hash = hash_text(seed, literal.type.text());
    hash = hash_bytes(hash, &literal.value, sizeof(literal.value));
    return hash_text(hash, literal.text.text());
}

}  // namespace

bool operator==(const Literal& a, const Literal& b) noexcept {
    return a.type == b.type && a.value == b.value && a.text == b.text;
}

bool operator==(const Link& a, const Link& b) noexcept {
    return a.from == b.from && a.from_pin == b.from_pin && a.to == b.to && a.to_pin == b.to_pin;
}

const char* capability_name(Capability single) noexcept {
    switch (single) {
        case Capability::None:
            return "none";
        case Capability::ReadWorld:
            return "read_world";
        case Capability::WriteWorld:
            return "write_world";
        case Capability::SpawnEntity:
            return "spawn_entity";
        case Capability::DestroyEntity:
            return "destroy_entity";
        case Capability::Physics:
            return "physics";
        case Capability::Audio:
            return "audio";
        case Capability::Network:
            return "network";
        case Capability::FileSystem:
            return "file_system";
        case Capability::Randomness:
            return "randomness";
        case Capability::WallClock:
            return "wall_clock";
        case Capability::NativeCall:
            return "native_call";
    }
    return "?";
}

// --- NodeType and the registry ------------------------------------------------------------------

NodeType::NodeType(Allocator& allocator, const NodeTypeDesc& desc) noexcept
    : name_(desc.name),
      plugin_(desc.plugin),
      version_(desc.version),
      pins_(allocator),
      required_(desc.requires_capabilities),
      determinism_(desc.determinism),
      pure_(desc.pure) {
    (void)pins_.append(desc.pins);
}

const PinDesc* NodeType::find_pin(Name pin, PinDirection direction) const noexcept {
    for (const PinDesc& candidate : pins_) {
        if (candidate.name == pin && candidate.direction == direction) {
            return &candidate;
        }
    }
    return nullptr;
}

NodeRegistry::NodeRegistry(Allocator& allocator) noexcept
    : types_(allocator), conversions_(allocator) {}

Status NodeRegistry::register_type(const NodeTypeDesc& desc) noexcept {
    if (find(desc.name) != nullptr) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "a node type of this name is already registered", 0});
    }
    NodeType type(types_.allocator(), desc);
    if (type.pins().size() != desc.pins.size()) {
        return make_unexpected(
            Error{ErrorCode::OutOfMemory, "the node type's pin table could not be grown", 0});
    }
    return types_.push_back(std::move(type));
}

const NodeType* NodeRegistry::find(Name type) const noexcept {
    for (const NodeType& candidate : types_) {
        if (candidate.name() == type) {
            return &candidate;
        }
    }
    return nullptr;
}

Status NodeRegistry::allow_conversion(Name from, Name to) noexcept {
    return conversions_.push_back(Conversion{from, to});
}

bool NodeRegistry::converts(Name from, Name to) const noexcept {
    if (from == to) {
        return true;
    }
    return std::ranges::any_of(conversions_, [from, to](const Conversion& conversion) noexcept {
        return conversion.from == from && conversion.to == to;
    });
}

// --- Diagnostics ----------------------------------------------------------------------------

void DiagnosticSink::report(const Diagnostic& diagnostic) noexcept {
    if (diagnostic.severity == Severity::Error) {
        ++errors_;
    } else if (diagnostic.severity == Severity::Warning) {
        ++warnings_;
    }
    if (entries_.size() >= cap_) {
        overflowed_ = true;
        return;
    }
    if (!entries_.push_back(diagnostic)) {
        overflowed_ = true;
    }
}

void DiagnosticSink::clear() noexcept {
    entries_.clear();
    errors_ = 0;
    warnings_ = 0;
    overflowed_ = false;
}

Status DebugMap::record(u32 location, NodeKey node, Name pin) noexcept {
    return sites_.push_back(Site{location, node, pin});
}

const DebugMap::Site* DebugMap::find(u32 location) const noexcept {
    for (const Site& site : sites_) {
        if (site.location == location) {
            return &site;
        }
    }
    return nullptr;
}

// --- Graph ------------------------------------------------------------------------------------

Graph::Graph(Allocator& allocator, Name graph_name) noexcept
    : name_(graph_name),
      nodes_(allocator),
      links_(allocator),
      layout_(allocator),
      interface_(allocator),
      properties_(allocator),
      opaque_(allocator) {}

Status Graph::add_node(NodeKey key, Name type, u32 version) noexcept {
    if (key == kInvalidNodeKey) {
        return make_unexpected(invalid("a node key of zero is 'no node'"));
    }
    if (find_node(key) != nullptr) {
        return make_unexpected(Error{ErrorCode::AlreadyExists,
                                     "two nodes cannot share one key: identity is what every diff, "
                                     "merge and diagnostic is keyed by",
                                     0});
    }
    GraphNode node;
    node.key = key;
    node.type = type;
    node.version = version;
    next_key_ = key >= next_key_ ? key + 1 : next_key_;
    return nodes_.push_back(node);
}

NodeKey Graph::allocate_key() noexcept {
    return next_key_++;
}

Status Graph::remove_node(NodeKey key) noexcept {
    for (usize index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].key != key) {
            continue;
        }
        nodes_.erase(index);
        for (usize link = links_.size(); link > 0; --link) {
            if (links_[link - 1].from == key || links_[link - 1].to == key) {
                links_.erase(link - 1);
            }
        }
        for (usize entry = layout_.size(); entry > 0; --entry) {
            if (layout_[entry - 1].key == key) {
                layout_.erase(entry - 1);
            }
        }
        for (usize entry = properties_.size(); entry > 0; --entry) {
            if (properties_[entry - 1].key == key) {
                properties_.erase(entry - 1);
            }
        }
        for (usize entry = opaque_.size(); entry > 0; --entry) {
            if (opaque_[entry - 1].key == key) {
                opaque_.erase(entry - 1);
            }
        }
        return ok();
    }
    return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
}

const GraphNode* Graph::find_node(NodeKey key) const noexcept {
    for (const GraphNode& node : nodes_) {
        if (node.key == key) {
            return &node;
        }
    }
    return nullptr;
}

GraphNode* Graph::find_node(NodeKey key) noexcept {
    for (GraphNode& node : nodes_) {
        if (node.key == key) {
            return &node;
        }
    }
    return nullptr;
}

Status Graph::mute(NodeKey key, bool muted) noexcept {
    GraphNode* node = find_node(key);
    if (node == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    node->muted = muted;
    return ok();
}

Graph::NodeProperties* Graph::properties_for(NodeKey key) noexcept {
    for (NodeProperties& entry : properties_) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

const Graph::NodeProperties* Graph::properties_for(NodeKey key) const noexcept {
    for (const NodeProperties& entry : properties_) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

Status Graph::set_property(NodeKey key, Name property, const Literal& value) noexcept {
    if (find_node(key) == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    NodeProperties* entry = properties_for(key);
    if (entry == nullptr) {
        auto created = properties_.emplace_back(NodeProperties{key, Array<Property>(allocator())});
        if (!created) {
            return make_unexpected(created.error());
        }
        entry = created.value();
    }
    for (Property& existing : entry->entries) {
        if (existing.name == property) {
            existing.value = value;
            return ok();
        }
    }
    return entry->entries.push_back(Property{property, value});
}

const Literal* Graph::property(NodeKey key, Name property) const noexcept {
    const NodeProperties* entry = properties_for(key);
    if (entry == nullptr) {
        return nullptr;
    }
    for (const Property& existing : entry->entries) {
        if (existing.name == property) {
            return &existing.value;
        }
    }
    return nullptr;
}

Span<const Property> Graph::properties(NodeKey key) const noexcept {
    const NodeProperties* entry = properties_for(key);
    return entry == nullptr ? Span<const Property>{} : entry->entries.span();
}

Status Graph::connect(NodeKey from, Name from_pin, NodeKey to, Name to_pin) noexcept {
    const GraphNode* target = find_node(to);
    if (find_node(from) == nullptr || target == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "a wire names a node that is not here", 0});
    }
    // A non-variadic input keeps the LAST wire, which is what an editor does. Whether the pin is
    // variadic is the registry's business, so the graph asks the resolved type when it has one and
    // keeps every wire when it does not — an opaque node's pin arity is not knowable.
    const PinDesc* pin = target->resolved != nullptr
                             ? target->resolved->find_pin(to_pin, PinDirection::Input)
                             : nullptr;
    const bool variadic = pin != nullptr && pin->variadic;
    if (!variadic && pin != nullptr) {
        for (usize index = links_.size(); index > 0; --index) {
            if (links_[index - 1].to == to && links_[index - 1].to_pin == to_pin) {
                links_.erase(index - 1);
            }
        }
    }
    return links_.push_back(Link{from, from_pin, to, to_pin});
}

Status Graph::disconnect(NodeKey to, Name to_pin, NodeKey from, Name from_pin) noexcept {
    bool removed = false;
    for (usize index = links_.size(); index > 0; --index) {
        const Link& link = links_[index - 1];
        const bool matches_source =
            from == kInvalidNodeKey || (link.from == from && link.from_pin == from_pin);
        if (link.to == to && link.to_pin == to_pin && matches_source) {
            links_.erase(index - 1);
            removed = true;
        }
    }
    return removed ? ok() : Status(make_unexpected(Error{ErrorCode::NotFound, "no such wire", 0}));
}

Status Graph::inputs_of(NodeKey node, Name pin, Array<Link>& out) const noexcept {
    for (const Link& link : links_) {
        if (link.to == node && link.to_pin == pin) {
            if (Status pushed = out.push_back(link); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status Graph::set_layout(const NodeLayout& layout) noexcept {
    if (find_node(layout.key) == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    for (NodeLayout& existing : layout_) {
        if (existing.key == layout.key) {
            existing = layout;
            return ok();
        }
    }
    return layout_.push_back(layout);
}

const NodeLayout* Graph::layout(NodeKey key) const noexcept {
    for (const NodeLayout& existing : layout_) {
        if (existing.key == key) {
            return &existing;
        }
    }
    return nullptr;
}

Status Graph::declare_interface(const PinDesc& pin) noexcept {
    for (const PinDesc& existing : interface_) {
        if (existing.name == pin.name && existing.direction == pin.direction) {
            return make_unexpected(invalid("this interface pin is already declared"));
        }
    }
    return interface_.push_back(pin);
}

Status Graph::set_subgraph(NodeKey key, Name graph_name) noexcept {
    GraphNode* node = find_node(key);
    if (node == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    node->subgraph = graph_name;
    return ok();
}

Status Graph::set_opaque_body(NodeKey key, std::string_view body) noexcept {
    if (find_node(key) == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    for (OpaqueBody& existing : opaque_) {
        if (existing.key == key) {
            existing.text.clear();
            return existing.text.append(Span<const char>(body.data(), body.size()));
        }
    }
    auto created = opaque_.emplace_back(OpaqueBody{key, Array<char>(allocator())});
    if (!created) {
        return make_unexpected(created.error());
    }
    return created.value()->text.append(Span<const char>(body.data(), body.size()));
}

std::string_view Graph::opaque_body(NodeKey key) const noexcept {
    for (const OpaqueBody& existing : opaque_) {
        if (existing.key == key) {
            return {existing.text.data(), existing.text.size()};
        }
    }
    return {};
}

bool Graph::is_opaque(NodeKey key) const noexcept {
    const GraphNode* node = find_node(key);
    return node != nullptr && node->resolved == nullptr && node->subgraph == Name{};
}

u32 Graph::opaque_count() const noexcept {
    u32 count = 0;
    for (const GraphNode& node : nodes_) {
        count += node.resolved == nullptr && node.subgraph == Name{} ? 1U : 0U;
    }
    return count;
}

void Graph::resolve(const NodeRegistry& registry) noexcept {
    for (GraphNode& node : nodes_) {
        node.resolved = registry.find(node.type);
        if (node.resolved != nullptr) {
            node.plugin = node.resolved->plugin();
        }
    }
}

Expected<Graph, Error> Graph::clone(Allocator& allocator) const noexcept {
    Graph copy(allocator, name_);
    copy.version_ = version_;
    copy.granted_ = granted_;
    copy.claims_deterministic_ = claims_deterministic_;
    if (Status appended = copy.nodes_.append(nodes_.span()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status appended = copy.links_.append(links_.span()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status appended = copy.layout_.append(layout_.span()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status appended = copy.interface_.append(interface_.span()); !appended) {
        return make_unexpected(appended.error());
    }
    for (const NodeProperties& entry : properties_) {
        auto created =
            copy.properties_.emplace_back(NodeProperties{entry.key, Array<Property>(allocator)});
        if (!created) {
            return make_unexpected(created.error());
        }
        if (Status appended = created.value()->entries.append(entry.entries.span()); !appended) {
            return make_unexpected(appended.error());
        }
    }
    for (const OpaqueBody& entry : opaque_) {
        auto created = copy.opaque_.emplace_back(OpaqueBody{entry.key, Array<char>(allocator)});
        if (!created) {
            return make_unexpected(created.error());
        }
        if (Status appended = created.value()->text.append(entry.text.span()); !appended) {
            return make_unexpected(appended.error());
        }
    }
    copy.next_key_ = next_key_;
    return copy;
}

u64 Graph::semantic_digest() const noexcept {
    // MEANING AND NOTHING ELSE (decision 4). Not the layout, not the order the author added things
    // in, and not `next_key_` — an editor that allocated and undid a key must not change the
    // digest of what it produced.
    u64 hash = hash_u64(kHashSeed, kGraphVersion);
    hash = hash_u64(hash, version_);
    hash = hash_text(hash, name_.text());
    hash = hash_u64(hash, static_cast<u64>(granted_));
    hash = hash_u64(hash, claims_deterministic_ ? 1ULL : 0ULL);

    for (const PinDesc& pin : interface_) {
        hash = hash_text(hash, pin.name.text());
        hash = hash_text(hash, pin.type.text());
        hash = hash_u64(hash, static_cast<u64>(pin.direction));
        hash = hash_u64(hash, (pin.execution ? 1ULL : 0ULL) | (pin.variadic ? 2ULL : 0ULL) |
                                  (pin.required ? 4ULL : 0ULL));
    }

    Array<u32> order(nodes_.allocator());
    if (!fill_indices(order, nodes_.size())) {
        return 0;
    }
    const Array<GraphNode>& nodes = nodes_;
    sort_indices(order, [&nodes](u32 a, u32 b) noexcept { return nodes[a].key < nodes[b].key; });
    for (const u32 index : order) {
        const GraphNode& node = nodes_[index];
        hash = hash_u64(hash, node.key);
        hash = hash_text(hash, node.type.text());
        hash = hash_u64(hash, node.version);
        hash = hash_u64(hash, node.muted ? 1ULL : 0ULL);
        hash = hash_text(hash, node.subgraph.text());
        hash = hash_text(hash, opaque_body(node.key));

        const Span<const Property> entries = properties(node.key);
        Array<u32> property_order(nodes_.allocator());
        if (!fill_indices(property_order, entries.size())) {
            return 0;
        }
        sort_indices(property_order, [entries](u32 a, u32 b) noexcept {
            return text_before(entries[a].name, entries[b].name);
        });
        hash = hash_u64(hash, static_cast<u64>(entries.size()));
        for (const u32 property_index : property_order) {
            hash = hash_text(hash, entries[property_index].name.text());
            hash = hash_literal(hash, entries[property_index].value);
        }
    }

    Array<u32> link_order(nodes_.allocator());
    if (!fill_indices(link_order, links_.size())) {
        return 0;
    }
    const Array<Link>& links = links_;
    sort_indices(link_order, [&links](u32 a, u32 b) noexcept {
        if (links[a].to != links[b].to) {
            return links[a].to < links[b].to;
        }
        if (links[a].to_pin != links[b].to_pin) {
            return text_before(links[a].to_pin, links[b].to_pin);
        }
        if (links[a].from != links[b].from) {
            return links[a].from < links[b].from;
        }
        return text_before(links[a].from_pin, links[b].from_pin);
    });
    for (const u32 index : link_order) {
        const Link& link = links_[index];
        hash = hash_u64(hash, link.from);
        hash = hash_text(hash, link.from_pin.text());
        hash = hash_u64(hash, link.to);
        hash = hash_text(hash, link.to_pin.text());
    }
    return hash;
}

// --- The library --------------------------------------------------------------------------------

Status GraphLibrary::add(Graph&& graph) noexcept {
    if (find(graph.name()) != nullptr) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "a graph of this name is already in the library", 0});
    }
    return graphs_.push_back(std::move(graph));
}

const Graph* GraphLibrary::find(Name graph_name) const noexcept {
    for (const Graph& graph : graphs_) {
        if (graph.name() == graph_name) {
            return &graph;
        }
    }
    return nullptr;
}

// --- Validation ---------------------------------------------------------------------------------

namespace {

/// The pin table a node presents: a registered type's, or a referenced subgraph's interface with
/// its directions flipped — an input of the subgraph is an input pin on the instance.
struct PinTable {
    Span<const PinDesc> pins;
    bool known = false;
    bool subgraph = false;
};

[[nodiscard]] PinTable pins_of(const GraphNode& node, const GraphLibrary* library) noexcept {
    PinTable table;
    if (node.subgraph != Name{}) {
        table.subgraph = true;
        const Graph* referenced = library == nullptr ? nullptr : library->find(node.subgraph);
        if (referenced != nullptr) {
            table.pins = referenced->interface_pins();
            table.known = true;
        }
        return table;
    }
    if (node.resolved != nullptr) {
        table.pins = node.resolved->pins();
        table.known = true;
    }
    return table;
}

[[nodiscard]] const PinDesc* find_pin(const PinTable& table, Name pin,
                                      PinDirection direction) noexcept {
    for (const PinDesc& candidate : table.pins) {
        if (candidate.name == pin && candidate.direction == direction) {
            return &candidate;
        }
    }
    return nullptr;
}

void report_unknown_nodes(const Graph& graph, const GraphLibrary* library,
                          DiagnosticSink& sink) noexcept {
    for (const GraphNode& node : graph.nodes()) {
        const PinTable table = pins_of(node, library);
        if (table.known) {
            continue;
        }
        Diagnostic diagnostic;
        diagnostic.severity = Severity::Error;
        diagnostic.node = node.key;
        diagnostic.detail = table.subgraph ? node.subgraph : node.type;
        diagnostic.message =
            table.subgraph
                ? "this node instantiates a graph the library does not contain"
                : "this node's type is not registered; the node and its wires are PRESERVED and "
                  "will be written back unchanged, but it cannot be compiled";
        sink.report(diagnostic);
    }
}

/// One wire's four checks: both pins exist, both point the right way, and the types convert.
void validate_link(const Graph& graph, const NodeRegistry& registry, const GraphLibrary* library,
                   const Link& link, DiagnosticSink& sink) noexcept {
    const GraphNode* source = graph.find_node(link.from);
    const GraphNode* target = graph.find_node(link.to);
    if (source == nullptr || target == nullptr) {
        Diagnostic diagnostic;
        diagnostic.node = target != nullptr ? target->key : link.from;
        diagnostic.pin = link.to_pin;
        diagnostic.message = "this wire names a node that is not in the graph";
        sink.report(diagnostic);
        return;
    }
    const PinTable source_pins = pins_of(*source, library);
    const PinTable target_pins = pins_of(*target, library);
    if (!source_pins.known || !target_pins.known) {
        // The node itself was already reported. A wire into an opaque node is PRESERVED rather than
        // reported a second time — decision 5.
        return;
    }
    const PinDesc* out = find_pin(source_pins, link.from_pin, PinDirection::Output);
    const PinDesc* in = find_pin(target_pins, link.to_pin, PinDirection::Input);
    if (out == nullptr) {
        Diagnostic diagnostic;
        diagnostic.node = source->key;
        diagnostic.pin = link.from_pin;
        diagnostic.message = "this node has no output pin of that name";
        sink.report(diagnostic);
        return;
    }
    if (in == nullptr) {
        Diagnostic diagnostic;
        diagnostic.node = target->key;
        diagnostic.pin = link.to_pin;
        diagnostic.message = "this node has no input pin of that name";
        sink.report(diagnostic);
        return;
    }
    if (out->execution != in->execution) {
        Diagnostic diagnostic;
        diagnostic.node = target->key;
        diagnostic.pin = link.to_pin;
        diagnostic.message = "an execution pin and a data pin cannot be wired together";
        sink.report(diagnostic);
        return;
    }
    if (!out->execution && !registry.converts(out->type, in->type)) {
        Diagnostic diagnostic;
        diagnostic.node = target->key;
        diagnostic.pin = link.to_pin;
        diagnostic.detail = out->type;
        diagnostic.message = "this pin's type does not accept the value wired into it";
        sink.report(diagnostic);
    }
}

void validate_required_inputs(const Graph& graph, const GraphLibrary* library,
                              DiagnosticSink& sink) noexcept {
    for (const GraphNode& node : graph.nodes()) {
        const PinTable table = pins_of(node, library);
        if (!table.known || node.muted) {
            continue;
        }
        for (const PinDesc& pin : table.pins) {
            if (pin.direction != PinDirection::Input || !pin.required) {
                continue;
            }
            bool wired = false;
            for (const Link& link : graph.links()) {
                wired = wired || (link.to == node.key && link.to_pin == pin.name);
            }
            if (wired || graph.property(node.key, pin.name) != nullptr) {
                continue;
            }
            Diagnostic diagnostic;
            diagnostic.node = node.key;
            diagnostic.pin = pin.name;
            diagnostic.message = "this input is required and has neither a wire nor a value";
            sink.report(diagnostic);
        }
    }
}

/// A cycle among DATA pins. Control flow may legitimately loop — `visual-scripting`'s IR has basic
/// blocks and back edges — and a data cycle is a value defined in terms of itself, which no
/// consumer can compile.
void validate_acyclic(const Graph& graph, const GraphLibrary* library,
                      DiagnosticSink& sink) noexcept {
    const Span<const GraphNode> nodes = graph.nodes();
    Array<u8> state(graph.allocator());
    Array<u32> stack(graph.allocator());
    if (!state.resize(nodes.size())) {
        return;
    }
    for (u8& mark : state) {
        mark = 0;
    }
    const auto index_of = [nodes](NodeKey key) noexcept {
        for (usize index = 0; index < nodes.size(); ++index) {
            if (nodes[index].key == key) {
                return static_cast<u32>(index);
            }
        }
        return static_cast<u32>(nodes.size());
    };
    const auto is_data_link = [&graph, library](const Link& link) noexcept {
        const GraphNode* target = graph.find_node(link.to);
        if (target == nullptr) {
            return false;
        }
        const PinTable table = pins_of(*target, library);
        const PinDesc* pin = find_pin(table, link.to_pin, PinDirection::Input);
        return pin != nullptr && !pin->execution;
    };

    for (usize root = 0; root < nodes.size(); ++root) {
        if (state[root] != 0) {
            continue;
        }
        if (!stack.push_back(static_cast<u32>(root))) {
            return;
        }
        while (!stack.empty()) {
            const u32 current = stack.back();
            if (state[current] == 1) {
                state[current] = 2;
                stack.pop_back();
                continue;
            }
            state[current] = 1;
            for (const Link& link : graph.links()) {
                if (link.from != nodes[current].key || !is_data_link(link)) {
                    continue;
                }
                const u32 next = index_of(link.to);
                if (next >= nodes.size()) {
                    continue;
                }
                if (state[next] == 1) {
                    Diagnostic diagnostic;
                    diagnostic.node = link.to;
                    diagnostic.pin = link.to_pin;
                    diagnostic.message = "this wire closes a cycle among data pins";
                    sink.report(diagnostic);
                    continue;
                }
                if (state[next] == 0 && !stack.push_back(next)) {
                    return;
                }
            }
        }
    }
}

}  // namespace

Status validate(const Graph& graph, const NodeRegistry& registry, const GraphLibrary* library,
                DiagnosticSink& sink) noexcept {
    report_unknown_nodes(graph, library, sink);
    for (const Link& link : graph.links()) {
        validate_link(graph, registry, library, link, sink);
    }
    validate_required_inputs(graph, library, sink);
    validate_acyclic(graph, library, sink);
    return ok();
}

}  // namespace cy::graph
