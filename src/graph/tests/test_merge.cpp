// CyberGraph's semantic diff, three-way merge and migration. M8.b task 2.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/merge.h>
#include <cy/test/test.h>

#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = Immediate::scalar(value);
    return literal;
}

/// Two wired nodes with a property on each. The base every case below diverges from.
[[nodiscard]] Expected<Graph, Error> base_graph() noexcept {
    Graph graph(allocator(), Name::intern("base"));
    if (!graph.add_node(1, Name::intern("m.source"))) {
        return make_unexpected(Error{ErrorCode::Internal, "add", 0});
    }
    if (!graph.add_node(2, Name::intern("m.sink"))) {
        return make_unexpected(Error{ErrorCode::Internal, "add", 0});
    }
    if (!graph.connect(1, Name::intern("out"), 2, Name::intern("in"))) {
        return make_unexpected(Error{ErrorCode::Internal, "connect", 0});
    }
    if (!graph.set_property(1, Name::intern("gain"), number(1.0F))) {
        return make_unexpected(Error{ErrorCode::Internal, "property", 0});
    }
    if (!graph.set_property(2, Name::intern("bias"), number(0.0F))) {
        return make_unexpected(Error{ErrorCode::Internal, "property", 0});
    }
    return graph;
}

[[nodiscard]] u32 count_of(Span<const Change> changes, ChangeKind kind) noexcept {
    u32 total = 0;
    for (const Change& change : changes) {
        total += change.kind == kind ? 1U : 0U;
    }
    return total;
}

Status raise_gain(Graph& graph, NodeKey node, u32 /*from*/) noexcept {
    const Literal* existing = graph.property(node, Name::intern("gain"));
    const f32 value = existing != nullptr ? existing->value.x : 1.0F;
    // Version 2 spells the gain in decibels rather than as a multiplier.
    return graph.set_property(node, Name::intern("gain_db"), number(value * 20.0F));
}

}  // namespace

CY_TEST_CASE("cybergraph_merge: a diff is a list of changes keyed by node and pin") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto other = base.value().clone(allocator());
    CY_REQUIRE(other.has_value());
    CY_REQUIRE(other.value().add_node(3, Name::intern("m.source")).has_value());
    CY_REQUIRE(other.value().set_property(1, Name::intern("gain"), number(2.0F)).has_value());
    CY_REQUIRE(other.value().disconnect(2, Name::intern("in")).has_value());

    Array<Change> changes(allocator());
    CY_REQUIRE(diff(base.value(), other.value(), changes).has_value());
    CY_CHECK_EQ(count_of(changes.span(), ChangeKind::NodeAdded), 1U);
    CY_CHECK_EQ(count_of(changes.span(), ChangeKind::PropertySet), 1U);
    CY_CHECK_EQ(count_of(changes.span(), ChangeKind::LinkRemoved), 1U);
    // Nothing else moved, and the diff says so rather than reporting the whole file.
    CY_CHECK_EQ(changes.size(), 3U);
}

CY_TEST_CASE("cybergraph_merge: two authors who touch different nodes always merge") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto ours = base.value().clone(allocator());
    auto theirs = base.value().clone(allocator());
    CY_REQUIRE(ours.has_value());
    CY_REQUIRE(theirs.has_value());
    CY_REQUIRE(ours.value().set_property(1, Name::intern("gain"), number(3.0F)).has_value());
    CY_REQUIRE(theirs.value().set_property(2, Name::intern("bias"), number(0.5F)).has_value());

    MergeReport report(allocator());
    auto merged = merge3(base.value(), ours.value(), theirs.value(), report);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(report.conflicts.size(), 0U);
    CY_CHECK_EQ(report.taken_from_theirs, 1U);
    const Literal* gain = merged.value().property(1, Name::intern("gain"));
    const Literal* bias = merged.value().property(2, Name::intern("bias"));
    CY_REQUIRE(gain != nullptr);
    CY_REQUIRE(bias != nullptr);
    CY_CHECK_EQ(gain->value.x, 3.0F);
    CY_CHECK_EQ(bias->value.x, 0.5F);
}

CY_TEST_CASE("cybergraph_merge: two authors who change one property conflict, and OURS is kept") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto ours = base.value().clone(allocator());
    auto theirs = base.value().clone(allocator());
    CY_REQUIRE(ours.has_value());
    CY_REQUIRE(theirs.has_value());
    CY_REQUIRE(ours.value().set_property(1, Name::intern("gain"), number(3.0F)).has_value());
    CY_REQUIRE(theirs.value().set_property(1, Name::intern("gain"), number(9.0F)).has_value());

    MergeReport report(allocator());
    auto merged = merge3(base.value(), ours.value(), theirs.value(), report);
    CY_REQUIRE(merged.has_value());
    CY_REQUIRE_EQ(report.conflicts.size(), 1U);
    CY_CHECK_EQ(report.conflicts[0].node, 1U);
    CY_CHECK_EQ(report.conflicts[0].detail, Name::intern("gain"));
    // The result is still a graph an author can open, and it is theirs to resolve.
    const Literal* gain = merged.value().property(1, Name::intern("gain"));
    CY_REQUIRE(gain != nullptr);
    CY_CHECK_EQ(gain->value.x, 3.0F);
}

CY_TEST_CASE("cybergraph_merge: two authors who made the SAME change agree rather than conflict") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto ours = base.value().clone(allocator());
    auto theirs = base.value().clone(allocator());
    CY_REQUIRE(ours.has_value());
    CY_REQUIRE(theirs.has_value());
    CY_REQUIRE(ours.value().set_property(1, Name::intern("gain"), number(4.0F)).has_value());
    CY_REQUIRE(theirs.value().set_property(1, Name::intern("gain"), number(4.0F)).has_value());

    MergeReport report(allocator());
    auto merged = merge3(base.value(), ours.value(), theirs.value(), report);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(report.conflicts.size(), 0U);
    CY_CHECK_EQ(report.already_agreed, 1U);
}

CY_TEST_CASE("cybergraph_merge: moving a node never conflicts") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto ours = base.value().clone(allocator());
    auto theirs = base.value().clone(allocator());
    CY_REQUIRE(ours.has_value());
    CY_REQUIRE(theirs.has_value());
    NodeLayout mine;
    mine.key = 1;
    mine.x = 10.0F;
    NodeLayout yours;
    yours.key = 1;
    yours.x = 900.0F;
    CY_REQUIRE(ours.value().set_layout(mine).has_value());
    CY_REQUIRE(theirs.value().set_layout(yours).has_value());

    MergeReport report(allocator());
    auto merged = merge3(base.value(), ours.value(), theirs.value(), report);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(report.conflicts.size(), 0U);
    const NodeLayout* layout = merged.value().layout(1);
    CY_REQUIRE(layout != nullptr);
    CY_CHECK_EQ(layout->x, 900.0F);
    // And the meaning did not move.
    CY_CHECK_EQ(merged.value().semantic_digest(), ours.value().semantic_digest());
}

CY_TEST_CASE("cybergraph_merge: a node one side deleted and the other edited is a conflict") {
    auto base = base_graph();
    CY_REQUIRE(base.has_value());
    auto ours = base.value().clone(allocator());
    auto theirs = base.value().clone(allocator());
    CY_REQUIRE(ours.has_value());
    CY_REQUIRE(theirs.has_value());
    CY_REQUIRE(ours.value().set_property(2, Name::intern("bias"), number(0.75F)).has_value());
    CY_REQUIRE(theirs.value().remove_node(2).has_value());

    MergeReport report(allocator());
    auto merged = merge3(base.value(), ours.value(), theirs.value(), report);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_GE(report.conflicts.size(), 1U);
    // Ours survives: the deletion is what needs a human, and losing the edit would be the worse
    // half of the guess.
    CY_CHECK(merged.value().find_node(2) != nullptr);
}

CY_TEST_CASE(
    "cybergraph_migration: a rule carries a node forward, and a missing rule is reported") {
    NodeRegistry registry(allocator());
    static const PinDesc pins[1] = {};
    NodeTypeDesc source;
    source.name = Name::intern("m.source");
    source.plugin = Name::intern("m");
    source.version = 2;
    source.pins = Span<const PinDesc>(pins, 0);
    CY_REQUIRE(registry.register_type(source).has_value());
    NodeTypeDesc sink;
    sink.name = Name::intern("m.sink");
    sink.plugin = Name::intern("m");
    sink.version = 4;
    sink.pins = Span<const PinDesc>(pins, 0);
    CY_REQUIRE(registry.register_type(sink).has_value());

    auto graph = base_graph();
    CY_REQUIRE(graph.has_value());

    MigrationTable table(allocator());
    MigrationRule rule;
    rule.type = Name::intern("m.source");
    rule.from_version = 1;
    rule.to_version = 2;
    rule.apply = &raise_gain;
    CY_REQUIRE(table.add(rule).has_value());
    // A rule that goes nowhere is refused rather than looping.
    MigrationRule backwards = rule;
    backwards.to_version = 1;
    CY_CHECK_FALSE(table.add(backwards).has_value());

    DiagnosticSink sink_out(allocator());
    auto migrated = table.migrate(graph.value(), registry, sink_out);
    CY_REQUIRE(migrated.has_value());
    CY_CHECK_EQ(migrated.value(), 1U);
    const GraphNode* node = graph.value().find_node(1);
    CY_REQUIRE(node != nullptr);
    CY_CHECK_EQ(node->version, 2U);
    const Literal* decibels = graph.value().property(1, Name::intern("gain_db"));
    CY_REQUIRE(decibels != nullptr);
    CY_CHECK_EQ(decibels->value.x, 20.0F);

    // `m.sink` is at version 4 with no rule at all: reported, and LEFT ALONE rather than guessed
    // at.
    CY_CHECK_EQ(sink_out.errors(), 1U);
    const GraphNode* untouched = graph.value().find_node(2);
    CY_REQUIRE(untouched != nullptr);
    CY_CHECK_EQ(untouched->version, 1U);
}

CY_TEST_CASE("cybergraph_migration: a node from a NEWER build is left exactly as it was") {
    NodeRegistry registry(allocator());
    static const PinDesc pins[1] = {};
    NodeTypeDesc source;
    source.name = Name::intern("m.source");
    source.plugin = Name::intern("m");
    source.version = 1;
    source.pins = Span<const PinDesc>(pins, 0);
    CY_REQUIRE(registry.register_type(source).has_value());

    Graph graph(allocator(), Name::intern("from_the_future"));
    CY_REQUIRE(graph.add_node(1, Name::intern("m.source"), 9).has_value());
    const u64 before = graph.semantic_digest();

    MigrationTable table(allocator());
    DiagnosticSink sink(allocator());
    auto migrated = table.migrate(graph, registry, sink);
    CY_REQUIRE(migrated.has_value());
    CY_CHECK_EQ(migrated.value(), 0U);
    CY_CHECK_EQ(sink.errors(), 1U);
    CY_CHECK_EQ(graph.semantic_digest(), before);
}
