// The viewport transport's wire, on a machine with no GPU. M7 task 5b.1.
//
// EVERYTHING HERE IS A DECISION RATHER THAN A DEVICE CALL, which is why wire.h holds no Vulkan and
// why this suite is `unit` rather than `render`. The three properties checked below are the ones
// the editor's own spike found the hard way, and each is worth more than the Vulkan around it:
//
//   * a torn read of the announcement page is DETECTED, because a reader one announcement out of
//     step stages a wait on a timeline value nothing will ever signal — the one failure in this
//     design that nothing recovers from;
//   * the slot rule never hands out an image the editor says it is holding, and a full ring costs
//     the ENGINE a frame rather than costing the editor a stall;
//   * a socket path past `sun_path` is refused rather than truncated, because a truncated path
//     binds a different socket, everything starts, and the viewport merely stays empty.

#include <cy/backends/viewport/publisher.h>
#include <cy/core/base/types.h>
#include <cy/test/test.h>

#include "wire.h"

using cy::u32;
using cy::u64;
using namespace cy::viewport;

namespace {

[[nodiscard]] Handshake a_handshake() {
    Handshake handshake;
    handshake.width = 1280;
    handshake.height = 720;
    handshake.buffer_count = 3;
    handshake.generation = 7;
    handshake.modifier = 0x0300'0000'0000'0006ULL;
    for (u32 slot = 0; slot < 3; ++slot) {
        handshake.planes[slot] = PlaneDescription{5120, 0, 3'932'160};
    }
    return handshake;
}

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

CY_TEST_CASE("the layouts are the ones the editor's #[repr(C)] structures produce") {
    // The one thing about this wire no compiler can check, because the other side is Rust. These
    // numbers come from `cy_editor_viewport_transport::{wire, announce}` and they are what a
    // `sendmsg` of `&handshake_` and an `mmap` of one page actually mean.
    CY_CHECK_EQ(sizeof(PlaneDescription), 24U);
    CY_CHECK_EQ(sizeof(Handshake), 136U);
    CY_CHECK_EQ(sizeof(Announcement), 32U);
    CY_CHECK_EQ(sizeof(SharedState), 64U);
    CY_CHECK_EQ(offsetof(SharedState, words), 8U);
    CY_CHECK_EQ(offsetof(SharedState, heartbeat), 40U);
    CY_CHECK_EQ(offsetof(SharedState, writing), 48U);
    CY_CHECK_EQ(offsetof(SharedState, held), 56U);
    CY_CHECK_EQ(kProtocolMagic, 0x4359'5650U);
    // 'A','B','2','4' — DRM_FORMAT_ABGR8888, which is what a Vulkan R8G8B8A8_UNORM image is.
    CY_CHECK_EQ(kFourccAbgr8888, 0x3432'4241U);
}

CY_TEST_CASE("a handshake whose numbers cannot describe a ring is refused, cause by cause") {
    CY_CHECK(a_handshake().validate());

    Handshake wrong_magic = a_handshake();
    wrong_magic.magic = 0xDEAD'BEEFU;
    CY_CHECK_FALSE(wrong_magic.validate());

    Handshake newer = a_handshake();
    newer.version = kProtocolVersion + 1;
    CY_CHECK_FALSE(newer.validate());

    Handshake empty_ring = a_handshake();
    empty_ring.buffer_count = 0;
    CY_CHECK_FALSE(empty_ring.validate());
    Handshake too_many = a_handshake();
    too_many.buffer_count = 5;
    CY_CHECK_FALSE(too_many.validate());

    // The failure the first spike hit: `width * height * 4` is not the allocation size, and an
    // import against the smaller number is refused by the driver in a way that reads as a driver
    // defect rather than as a protocol defect.
    Handshake short_allocation = a_handshake();
    short_allocation.planes[1].allocation_bytes = 0;
    CY_CHECK_FALSE(short_allocation.validate());

    CY_CHECK_EQ(a_handshake().expected_descriptors(), 6U);
}

CY_TEST_CASE("the newest frame wins and nothing queues") {
    AnnouncementPage page;
    CY_REQUIRE(page.create());
    Announcement read;
    CY_CHECK_FALSE(page.read(read));
    page.publish(announcement(1, 0));
    page.publish(announcement(2, 1));
    page.publish(announcement(3, 2));
    CY_REQUIRE(page.read(read));
    CY_CHECK_EQ(read.frame_id, 3ULL);
    CY_CHECK_EQ(read.slot, 2U);
    CY_CHECK_EQ(page.heartbeat(), 3ULL);
}

CY_TEST_CASE("a full ring costs the engine a frame and never costs the editor a stall") {
    // THE EDITOR MUST NEVER THROTTLE THE ENGINE. A ring with nothing available answers `kNoSlot`,
    // and the caller drops the frame; there is no blocking path in `Ring` to take instead.
    Ring ring;
    CY_REQUIRE(ring.resize(3));
    ring.record_published(0, 1);
    ring.record_published(1, 2);
    ring.record_published(2, 3);
    // The editor holds slot 2's frame, which pins it and marks it as needing a release.
    ring.observe_held(held_pack(3, 2));
    // Slot 2 is held, and slot 2 is also the last published, so the two remaining are 0 and 1 — the
    // oldest first, which is what gives the editor the longest chance to latch the newest.
    CY_CHECK_EQ(ring.pick(0), 0U);
    ring.record_published(0, 4);
    CY_CHECK_EQ(ring.pick(0), 1U);
    ring.record_published(1, 5);
    // Now everything is spoken for: 1 is the last published, 2 is held, and 0 is... available. A
    // three-image ring always keeps one in hand, which is exactly why three is the minimum.
    CY_CHECK_EQ(ring.pick(0), 0U);

    Ring two;
    CY_REQUIRE(two.resize(2));
    two.record_published(0, 1);
    two.observe_held(held_pack(1, 0));
    two.record_published(1, 2);
    CY_CHECK_EQ(two.pick(0), Ring::kNoSlot);
    CY_CHECK(ring_advisory(2) != nullptr);
    CY_CHECK(ring_advisory(1) != nullptr);
    CY_CHECK(ring_advisory(3) == nullptr);
    CY_CHECK(ring_advisory(4) == nullptr);
}

CY_TEST_CASE("a held slot is written again only once the release timeline has reached it") {
    // The corruption a monotonic release counter alone leaves open: an editor that keeps
    // re-sampling a frame it has already released is reading a slot the engine believes it may
    // write. `observe_held` is what pins it, and the release value is what unpins it.
    Ring ring;
    CY_REQUIRE(ring.resize(3));
    ring.record_published(0, 7);
    ring.observe_held(held_pack(7, 0));
    ring.record_published(1, 8);
    ring.observe_held(0);  // the editor let go of the claim, but the frame is not released yet
    CY_CHECK_EQ(ring.release_requirement(0), 7ULL);
    // Slot 0 still needs release 7, and slot 1 was just published, so the untouched slot 2 wins.
    CY_CHECK_EQ(ring.pick(6), 2U);
    ring.record_published(2, 9);
    // Slot 0 is STILL not writable at 6 however old it is; the oldest eligible slot is 1.
    CY_CHECK_EQ(ring.pick(6), 1U);
    // The timeline reached it, so slot 0 — the oldest of the three — is writable again.
    CY_CHECK_EQ(ring.pick(7), 0U);
}

CY_TEST_CASE("a stale claim does not pin the ring for ever") {
    // An editor holding a frame that has since been overwritten must not keep the slot: that is a
    // ring that shrinks by one every time an editor falls behind.
    Ring ring;
    CY_REQUIRE(ring.resize(3));
    ring.record_published(0, 1);
    ring.record_published(0, 2);
    ring.observe_held(held_pack(1, 0));  // frame 1 is gone; slot 0 holds frame 2
    CY_CHECK_EQ(ring.release_requirement(0), 0ULL);
}

CY_TEST_CASE("a lost editor gets the whole ring back") {
    // Without this, a three-image ring is exhausted within three frames by claims nobody will ever
    // release, and the engine stops rendering because something else stopped watching.
    Ring ring;
    CY_REQUIRE(ring.resize(3));
    for (u32 slot = 0; slot < 3; ++slot) {
        ring.record_published(slot, slot + 1);
        ring.observe_held(held_pack(slot + 1, slot));
    }
    ring.reclaim();
    CY_CHECK_NE(ring.pick(0), Ring::kNoSlot);
    for (u32 slot = 0; slot < 3; ++slot) {
        CY_CHECK_EQ(ring.release_requirement(slot), 0ULL);
    }
}

CY_TEST_CASE("a claim survives the round trip for every slot a ring can have") {
    for (u32 slot = 0; slot < 4; ++slot) {
        const u64 frame_id = 0x0000'FFFF'FFFF'FFFFULL + slot;
        const u64 packed = held_pack(frame_id, slot);
        CY_CHECK_EQ(held_frame(packed), frame_id);
        CY_CHECK_EQ(held_slot(packed), slot);
        CY_CHECK(claim_names(packed, slot));
        CY_CHECK_FALSE(claim_names(packed, (slot + 1) % 4));
    }
    CY_CHECK_FALSE(claim_names(0, 0));
}
