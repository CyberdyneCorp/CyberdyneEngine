#pragma once
// CyberGraph: the shared authoring layer every graph consumer adopts. M8.b task 2.1.
//
// ================================================================================================
// THIS, AND NOT AN INTERMEDIATE REPRESENTATION, IS WHAT IS SHARED
// ================================================================================================
//
// `visual-scripting`'s first requirement is normative and it names this layer exactly:
//
//   "Visual authoring SHALL be provided as shared graph infrastructure with domain-specific
//    lowering, not as one universal graph language. [...] The infrastructure SHALL own: node and
//    pin models, typed connections, stable identity, serialization, subgraphs, the editor canvas
//    and its undo, diffing and merging, versioning and migration, and debugging."
//
// The same requirement forbids the other reading — "Scenario: No universal representation — WHEN a
// proposal would route material expressions and gameplay control flow through one intermediate
// representation, THEN it SHALL be rejected against this requirement" — and M8.b's spike measured
// what it would cost: five consumers needing an escape hatch against a budget of two.
//
// So: ONE editor, ONE diff format, ONE debugging model, ONE migration mechanism, and SIX
// compilers. This header is the first half of that sentence.
//
// ================================================================================================
// THE DECISIONS THAT MAKE ONE AUTHORING LAYER SERVE SEVEN DOMAINS
// ================================================================================================
//
// 1. A NODE TYPE IS DATA, NOT AN ENUMERATOR. `NodeTypeDesc` is registered at run time by whoever
//    owns the domain, which is what lets a plugin add a node without this module changing — and
//    what makes "the plugin is missing" a state that has to be representable rather than a crash.
//
// 2. A PIN TYPE IS A NAME. `visual-scripting`: "a universal variant type SHALL NOT be the default
//    pin type". A pin carries the name of its type — `float`, `entity`, `gameplay_tag`, `pose`,
//    `bt_status` — and the registry says which names convert to which. An enumeration here would
//    force every domain's type lattice into one, which is precisely what the spike refuted.
//
// 3. IDENTITY IS A KEY THE AUTHOR OWNS, NOT AN INDEX. `NodeKey` survives insertion, deletion,
//    reordering and a round trip through text. Every diff, every merge, every diagnostic and every
//    debug-map entry is keyed by it, because an index is a diff that reports a hundred changes
//    when a node is inserted at the top.
//
// 4. LAYOUT IS A SIDE TABLE AND IS NOT IN THE SEMANTIC DIGEST. Moving a node must not recompile a
//    graph and must not conflict in a merge. `Graph::semantic_digest()` closes over nodes, pins,
//    links, properties and the interface, and over nothing an editor's canvas owns.
//
// 5. A NODE WHOSE PLUGIN IS MISSING IS PRESERVED VERBATIM. Its properties are kept as authored
//    text, its links are kept, and writing the graph back reproduces it byte for byte. An editor
//    that silently drops what it cannot understand destroys an author's work on open.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/graph/expr.h>

#include <string_view>

namespace cy::graph {

/// A node's stable authoring identity. Zero is "no node".
using NodeKey = u64;
inline constexpr NodeKey kInvalidNodeKey = 0;

/// The format version of an authored graph. A graph records the version it was written at, and
/// migration is what carries an older one forward.
inline constexpr u32 kGraphVersion = 1;

/// A value an author typed into a node: a literal, a name, an asset path, a tag.
///
/// NOT A UNIVERSAL PIN TYPE — decision 2. This is what a node's own PROPERTY holds, which is a
/// different thing from what flows along a wire: a property is authored once and is part of the
/// graph's text, and a pin's type is the domain's.
struct Literal {
    /// The pin type this literal is written at: `float`, `int`, `bool`, `name`, `vec3`, ...
    Name type;
    Immediate value;
    /// An identifier, a tag, an asset path. Empty for a purely numeric literal.
    Name text;

    friend bool operator==(const Literal& a, const Literal& b) noexcept;
    friend bool operator!=(const Literal& a, const Literal& b) noexcept { return !(a == b); }
};

struct Property {
    Name name;
    Literal value;
};

enum class PinDirection : u8 { Input = 0, Output };

/// One pin on a node type, or one pin of a subgraph's interface.
struct PinDesc {
    Name name;
    /// The pin type's NAME (decision 2), not an index into anyone's lattice.
    Name type;
    PinDirection direction = PinDirection::Input;
    /// An execution pin — control flow — rather than a data pin. `visual-scripting`'s IR has
    /// explicit control flow; the authoring layer has to be able to say which wires carry it, and
    /// a domain that has no control flow simply declares none.
    bool execution = false;
    /// The pin accepts any number of wires. A data input that is not variadic accepts one, and the
    /// last wire wins, which is what an editor does.
    bool variadic = false;
    /// An input that must be wired or carry a property before the graph compiles.
    bool required = false;
};

/// What a node is allowed to do. A graph GRANTS a set; a node type REQUIRES one; the audit reports
/// every node that requires what its graph was not granted.
enum class Capability : u32 {
    None = 0,
    ReadWorld = 1U << 0U,
    WriteWorld = 1U << 1U,
    SpawnEntity = 1U << 2U,
    DestroyEntity = 1U << 3U,
    Physics = 1U << 4U,
    Audio = 1U << 5U,
    Network = 1U << 6U,
    FileSystem = 1U << 7U,
    /// Draws from a random stream. Deterministic ONLY when the stream is derived from simulation
    /// state; the audit says so.
    Randomness = 1U << 8U,
    /// Reads a clock that is not the simulation's. Never deterministic.
    WallClock = 1U << 9U,
    /// Calls out to native code the graph cannot see into.
    NativeCall = 1U << 10U,
};

[[nodiscard]] constexpr Capability operator|(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr Capability operator&(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<u32>(a) & static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_capability(Capability set, Capability wanted) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(wanted)) == static_cast<u32>(wanted) &&
           static_cast<u32>(wanted) != 0;
}
[[nodiscard]] constexpr Capability without(Capability set, Capability removed) noexcept {
    return static_cast<Capability>(static_cast<u32>(set) & ~static_cast<u32>(removed));
}
[[nodiscard]] const char* capability_name(Capability single) noexcept;

/// What a node type promises about repeatability.
enum class Determinism : u8 {
    /// Same inputs, same outputs, on every machine and every run.
    Deterministic = 0,
    /// Deterministic only when the simulation supplies its stream — a random draw seeded from
    /// simulation state is here, and a wall clock is not.
    SeedDependent,
    /// Never deterministic. A graph that reaches one of these cannot be part of the simulation.
    NonDeterministic,
};

/// A node type as a domain registers it. The arrays are borrowed: the registry copies them.
struct NodeTypeDesc {
    Name name;
    /// The plugin that owns it. Recorded on every node so that a graph opened without that plugin
    /// can say WHOSE node it is preserving rather than only that it does not know.
    Name plugin;
    /// The version this build of the type is at. A node authored at a lower version is migrated.
    u32 version = 1;
    Span<const PinDesc> pins;
    Capability requires_capabilities = Capability::None;
    Determinism determinism = Determinism::Deterministic;
    /// A pure node: its outputs depend on its inputs and on nothing else. Only a pure node may be
    /// lowered onto the shared expression core, because that core's identity is a content hash.
    bool pure = true;
};

/// A registered node type. Owns its pin table.
class NodeType {
public:
    NodeType(Allocator& allocator, const NodeTypeDesc& desc) noexcept;

    NodeType(const NodeType&) = delete;
    NodeType& operator=(const NodeType&) = delete;
    NodeType(NodeType&&) noexcept = default;
    NodeType& operator=(NodeType&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Name plugin() const noexcept { return plugin_; }
    [[nodiscard]] u32 version() const noexcept { return version_; }
    [[nodiscard]] Span<const PinDesc> pins() const noexcept { return pins_.span(); }
    [[nodiscard]] const PinDesc* find_pin(Name pin, PinDirection direction) const noexcept;
    [[nodiscard]] Capability required() const noexcept { return required_; }
    [[nodiscard]] Determinism determinism() const noexcept { return determinism_; }
    [[nodiscard]] bool pure() const noexcept { return pure_; }

private:
    Name name_;
    Name plugin_;
    u32 version_ = 1;
    Array<PinDesc> pins_;
    Capability required_ = Capability::None;
    Determinism determinism_ = Determinism::Deterministic;
    bool pure_ = true;
};

/// The node types and pin-type conversions one editor session knows about.
class NodeRegistry {
public:
    explicit NodeRegistry(Allocator& allocator) noexcept;

    NodeRegistry(const NodeRegistry&) = delete;
    NodeRegistry& operator=(const NodeRegistry&) = delete;

    [[nodiscard]] Status register_type(const NodeTypeDesc& desc) noexcept;
    [[nodiscard]] const NodeType* find(Name type) const noexcept;

    /// Declare that a value of `from` may be wired into a pin of `to` without an explicit
    /// conversion node. A pin type converts to itself without being declared.
    [[nodiscard]] Status allow_conversion(Name from, Name to) noexcept;
    [[nodiscard]] bool converts(Name from, Name to) const noexcept;

    [[nodiscard]] usize size() const noexcept { return types_.size(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return types_.allocator(); }

private:
    struct Conversion {
        Name from;
        Name to;
    };

    Array<NodeType> types_;
    Array<Conversion> conversions_;
};

/// One node in an authored graph.
struct GraphNode {
    /// Stable authoring identity (decision 3).
    NodeKey key = kInvalidNodeKey;
    Name type;
    /// The plugin the node was authored against, recorded even when the type is registered, so an
    /// opaque node can name its owner.
    Name plugin;
    /// The version the node was authored at. Migration reads it.
    u32 version = 1;
    /// The author muted it. It stays in the graph and contributes nothing — an editor still shows
    /// it, and a lowering turns it into whatever "nothing" means in its domain.
    bool muted = false;
    /// The graph this node instantiates, when it is a subgraph instance.
    Name subgraph;
    /// Resolved at load: null when the plugin that owns this node type is missing (decision 5).
    const NodeType* resolved = nullptr;
};

/// A wire. Deterministically ordered by (to, to_pin, from, from_pin) wherever order is observable.
struct Link {
    NodeKey from = kInvalidNodeKey;
    Name from_pin;
    NodeKey to = kInvalidNodeKey;
    Name to_pin;

    friend bool operator==(const Link& a, const Link& b) noexcept;
    friend bool operator!=(const Link& a, const Link& b) noexcept { return !(a == b); }
};

/// Where a node sits on the canvas, and what colour the author gave it. A SIDE TABLE, outside the
/// semantic digest (decision 4).
struct NodeLayout {
    NodeKey key = kInvalidNodeKey;
    f32 x = 0.0F;
    f32 y = 0.0F;
    u32 tint = 0;
    Name comment;
};

/// Severity of an authoring diagnostic.
enum class Severity : u8 { Info = 0, Warning, Error };

/// NODE- AND PIN-PRECISE. `visual-scripting`: a diagnostic that names a graph and not a pin sends
/// an author hunting through a canvas.
struct Diagnostic {
    Severity severity = Severity::Error;
    NodeKey node = kInvalidNodeKey;
    /// The pin the diagnostic is about, or an empty name when it is about the node.
    Name pin;
    const char* message = "";
    /// A second name the message refers to — a type, a missing plugin, the other end of a wire.
    Name detail;
};

/// Where diagnostics go. An array with a cap, so a graph with ten thousand broken wires reports the
/// first hundred rather than exhausting memory reporting all of them.
class DiagnosticSink {
public:
    explicit DiagnosticSink(Allocator& allocator, usize cap = 256) noexcept
        : entries_(allocator), cap_(cap) {}

    void report(const Diagnostic& diagnostic) noexcept;
    [[nodiscard]] Span<const Diagnostic> entries() const noexcept { return entries_.span(); }
    [[nodiscard]] u32 errors() const noexcept { return errors_; }
    [[nodiscard]] u32 warnings() const noexcept { return warnings_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    void clear() noexcept;

private:
    Array<Diagnostic> entries_;
    usize cap_;
    u32 errors_ = 0;
    u32 warnings_ = 0;
    bool overflowed_ = false;
};

/// THE DEBUG MAP. `visual-scripting` requires "a debug map from program location to node and pin",
/// and every lowering in `lower/` fills one in. It lives here rather than in a lowering because a
/// debugger that had to know which compiler produced a program is a debugger per compiler.
class DebugMap {
public:
    struct Site {
        u32 location = 0;
        NodeKey node = kInvalidNodeKey;
        Name pin;
    };

    explicit DebugMap(Allocator& allocator) noexcept : sites_(allocator) {}

    [[nodiscard]] Status record(u32 location, NodeKey node, Name pin = Name{}) noexcept;
    [[nodiscard]] const Site* find(u32 location) const noexcept;
    [[nodiscard]] Span<const Site> sites() const noexcept { return sites_.span(); }

private:
    Array<Site> sites_;
};

/// An authored graph.
///
/// The unit an editor edits, a diff diffs, a merge merges and a lowering compiles. It knows nothing
/// about any domain: what a node MEANS is the business of the lowering that reads it.
class Graph {
public:
    Graph(Allocator& allocator, Name graph_name) noexcept;

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) noexcept = default;
    Graph& operator=(Graph&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    void set_name(Name graph_name) noexcept { name_ = graph_name; }
    [[nodiscard]] u32 version() const noexcept { return version_; }
    void set_version(u32 version) noexcept { version_ = version; }

    // --- Nodes ---------------------------------------------------------------------------------

    /// Add a node with an author-owned key. A key already in the graph is refused: two nodes with
    /// one identity would make every diff and every merge ambiguous.
    [[nodiscard]] Status add_node(NodeKey key, Name type, u32 version = 1) noexcept;
    /// A key no node in this graph uses. Monotone and recorded, so it survives a round trip and two
    /// editors adding a node do not both pick the same one after a reload.
    [[nodiscard]] NodeKey allocate_key() noexcept;
    [[nodiscard]] Status remove_node(NodeKey key) noexcept;
    [[nodiscard]] const GraphNode* find_node(NodeKey key) const noexcept;
    [[nodiscard]] GraphNode* find_node(NodeKey key) noexcept;
    [[nodiscard]] Span<const GraphNode> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] Status mute(NodeKey key, bool muted) noexcept;

    // --- Properties ----------------------------------------------------------------------------

    [[nodiscard]] Status set_property(NodeKey key, Name property, const Literal& value) noexcept;
    [[nodiscard]] const Literal* property(NodeKey key, Name property) const noexcept;
    [[nodiscard]] Span<const Property> properties(NodeKey key) const noexcept;

    // --- Links ---------------------------------------------------------------------------------

    /// Wire an output pin into an input pin. A non-variadic input keeps the LAST wire, which is
    /// what an editor does; a variadic input accumulates them in wiring order.
    [[nodiscard]] Status connect(NodeKey from, Name from_pin, NodeKey to, Name to_pin) noexcept;
    [[nodiscard]] Status disconnect(NodeKey to, Name to_pin, NodeKey from = kInvalidNodeKey,
                                    Name from_pin = Name{}) noexcept;
    [[nodiscard]] Span<const Link> links() const noexcept { return links_.span(); }
    /// The wires arriving at one input pin, appended to `out`.
    [[nodiscard]] Status inputs_of(NodeKey node, Name pin, Array<Link>& out) const noexcept;

    // --- Layout, a side table (decision 4) -------------------------------------------------------

    [[nodiscard]] Status set_layout(const NodeLayout& layout) noexcept;
    [[nodiscard]] const NodeLayout* layout(NodeKey key) const noexcept;
    [[nodiscard]] Span<const NodeLayout> layouts() const noexcept { return layout_.span(); }

    // --- Subgraphs -----------------------------------------------------------------------------

    /// Declare one pin of this graph's own interface, so it can be instantiated as a subgraph.
    [[nodiscard]] Status declare_interface(const PinDesc& pin) noexcept;
    [[nodiscard]] Span<const PinDesc> interface_pins() const noexcept { return interface_.span(); }
    /// Mark a node as an instance of another graph. The node's pins are then the referenced
    /// graph's interface rather than a registered type's.
    [[nodiscard]] Status set_subgraph(NodeKey key, Name graph_name) noexcept;

    // --- Capabilities --------------------------------------------------------------------------

    void grant(Capability capabilities) noexcept { granted_ = granted_ | capabilities; }
    void revoke(Capability capabilities) noexcept { granted_ = without(granted_, capabilities); }
    [[nodiscard]] Capability granted() const noexcept { return granted_; }
    /// What the author claims about this graph. The determinism audit checks the claim rather than
    /// trusting it.
    void set_claims_deterministic(bool claims) noexcept { claims_deterministic_ = claims; }
    [[nodiscard]] bool claims_deterministic() const noexcept { return claims_deterministic_; }

    // --- Opaque preservation (decision 5) --------------------------------------------------------

    /// Keep a node's authored body verbatim because the plugin that owns its type is missing.
    [[nodiscard]] Status set_opaque_body(NodeKey key, std::string_view body) noexcept;
    [[nodiscard]] std::string_view opaque_body(NodeKey key) const noexcept;
    [[nodiscard]] bool is_opaque(NodeKey key) const noexcept;
    [[nodiscard]] u32 opaque_count() const noexcept;

    // --- Identity ------------------------------------------------------------------------------

    /// A content hash over MEANING and nothing else: the nodes, their types and versions, their
    /// properties, the links, the interface and the granted capabilities. Layout is excluded, and
    /// so is the order the author happened to add things in.
    [[nodiscard]] u64 semantic_digest() const noexcept;

    /// Resolve every node's type against a registry. A node whose type is absent keeps whatever
    /// body it was loaded with and is reported, not dropped.
    void resolve(const NodeRegistry& registry) noexcept;

    /// A deep copy, including the layout and the opaque bodies. A graph is move-only because it
    /// owns several arrays and an accidental copy of one in a loop is a real cost; a merge needs an
    /// explicit one, so it asks for it.
    [[nodiscard]] Expected<Graph, Error> clone(Allocator& allocator) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return nodes_.allocator(); }

private:
    struct NodeProperties {
        NodeKey key = kInvalidNodeKey;
        Array<Property> entries;
    };
    struct OpaqueBody {
        NodeKey key = kInvalidNodeKey;
        Array<char> text;
    };

    [[nodiscard]] NodeProperties* properties_for(NodeKey key) noexcept;
    [[nodiscard]] const NodeProperties* properties_for(NodeKey key) const noexcept;

    Name name_;
    u32 version_ = kGraphVersion;
    NodeKey next_key_ = 1;
    Capability granted_ = Capability::None;
    bool claims_deterministic_ = true;
    Array<GraphNode> nodes_;
    Array<Link> links_;
    Array<NodeLayout> layout_;
    Array<PinDesc> interface_;
    Array<NodeProperties> properties_;
    Array<OpaqueBody> opaque_;
};

/// A named set of graphs: what a subgraph instance resolves against.
class GraphLibrary {
public:
    explicit GraphLibrary(Allocator& allocator) noexcept : graphs_(allocator) {}

    GraphLibrary(const GraphLibrary&) = delete;
    GraphLibrary& operator=(const GraphLibrary&) = delete;

    [[nodiscard]] Status add(Graph&& graph) noexcept;
    [[nodiscard]] const Graph* find(Name graph_name) const noexcept;
    [[nodiscard]] Span<const Graph> graphs() const noexcept { return graphs_.span(); }

private:
    Array<Graph> graphs_;
};

/// Check a graph against a registry, reporting node- and pin-precise diagnostics.
///
/// What it checks: an unregistered node type (an ERROR that does not destroy the node — see
/// decision 5), a wire naming a pin that does not exist, a wire whose direction is wrong, a wire
/// whose pin types do not convert, a required input with neither a wire nor a property, a subgraph
/// instance whose graph is missing, and a CYCLE among pure data pins.
///
/// `library` may be null in a graph that instantiates no subgraph.
[[nodiscard]] Status validate(const Graph& graph, const NodeRegistry& registry,
                              const GraphLibrary* library, DiagnosticSink& sink) noexcept;

}  // namespace cy::graph
