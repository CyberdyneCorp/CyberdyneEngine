// The viewport transport's two concurrent protocols, under contention. M7 task 5b.1.
//
// SEPARATE FROM `unit.viewport_publisher` BECAUSE OF WHAT THEY COST, not because of what they are.
// Both cases below run two threads against a shared page for tens of thousands of iterations, which
// is milliseconds rather than microseconds — and `testing-and-quality`'s taxonomy puts a test that
// expensive in the next suite up. Making them cheap enough for the unit budget would mean running
// them for too few iterations to hit the races they exist to find, which is the worse trade: the
// editor's own spike reproduced real corruption here, at rates of a few per cent.
//
// What each is for is in the case, and in `src/wire.h` beside the words they protect.

#include <cy/backends/viewport/publisher.h>
#include <cy/core/base/types.h>
#include <cy/test/test.h>

#include "wire.h"

#include <atomic>
#include <thread>

using cy::u32;
using cy::u64;
using namespace cy::viewport;

namespace {

[[nodiscard]] Announcement announcement(u64 frame_id, u32 slot) {
    Announcement frame;
    frame.frame_id = frame_id;
    frame.slot = slot;
    frame.generation = 1;
    frame.timeline_value = frame_id;
    frame.submitted_nanos = frame_id * 1'000'000ULL;
    return frame;
}

}  // namespace

CY_TEST_CASE("a concurrent writer never hands a reader half a frame") {
    // The property the seqlock exists for. A reader that saw half of one announcement and half of
    // another would produce a slot from one frame and a timeline value from another — a wait on a
    // value that will not be reached for that slot, which is the one failure nothing recovers from.
    AnnouncementPage page;
    CY_REQUIRE(page.create());
    std::atomic<bool> finished{false};
    std::thread writer([&page, &finished] {
        for (u64 frame_id = 1; frame_id < 200'000; ++frame_id) {
            page.publish(announcement(frame_id, static_cast<u32>(frame_id % 3)));
        }
        finished.store(true, std::memory_order_release);
    });

    u64 seen = 0;
    u64 torn = 0;
    while (!finished.load(std::memory_order_acquire) || seen == 0) {
        Announcement read;
        if (page.read(read)) {
            if (read.slot != static_cast<u32>(read.frame_id % 3) ||
                read.timeline_value != read.frame_id) {
                torn += 1;
            }
            seen = (read.frame_id > seen) ? read.frame_id : seen;
        }
    }
    writer.join();
    CY_CHECK(seen > 0);
    CY_CHECK_EQ(torn, 0ULL);
}

namespace {

constexpr u32 kContended = 2;

/// Who is inside the contended slot right now, and whether both ever were at once.
struct Occupancy {
    std::atomic<u32> inside{0};
    std::atomic<u64> both_inside{0};

    void enter() noexcept {
        if (inside.fetch_add(1, std::memory_order_seq_cst) != 0) {
            both_inside.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void leave() noexcept { inside.fetch_sub(1, std::memory_order_seq_cst); }
};

/// The engine's half of the protocol, once: declare the slot, then look at the claim. True when it
/// found the slot free and wrote.
bool engine_reserves(AnnouncementPage& page, Occupancy& occupancy) noexcept {
    page.set_writing(true, kContended);
    const bool free = held_slot(page.held()) != kContended;
    if (free) {
        occupancy.enter();
        occupancy.leave();
    }
    page.set_writing(false, 0);
    return free;
}

/// The editor's half, once: claim the slot, then look at the engine's intent. True when it found
/// the engine elsewhere and read. `writing` holds the slot plus one.
bool editor_claims(AnnouncementPage& page, u64 frame_id, Occupancy& occupancy) noexcept {
    page.set_held_for_test(held_pack(frame_id, kContended));
    const bool free = page.writing_for_test() != kContended + 1U;
    if (free) {
        occupancy.enter();
        occupancy.leave();
    }
    page.set_held_for_test(0);
    return free;
}

}  // namespace

CY_TEST_CASE("the two-flag reservation never lets both sides into one slot") {
    // The classic two-flag protocol, and its correctness comes from SeqCst on both sides: the
    // engine stores its intent then reads `held`; the editor stores `held` then reads the intent.
    // In any total order at least one of the two reads sees the other's store, so `both_inside`
    // stays zero however the two threads interleave.
    //
    // WHAT IS COUNTED IS DECIDED BY THE PROTOCOL, NOT BY THE SCHEDULER. This case used to assert
    // that the engine found the slot free at least once in 50 000 racing tries, which depends on
    // when the scheduler runs each thread: the hosted macOS runner ran the engine's whole loop
    // while the editor held the slot and failed `wrote_blind > 0` with nothing wrong. Now both
    // threads start together, race for as long as the editor claims, and each side then makes one
    // more attempt after the other has finished — which the protocol says must succeed.
    AnnouncementPage page;
    CY_REQUIRE(page.create());
    Occupancy occupancy;
    std::atomic<bool> go{false};
    std::atomic<bool> editor_done{false};
    u64 wrote = 0;
    std::thread engine([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        while (!editor_done.load(std::memory_order_acquire)) {
            wrote += engine_reserves(page, occupancy) ? 1U : 0U;
        }
    });
    go.store(true, std::memory_order_release);
    u64 claimed = 0;
    for (u64 frame_id = 1; frame_id < 50'000; ++frame_id) {
        claimed += editor_claims(page, frame_id, occupancy) ? 1U : 0U;
    }
    editor_done.store(true, std::memory_order_release);
    engine.join();

    // Each side alone: nothing contends, so each must get in.
    CY_CHECK(engine_reserves(page, occupancy));
    CY_CHECK(editor_claims(page, 50'000, occupancy));
    CY_CHECK_EQ(occupancy.both_inside.load(), 0ULL);
    CY_TEST_MESSAGE("raced: the engine wrote " << wrote << " times, the editor read " << claimed
                                               << " times");
    CY_CHECK_EQ(page.held(), 0ULL);
}

CY_TEST_CASE("an engine that only ever found the slot held still writes once it is released") {
    // The schedule the hosted macOS runner produced, made deterministic: the editor holds the slot
    // for every one of the engine's attempts. None may write; the first after the release must.
    AnnouncementPage page;
    CY_REQUIRE(page.create());
    Occupancy occupancy;
    page.set_held_for_test(held_pack(1, kContended));
    u64 wrote = 0;
    for (u32 attempt = 0; attempt < 1'000; ++attempt) {
        wrote += engine_reserves(page, occupancy) ? 1U : 0U;
    }
    CY_CHECK_EQ(wrote, 0ULL);
    page.set_held_for_test(0);
    CY_CHECK(engine_reserves(page, occupancy));
    CY_CHECK_EQ(occupancy.both_inside.load(), 0ULL);
}
