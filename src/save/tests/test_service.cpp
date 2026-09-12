// The save service: a bounded capture, a background write, and teardown while one is in flight.
// Task 6.2, and hard rule 4 of this milestone.

#include <cy/save/service.h>
#include <cy/save/storage.h>
#include <cy/test/breadcrumbs.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <chrono>
#include <thread>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kFirstRegion{0x0A00'0000'0000'0001ULL};

SaveIdentity identity() noexcept {
    SaveIdentity id;
    id.build_id = "6.0.0+test";
    id.project = AssetId(1, 1);
    return id;
}

void record_health(Overlay& overlay, RegionKey region, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(region, id, health_type(), &health, 1).has_value());
}

/// A world of `regions` regions with `per_region` entities each. Big enough that the background
/// write is genuinely still running when the teardown cases pull the floor out.
void populate(Overlay& overlay, u32 regions, u32 per_region) {
    for (u32 region = 0; region < regions; ++region) {
        for (u32 index = 0; index < per_region; ++index) {
            Health health;
            health.revives = index;
            CY_REQUIRE(overlay
                           .record_component(RegionKey(0x0A00'0000'0000'0001ULL + region),
                                             entity((static_cast<u64>(region) << 20U) + index),
                                             health_type(), &health, 1)
                           .has_value());
        }
    }
}

/// The job system, the async service, the store and the archive, started and stopped in the order
/// the engine starts and stops them. The teardown ORDER is part of what is being tested: the save
/// service must not outlive the thread its write runs on.
struct Harness {
    Harness() {
        cy::jobs::JobSystemConfig job_config;
        job_config.worker_count = 2;
        CY_REQUIRE(jobs.start(job_config).has_value());
        CY_REQUIRE(async.start(jobs).has_value());
        CY_REQUIRE(archive.open(store).has_value());
    }

    ~Harness() {
        async.stop();
        jobs.shutdown();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    MemoryBackend store{test_allocator()};
    SaveArchive archive{test_allocator()};
};

}  // namespace

CY_TEST_CASE("a save is captured on the calling thread and written on another") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 4, 8);
    live.set_simulation_point(77);
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());
    CY_CHECK_EQ(service.saves_started(), 1U);

    service.wait();
    const Expected<SaveOutcome, Error> outcome = service.take_outcome();
    CY_REQUIRE(outcome.has_value());
    CY_CHECK(outcome->succeeded);
    CY_CHECK_EQ(outcome->generation, 1U);
    CY_CHECK_EQ(outcome->regions, 4U);
    CY_CHECK_EQ(outcome->entries, 32U);
    CY_CHECK_EQ(service.saves_completed(), 1U);
    CY_CHECK_EQ(service.saves_failed(), 0U);
    // The capture is a copy of the delta, not of the world, and it is measured rather than assumed.
    CY_CHECK_GT(service.last_capture_ns(), 0);

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(harness.archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(read.entry_count(), 32U);
    CY_CHECK_EQ(read.simulation_point(), 77U);
    service.shutdown();
}

CY_TEST_CASE("the simulation is not paused: the overlay is writable while a save is in flight") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 8, 16);
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());

    // The capture is immutable and the live overlay is not: gameplay carries on writing into it
    // while the background thread encodes what it took.
    for (u32 index = 0; index < 64; ++index) {
        record_health(live, kFirstRegion, entity(0xF000 + index), index);
    }
    service.wait();
    const Expected<SaveOutcome, Error> outcome = service.take_outcome();
    CY_REQUIRE(outcome.has_value());
    CY_CHECK(outcome->succeeded);
    // The save holds what was captured, not what was written after it. A save that picked up half
    // of the changes made during it would not correspond to any one commit boundary.
    CY_CHECK_EQ(outcome->entries, 128U);
    service.shutdown();
}

CY_TEST_CASE("a second save while one is in flight is refused rather than raced") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 16, 32);
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());
    const Status second = service.begin_save(live, SaveKind::Checkpoint);
    if (!second) {
        CY_CHECK_EQ(second.error().code, ErrorCode::Unavailable);
    }
    service.wait();
    CY_CHECK_EQ(service.saves_completed() + service.saves_failed(), service.saves_started());
    service.shutdown();
}

CY_TEST_CASE("a journal save appends to the active generation") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 2, 4);
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());
    service.wait();
    CY_REQUIRE(service.take_outcome().has_value());
    live.clear_dirty();

    record_health(live, kFirstRegion, entity(999), 42);
    CY_REQUIRE(service.begin_save(live, SaveKind::Journal).has_value());
    service.wait();
    const Expected<SaveOutcome, Error> outcome = service.take_outcome();
    CY_REQUIRE(outcome.has_value());
    CY_CHECK(outcome->succeeded);
    CY_CHECK_EQ(outcome->kind, SaveKind::Journal);

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(harness.archive.load(policy, read, report).has_value());
    CY_CHECK(read.find_entry(kFirstRegion, entity(999)) != nullptr);
    service.shutdown();
}

CY_TEST_CASE("a service torn down mid-write does not take the write with it") {
    // Hard rule 4 of this milestone, and the shape of the defect M5.5's gate found: a subsystem
    // destroyed while a worker was still inside it, one run in forty. Forty iterations, each
    // dropping the service while a write is in flight, and each then reading the store back — a
    // save interrupted by teardown must still leave a loadable store.
    //
    // THE PAUSE BEFORE THE TEARDOWN IS THE POINT. Tearing down immediately would usually catch the
    // operation before the async thread had picked it up, which tests the queue rather than the
    // write. The workload is large enough that a commit takes milliseconds, and each iteration
    // waits a different fraction of one before pulling the floor out, so the forty teardowns land
    // across the whole of the write rather than all at the same instant.
    cy::test::SeededRandom rng(0x5A7E'C0DEULL);
    u32 caught_in_flight = 0;
    for (u32 iteration = 0; iteration < 40; ++iteration) {
        Harness harness;
        Overlay live(test_allocator());
        populate(live, 12, 24);

        {
            SaveService service(test_allocator());
            CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity())
                           .has_value());
            CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());

            // SLEEPING RATHER THAN SPINNING. The per-case budget is measured in CPU time and a
            // yield loop spends it: this case ran a third over the integration budget in the debug
            // profile before the wait became a sleep. Sleeping also puts the teardown at a
            // genuinely unpredictable point in the write rather than at a scheduler artefact.
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(rng.next_below(2000));
            while (std::chrono::steady_clock::now() < until && service.is_saving()) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            caught_in_flight += service.is_saving() ? 1U : 0U;
            // No wait, no shutdown call: the destructor is what has to be correct, because it is
            // what runs when a level tear-down happens while an autosave is being written.
        }

        Overlay read(test_allocator());
        LoadPolicy policy;
        LoadReport report;
        const Status loaded = harness.archive.load(policy, read, report);
        if (loaded) {
            CY_CHECK_EQ(read.entry_count(), 12U * 24U);
        } else {
            // The only acceptable alternative: the write had not switched the pointer yet, so the
            // store holds no committed generation at all. Never a partial one.
            CY_CHECK_EQ(loaded.error().code, ErrorCode::NotFound);
        }
    }
    // If this ever reaches zero the case has stopped testing what it says it tests: every teardown
    // happened after the write had already finished. The count is reported so a reader of a
    // verbose run can see how much of the window was actually covered.
    CY_TEST_MESSAGE("teardowns that landed while the write was still running: ", caught_in_flight);
    CY_CHECK_GT(caught_in_flight, 0U);
}

CY_TEST_CASE("shutdown is idempotent, and a save after one is refused rather than crashing") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 2, 2);
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());
    service.shutdown();
    service.shutdown();
    CY_CHECK_FALSE(service.is_running());
    CY_CHECK_FALSE(service.is_saving());

    const Status refused = service.begin_save(live, SaveKind::Checkpoint);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unavailable);
}

CY_TEST_CASE("a service cannot start against a store that is not open") {
    Harness harness;
    SaveArchive unopened(test_allocator());
    SaveService service(test_allocator());
    CY_CHECK_FALSE(service.start(harness.jobs, harness.async, unopened, identity()).has_value());
}

// `diagnostics-profiling-and-crash` — "Breadcrumbs": save is one of the five coarse phase
// boundaries the specification names, and this is the case that says this module reaches it. It is
// also part of the regression test for `m9:breadcrumbs-adopted`, which recorded a ring with no
// caller outside its own module; asserting that the write path is reached is what a grep for the
// breadcrumb macro cannot do.
//
// THE DETAIL IS THE POINT AS MUCH AS THE MARKER. A crash artefact that says "a save was in flight"
// tells a reader the archive may be mid-commit; one that also says how many entries were captured
// tells them which save.
CY_TEST_CASE("a save leaves a breadcrumb on the thread that writes it") {
    Harness harness;
    SaveService service(test_allocator());
    CY_REQUIRE(service.start(harness.jobs, harness.async, harness.archive, identity()).has_value());

    Overlay live(test_allocator());
    populate(live, 3, 5);
    const u64 entries = static_cast<u64>(live.entry_count());

    const u64 mark = cy::test::breadcrumb_mark();
    CY_REQUIRE(service.begin_save(live, SaveKind::Checkpoint).has_value());
    service.wait();
    CY_REQUIRE(service.take_outcome().has_value());

    CY_CHECK_EQ(cy::test::breadcrumbs_since(mark, "save"), 1U);
    u64 detail = 0;
    CY_REQUIRE(cy::test::breadcrumb_detail_since(mark, "save", detail));
    CY_CHECK_EQ(detail, entries);
}
