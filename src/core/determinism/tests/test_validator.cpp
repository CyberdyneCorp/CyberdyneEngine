// M9 TASK 2.5 — the validator: which tick, then which field.
//
// The two halves are tested as two halves, because they fail differently. `TickHashComparison` is a
// per-tick root hash from each side and answers *when*; `localise()` descends two full trees and
// answers *where*. The chaos half — running one scenario under deliberately different execution
// conditions — is in the integration suite (test_chaos.cpp), because a case that runs a scenario
// several times is not a microsecond of arithmetic.

#include <cy/core/determinism/validator.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

using namespace cy;
using namespace cy::determinism;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// Two entities, one component each, two fields. Built twice with one value changed, which is what
/// a divergence looks like when you can see it.
void build_tree(StateHashTree& tree, f32 health_of_second, u64 second_entity) noexcept {
    CY_REQUIRE(tree.begin(HashLevel::World, 0, "world").has_value());
    CY_REQUIRE(tree.begin(HashLevel::Archetype, 1, "robots").has_value());
    const u64 entities[] = {41, second_entity};
    const f32 healths[] = {10.0F, health_of_second};
    for (u32 index = 0; index < 2; ++index) {
        CY_REQUIRE(tree.begin(HashLevel::Entity, entities[index], "").has_value());
        CY_REQUIRE(tree.begin(HashLevel::Component, 7, "Robot").has_value());
        CY_REQUIRE(tree.begin(HashLevel::Field, 1, "health").has_value());
        tree.mix_f32(healths[index]);
        CY_REQUIRE(tree.end().has_value());
        CY_REQUIRE(tree.begin(HashLevel::Field, 2, "charge").has_value());
        tree.mix_u64(100);
        CY_REQUIRE(tree.end().has_value());
        CY_REQUIRE(tree.end().has_value());
        CY_REQUIRE(tree.end().has_value());
    }
    CY_REQUIRE(tree.end().has_value());
    CY_REQUIRE(tree.end().has_value());
}

}  // namespace

CY_TEST_CASE(
    "determinism: the comparison names the first tick that disagreed, not the first heard") {
    TickHashComparison comparison(allocator());
    for (u64 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(comparison.observe(RunSide::Left, tick, 1000 + tick).has_value());
        CY_REQUIRE(comparison.observe(RunSide::Right, tick, 1000 + tick).has_value());
    }
    CY_CHECK_FALSE(comparison.diverged());
    CY_CHECK_EQ(comparison.last_agreeing_tick(), u64{4});
    CY_CHECK_EQ(comparison.ticks_compared(), 5U);
    CY_CHECK_FALSE(comparison.truncated());

    // Two ticks disagree; the EARLIER one is the answer, and it stays the answer when the later one
    // arrives afterwards.
    CY_REQUIRE(comparison.observe(RunSide::Left, 5, 1).has_value());
    CY_REQUIRE(comparison.observe(RunSide::Right, 5, 2).has_value());
    CY_REQUIRE(comparison.observe(RunSide::Left, 6, 3).has_value());
    CY_REQUIRE(comparison.observe(RunSide::Right, 6, 4).has_value());
    CY_CHECK(comparison.diverged());
    CY_CHECK_EQ(comparison.first_diverging_tick(), u64{5});
    CY_CHECK_EQ(comparison.last_agreeing_tick(), u64{4});

    // Out of order is refused. A comparison that accepted them in any order would report the first
    // disagreement it happened to be told about rather than the first that happened.
    CY_CHECK_FALSE(comparison.observe(RunSide::Left, 3, 9).has_value());
}

CY_TEST_CASE("determinism: a run that stopped early is truncated, not divergent") {
    TickHashComparison comparison(allocator());
    for (u64 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(comparison.observe(RunSide::Left, tick, tick).has_value());
    }
    for (u64 tick = 0; tick < 2; ++tick) {
        CY_REQUIRE(comparison.observe(RunSide::Right, tick, tick).has_value());
    }
    // A run that crashed at tick 2 agrees with everything up to tick 1. Calling that a divergence
    // would send the reader looking for a state difference that is not there.
    CY_CHECK_FALSE(comparison.diverged());
    CY_CHECK(comparison.truncated());
    CY_CHECK_EQ(comparison.ticks_compared(), 2U);
    CY_CHECK_EQ(comparison.last_agreeing_tick(), u64{1});
}

CY_TEST_CASE("determinism: a divergence is narrowed to a field on an entity") {
    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 20.0F, 42);
    build_tree(right, 20.5F, 42);
    CY_REQUIRE_NE(left.root_hash(), right.root_hash());

    FieldDivergence divergence;
    localise(left, right, divergence);
    CY_REQUIRE(divergence.diverged);
    CY_CHECK_FALSE(divergence.shape_mismatch);
    // "the result of a mismatch is a named field on a named entity rather than a statement that two
    // numbers differ."
    CY_CHECK_EQ(divergence.entity, u64{42});
    CY_CHECK_EQ(divergence.component, u64{7});
    CY_CHECK(std::strcmp(divergence.component_name, "Robot") == 0);
    CY_CHECK_EQ(divergence.field, u64{1});
    CY_CHECK(std::strcmp(divergence.field_name, "health") == 0);
    CY_CHECK_NE(divergence.left, divergence.right);
}

CY_TEST_CASE("determinism: an entity present in one run is a shape mismatch, not a value one") {
    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 20.0F, 42);
    build_tree(right, 20.0F, 43);  // the same content under a different identifier

    FieldDivergence divergence;
    localise(left, right, divergence);
    CY_REQUIRE(divergence.diverged);
    // The two have completely different causes, so they are reported as different things.
    CY_CHECK(divergence.shape_mismatch);
    CY_CHECK_EQ(divergence.entity, u64{42});
}

CY_TEST_CASE("determinism: two identical trees produce no divergence at all") {
    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 20.0F, 42);
    build_tree(right, 20.0F, 42);
    CY_CHECK_EQ(left.root_hash(), right.root_hash());

    FieldDivergence divergence;
    localise(left, right, divergence);
    CY_CHECK_FALSE(divergence.diverged);
    CY_CHECK_EQ(divergence.entity, u64{0});
    CY_CHECK(std::strcmp(divergence.field_name, "") == 0);
}

CY_TEST_CASE("determinism: chaos conditions are reproducible from their seed") {
    // "a divergence found by a chaos run is reproducible by quoting the seed and the index rather
    // than by luck."
    for (u32 index = 0; index < 8; ++index) {
        const ExecutionConditions first = chaos_conditions(index, 0xC0FFEEULL, 8);
        const ExecutionConditions again = chaos_conditions(index, 0xC0FFEEULL, 8);
        CY_CHECK(first == again);
        CY_CHECK_GE(first.worker_count, 1U);
        CY_CHECK_LE(first.worker_count, 8U);
    }
    // A different seed gives a different set. If it did not, chaos would be one environment with a
    // parameter nobody read.
    bool any_difference = false;
    for (u32 index = 0; index < 8; ++index) {
        any_difference =
            any_difference || !(chaos_conditions(index, 1, 8) == chaos_conditions(index, 2, 8));
    }
    CY_CHECK(any_difference);
}
