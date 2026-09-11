// M9 SECTION 4 — TEARDOWN UNDER LOAD.
//
// Every structure in this module is bounded and therefore evicts: the reliability channel compacts
// its arenas when the head advances, the local network's in-flight list and inboxes compact when
// they drain, the profiler's ring drops ticks older than its window, the prediction ledger trims to
// its window, the proxy history wraps, and a peer's baseline recycles the entries of an entity it
// forgot. **Eviction is where a bounded structure leaks**, because the object that goes away is the
// one nobody is looking at any more, and a leak there is invisible in every test that does not
// exceed the bound.
//
// So this suite drives each of them PAST its bound, destroys everything, and asks the allocator
// what is left — blocks outstanding and bytes outstanding, both asserted to zero, with the totals
// checked as well so that a suite which allocated nothing cannot report the same zeroes.
//
// ================================================================================================
// WHY A LOCAL COUNTING ALLOCATOR AND NOT `cy::TrackingAllocator`
// ================================================================================================
//
// The same reason `src/replay/tests/test_teardown.cpp` gives, and the finding is its own:
// `TrackingAllocator` reports a false double free whenever the upstream allocator hands back an
// address it recently returned, which is what an allocator does constantly. The counting allocator
// below cannot make that mistake, because it counts rather than remembers.

#include "fixture.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/networking/interest.h>
#include <cy/networking/local_transport.h>
#include <cy/networking/prediction.h>
#include <cy/networking/profiler.h>
#include <cy/networking/replication.h>
#include <cy/networking/scheduler.h>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

/// Counts blocks and bytes outstanding. Forwards everything; remembers nothing about addresses.
class CountingAllocator final : public cy::Allocator {
public:
    CountingAllocator() noexcept
        : cy::Allocator(cy::MemoryDomain::World, "networking-teardown"),
          upstream_(cy::system_allocator(cy::MemoryDomain::World)) {}

    [[nodiscard]] u64 live_blocks() const noexcept { return live_blocks_; }
    [[nodiscard]] u64 live_bytes() const noexcept { return live_bytes_; }
    [[nodiscard]] u64 total_blocks() const noexcept { return total_blocks_; }
    [[nodiscard]] u64 peak_bytes() const noexcept { return peak_bytes_; }
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

constexpr u8 kPayload[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

}  // namespace

CY_TEST_CASE("networking teardown: the transports, past every window") {
    CountingAllocator counting;
    {
        LocalNetwork network(counting, 0x7EED'0001ULL);
        NetworkConditions adverse;
        adverse.latency_ms = 60;
        adverse.jitter_ms = 20;
        adverse.loss_percent = 20;
        adverse.duplication_percent = 10;
        adverse.reorder_percent = 10;
        network.set_conditions(adverse);

        const PeerId server_id = network.add_host("server").value();
        const PeerId client_id = network.add_host("client").value();
        LocalTransport server(counting, network, server_id);
        LocalTransport client(counting, network, client_id);
        CY_REQUIRE(client.connect("server").has_value());
        CY_REQUIRE(server.accept(client_id).has_value());

        // Two thousand ticks over five channels at 20 % loss: the retransmission queues fill and
        // drain repeatedly, the reorder buffers hold gaps, the in-flight list wraps, and both
        // inboxes are compacted hundreds of times.
        for (u64 tick = 0; tick < 2000; ++tick) {
            for (ChannelId channel = 0; channel < 5; ++channel) {
                const DeliveryMode delivery =
                    channel % 2 == 0 ? DeliveryMode::Unreliable : DeliveryMode::ReliableOrdered;
                (void)client.send(server_id, channel, delivery, cy::Span<const u8>(kPayload, 16));
                (void)server.send(client_id, channel, delivery, cy::Span<const u8>(kPayload, 16));
            }
            client.advance(tick * 16);
            server.advance(tick * 16);
            Datagram datagram;
            while (server.receive(datagram)) {
            }
            while (client.receive(datagram)) {
            }
        }
        CY_CHECK_GT(network.dropped(), u64{0});
        CY_CHECK_GT(network.duplicated(), u64{0});
        CY_CHECK_GT(server.stats(client_id).retransmissions, u64{0});
    }

    CY_CHECK_GT(counting.total_blocks(), u64{0});
    CY_CHECK_GT(counting.peak_bytes(), u64{0});
    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK_EQ(counting.oversized_frees(), u64{0});
}

CY_TEST_CASE("networking teardown: the baseline, the interest set and the scheduler") {
    CountingAllocator counting;
    {
        SchemaSet schemas(counting);
        CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
        PeerBaseline baseline(counting, schemas);
        SnapshotWriter writer(counting, schemas);
        InterestSet interest(counting);
        PriorityScheduler scheduler(counting);
        NetworkIdMinter minter(1);

        const PeerId peer = PeerId::make(1, 1);
        cy::Array<NetworkId> living(counting);

        // Spawn eight hundred, forget six hundred, spawn eight hundred more. The baseline's entry
        // recycling and the interest set's cell buckets both go round the loop several times.
        for (u32 round = 0; round < 3; ++round) {
            for (u32 index = 0; index < 800; ++index) {
                const NetworkId id = minter.mint();
                RelevanceSubject subject;
                subject.id = id;
                subject.cell = index % 32;
                subject.position_x = static_cast<cy::i64>(index % 100);
                CY_REQUIRE(interest.place(subject).has_value());
                CY_REQUIRE(living.push_back(id).has_value());

                SampleTransform value;
                value.health = static_cast<cy::u16>(index % 500);
                CY_REQUIRE(writer.open((round * 800) + index + 1, index, false).has_value());
                CY_REQUIRE(writer.add_update(id, 0, &value, true, baseline).has_value());
                CY_REQUIRE(writer.close().has_value());
                CY_REQUIRE(baseline.note_known(id).has_value());
            }
            baseline.acknowledge((round * 800) + 400);
            for (u32 index = 0; index < 600 && !living.empty(); ++index) {
                const NetworkId id = living[living.size() - 1];
                living.pop_back();
                baseline.forget(id);
                interest.remove(id);
            }
        }

        cy::Array<CellId> cells(counting);
        for (u64 cell = 0; cell < 32; ++cell) {
            CY_REQUIRE(cells.push_back(cell).has_value());
        }
        CY_REQUIRE(interest.set_interest_cells(peer, cells.span()).has_value());

        PeerInterest view;
        view.peer = peer;
        view.relevance_distance_squared = 1000LL * 1000LL;
        cy::Array<Candidate> candidates(counting);
        cy::Array<NetworkId> left(counting);
        cy::Array<ScheduledEntry> scheduled(counting);
        BandwidthBudget budget;
        budget.bytes_per_tick = 1024;
        for (u64 tick = 1; tick <= 50; ++tick) {
            candidates.clear();
            left.clear();
            scheduled.clear();
            CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
            CY_REQUIRE(scheduler
                           .select(
                               peer, candidates.span(), tick, budget,
                               [](void*, NetworkId, u32) noexcept { return 30U; }, nullptr,
                               scheduled)
                           .has_value());
            for (auto& entry : scheduled) {
                CY_REQUIRE(scheduler.note_sent(peer, entry.id, tick).has_value());
            }
        }
    }

    CY_CHECK_GT(counting.total_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK_EQ(counting.oversized_frees(), u64{0});
}

CY_TEST_CASE("networking teardown: the profiler, the prediction window and the proxy ring") {
    CountingAllocator counting;
    {
        NetworkProfiler profiler(counting);
        PredictionLedger ledger(counting);
        ReconciliationPolicy policy;
        policy.window_ticks = 32;
        ledger.set_policy(policy);
        ProxyHistory history(counting, /*window_ticks=*/24);
        InputBuffer inputs(counting);
        NetworkIdMinter minter(1);

        // Ten times the profiler's window, and four times the prediction window. Both must be
        // evicting for most of it, or the case is not driving the path it is about.
        for (u64 tick = 1; tick <= u64{kProfilerTicks} * 10; ++tick) {
            for (u32 index = 0; index < 8; ++index) {
                TrafficRecord record;
                record.tick = tick;
                record.peer = PeerId::make(index % 4, 1);
                record.entity = NetworkId::make(1, index);
                record.schema = 0;
                record.bytes = 30 + index;
                record.rule = RelevanceRule::CellMembership;
                CY_REQUIRE(profiler.note_traffic(record).has_value());
            }
            CY_REQUIRE(ledger.predict(tick, 0x1000 + tick).has_value());

            const CollisionProxy proxies[3] = {
                {minter.mint(), static_cast<cy::i64>(tick), 0, 0, 50},
                {minter.mint(), 0, static_cast<cy::i64>(tick), 0, 50},
                {minter.mint(), 0, 0, static_cast<cy::i64>(tick), 50},
            };
            CY_REQUIRE(
                history.record(tick, cy::Span<const CollisionProxy>(proxies, 3)).has_value());

            cy::replay::LogRecord command;
            command.kind = cy::replay::RecordKind::Command;
            command.tick = tick;
            CY_REQUIRE(inputs.retain(command).has_value());
            if (tick % 7 == 0) {
                (void)inputs.acknowledge(tick - 3);
            }

            CorrectionExplanation correction;
            correction.found = true;
            correction.noticed_tick = tick;
            correction.magnitude = tick;
            if (tick % 11 == 0) {
                CY_REQUIRE(profiler.note_correction(correction).has_value());
            }
        }

        CY_CHECK_GT(profiler.evicted(), u64{0});
        CY_CHECK_LE(profiler.retained(), (kProfilerTicks * 8) + 8);
        CY_CHECK_LE(ledger.retained(), 33U);
        CY_CHECK_GT(history.recorded_ticks(), u64{24});
    }

    CY_CHECK_GT(counting.total_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_blocks(), u64{0});
    CY_CHECK_EQ(counting.live_bytes(), u64{0});
    CY_CHECK_EQ(counting.oversized_frees(), u64{0});
}
