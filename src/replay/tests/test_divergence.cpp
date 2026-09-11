// M9 TASKS 1.5 AND 2.5 — the divergence window: the small capture the validator produces.

#include "fixture.h"

#include <cy/core/determinism/validator.h>
#include <cy/replay/divergence.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::determinism::HashLevel;
using cy::determinism::RunSide;
using cy::determinism::StateHashTree;
using cy::determinism::TickHashComparison;

namespace {

void build_tree(StateHashTree& tree, cy::f32 health) noexcept {
    CY_REQUIRE(tree.begin(HashLevel::World, 0, "world").has_value());
    CY_REQUIRE(tree.begin(HashLevel::Entity, 42, "").has_value());
    CY_REQUIRE(tree.begin(HashLevel::Component, 7, "Robot").has_value());
    CY_REQUIRE(tree.begin(HashLevel::Field, 1, "health").has_value());
    tree.mix_f32(health);
    CY_REQUIRE(tree.end().has_value());
    CY_REQUIRE(tree.end().has_value());
    CY_REQUIRE(tree.end().has_value());
    CY_REQUIRE(tree.end().has_value());
}

}  // namespace

CY_TEST_CASE("replay: the window carries the checkpoint, the commands and the field") {
    RecordLog log(allocator(), manifest());
    for (u64 tick = 0; tick < 30; ++tick) {
        if (tick % 10 == 0) {
            CY_REQUIRE(log.append(checkpoint_record(tick, tick * 4)).has_value());
        }
        CY_REQUIRE(log.append(command_record(tick, 0, static_cast<cy::i32>(tick))).has_value());
        CY_REQUIRE(log.append(command_record(tick, 1, static_cast<cy::i32>(tick) + 1)).has_value());
        CY_REQUIRE(log.append(hash_record(tick, 0x9000ULL + tick)).has_value());
    }

    TickHashComparison comparison(allocator());
    for (u64 tick = 0; tick < 26; ++tick) {
        CY_REQUIRE(comparison.observe(RunSide::Left, tick, 0x9000ULL + tick).has_value());
        // The right-hand run agrees until tick 25.
        CY_REQUIRE(comparison.observe(RunSide::Right, tick, tick == 25 ? 0xBAD : 0x9000ULL + tick)
                       .has_value());
    }
    CY_REQUIRE(comparison.diverged());
    CY_REQUIRE_EQ(comparison.first_diverging_tick(), u64{25});

    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 1.0F);
    build_tree(right, 2.0F);

    cy::Array<LogRecord> records(allocator());
    DivergenceWindow window;
    DivergenceCursor cursor(log);
    CY_REQUIRE(cursor.capture(comparison, left, right, records, window).has_value());

    CY_REQUIRE(window.valid);
    CY_CHECK(window.has_last_agreeing);
    CY_CHECK_EQ(window.last_agreeing_tick, u64{24});
    CY_CHECK_EQ(window.first_diverging_tick, u64{25});
    // The last agreeing snapshot, as the checkpoint at or before tick 24.
    CY_CHECK(window.has_checkpoint);
    CY_CHECK_EQ(window.checkpoint.tick, u64{20});
    // The commands in between: tick 25 only, two of them.
    CY_CHECK_EQ(window.command_count, 2U);
    CY_CHECK_EQ(window.external_count, 0U);
    // The random trace is NOT recorded and does not need to be: a draw is a pure function of the
    // seed, the stream, the point, the entity and the index, so the seed is what the window
    // carries.
    CY_CHECK_EQ(window.session_seed, manifest().session_seed);
    // And the field.
    CY_REQUIRE(window.field.diverged);
    CY_CHECK_EQ(window.field.entity, u64{42});
    CY_CHECK(std::strcmp(window.field.component_name, "Robot") == 0);
    CY_CHECK(std::strcmp(window.field.field_name, "health") == 0);
    CY_CHECK_NE(window.left_hash, u64{0});
    CY_CHECK_NE(window.right_hash, u64{0});

    // Every record in (24, 25] and nothing outside it.
    CY_REQUIRE(records.size() > cy::usize{0});
    for (const LogRecord& record : records) {
        CY_CHECK_GT(record.tick, u64{24});
        CY_CHECK_LE(record.tick, u64{25});
    }
}

CY_TEST_CASE("replay: a window around no divergence is refused") {
    // Producing a "window" around no divergence is exactly the shape of a report that says
    // something happened when nothing did.
    RecordLog log(allocator(), manifest());
    CY_REQUIRE(log.append(command_record(0, 0, 1)).has_value());

    TickHashComparison comparison(allocator());
    CY_REQUIRE(comparison.observe(RunSide::Left, 0, 1).has_value());
    CY_REQUIRE(comparison.observe(RunSide::Right, 0, 1).has_value());
    CY_REQUIRE_FALSE(comparison.diverged());

    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 1.0F);
    build_tree(right, 1.0F);

    cy::Array<LogRecord> records(allocator());
    DivergenceWindow window;
    DivergenceCursor cursor(log);
    CY_CHECK_FALSE(cursor.capture(comparison, left, right, records, window).has_value());
    CY_CHECK_FALSE(window.valid);
}

CY_TEST_CASE("replay: a divergence in the first chunk has no checkpoint, and says so") {
    RecordLog log(allocator(), manifest());
    for (u64 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(log.append(command_record(tick, 0, 1)).has_value());
    }
    TickHashComparison comparison(allocator());
    CY_REQUIRE(comparison.observe(RunSide::Left, 0, 1).has_value());
    CY_REQUIRE(comparison.observe(RunSide::Right, 0, 2).has_value());

    StateHashTree left(allocator());
    StateHashTree right(allocator());
    build_tree(left, 1.0F);
    build_tree(right, 3.0F);

    cy::Array<LogRecord> records(allocator());
    DivergenceWindow window;
    DivergenceCursor cursor(log);
    CY_REQUIRE(cursor.capture(comparison, left, right, records, window).has_value());
    // The session diverged at its first tick: there is no last agreeing tick and no checkpoint, and
    // a report that implied one would send the reader looking for a capture that does not exist.
    CY_CHECK_FALSE(window.has_last_agreeing);
    CY_CHECK_FALSE(window.has_checkpoint);
    CY_CHECK_EQ(window.first_diverging_tick, u64{0});
}
