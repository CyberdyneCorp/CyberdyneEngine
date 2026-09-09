// CyberGraph's capability sets and determinism audit. M8.b task 2.1.
//
// The case that matters most is the last one: an audit that cannot examine a node reports
// INCOMPLETE rather than clean. A gate that treats "I could not look" as "nothing found" goes green
// on the day a plugin fails to load.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/audit.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

[[nodiscard]] Status register_type(NodeRegistry& registry, const char* name, Capability required,
                                   Determinism determinism) noexcept {
    static const PinDesc pins[1] = {};
    NodeTypeDesc desc;
    desc.name = Name::intern(name);
    desc.plugin = Name::intern("audit");
    desc.pins = Span<const PinDesc>(pins, 0);
    desc.requires_capabilities = required;
    desc.determinism = determinism;
    return registry.register_type(desc);
}

[[nodiscard]] Status register_all(NodeRegistry& registry) noexcept {
    if (Status added =
            register_type(registry, "a.pure", Capability::None, Determinism::Deterministic);
        !added) {
        return added;
    }
    if (Status added =
            register_type(registry, "a.spawn", Capability::SpawnEntity, Determinism::Deterministic);
        !added) {
        return added;
    }
    if (Status added = register_type(registry, "a.clock", Capability::WallClock,
                                     Determinism::NonDeterministic);
        !added) {
        return added;
    }
    return register_type(registry, "a.random", Capability::Randomness, Determinism::SeedDependent);
}

}  // namespace

CY_TEST_CASE(
    "cybergraph_audit: a node needing a capability the graph lacks is named, with the capability") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_all(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("a.pure")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("a.spawn")).has_value());

    AuditReport report;
    DiagnosticSink sink(allocator());
    CY_REQUIRE(audit(graph, registry, report, sink).has_value());
    CY_CHECK_FALSE(report.passed());
    CY_CHECK(has_capability(report.missing, Capability::SpawnEntity));
    CY_REQUIRE_EQ(sink.errors(), 1U);
    CY_CHECK_EQ(sink.entries()[0].node, 2U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("spawn_entity"));

    // Granted deliberately, it passes — and the report still says what was required.
    graph.grant(Capability::SpawnEntity);
    AuditReport granted;
    DiagnosticSink clean(allocator());
    CY_REQUIRE(audit(graph, registry, granted, clean).has_value());
    CY_CHECK(granted.passed());
    CY_CHECK(has_capability(granted.required, Capability::SpawnEntity));
    CY_CHECK_EQ(clean.errors(), 0U);
}

CY_TEST_CASE("cybergraph_audit: a determinism claim is CHECKED against the nodes, not trusted") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_all(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    graph.grant(Capability::WallClock);
    CY_REQUIRE(graph.add_node(1, Name::intern("a.clock")).has_value());
    graph.set_claims_deterministic(true);

    AuditReport report;
    DiagnosticSink sink(allocator());
    CY_REQUIRE(audit(graph, registry, report, sink).has_value());
    CY_CHECK_FALSE(report.deterministic);
    CY_CHECK_EQ(report.nondeterministic_nodes, 1U);
    CY_CHECK_EQ(sink.errors(), 1U);

    // A graph that does not claim determinism is not lied to about; the audit still reports what it
    // found, and reports no error.
    graph.set_claims_deterministic(false);
    AuditReport honest;
    DiagnosticSink quiet(allocator());
    CY_REQUIRE(audit(graph, registry, honest, quiet).has_value());
    CY_CHECK_FALSE(honest.deterministic);
    CY_CHECK_EQ(quiet.errors(), 0U);
}

CY_TEST_CASE("cybergraph_audit: a seed-dependent node is deterministic and is counted") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_all(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    graph.grant(Capability::Randomness);
    CY_REQUIRE(graph.add_node(1, Name::intern("a.random")).has_value());

    AuditReport report;
    DiagnosticSink sink(allocator());
    CY_REQUIRE(audit(graph, registry, report, sink).has_value());
    CY_CHECK(report.deterministic);
    CY_CHECK_EQ(report.seed_dependent_nodes, 1U);
    CY_CHECK(report.passed());
}

CY_TEST_CASE("cybergraph_audit: a muted node is skipped") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_all(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("a.clock")).has_value());
    CY_REQUIRE(graph.mute(1, true).has_value());

    AuditReport report;
    DiagnosticSink sink(allocator());
    CY_REQUIRE(audit(graph, registry, report, sink).has_value());
    CY_CHECK_EQ(report.nodes_examined, 0U);
    CY_CHECK(report.passed());
}

CY_TEST_CASE("cybergraph_audit: a node the audit cannot examine makes the answer INCOMPLETE") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(register_all(registry).has_value());
    Graph graph(allocator(), Name::intern("g"));
    CY_REQUIRE(graph.add_node(1, Name::intern("a.pure")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("thirdparty.unknown")).has_value());

    AuditReport report;
    DiagnosticSink sink(allocator());
    CY_REQUIRE(audit(graph, registry, report, sink).has_value());
    CY_CHECK_EQ(report.nodes_opaque, 1U);
    CY_CHECK_FALSE(report.complete);
    // Nothing was found, and that is NOT a pass.
    CY_CHECK_EQ(report.missing, Capability::None);
    CY_CHECK_FALSE(report.passed());
    CY_CHECK_EQ(sink.warnings(), 1U);
}
