// CyberGraph: identity, typed pins, the textual round trip, subgraphs and opaque preservation.
// M8.b task 2.1.
//
// Each case is one of cybergraph.h's five decisions stated as a check.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/text.h>
#include <cy/test/test.h>

#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

[[nodiscard]] PinDesc pin(const char* name, const char* type, PinDirection direction,
                          bool required = false) noexcept {
    PinDesc desc;
    desc.name = Name::intern(name);
    desc.type = Name::intern(type);
    desc.direction = direction;
    desc.required = required;
    return desc;
}

/// Two node types: a source of a float, and a consumer that requires one.
[[nodiscard]] Status register_types(NodeRegistry& registry) noexcept {
    static const PinDesc source_pins[] = {pin("value", "float", PinDirection::Output)};
    NodeTypeDesc source;
    source.name = Name::intern("test.source");
    source.plugin = Name::intern("test");
    source.pins = Span<const PinDesc>(source_pins, 1);
    if (Status added = registry.register_type(source); !added) {
        return added;
    }
    static const PinDesc sink_pins[] = {pin("in", "float", PinDirection::Input, true),
                                        pin("out", "float", PinDirection::Output)};
    NodeTypeDesc sink;
    sink.name = Name::intern("test.sink");
    sink.plugin = Name::intern("test");
    sink.pins = Span<const PinDesc>(sink_pins, 2);
    if (Status added = registry.register_type(sink); !added) {
        return added;
    }
    static const PinDesc entity_pins[] = {pin("value", "entity", PinDirection::Output)};
    NodeTypeDesc entity;
    entity.name = Name::intern("test.entity");
    entity.plugin = Name::intern("test");
    entity.pins = Span<const PinDesc>(entity_pins, 1);
    return registry.register_type(entity);
}

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = Immediate::scalar(value);
    return literal;
}

}  // namespace

CY_TEST_CASE("cybergraph: identity is an author-owned key, and two nodes cannot share one") {
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(7, Name::intern("test.source")).has_value());
    CY_CHECK_FALSE(graph.add_node(7, Name::intern("test.sink")).has_value());
    // An allocated key never collides with one the author already used.
    const NodeKey allocated = graph.allocate_key();
    CY_CHECK_GT(allocated, 7U);
    CY_CHECK(graph.add_node(allocated, Name::intern("test.sink")).has_value());
}

CY_TEST_CASE("cybergraph: layout is a side table and is not in the semantic digest") {
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());
    const u64 before = graph.semantic_digest();

    NodeLayout layout;
    layout.key = 1;
    layout.x = 400.0F;
    layout.y = -120.0F;
    layout.tint = 0xFF00FFU;
    CY_REQUIRE(graph.set_layout(layout).has_value());
    CY_CHECK_EQ(graph.semantic_digest(), before);

    // A property, by contrast, IS meaning.
    CY_REQUIRE(graph.set_property(1, Name::intern("value"), number(3.0F)).has_value());
    CY_CHECK_NE(graph.semantic_digest(), before);
}

CY_TEST_CASE(
    "cybergraph: the digest is a function of meaning, not of the order things were added") {
    Graph first(allocator(), Name::intern("g"));
    CY_REQUIRE(first.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(first.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(first.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());
    CY_REQUIRE(first.set_property(2, Name::intern("a"), number(1.0F)).has_value());
    CY_REQUIRE(first.set_property(2, Name::intern("b"), number(2.0F)).has_value());

    Graph second(allocator(), Name::intern("g"));
    CY_REQUIRE(second.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(second.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(second.set_property(2, Name::intern("b"), number(2.0F)).has_value());
    CY_REQUIRE(second.set_property(2, Name::intern("a"), number(1.0F)).has_value());
    CY_REQUIRE(second.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());

    CY_CHECK_EQ(first.semantic_digest(), second.semantic_digest());
}

CY_TEST_CASE("cybergraph: a wire whose pin types do not convert is a PIN-PRECISE diagnostic") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_types(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.entity")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    CY_REQUIRE(validate(graph, registry, nullptr, sink).has_value());
    CY_REQUIRE_EQ(sink.errors(), 1U);
    CY_CHECK_EQ(sink.entries()[0].node, 2U);
    CY_CHECK_EQ(sink.entries()[0].pin, Name::intern("in"));
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("entity"));

    // A declared conversion makes it legal, and nothing else changes.
    NodeRegistry converting(allocator());
    CY_REQUIRE(register_types(converting).has_value());
    CY_REQUIRE(
        converting.allow_conversion(Name::intern("entity"), Name::intern("float")).has_value());
    graph.resolve(converting);
    DiagnosticSink clean(allocator());
    CY_REQUIRE(validate(graph, converting, nullptr, clean).has_value());
    CY_CHECK_EQ(clean.errors(), 0U);
}

CY_TEST_CASE("cybergraph: a required input with neither a wire nor a value is reported") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_types(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.sink")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    CY_REQUIRE(validate(graph, registry, nullptr, sink).has_value());
    CY_REQUIRE_EQ(sink.errors(), 1U);
    CY_CHECK_EQ(sink.entries()[0].pin, Name::intern("in"));

    // A property standing in for the wire satisfies it.
    CY_REQUIRE(graph.set_property(1, Name::intern("in"), number(0.5F)).has_value());
    DiagnosticSink second(allocator());
    CY_REQUIRE(validate(graph, registry, nullptr, second).has_value());
    CY_CHECK_EQ(second.errors(), 0U);
}

CY_TEST_CASE("cybergraph: a cycle among data pins is reported at the wire that closes it") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_types(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("out"), 2, Name::intern("in")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("out"), 1, Name::intern("in")).has_value());
    graph.resolve(registry);

    DiagnosticSink sink(allocator());
    CY_REQUIRE(validate(graph, registry, nullptr, sink).has_value());
    CY_CHECK_GE(sink.errors(), 1U);
}

CY_TEST_CASE("cybergraph: the textual round trip preserves meaning and reproduces the bytes") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_types(registry).has_value());
    Graph graph(allocator(), Name::intern("worn"));
    CY_REQUIRE(graph.add_node(3, Name::intern("test.source")).has_value());
    CY_REQUIRE(graph.add_node(1, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("value"), 1, Name::intern("in")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("gain"), number(0.25F)).has_value());
    Literal label;
    label.type = Name::intern("name");
    label.text = Name::intern("the mixer");
    CY_REQUIRE(graph.set_property(1, Name::intern("label"), label).has_value());
    graph.grant(Capability::ReadWorld | Capability::Physics);
    NodeLayout layout;
    layout.key = 3;
    layout.x = 12.5F;
    layout.y = -4.0F;
    CY_REQUIRE(graph.set_layout(layout).has_value());
    graph.resolve(registry);

    Array<char> text(allocator());
    CY_REQUIRE(write_graph(graph, text).has_value());

    DiagnosticSink sink(allocator());
    auto parsed =
        parse_graph(std::string_view(text.data(), text.size()), &registry, allocator(), sink);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_CHECK_EQ(parsed.value().semantic_digest(), graph.semantic_digest());

    // And the form is canonical: writing what was read reproduces the bytes.
    Array<char> again(allocator());
    CY_REQUIRE(write_graph(parsed.value(), again).has_value());
    CY_REQUIRE_EQ(again.size(), text.size());
    bool identical = true;
    for (usize index = 0; index < text.size(); ++index) {
        identical = identical && text[index] == again[index];
    }
    CY_CHECK(identical);
}

CY_TEST_CASE("cybergraph: a node whose plugin is missing is PRESERVED, not dropped") {
    NodeRegistry full(allocator());
    CY_REQUIRE(register_types(full).has_value());
    Graph graph(allocator(), Name::intern("with_plugin"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("thirdparty.mystery"), 3).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("secret"), number(7.5F)).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());

    // Written by a build that HAS the plugin.
    NodeRegistry with_mystery(allocator());
    CY_REQUIRE(register_types(with_mystery).has_value());
    static const PinDesc mystery_pins[] = {pin("in", "float", PinDirection::Input)};
    NodeTypeDesc mystery;
    mystery.name = Name::intern("thirdparty.mystery");
    mystery.plugin = Name::intern("thirdparty");
    mystery.version = 3;
    mystery.pins = Span<const PinDesc>(mystery_pins, 1);
    CY_REQUIRE(with_mystery.register_type(mystery).has_value());
    graph.resolve(with_mystery);
    Array<char> text(allocator());
    CY_REQUIRE(write_graph(graph, text).has_value());

    // Read by a build that does NOT.
    DiagnosticSink sink(allocator());
    auto reopened =
        parse_graph(std::string_view(text.data(), text.size()), &full, allocator(), sink);
    CY_REQUIRE(reopened.has_value());
    CY_CHECK_EQ(reopened.value().opaque_count(), 1U);
    CY_CHECK_EQ(sink.warnings(), 1U);
    CY_CHECK(reopened.value().is_opaque(2));
    // Its wires survive too.
    CY_CHECK_EQ(reopened.value().links().size(), 1U);

    // And writing it back reproduces the file byte for byte: nothing was guessed and nothing lost.
    Array<char> rewritten(allocator());
    CY_REQUIRE(write_graph(reopened.value(), rewritten).has_value());
    CY_REQUIRE_EQ(rewritten.size(), text.size());
    bool identical = true;
    for (usize index = 0; index < text.size(); ++index) {
        identical = identical && text[index] == rewritten[index];
    }
    CY_CHECK(identical);
}

CY_TEST_CASE("cybergraph: a subgraph instance takes its pins from the graph it names") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_types(registry).has_value());

    Graph inner(allocator(), Name::intern("inner"));
    CY_REQUIRE(inner.declare_interface(pin("amount", "float", PinDirection::Input)).has_value());
    CY_REQUIRE(inner.declare_interface(pin("result", "float", PinDirection::Output)).has_value());
    GraphLibrary library(allocator());
    CY_REQUIRE(library.add(std::move(inner)).has_value());

    Graph outer(allocator(), Name::intern("outer"));
    CY_REQUIRE(outer.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(outer.add_node(2, Name::intern("subgraph")).has_value());
    CY_REQUIRE(outer.set_subgraph(2, Name::intern("inner")).has_value());
    CY_REQUIRE(outer.connect(1, Name::intern("value"), 2, Name::intern("amount")).has_value());
    outer.resolve(registry);

    DiagnosticSink sink(allocator());
    CY_REQUIRE(validate(outer, registry, &library, sink).has_value());
    CY_CHECK_EQ(sink.errors(), 0U);

    // Without the library the instance is an error, and it names the graph it wanted.
    DiagnosticSink missing(allocator());
    CY_REQUIRE(validate(outer, registry, nullptr, missing).has_value());
    CY_REQUIRE_EQ(missing.errors(), 1U);
    CY_CHECK_EQ(missing.entries()[0].detail, Name::intern("inner"));
}

CY_TEST_CASE("cybergraph: the debug map answers 'which node is this program location?'") {
    DebugMap map(allocator());
    CY_REQUIRE(map.record(0, 11, Name::intern("value")).has_value());
    CY_REQUIRE(map.record(1, 12).has_value());
    const DebugMap::Site* site = map.find(0);
    CY_REQUIRE(site != nullptr);
    CY_CHECK_EQ(site->node, 11U);
    CY_CHECK_EQ(site->pin, Name::intern("value"));
    CY_CHECK(map.find(9) == nullptr);
}

CY_TEST_CASE("cybergraph: removing a node removes its wires, its layout and its properties") {
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("test.source")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("test.sink")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("value"), 2, Name::intern("in")).has_value());
    CY_REQUIRE(graph.set_property(2, Name::intern("gain"), number(1.0F)).has_value());
    NodeLayout layout;
    layout.key = 2;
    CY_REQUIRE(graph.set_layout(layout).has_value());

    CY_REQUIRE(graph.remove_node(2).has_value());
    CY_CHECK_EQ(graph.links().size(), 0U);
    CY_CHECK(graph.layout(2) == nullptr);
    CY_CHECK(graph.property(2, Name::intern("gain")) == nullptr);
    CY_CHECK_FALSE(graph.remove_node(2).has_value());
}
