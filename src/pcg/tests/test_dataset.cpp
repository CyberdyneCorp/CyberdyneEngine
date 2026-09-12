// Typed spatial datasets, and attributes addressed by compiled identifiers. M10 tasks.md 4.1.
//
// `procedural-content-generation` — "Typed spatial datasets": "Dataset elements SHALL carry typed
// attributes addressed by compiled identifiers ... Attribute names SHALL NOT be resolved by string
// lookup at execution time", and its scenario: "WHEN a generator reads a point's species attribute
// THEN it SHALL use a compiled identifier, not a string key."
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `AttributeTable::intern()`'s type check — the
// `existing->type != type` branch — was deleted, and "an attribute interned with a second type is
// refused" went red on its `CY_REQUIRE_FALSE(again.has_value())`, the second intern having handed
// back an identifier for a column of the wrong width. The branch was then restored. The same run is
// reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/dataset.h>
#include <cy/pcg/graph.h>
#include <cy/pcg/program.h>

#include "fixtures.h"

using cy::pcg::AttributeDecl;
using cy::pcg::AttributeId;
using cy::pcg::AttributeTable;
using cy::pcg::AttributeType;
using cy::pcg::Digest;
using cy::pcg::PointSet;
using cy::pcg::Raster;
namespace test = cy::pcg::test;

CY_TEST_CASE("an attribute is interned once and answers to its identifier") {
    AttributeTable table(test::allocator());
    cy::Expected<AttributeId, cy::Error> density = table.intern("density", AttributeType::F32);
    CY_REQUIRE(density.has_value());
    CY_CHECK(density->is_valid());

    // Re-interning the same name is the same identifier: two nodes that both write `density` write
    // one column, which is what makes a subgraph composable at all.
    cy::Expected<AttributeId, cy::Error> again = table.intern("density", AttributeType::F32);
    CY_REQUIRE(again.has_value());
    CY_CHECK(*again == *density);
    CY_CHECK_EQ(table.size(), 1u);

    // A default-constructed identifier is invalid rather than being the first column's.
    CY_CHECK_FALSE(AttributeId{}.is_valid());
    CY_CHECK(table.find(AttributeId{}) == nullptr);
}

CY_TEST_CASE("an attribute interned with a second type is refused") {
    AttributeTable table(test::allocator());
    CY_REQUIRE(table.intern("biome", AttributeType::I32).has_value());

    // Two nodes disagreeing about a column's shape is a graph defect. Resolving it by
    // last-writer-wins would make the compiled column's WIDTH depend on node order.
    cy::Expected<AttributeId, cy::Error> again = table.intern("biome", AttributeType::F32);
    CY_REQUIRE_FALSE(again.has_value());
    CY_CHECK(again.error().code == cy::ErrorCode::AlreadyExists);
    CY_CHECK_EQ(table.size(), 1u);
}

CY_TEST_CASE("a compiled program carries identifiers, and its stages resolve no names") {
    cy::Expected<test::ForestGraph, cy::Error> forest = test::build_forest(test::allocator());
    CY_REQUIRE(forest.has_value());

    cy::pcg::CompileReport report(test::allocator());
    cy::Array<cy::pcg::CompileDiagnostic> diagnostics(test::allocator());
    cy::Expected<cy::pcg::Program, cy::Error> program =
        cy::pcg::compile(test::allocator(), forest->graph, report, diagnostics);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(diagnostics.size(), 0u);

    // Every stage that writes a channel writes an IDENTIFIER the table resolves. A stage carrying a
    // name would have to be looked up at execution, which is the sentence this case is about.
    for (const cy::pcg::Stage& stage : program->stages()) {
        if (!stage.output.is_valid()) {
            continue;
        }
        CY_CHECK(program->attributes().find(stage.output) != nullptr);
    }
    // And the two columns the evaluator owns are in the program's own table, so a diagnostic can
    // name them without the evaluator ever having to.
    CY_CHECK(program->priority_attribute().is_valid());
    CY_CHECK(program->density_attribute().is_valid());
    CY_CHECK(program->density_attribute() == forest->attributes.density);
}

CY_TEST_CASE("a point set refuses to grow past the capacity it reserved") {
    PointSet points(test::allocator());
    const AttributeDecl columns[1] = {AttributeDecl{"weight", AttributeId{1}, AttributeType::F32}};
    CY_REQUIRE(points.reserve(4, cy::Span<const AttributeDecl>(columns, 1)));

    for (cy::u32 index = 0; index < 4; ++index) {
        cy::Expected<cy::u32, cy::Error> added =
            points.add(static_cast<cy::f32>(index), 0.0F, 0.0F, index);
        CY_REQUIRE(added.has_value());
        CY_CHECK_EQ(*added, index);
    }
    // THE REFUSAL. A container that grew here would allocate per point, which is the thing
    // "no per-point heap allocation SHALL occur" forbids; `test_scale.cpp` measures the same
    // property against an allocator at a million points.
    cy::Expected<cy::u32, cy::Error> overflow = points.add(9.0F, 0.0F, 0.0F, 9);
    CY_REQUIRE_FALSE(overflow.has_value());
    CY_CHECK(overflow.error().code == cy::ErrorCode::OutOfRange);
    CY_CHECK_EQ(points.size(), 4u);
}

CY_TEST_CASE("retain compacts in place and keeps each survivor's original slot") {
    PointSet points(test::allocator());
    const AttributeDecl columns[1] = {AttributeDecl{"weight", AttributeId{1}, AttributeType::F32}};
    CY_REQUIRE(points.reserve(6, cy::Span<const AttributeDecl>(columns, 1)));
    for (cy::u32 index = 0; index < 6; ++index) {
        CY_REQUIRE(points.add(static_cast<cy::f32>(index), 0.0F, 0.0F, index * 10).has_value());
        points.set_identity(index, 1000 + index);
        points.set_f32(AttributeId{1}, index, static_cast<cy::f32>(index) * 0.5F);
    }
    const cy::u8 keep[6] = {0, 1, 0, 1, 1, 0};
    CY_REQUIRE(points.retain(cy::Span<const cy::u8>(keep, 6)));
    CY_REQUIRE_EQ(points.size(), 3u);

    // THE SLOTS ARE THE ORIGINAL ONES, not 0, 1, 2. A filtered set that renumbered would be
    // `SurvivorRank` under another name, and identity.h's whole argument is that a rank does not
    // survive an edit.
    CY_CHECK_EQ(points.slot(0), 10u);
    CY_CHECK_EQ(points.slot(1), 30u);
    CY_CHECK_EQ(points.slot(2), 40u);
    CY_CHECK_EQ(points.identity(0), 1001u);
    CY_CHECK_EQ(points.identity(2), 1004u);
    CY_CHECK_NEAR(points.get_f32(AttributeId{1}, 1), 1.5F, 1e-6F);
}

CY_TEST_CASE("a point set's digest covers positions, slots, identities and columns") {
    const AttributeDecl columns[1] = {AttributeDecl{"weight", AttributeId{1}, AttributeType::F32}};

    PointSet a(test::allocator());
    CY_REQUIRE(a.reserve(2, cy::Span<const AttributeDecl>(columns, 1)));
    CY_REQUIRE(a.add(1.0F, 2.0F, 3.0F, 7).has_value());
    a.set_identity(0, 42);
    a.set_f32(AttributeId{1}, 0, 0.25F);

    cy::Expected<PointSet, cy::Error> b = a.clone();
    CY_REQUIRE(b.has_value());
    CY_CHECK_EQ(a.digest(), b->digest());

    // The IDENTITY half is checked separately, because the spike measured a configuration whose
    // output was bit-identical and whose identities had all moved. A digest that covered positions
    // alone would have called `counter` sound in all twelve trials.
    b->set_identity(0, 43);
    CY_CHECK_NE(a.digest(), b->digest());
}

CY_TEST_CASE("a digest treats negative zero as zero") {
    // Two regions that computed -0.0 and +0.0 are the same region. Digesting the raw bit pattern
    // would make them differ, and the fixed point would then re-dirty the world for ever.
    Digest positive;
    positive.f32_value(0.0F);
    Digest negative;
    negative.f32_value(-0.0F);
    CY_CHECK_EQ(positive.value(), negative.value());

    Digest other;
    other.f32_value(1.0F);
    CY_CHECK_NE(positive.value(), other.value());
}

CY_TEST_CASE("a raster addresses its channels by identifier and digests them per channel") {
    Raster raster(test::allocator());
    const AttributeId channels[2] = {AttributeId{1}, AttributeId{2}};
    CY_REQUIRE(raster.reset(4, cy::Span<const AttributeId>(channels, 2)));
    CY_CHECK_EQ(raster.edge(), 4u);
    CY_CHECK(raster.has(AttributeId{1}));
    CY_CHECK_FALSE(raster.has(AttributeId{3}));

    const cy::u64 first_before = raster.digest_of(AttributeId{1});
    const cy::u64 second_before = raster.digest_of(AttributeId{2});
    // Two empty channels do not digest alike: the identifier participates, so a stage that wrote
    // the wrong channel is a change rather than a coincidence.
    CY_CHECK_NE(first_before, second_before);

    raster.set(AttributeId{1}, 2, 3, 5.0F);
    CY_CHECK_NEAR(raster.at(AttributeId{1}, 2, 3), 5.0F, 1e-6F);
    CY_CHECK_NE(raster.digest_of(AttributeId{1}), first_before);
    // A channel nothing touched is unchanged, which is what lets one stage's dirty set be computed
    // from its own output rather than from the whole raster.
    CY_CHECK_EQ(raster.digest_of(AttributeId{2}), second_before);
}
