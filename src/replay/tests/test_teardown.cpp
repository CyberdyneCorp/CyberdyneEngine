// M9 SECTION 1 — TEARDOWN UNDER LOAD.
//
// Every structure in this module is bounded and therefore evicts: the rollback ring drops captures
// when its memory budget is exceeded, the crash buffer overwrites when the ring wraps, and the
// ledger drops its oldest entry when capacity is reached. **Eviction is where a bounded structure
// leaks**, because the object that goes away is the one nobody is looking at any more, and a leak
// there is invisible in every test that does not exceed the bound.
//
// So this suite drives each of them past its bound, destroys everything, and asks the allocator
// what is left — blocks outstanding and bytes outstanding, both asserted to zero, with the totals
// checked as well so that a suite which allocated nothing cannot report the same zeroes.
//
// ================================================================================================
// WHY A LOCAL COUNTING ALLOCATOR AND NOT `cy::TrackingAllocator`
// ================================================================================================
//
// `TrackingAllocator` was the obvious instrument and it turned out not to be usable for this, for a
// reason worth writing down because it will catch the next person too. **It reports a false double
// free whenever the upstream allocator hands back an address it recently returned**, which is what
// an allocator does constantly. Measured on this tree at dd24144, with nothing leaked at all:
//
//     200 x { p = tracking.allocate(512, 8); tracking.deallocate(p, 512, 8); }
//     -> live=100  bytes=51200  double_frees=100  total=200
//
// Every second free is scored as a double free and its block is left on the live list, so the
// "leak" it reports is exactly half of a perfectly balanced workload. The signature is
// `live == double_frees`, and it appears for `cy::ecs::Snapshot` and for this module's own
// containers alike — which is how it was found: three different subjects reporting the same
// impossible symmetry. `src/core/memory/` is not this phase's to change; the finding is reported
// with the repro above rather than worked around in silence.
//
// The counting allocator below cannot make that mistake, because it counts rather than remembers:
// every `allocate` adds, every `deallocate` subtracts, and a balanced workload ends at zero
// whatever addresses the upstream chooses to reuse.

#include "fixture.h"

#include <cy/core/determinism/provider.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/snapshot.h>
#include <cy/ecs/world.h>
#include <cy/replay/external.h>
#include <cy/replay/ledger.h>
#include <cy/replay/readers.h>
#include <cy/replay/snapshot.h>

using namespace cy::replay_test;

namespace {

using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::determinism::Epoch;
using cy::determinism::Participates;
using cy::determinism::SimulationPoint;
using cy::determinism::StateProvider;
using cy::determinism::StateProviderRegistry;

constexpr u32 kEntities = 32;
constexpr u64 kCaptures = 12;

/// Counts blocks and bytes outstanding. Forwards everything; remembers nothing about addresses.
class CountingAllocator final : public cy::Allocator {
public:
    CountingAllocator() noexcept
        : cy::Allocator(cy::MemoryDomain::World, "replay-teardown"),
          upstream_(cy::system_allocator(cy::MemoryDomain::World)) {}

    [[nodiscard]] u64 live_blocks() const noexcept { return live_blocks_; }
    [[nodiscard]] u64 live_bytes() const noexcept { return live_bytes_; }
    [[nodiscard]] u64 total_blocks() const noexcept { return total_blocks_; }
    [[nodiscard]] u64 peak_bytes() const noexcept { return peak_bytes_; }
    /// Frees whose size did not match an outstanding total. A size mismatch is how a hand-placed
    /// object is returned with the wrong `sizeof`, which no counter would otherwise notice.
    [[nodiscard]] u64 oversized_frees() const noexcept { return oversized_frees_; }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override {
        void* block = upstream_.allocate(size, alignment);
        if (block != nullptr) {
            ++live_blocks_;
            ++total_blocks_;
            live_bytes_ += size;
            peak_bytes_ = live_bytes_ > peak_bytes_ ? live_bytes_ : peak_bytes_;
        }
        return block;
    }

    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override {
        void* block = upstream_.reallocate(pointer, old_size, new_size, alignment);
        if (block != nullptr) {
            live_bytes_ = live_bytes_ + new_size - old_size;
            ++total_blocks_;
        }
        return block;
    }

    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override {
        if (pointer != nullptr) {
            if (live_blocks_ == 0 || size > live_bytes_) {
                ++oversized_frees_;
            } else {
                --live_blocks_;
                live_bytes_ -= size;
            }
        }
        upstream_.deallocate(pointer, size, alignment);
    }

private:
    cy::Allocator& upstream_;
    u64 live_blocks_ = 0;
    u64 live_bytes_ = 0;
    u64 total_blocks_ = 0;
    u64 peak_bytes_ = 0;
    u64 oversized_frees_ = 0;
};

struct Position {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
};

/// A provider whose capture is large enough that the ring's budget bites after a few captures.
class BulkProvider final : public StateProvider {
public:
    [[nodiscard]] const char* name() const noexcept override { return "bulk"; }
    [[nodiscard]] Participates participation() const noexcept override {
        return Participates::Rollback | Participates::Checkpoint;
    }
    [[nodiscard]] cy::Status capture(cy::Array<u8>& out) const noexcept override {
        for (u32 index = 0; index < 4096; ++index) {
            if (cy::Status pushed = out.push_back(static_cast<u8>(index)); !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    }
    [[nodiscard]] cy::Status restore(cy::Span<const u8> bytes) noexcept override {
        return bytes.size() == 4096
                   ? cy::ok()
                   : cy::fail(cy::ErrorCode::InvalidArgument, "bulk provider: wrong size");
    }
};

cy::Status one_value(void* /*user*/, cy::Array<u8>& out) noexcept {
    return out.push_back(7);
}

[[nodiscard]] bool populate(cy::ecs::World& world, cy::ecs::ComponentTypeId& position) noexcept {
    if (!world.initialize().has_value()) {
        return false;
    }
    auto registered =
        world.components().register_builtin("Position", sizeof(Position), alignof(Position));
    if (!registered) {
        return false;
    }
    position = *registered;
    for (u32 index = 0; index < kEntities; ++index) {
        auto created = world.create();
        if (!created || !world.add(*created, position).has_value()) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("replay: the log, the crash ring, the ledger and the externals tear down clean") {
    CountingAllocator counting;
    {
        // THE CRASH RING, PAST ITS CAPACITY.
        CrashReplayBuffer crash(counting);
        CY_REQUIRE(crash.reserve(16).has_value());
        for (u64 tick = 0; tick < 200; ++tick) {
            crash.push(command_record(tick, 0, static_cast<cy::i32>(tick)));
        }
        CY_CHECK(crash.overwritten() > u64{0});
        cy::Array<LogRecord> flushed(counting);
        CY_REQUIRE(crash.flush(flushed).has_value());

        // THE LEDGER, PAST ITS CAPACITY, and then pruned by a window that has moved on.
        SideEffectLedger ledger(counting, 32);
        CY_REQUIRE(
            ledger.declare({1, "explosion", Speculation::Speculative, Reconciliation::Cancel})
                .has_value());
        for (u64 tick = 0; tick < 300; ++tick) {
            (void)ledger.realise(1, tick, SimulationPoint{Epoch{1}, tick}, false);
        }
        CY_CHECK(ledger.report().dropped_for_capacity > 0U);
        ledger.prune_before(290);

        // A LOG AND ITS EXTERNAL RESULTS, written and read back, so the byte arrays around the
        // encoder and the compressor are torn down too.
        RecordLog log(counting, manifest());
        ExternalResults external(counting, ExternalMode::Recording);
        CY_REQUIRE(external.declare({1, "service", ExternalKind::ServiceResponse}).has_value());
        cy::Array<u8> value(counting);
        for (u64 tick = 0; tick < 300; ++tick) {
            CY_REQUIRE(log.append(command_record(tick, 0, 1)).has_value());
            CY_REQUIRE(
                external
                    .consume(1, SimulationPoint{Epoch{1}, tick}, &one_value, nullptr, log, value)
                    .has_value());
        }
        cy::Array<u8> bytes(counting);
        CY_REQUIRE(write_log(log, bytes).has_value());
        RecordLog restored(counting, manifest());
        RejectReason why = RejectReason::Corrupt;
        CY_REQUIRE(read_log(bytes.span(), restored, why).has_value());
        ExternalResults replaying(counting, ExternalMode::Replaying);
        CY_REQUIRE(replaying.declare({1, "service", ExternalKind::ServiceResponse}).has_value());
        CY_REQUIRE(replaying.adopt(restored).has_value());
    }

    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK_EQ(counting.oversized_frees(), u64{0});
    // The allocator saw real traffic. Without this, a teardown suite that allocated nothing at all
    // would report the same zeroes and read identically to one that allocated and freed correctly.
    CY_CHECK(counting.total_blocks() > u64{40});
    CY_CHECK(counting.peak_bytes() > u64{10000});
}

CY_TEST_CASE(
    "replay: the rollback ring frees every capture it evicts and every one it still holds") {
    CountingAllocator counting;
    {
        cy::ecs::World world(counting);
        cy::ecs::ComponentTypeId position = 0;
        CY_REQUIRE(populate(world, position));

        StateProviderRegistry providers(counting);
        BulkProvider bulk;
        CY_REQUIRE(providers.add(bulk).has_value());
        providers.finalize();

        // PAST THE BUDGET. One capture is well over 4 KiB of provider state, so a 12 KiB budget
        // holds about three and the rest are evicted — each of them a hand-placed object that has
        // to be destroyed and returned, which is the arrangement that gets a destructor call wrong.
        SnapshotRing ring(counting, u64{12} * 1024);
        for (u64 tick = 0; tick < kCaptures; ++tick) {
            CY_REQUIRE(ring.capture(world, providers, SimulationPoint{Epoch{1}, tick}).has_value());
        }
        CY_CHECK(ring.evictions() > 0U);
        CY_CHECK(ring.size() > 0U);

        // Restoring from a point the window has evicted is a refusal, not a crash — exercised here
        // because a refusal path that freed the wrong thing would look like a pass everywhere else.
        WindowRefusal refusal = WindowRefusal::None;
        CY_CHECK(ring.find(SimulationPoint{Epoch{1}, 0}, refusal) == nullptr);
        CY_CHECK(refusal == WindowRefusal::RequiresResynchronisation);
    }

    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK_EQ(counting.oversized_frees(), u64{0});
    CY_CHECK(counting.total_blocks() > u64{40});
}
