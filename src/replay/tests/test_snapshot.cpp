// M9 TASK 1.3 — snapshot kinds, checkpoints, and the bounded rollback window.
//
// `integration` rather than `unit`: every case builds an `ecs::World`, registers components and
// captures it, which is not a millisecond of arithmetic. The taxonomy's own rule.

#include "fixture.h"

#include <cy/core/determinism/provider.h>
#include <cy/ecs/world.h>
#include <cy/replay/snapshot.h>

#include <cstring>

using namespace cy::replay_test;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::determinism::Epoch;
using cy::determinism::Participates;
using cy::determinism::SimulationPoint;
using cy::determinism::StateProvider;
using cy::determinism::StateProviderRegistry;

namespace {

struct Position {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
};

/// A random source's seed and cursor, as a provider. `replay-and-rollback`: "random streams and
/// provider state SHALL be restored with entity state".
class SeedProvider final : public StateProvider {
public:
    SeedProvider(const char* name, Participates participation) noexcept
        : name_(name), participation_(participation) {}

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] Participates participation() const noexcept override { return participation_; }
    [[nodiscard]] cy::Status capture(cy::Array<u8>& out) const noexcept override {
        for (u32 byte = 0; byte < 8; ++byte) {
            if (cy::Status pushed = out.push_back(static_cast<u8>((seed >> (byte * 8U)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    }
    [[nodiscard]] cy::Status restore(cy::Span<const u8> bytes) noexcept override {
        if (bytes.size() != 8) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "seed provider: wrong size");
        }
        seed = 0;
        for (u32 byte = 0; byte < 8; ++byte) {
            seed |= static_cast<u64>(bytes[byte]) << (byte * 8U);
        }
        return cy::ok();
    }

    u64 seed = 0;

private:
    const char* name_;
    Participates participation_;
};

struct Fixture {
    cy::ecs::World world{allocator()};
    StateProviderRegistry providers{allocator()};
    SeedProvider rollback_seed{"random", Participates::Rollback | Participates::Checkpoint};
    SeedProvider heavy{"navigation-index", Participates::Save};
    cy::ecs::ComponentTypeId position = 0;
    cy::ecs::Entity entities[4] = {};

    [[nodiscard]] bool build() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        auto registered =
            world.components().register_builtin("Position", sizeof(Position), alignof(Position));
        if (!registered) {
            return false;
        }
        position = *registered;
        for (u32 index = 0; index < 4; ++index) {
            auto created = world.create();
            if (!created) {
                return false;
            }
            entities[index] = *created;
            if (!world.add(entities[index], position).has_value()) {
                return false;
            }
            if (!world.set(entities[index], position, Position{static_cast<cy::f32>(index), 0.0F})
                     .has_value()) {
                return false;
            }
        }
        rollback_seed.seed = 0xABCD'1234ULL;
        heavy.seed = 0x9999ULL;
        if (!providers.add(rollback_seed).has_value() || !providers.add(heavy).has_value()) {
            return false;
        }
        providers.finalize();
        return true;
    }

    [[nodiscard]] Position position_of(u32 index) const noexcept {
        Position value;
        const void* stored = world.get(entities[index], position);
        if (stored != nullptr) {
            std::memcpy(&value, stored, sizeof(Position));
        }
        return value;
    }
};

}  // namespace

CY_TEST_CASE("replay: the four kinds do not share an encoding, and rollback is not a save") {
    // "The save encoding SHALL NOT be used for rollback." Structural rather than advisory: a
    // rollback capture holds an in-memory snapshot and no byte stream, and a save capture holds a
    // stream and no snapshot.
    CY_CHECK(encoding_of(SnapshotKind::Rollback) == SnapshotEncoding::CurrentLayoutInMemory);
    CY_CHECK(encoding_of(SnapshotKind::ReplayCheckpoint) ==
             SnapshotEncoding::CurrentLayoutCompressed);
    CY_CHECK(encoding_of(SnapshotKind::SaveCheckpoint) == SnapshotEncoding::TaggedVersioned);
    CY_CHECK(encoding_of(SnapshotKind::DebugCapture) == SnapshotEncoding::Rich);

    Fixture fixture;
    CY_REQUIRE(fixture.build());

    StateCapture rollback(allocator(), SnapshotKind::Rollback);
    StateCapture save(allocator(), SnapshotKind::SaveCheckpoint);
    CY_REQUIRE(rollback.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{1}, 10})
                   .has_value());
    CY_REQUIRE(
        save.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{1}, 10}).has_value());

    CY_CHECK(rollback.holds_in_memory_snapshot());
    CY_CHECK_FALSE(rollback.holds_tagged_stream());
    CY_CHECK(save.holds_tagged_stream());
    CY_CHECK_FALSE(save.holds_in_memory_snapshot());

    // Participation is declared per provider and read per kind: the rollback capture takes the
    // random source and skips the navigation index, and the save capture does the opposite.
    CY_CHECK_EQ(rollback.providers_captured(), 1U);
    CY_CHECK_EQ(rollback.providers_declined(), 1U);
    CY_CHECK_EQ(save.providers_captured(), 1U);
    CY_CHECK_EQ(save.providers_declined(), 1U);

    // And a save is not restored through the rollback path: `ecs::deserialize` mints fresh
    // identifiers, and identity is part of the state hash.
    CY_CHECK_FALSE(save.restore(fixture.world, fixture.providers).has_value());
}

CY_TEST_CASE("replay: a restore resumes exactly — entities, identity and provider state") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());

    StateCapture capture(allocator(), SnapshotKind::Rollback);
    CY_REQUIRE(capture.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{1}, 10})
                   .has_value());
    CY_CHECK(capture.point() == (SimulationPoint{Epoch{1}, 10}));
    CY_CHECK_GT(capture.bytes(), u64{0});

    // Move the world on: values change, an entity is added, the random source advances.
    for (const cy::ecs::Entity entity : fixture.entities) {
        CY_REQUIRE(fixture.world.set(entity, fixture.position, Position{99.0F, 99.0F}).has_value());
    }
    auto extra = fixture.world.create();
    CY_REQUIRE(extra.has_value());
    CY_REQUIRE(fixture.world.add(*extra, fixture.position).has_value());
    fixture.rollback_seed.seed = 0xDEADULL;

    CY_REQUIRE(capture.restore(fixture.world, fixture.providers).has_value());

    for (u32 index = 0; index < 4; ++index) {
        // Restored by identity, not by equivalent content: the captured entity handles still name
        // the right rows.
        CY_CHECK_EQ(fixture.position_of(index).x, static_cast<cy::f32>(index));
    }
    CY_CHECK_EQ(fixture.rollback_seed.seed, u64{0xABCD'1234ULL});
    // The provider that declined rollback was not restored either — participation is declared, and
    // a restore that quietly put it back would make the declaration meaningless.
    CY_CHECK_EQ(fixture.heavy.seed, u64{0x9999ULL});
}

CY_TEST_CASE("replay: a checkpoint's provider half is compressed and expands back exactly") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());

    StateCapture checkpoint(allocator(), SnapshotKind::ReplayCheckpoint);
    CY_REQUIRE(checkpoint.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{2}, 7})
                   .has_value());
    fixture.rollback_seed.seed = 0;
    CY_REQUIRE(checkpoint.restore(fixture.world, fixture.providers).has_value());
    CY_CHECK_EQ(fixture.rollback_seed.seed, u64{0xABCD'1234ULL});
}

CY_TEST_CASE("replay: a capture refuses an unfinalised provider registry") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    StateProviderRegistry providers(allocator());
    SeedProvider seed{"random", Participates::Rollback};
    CY_REQUIRE(providers.add(seed).has_value());
    // Unfinalised: the order depends on when plugins loaded, and capturing in that order would make
    // load order part of the capture.
    StateCapture capture(allocator(), SnapshotKind::Rollback);
    CY_CHECK_FALSE(capture.capture(world, providers, SimulationPoint{Epoch{1}, 0}).has_value());
    CY_CHECK_FALSE(capture.captured());
}

CY_TEST_CASE("replay: the checkpoint policy is a policy, not a constant") {
    CheckpointPolicy policy;
    policy.min_tick_interval = 60;
    policy.max_tick_interval = 600;
    policy.command_volume_trigger = 1000;
    policy.expected_seek_ticks = 300;

    CheckpointState state;
    state.ticks_since_last = 59;
    state.commands_since_last = 100000;
    // The floor wins over everything: a command storm must not be able to ask for a checkpoint
    // every tick.
    CY_CHECK_FALSE(policy.due(state));

    state.ticks_since_last = 61;
    // ...and just past the floor, the command volume brings the checkpoint forward, because seeking
    // cost is the commands between checkpoints rather than the ticks.
    CY_CHECK(policy.due(state));

    state.commands_since_last = 0;
    state.ticks_since_last = 299;
    CY_CHECK_FALSE(policy.due(state));
    state.ticks_since_last = 300;
    CY_CHECK(policy.due(state));
    // Expected seeking behaviour moved the interval: a viewer who scrubs gets 300 rather than 600.
    CY_CHECK_EQ(policy.chosen_interval(state), 300U);

    // A storage budget already spent stretches the interval to the maximum rather than truncating
    // the session.
    CheckpointPolicy budgeted = policy;
    budgeted.storage_budget = 1024;
    CheckpointState spent = state;
    spent.last_capture_bytes = 512;
    spent.storage_used = 4096;
    CY_CHECK_EQ(budgeted.chosen_interval(spent), 600U);
}

CY_TEST_CASE("replay: the rollback window is a memory budget, and it reports what it dropped") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());

    StateCapture measure(allocator(), SnapshotKind::Rollback);
    CY_REQUIRE(measure.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{1}, 0})
                   .has_value());
    const u64 one_capture = measure.bytes();
    CY_REQUIRE(one_capture > u64{0});

    // Room for about three.
    SnapshotRing ring(allocator(), (one_capture * 3) + (one_capture / 2));
    for (u64 tick = 0; tick < 10; ++tick) {
        CY_REQUIRE(ring.capture(fixture.world, fixture.providers, SimulationPoint{Epoch{1}, tick})
                       .has_value());
    }
    CY_CHECK_LE(ring.bytes(), ring.budget());
    CY_CHECK_GT(ring.evictions(), 0U);
    CY_CHECK_GT(ring.size(), 0U);
    CY_CHECK_LE(ring.size(), 4U);

    WindowRefusal refusal = WindowRefusal::None;
    const StateCapture* recent = ring.find(SimulationPoint{Epoch{1}, 9}, refusal);
    CY_REQUIRE(recent != nullptr);
    CY_CHECK(refusal == WindowRefusal::None);
    CY_CHECK_EQ(recent->point().tick, u64{9});

    // "a request older than the window SHALL be reported as requiring full resynchronisation rather
    // than triggering an unbounded replay."
    CY_CHECK(ring.find(SimulationPoint{Epoch{1}, 0}, refusal) == nullptr);
    CY_CHECK(refusal == WindowRefusal::RequiresResynchronisation);

    SnapshotRing empty(allocator(), 1024);
    CY_CHECK(empty.find(SimulationPoint{Epoch{1}, 0}, refusal) == nullptr);
    CY_CHECK(refusal == WindowRefusal::Empty);
}
