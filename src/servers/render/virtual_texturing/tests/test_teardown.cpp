// Teardown mid-production, and the concurrent half of "feedback never blocks a frame". M6 tasks 5.3
// and 5.4, and the milestone's hard rule about testing teardown under load.
//
// --- WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
//
// M5.5's gate found this project's first real engine defect by tearing a subsystem down while it
// was busy: Jolt's job bridge destroyed its free list underneath a worker that was still releasing
// a job — one run in forty, a SIGTRAP with no physics call on the stack. `VirtualTextureSystem` has
// exactly that shape. Its production workers write into staging bytes the physical caches own, so a
// destruction order that freed a cache before joining a worker would be the same defect with
// different types.
//
// The structural answer is in system.h: `pool_` is the LAST member, so it is the FIRST destroyed,
// and every worker is joined before a cache, a page table or a staging buffer is touched. This file
// is what would notice if that line moved.
//
// --- THE ONE ORDERING RULE A READER MUST KEEP
// ---------------------------------------------------------
//
// In every case below the PRODUCER is declared before the SYSTEM. The producer registry holds a
// non-owning pointer — a producer outlives the pages it makes and belongs to whatever configured it
// — so the system must unwind first. Reversing those two lines is a use-after-free that the pool's
// own join cannot help with, because by then the producer is what has gone.

#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/test/test.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace cy::render::vt;
using cy::u32;
using cy::u64;

namespace {

constexpr u32 kBytesPerTile = 1024;

/// Does enough work per page that a teardown lands in the middle of one.
class BusyProducer final : public PageProducer {
public:
    [[nodiscard]] const char* producer_name() const noexcept override { return "test-busy"; }
    [[nodiscard]] cy::f32 declared_cost_ms() const noexcept override { return 0.5F; }

    cy::Status produce(const ProductionRequest& request) noexcept override {
        u32 sum = 0;
        for (u32 index = 0; index < request.bytes; ++index) {
            request.destination[index] = static_cast<cy::u8>(index + request.address.mip);
            sum += request.destination[index];
        }
        checksum_.fetch_add(sum, std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        return cy::ok();
    }

    [[nodiscard]] u64 calls() const noexcept { return calls_.load(std::memory_order_relaxed); }

private:
    std::atomic<u64> calls_{0};
    std::atomic<u64> checksum_{0};
};

[[nodiscard]] VirtualTextureDesc terrain(u32 id = 1) noexcept {
    VirtualTextureDesc desc;
    desc.id = id;
    desc.width = 4096;
    desc.height = 4096;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 6;
    desc.layers = 1;
    desc.mip_tail_levels = 2;
    desc.semantic = TextureSemantic::Colour;
    desc.model = ResidencyModel::VirtualRuntime;
    desc.bytes_per_tile = kBytesPerTile;
    return desc;
}

[[nodiscard]] TileCacheDesc cache_desc(u32 slots) noexcept {
    TileCacheDesc desc;
    desc.format = FormatClass::BlockColour;
    desc.tile_size = 128;
    desc.border = 4;
    desc.bytes_per_tile = kBytesPerTile;
    desc.tile_capacity = slots;
    return desc;
}

[[nodiscard]] cy::residency::SubsystemPolicy residency_policy() noexcept {
    cy::residency::SubsystemPolicy policy;
    policy.domain = cy::MemoryDomain::Gpu;
    policy.budget_bytes = 0;  // unbudgeted: the CACHE is what runs out here, not the policy
    policy.min_residency_frames = 0;
    return policy;
}

/// Spin until the producer has run at least once more than `before`, so that a destruction which
/// follows lands with a job genuinely in flight rather than on a queue nothing has touched.
///
/// A SPIN AND NOT A SLEEP: the wait is for another thread to make progress, and the bound exists
/// only so that a broken pool fails the case instead of hanging the suite. A `yield` rather than a
/// busy loop, because eight of these can run on a four-core agent.
void wait_for_production(const BusyProducer& producer, u64 before) noexcept {
    for (u64 spin = 0; spin < 100'000'000ULL; ++spin) {
        if (producer.calls() > before) {
            return;
        }
        std::this_thread::yield();
    }
}

/// Ask for `count` mip 0 pages and hand the schedule to the system, dispatching production.
void dispatch(VirtualTextureSystem& system, cy::residency::ResidencyServer& server, u32 count) {
    for (u32 index = 0; index < count; ++index) {
        cy::residency::Request request;
        VirtualAddress address;
        address.texture = 1;
        address.mip = 0;
        address.tile_x = static_cast<cy::u16>(index % 32);
        address.tile_y = static_cast<cy::u16>(index / 32);
        request.key = page_key(address.encode());
        request.bytes = kBytesPerTile;
        request.inputs.importance = 1.0F;
        if (!server.request(request)) {
            return;
        }
    }
    cy::residency::Schedule frame;
    if (!server.schedule(cy::residency::ScheduleOptions{0.0, 0}, frame)) {
        return;
    }
    // Deliberately NOT checked: the cache runs out part way through, which is the state a teardown
    // should also survive.
    static_cast<void>(system.apply(frame, server, 0.0));
}

}  // namespace

CY_TEST_CASE("a system is destroyed mid-production, a hundred times over") {
    // The producer outlives every system that points at it. See the note at the top of this file.
    BusyProducer producer;

    for (u32 cycle = 0; cycle < 100; ++cycle) {
        cy::residency::ResidencyServer server;
        CY_REQUIRE(
            server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy()));

        VirtualTextureSystem system;
        CY_REQUIRE(system.configure_cache(cache_desc(128)));
        CY_REQUIRE(system.start_production(4));
        CY_REQUIRE(system.register_texture(terrain()));
        CY_REQUIRE(system.producers().register_producer(1, producer));
        CY_REQUIRE(system.make_mip_tail_resident(1));

        const u64 before = producer.calls();
        dispatch(system, server, 96);
        wait_for_production(producer, before);

        // No quiesce, no drain, no shutdown call. The system is destroyed with ninety-odd jobs
        // still queued and at least one worker inside `produce`, and the member order is what makes
        // that survivable.
    }

    CY_CHECK_GT(producer.calls(), 0U);
}

CY_TEST_CASE("a texture is unregistered while its own pages are being produced") {
    BusyProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy()));

    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(128)));
    CY_REQUIRE(system.start_production(4));

    for (u32 cycle = 0; cycle < 50; ++cycle) {
        CY_REQUIRE(system.register_texture(terrain()));
        CY_REQUIRE(system.producers().register_producer(1, producer));
        CY_REQUIRE(system.make_mip_tail_resident(1));
        const u64 before = producer.calls();
        dispatch(system, server, 96);
        wait_for_production(producer, before);

        // `unregister_texture` quiesces the pool before releasing the tiles a running job holds a
        // pointer into. Without that, this loop is a use-after-free with a low reproduction rate.
        CY_CHECK(system.unregister_texture(1));
        CY_CHECK_EQ(system.texture_count(), 0U);
        server.reset();
    }
    CY_CHECK_GT(producer.calls(), 0U);
}

CY_TEST_CASE("reset runs while production is in flight, repeatedly") {
    BusyProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy()));

    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(128)));
    CY_REQUIRE(system.start_production(4));

    for (u32 cycle = 0; cycle < 50; ++cycle) {
        CY_REQUIRE(system.register_texture(terrain()));
        CY_REQUIRE(system.producers().register_producer(1, producer));
        CY_REQUIRE(system.make_mip_tail_resident(1));
        const u64 before = producer.calls();
        dispatch(system, server, 96);
        wait_for_production(producer, before);
        system.reset();
        server.reset();
    }
    CY_CHECK_EQ(system.texture_count(), 0U);
    CY_CHECK(system.cache(FormatClass::BlockColour).configured());
}

CY_TEST_CASE("feedback never blocks a frame: writers progress while the resolver churns") {
    // THE CONCURRENT HALF OF THE EXIT CRITERION, and a note on what it can honestly assert.
    //
    // `record` is wait-free — two atomic increments, a bounds test and a store, with no lock, no
    // allocation and no retry loop — and that is a property of the CODE rather than of a run. Two
    // things this case deliberately does not gate on, both measured and both withdrawn:
    //
    //   * A ceiling on the worst single `record`. The measurement is wall clock around a call that
    //     can be descheduled, so on a loaded agent it reports the operating system: it read 96 ms
    //     and 121 ms under twenty parallel repeats, and 5 to 9 microseconds when the machine was
    //     quiet. It is reported below rather than asserted.
    //   * A throughput ratio against the resolver's rounds. The loop stops when the writers reach
    //     their target, so the ratio is an artefact of the stopping rule and not of the design.
    //
    // What this case does is run the concurrent path under continuous contention and assert the two
    // things that are true regardless of scheduling: the writers make real progress while the
    // resolver churns, and NO SAMPLE IS LOST between the banks — every `record` is counted exactly
    // once, as recorded or as dropped, which is what would fail if a writer ever landed in a bank
    // the resolver had already begun reading. Running it under ThreadSanitizer is the other half.
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(1024));

    constexpr u32 kWriters = 4;
    /// How often a writer publishes its progress. Every iteration would put the writers on one
    /// cache line and make the contention this case measures its own doing; never would leave the
    /// main thread unable to tell whether the writers had started at all.
    constexpr u64 kPublishEvery = 256;
    constexpr u64 kWantedRecords = 10'000;
    constexpr u32 kMinRounds = 200;
    constexpr u32 kMaxRounds = 4'000;

    std::atomic<bool> stop{false};
    std::atomic<u64> completed{0};
    std::atomic<u64> worst_nanoseconds{0};
    std::thread writers[kWriters];

    for (u32 index = 0; index < kWriters; ++index) {
        writers[index] = std::thread([&feedback, &stop, &completed, &worst_nanoseconds, index]() {
            VirtualAddress address;
            address.texture = index;
            u32 tile = 0;
            u64 local_worst = 0;
            u64 local_completed = 0;
            while (!stop.load(std::memory_order_acquire)) {
                address.tile_x = static_cast<cy::u16>(tile++ % 512);
                const auto started = std::chrono::steady_clock::now();
                feedback.record(address.encode());
                const auto elapsed =
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
                local_worst = (elapsed > local_worst) ? elapsed : local_worst;
                ++local_completed;
                if ((local_completed % kPublishEvery) == 0) {
                    completed.fetch_add(kPublishEvery, std::memory_order_relaxed);
                }
            }
            completed.fetch_add(local_completed % kPublishEvery, std::memory_order_relaxed);
            u64 published = worst_nanoseconds.load(std::memory_order_relaxed);
            while (local_worst > published &&
                   !worst_nanoseconds.compare_exchange_weak(published, local_worst,
                                                            std::memory_order_relaxed)) {
                // compare_exchange_weak refreshes `published` on failure; the loop retries.
            }
        });
    }

    // At least `kMinRounds`, and not before the writers have got somewhere: the main thread is fast
    // enough to finish its own loop before a writer has been scheduled, and a case that stopped
    // there would measure nothing while reporting success. `kMaxRounds` bounds the case's own CPU,
    // which the integration suite budgets at one second.
    cy::Array<FeedbackRequest> requests;
    u32 rounds = 0;
    while (rounds < kMinRounds ||
           (completed.load(std::memory_order_relaxed) < kWantedRecords && rounds < kMaxRounds)) {
        feedback.swap();
        CY_REQUIRE(feedback.resolve(requests));
        ++rounds;
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
        writer.join();
    }

    const u64 records = completed.load(std::memory_order_relaxed);
    CY_CHECK_GE(records, kWantedRecords);
    // Every sample was either recorded or dropped: none was lost between the two counters, and none
    // was written into a bank the resolver had already started reading.
    CY_CHECK_EQ(feedback.recorded() + feedback.dropped(), records);

    CY_TEST_MESSAGE("records ", records, " over ", rounds, " resolver rounds; worst record ",
                    worst_nanoseconds.load(std::memory_order_relaxed), " ns; ",
                    feedback.drain_spins(), " drain spins");
}

CY_TEST_CASE("a production pool is stopped and restarted while jobs are queued") {
    BusyProducer producer;
    cy::residency::ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(cy::residency::Subsystem::Texture, residency_policy()));

    VirtualTextureSystem system;
    CY_REQUIRE(system.configure_cache(cache_desc(128)));
    CY_REQUIRE(system.register_texture(terrain()));
    CY_REQUIRE(system.producers().register_producer(1, producer));
    CY_REQUIRE(system.make_mip_tail_resident(1));

    for (u32 cycle = 0; cycle < 20; ++cycle) {
        // `start_production` stops and joins the previous pool before starting a new one, so this
        // is a teardown with work outstanding on every iteration.
        CY_REQUIRE(system.start_production(1 + (cycle % 4)));
        dispatch(system, server, 32);
        server.reset();
    }
    CY_CHECK_GT(system.production_workers(), 0U);
}
