// The override layer, orphans and the persistent delta. M10 tasks.md 4.2, whose second exit
// criterion is "a hand-placed override that survives regeneration of its region".
//
// `procedural-content-generation` — "Overrides and regeneration": "Regeneration SHALL merge
// overrides by stable identity. An override whose target survives regeneration SHALL survive with
// it. An override whose target no longer exists SHALL become an ORPHAN: retained, reported, and
// resolvable — NEVER SILENTLY DISCARDED"; and "Persistence of generated content": "Generated base
// content SHALL NOT be saved ... plus persistent exceptions."
//
// ================================================================================================
// WHY `mis-bound` IS THE OUTCOME THESE CASES ARE SHAPED AROUND
// ================================================================================================
//
// design.md §1.4, 7 877 hand-placed overrides rebound across twelve regenerations: `derived` lost
// 235 and mis-bound ZERO; `counter` mis-bound 3 351 — 43% — on an ORDINARY FULL regeneration. A
// lost override is visible and reportable; a mis-bound one silently moves a different object. So
// the cases below check not only that an override binds but that it binds to the RIGHT instance,
// and that a spatial re-anchor is reported as a guess rather than folded into the bind count.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `bind_one()`'s version guard — the
// `record.written_against_version != current_version` test that keeps the spatial fallback shut
// within one version — was deleted, and "an override whose instance is gone is an orphan, not a
// neighbour" went red: the override re-anchored to the nearest surviving point and was counted
// `Reanchored` instead of `Orphaned`, which is a designer's edit quietly moving to a different
// tree. The guard was then restored. Reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/overrides.h>

#include "fixtures.h"

using cy::pcg::AttributeDecl;
using cy::pcg::AttributeId;
using cy::pcg::AttributeType;
using cy::pcg::BindResult;
using cy::pcg::GeneratedId;
using cy::pcg::MergeResult;
using cy::pcg::Override;
using cy::pcg::OverrideLayer;
using cy::pcg::OverrideOp;
using cy::pcg::OverrideOrigin;
using cy::pcg::PersistentDelta;
using cy::pcg::PointSet;
namespace test = cy::pcg::test;

namespace {

constexpr AttributeId kVariant{1};

/// A generated region's output, by hand: `count` points at one-metre spacing, with the identities a
/// scatter would have derived for slots 0..count-1.
[[nodiscard]] PointSet region_points(cy::u32 count, cy::u32 first_slot = 0) noexcept {
    PointSet points(test::allocator());
    const AttributeDecl columns[1] = {AttributeDecl{"variant", kVariant, AttributeType::I32}};
    CY_REQUIRE(points.reserve(count, cy::Span<const AttributeDecl>(columns, 1)));
    const cy::pcg::NodeIdentity node = cy::pcg::node_identity(test::kScatterNode);
    const cy::pcg::RegionKey key = cy::pcg::region_key(2, 3, 0);
    for (cy::u32 index = 0; index < count; ++index) {
        const cy::u32 slot = first_slot + index;
        cy::Expected<cy::u32, cy::Error> at =
            points.add(static_cast<cy::f32>(slot) * 1.0F, 0.0F, 4.0F, slot);
        CY_REQUIRE(at.has_value());
        points.set_identity(*at, cy::pcg::derive_identity(99, node, key, slot).value);
        points.set_i32(kVariant, *at, 0);
    }
    return points;
}

}  // namespace

CY_TEST_CASE("a moved tree stays moved across a regeneration of its region") {
    // "WHEN a designer moves a generated tree and the region is regenerated THEN the override SHALL
    // reattach BY IDENTITY and the tree SHALL remain where it was placed."
    const PointSet base = region_points(8);
    const GeneratedId target{base.identity(3)};

    OverrideLayer layer(test::allocator());
    Override move;
    move.target = target;
    move.op = OverrideOp::Move;
    move.origin = OverrideOrigin::Authored;
    move.anchor_x = 1000.0 + static_cast<cy::f64>(base.x(3));
    move.anchor_z = 2000.0 + static_cast<cy::f64>(base.z(3));
    move.x = 1050.0;
    move.y = 7.0;
    move.z = 2060.0;
    move.written_against_version = 1;
    CY_REQUIRE(layer.place(move));

    // The region is regenerated: a fresh point set, the same seed, the same slots — which is what a
    // regeneration with unchanged inputs produces.
    const PointSet regenerated = region_points(8);
    cy::Expected<MergeResult, cy::Error> merged = layer.merge(regenerated, 1000.0, 2000.0, 0.0F, 1);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(merged->bound, 1u);
    CY_CHECK_EQ(merged->orphaned, 0u);
    CY_CHECK_EQ(merged->reanchored, 0u);
    CY_CHECK_EQ(merged->points.size(), 8u);

    // It bound to the RIGHT instance — the identity it named, not its neighbour — and it moved.
    cy::usize found = merged->points.size();
    for (cy::usize index = 0; index < merged->points.size(); ++index) {
        if (merged->points.identity(index) == target.value) {
            found = index;
        }
    }
    CY_REQUIRE(found < merged->points.size());
    CY_CHECK_NEAR(merged->points.x(found), 50.0F, 1e-3F);
    CY_CHECK_NEAR(merged->points.z(found), 60.0F, 1e-3F);
    CY_CHECK_NEAR(merged->points.y(found), 7.0F, 1e-3F);
    // And the slot travelled with it: the instance did not become a different instance by being
    // dragged.
    CY_CHECK_EQ(merged->points.slot(found), 3u);
}

CY_TEST_CASE("a felled tree does not return") {
    // "WHEN a region is regenerated after a player felled a generated tree THEN the persistence
    // delta SHALL still refer to that tree and it SHALL remain felled."
    const PointSet base = region_points(8);
    const GeneratedId felled{base.identity(5)};

    OverrideLayer layer(test::allocator());
    Override removal;
    removal.target = felled;
    removal.op = OverrideOp::Delete;
    removal.origin = OverrideOrigin::Gameplay;
    removal.anchor_x = static_cast<cy::f64>(base.x(5));
    removal.anchor_z = static_cast<cy::f64>(base.z(5));
    removal.written_against_version = 1;
    CY_REQUIRE(layer.place(removal));

    const PointSet regenerated = region_points(8);
    cy::Expected<MergeResult, cy::Error> merged = layer.merge(regenerated, 0.0, 0.0, 0.0F, 1);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(merged->deleted, 1u);
    CY_CHECK_EQ(merged->points.size(), 7u);
    for (cy::usize index = 0; index < merged->points.size(); ++index) {
        CY_CHECK_NE(merged->points.identity(index), felled.value);
    }
}

CY_TEST_CASE("a locked instance survives a regeneration that no longer produces it") {
    const PointSet base = region_points(8);
    const GeneratedId locked{base.identity(7)};

    OverrideLayer layer(test::allocator());
    Override lock;
    lock.target = locked;
    lock.op = OverrideOp::Lock;
    lock.anchor_x = static_cast<cy::f64>(base.x(7));
    lock.anchor_z = static_cast<cy::f64>(base.z(7));
    lock.written_against_version = 1;
    CY_REQUIRE(layer.place(lock));

    const PointSet regenerated = region_points(8);
    cy::Expected<MergeResult, cy::Error> merged = layer.merge(regenerated, 0.0, 0.0, 0.0F, 1);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(merged->locked, 1u);
    CY_CHECK_EQ(merged->points.size(), 8u);
}

CY_TEST_CASE("an override whose instance is gone is an orphan, not a neighbour") {
    // THE CASE §1.4 IS ABOUT. A rule change removed the instance; the override must be REPORTED
    // rather than quietly rebound to whatever is nearest — which is what `counter` did to 43% of
    // the spike's overrides and what nothing in a running world would have noticed.
    const PointSet base = region_points(8);
    const GeneratedId vanished{base.identity(5)};

    OverrideLayer layer(test::allocator());
    Override move;
    move.target = vanished;
    move.op = OverrideOp::Move;
    move.origin = OverrideOrigin::Authored;
    move.anchor_x = static_cast<cy::f64>(base.x(5));
    move.anchor_z = static_cast<cy::f64>(base.z(5));
    move.x = 12.0;
    move.z = 12.0;
    move.written_against_version = 1;
    CY_REQUIRE(layer.place(move));

    // The rule changed: slots 5 and above are no longer produced. Slot 4 is a metre away from where
    // the override's anchor sits, so a spatial guess would find it.
    const PointSet regenerated = region_points(5);
    cy::Expected<MergeResult, cy::Error> merged =
        layer.merge(regenerated, 0.0, 0.0, 10.0F /* a generous tolerance */, 1);
    CY_REQUIRE(merged.has_value());
    CY_CHECK_EQ(merged->orphaned, 1u);
    CY_CHECK_EQ(merged->reanchored, 0u);
    CY_CHECK_EQ(merged->bound, 0u);
    // Retained and reported, with the identity it was written against, so it can be resolved.
    CY_REQUIRE(layer.refresh_orphans(*merged));
    CY_REQUIRE_EQ(layer.orphans().size(), 1u);
    CY_CHECK(layer.orphans()[0].target == vanished);
    CY_CHECK(layer.orphans()[0].result == BindResult::Orphaned);
    CY_CHECK(test::same_text(cy::pcg::bind_result_name(BindResult::Orphaned), "orphaned"));
}

CY_TEST_CASE("a generator version change opens the spatial anchor, and reports it as a guess") {
    // "Exceptions SHALL be anchored by stable identity AND SPATIALLY, so they can be re-resolved
    // after a GENERATOR VERSION CHANGE, and reported as orphaned when they cannot."
    const PointSet base = region_points(8);
    const GeneratedId vanished{base.identity(5)};

    OverrideLayer layer(test::allocator());
    Override removal;
    removal.target = vanished;
    removal.op = OverrideOp::Delete;
    removal.origin = OverrideOrigin::Gameplay;
    removal.anchor_x = static_cast<cy::f64>(base.x(5));
    removal.anchor_z = static_cast<cy::f64>(base.z(5));
    removal.written_against_version = 1;
    CY_REQUIRE(layer.place(removal));

    PersistentDelta delta(test::allocator());
    CY_REQUIRE(cy::pcg::collect_persistent(layer, 99, 1, 0xabcd, delta));
    CY_REQUIRE_EQ(delta.exceptions.size(), 1u);

    // Version 2 produces a different set of identities for the same square, so the exception cannot
    // bind by identity — and NOW the spatial anchor is allowed to answer.
    PointSet moved_identities(test::allocator());
    const AttributeDecl columns[1] = {AttributeDecl{"variant", kVariant, AttributeType::I32}};
    CY_REQUIRE(moved_identities.reserve(8, cy::Span<const AttributeDecl>(columns, 1)));
    const cy::pcg::NodeIdentity node = cy::pcg::node_identity(test::kScatterNode);
    const cy::pcg::RegionKey key = cy::pcg::region_key(2, 3, 0);
    for (cy::u32 index = 0; index < 8; ++index) {
        cy::Expected<cy::u32, cy::Error> at =
            moved_identities.add(static_cast<cy::f32>(index), 0.0F, 4.0F, index);
        CY_REQUIRE(at.has_value());
        // A different SEED stands in for a rule change: the same geometry, different identities.
        moved_identities.set_identity(*at, cy::pcg::derive_identity(1234, node, key, index).value);
    }

    cy::Expected<MergeResult, cy::Error> resolved = cy::pcg::re_resolve(
        test::allocator(), delta, moved_identities, 0.0, 0.0, /*current_version=*/2, 2.0F);
    CY_REQUIRE(resolved.has_value());
    // REPORTED AS A GUESS, not folded into the bind count: a designer should see that this one was
    // re-anchored rather than found.
    CY_CHECK_EQ(resolved->reanchored, 1u);
    CY_CHECK_EQ(resolved->bound, 0u);
    CY_REQUIRE_EQ(resolved->reports.size(), 1u);
    CY_CHECK(resolved->reports[0].result == BindResult::Reanchored);
    CY_CHECK(resolved->reports[0].reanchored_to.is_valid());
    CY_CHECK_LT(resolved->reports[0].distance_metres, 2.0F);
}

CY_TEST_CASE("a save records the exceptions and not the forest") {
    // "WHEN a player fells two hundred trees in a generated forest of a million THEN the save SHALL
    // record two hundred exceptions." The check is that the size is a function of the EXCEPTIONS
    // and of nothing about the world they are in.
    const PointSet base = region_points(200);

    OverrideLayer layer(test::allocator());
    const cy::pcg::NodeIdentity node = cy::pcg::node_identity(test::kScatterNode);
    const cy::pcg::RegionKey key = cy::pcg::region_key(2, 3, 0);
    for (cy::u32 slot = 0; slot < 200; ++slot) {
        Override felled;
        felled.target = cy::pcg::derive_identity(99, node, key, slot);
        felled.op = OverrideOp::Delete;
        felled.origin = OverrideOrigin::Gameplay;
        felled.written_against_version = 1;
        CY_REQUIRE(layer.place(felled));
    }
    // And an author's own edit, which is PROJECT CONTENT and must not be in the save.
    Override authored;
    authored.target = GeneratedId{base.identity(0)};
    authored.op = OverrideOp::Move;
    authored.origin = OverrideOrigin::Authored;
    authored.written_against_version = 1;
    CY_REQUIRE(layer.place(authored));

    PersistentDelta delta(test::allocator());
    CY_REQUIRE(cy::pcg::collect_persistent(layer, 99, 1, 0xabcd, delta));
    CY_CHECK_EQ(delta.exceptions.size(), 200u);
    CY_CHECK_EQ(delta.seed, 99u);
    CY_CHECK_EQ(delta.generator_version, 1u);

    // The size is the exceptions plus a header, whatever the forest holds. A save that carried the
    // generated base would be orders of magnitude larger, and would be the forbidden pattern
    // "Saving generated base content rather than exceptions".
    const cy::u64 bytes = delta.encoded_bytes();
    CY_CHECK_LT(bytes, 200u * 128u);
    CY_CHECK_GT(bytes, 200u * 16u);
}

CY_TEST_CASE("two overrides of one instance are two records, and one is replaceable") {
    const PointSet base = region_points(4);
    const GeneratedId target{base.identity(1)};

    OverrideLayer layer(test::allocator());
    Override move;
    move.target = target;
    move.op = OverrideOp::Move;
    move.x = 1.0;
    CY_REQUIRE(layer.place(move));
    Override lock;
    lock.target = target;
    lock.op = OverrideOp::Lock;
    CY_REQUIRE(layer.place(lock));
    // A designer who both moved and locked a tree has written TWO overrides of one instance.
    CY_CHECK_EQ(layer.size(), 2u);

    // Moving it again is the same override, to the second place.
    move.x = 5.0;
    CY_REQUIRE(layer.place(move));
    CY_CHECK_EQ(layer.size(), 2u);
    CY_CHECK(layer.remove(target, OverrideOp::Lock));
    CY_CHECK_FALSE(layer.remove(target, OverrideOp::Delete));
}
