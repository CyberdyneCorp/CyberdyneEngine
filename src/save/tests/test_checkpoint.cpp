// Checkpoints and restore. `save-and-persistence` — "Checkpoints and restore".
//
// Three sentences, three claims, and the middle one is the one a reviewer would otherwise take on
// trust:
//
//   1. "retaining what is required to restore session, world, and participant state WITHOUT
//      RESTARTING THE APPLICATION" — the restore is a merge into a live overlay, and what comes
//      back is what was captured, including the state that changed after the capture being gone.
//   2. "Checkpoint restore SHALL INCREMENT THE SIMULATION EPOCH, so temporal caches and histories
//      treat themselves as stale." A stamp taken before the restore must read as stale afterwards,
//      which is exactly what `determinism::is_stale()` answers and nothing else would catch.
//   3. "Checkpoints MAY be retained in memory as well as on storage, SUBJECT TO THE MEMORY BUDGET."
//      A store over its budget evicts; an evicted checkpoint is reported as evicted rather than as
//      absent; and one checkpoint larger than the whole budget is refused rather than retained.
//
// HOW TO MAKE IT FAIL:
//   * delete the `epoch.advance(...)` line in CheckpointStore::restore() -> case 2 goes red, and it
//     is the only check in this file that would notice;
//   * make eviction unconditional or never happen                       -> case 3 goes red;
//   * make restore() merge rather than replace                          -> case 1 goes red.

#include <cy/core/determinism/epoch.h>
#include <cy/save/checkpoint.h>
#include <cy/save/overlay.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstring>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};

void record_health(Overlay& overlay, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(kVillage, id, health_type(), &health, 1).has_value());
}

u32 revives_of(const Overlay& overlay, PersistentId id) {
    const ComponentDelta* delta =
        overlay.find_component(kVillage, id, reflect::TypeId(kHealthTypeId));
    if (delta == nullptr) {
        return 0xFFFF'FFFFU;
    }
    const Span<const u8> bytes = delta->record.bytes(reflect::FieldId(kHealthRevives));
    if (bytes.size() != sizeof(u32)) {
        return 0xFFFF'FFFEU;
    }
    u32 value = 0;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

determinism::SimulationPoint at(u32 epoch, u64 tick) {
    return determinism::SimulationPoint{determinism::Epoch{epoch}, tick};
}

}  // namespace

CY_TEST_CASE("a checkpoint restores the session without restarting it") {
    Overlay live(test_allocator());
    record_health(live, entity(1), 3);

    CheckpointStore store(test_allocator());
    const auto taken = store.take(live, at(0, 1000));
    CY_REQUIRE(taken.has_value());
    CY_CHECK_EQ(taken.value().entries, 1U);
    CY_CHECK(taken.value().resident);
    CY_CHECK(taken.value().bytes > 0U);

    // Play continues past the checkpoint, and then the player dies.
    record_health(live, entity(1), 9);
    record_health(live, entity(2), 1);
    CY_CHECK_EQ(revives_of(live, entity(1)), 9U);

    determinism::EpochCounter epoch;
    const auto restored = store.restore(live, epoch);
    CY_REQUIRE(restored.has_value());

    // The same live overlay, not a new one: no application restart, no reload.
    CY_CHECK_EQ(revives_of(live, entity(1)), 3U);
    CY_CHECK(live.find_entry(kVillage, entity(2)) == nullptr);
    CY_CHECK_EQ(restored.value().entries, 1U);
    CY_CHECK_EQ(restored.value().captured_at.tick, 1000ULL);
}

CY_TEST_CASE("restoring a checkpoint increments the simulation epoch") {
    Overlay live(test_allocator());
    record_health(live, entity(1), 1);

    CheckpointStore store(test_allocator());
    CY_REQUIRE(store.take(live, at(0, 500)).has_value());

    determinism::EpochCounter epoch;
    const determinism::Epoch before = epoch.current();

    // A cache stamped now, in the epoch the session is currently in.
    const determinism::SimulationPoint stamp{before, 500};
    CY_CHECK_FALSE(determinism::is_stale(stamp, determinism::SimulationPoint{before, 640}));

    const auto restored = store.restore(live, epoch);
    CY_REQUIRE(restored.has_value());

    CY_CHECK_EQ(restored.value().epoch_before.value, before.value);
    CY_CHECK_EQ(restored.value().epoch_after.value, before.value + 1U);
    CY_CHECK_EQ(epoch.current().value, before.value + 1U);
    CY_CHECK(epoch.reason() == determinism::EpochReason::CheckpointRestore);

    // THE POINT OF THE REQUIREMENT. The cache stamped before the restore now reads as stale, and
    // the tick alone would never have said so: the restore put the session back at tick 500.
    const determinism::SimulationPoint now{epoch.current(), 500};
    CY_CHECK(determinism::is_stale(stamp, now));
}

CY_TEST_CASE("retention is bounded by the memory budget") {
    Overlay live(test_allocator());
    record_health(live, entity(1), 1);

    CheckpointStore store(test_allocator());
    CheckpointConfig config;
    config.memory_slots = 2;
    config.memory_budget_bytes = 1ULL << 20;
    CY_REQUIRE(store.configure(config).has_value());

    const auto first = store.take(live, at(0, 100));
    CY_REQUIRE(first.has_value());
    record_health(live, entity(1), 2);
    CY_REQUIRE(store.take(live, at(0, 200)).has_value());
    record_health(live, entity(1), 3);
    CY_REQUIRE(store.take(live, at(0, 300)).has_value());

    // Three taken, two slots: the oldest is gone, and the history still knows it existed.
    CY_CHECK_EQ(store.resident_count(), 2U);
    CY_CHECK_EQ(store.evictions(), 1U);
    CY_CHECK_EQ(store.history().size(), 3U);
    CY_CHECK_FALSE(store.history()[0].resident);
    CY_CHECK(store.history()[2].resident);
    CY_CHECK(store.resident_bytes() <= config.memory_budget_bytes);

    // Evicted and never-taken are different answers, and a caller that retries needs to know which.
    determinism::EpochCounter epoch;
    Overlay out(test_allocator());
    const auto evicted = store.restore(first.value().id, out, epoch);
    CY_REQUIRE_FALSE(evicted.has_value());
    CY_CHECK(evicted.error().code == ErrorCode::Unavailable);
    const auto never = store.restore(9999U, out, epoch);
    CY_REQUIRE_FALSE(never.has_value());
    CY_CHECK(never.error().code == ErrorCode::NotFound);
    // A refused restore does not advance the epoch: nothing was restored.
    CY_CHECK_EQ(epoch.current().value, 0U);
}

CY_TEST_CASE("one checkpoint larger than the whole budget is refused, not silently dropped") {
    Overlay live(test_allocator());
    for (u64 index = 0; index < 64; ++index) {
        record_health(live, entity(index), static_cast<u32>(index));
    }

    CheckpointStore store(test_allocator());
    CheckpointConfig config;
    config.memory_slots = 4;
    config.memory_budget_bytes = 16;  // smaller than any real overlay
    CY_REQUIRE(store.configure(config).has_value());

    const auto refused = store.take(live, at(0, 10));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == ErrorCode::OutOfMemory);
    CY_CHECK_EQ(store.resident_count(), 0U);
    // And the failure is at TAKE time, where the caller can act on it, rather than at restore time.
    determinism::EpochCounter epoch;
    Overlay out(test_allocator());
    CY_REQUIRE_FALSE(store.restore(out, epoch).has_value());
}
