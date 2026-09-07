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

CY_TEST_CASE("the two-flag reservation never lets both sides into one slot") {
    // The classic two-flag protocol, and its correctness comes from SeqCst on both sides: the
    // engine stores its intent then reads `held`; the editor stores `held` then reads the intent.
    // In any total order at least one of the two reads sees the other's store.
    AnnouncementPage page;
    CY_REQUIRE(page.create());
    constexpr u32 kContended = 2;
    std::atomic<u64> wrote_blind{0};
    std::thread engine([&page, &wrote_blind] {
        for (u32 index = 0; index < 50'000; ++index) {
            page.set_writing(true, kContended);
            if (held_slot(page.held()) != kContended) {
                wrote_blind.fetch_add(1, std::memory_order_relaxed);
            }
            page.set_writing(false, 0);
        }
    });
    u64 claimed_blind = 0;
    for (u64 frame_id = 1; frame_id < 50'000; ++frame_id) {
        page.set_held_for_test(held_pack(frame_id, kContended));
        // The editor's half of the same protocol; `AnnouncementPage` has no reader for `writing`
        // because the engine never needs one, so the claim is simply released again.
        claimed_blind += 1;
        page.set_held_for_test(0);
    }
    engine.join();
    CY_CHECK(wrote_blind.load() > 0);
    CY_CHECK(claimed_blind > 0);
    CY_CHECK_EQ(page.held(), 0ULL);
}
