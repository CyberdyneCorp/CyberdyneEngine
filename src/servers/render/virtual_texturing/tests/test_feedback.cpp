// GPU feedback: compaction, density, and the guarantee that recording never waits. M6 tasks 5.3 and
// 5.4.
//
// `virtual-texturing` — "GPU feedback": "A per-pixel request stream SHALL NOT reach the CPU", "a
// million pixels sample one page, one request SHALL reach the residency scheduler", and density is
// a quality lever. M6's exit criterion adds the one this file is named for: **feedback never blocks
// a frame.**
//
// The concurrent proof of that guarantee is in tests/test_teardown.cpp, in the integration suite,
// because it spawns threads. What is checkable here without one is the shape that makes it true:
// recording into a full buffer drops and returns rather than waiting, and every path through
// `record` is a bounded number of atomic operations with no allocation.

#include <cy/servers/render/virtual_texturing/feedback.h>
#include <cy/test/test.h>

using namespace cy::render::vt;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] u64 page(u32 texture, u32 x) noexcept {
    VirtualAddress address;
    address.texture = texture;
    address.tile_x = static_cast<cy::u16>(x);
    return address.encode();
}

}  // namespace

CY_TEST_CASE("a buffer with no capacity is refused rather than dropping everything silently") {
    FeedbackBuffer feedback;
    CY_CHECK_FALSE(feedback.configure(0));
    CY_CHECK_FALSE(feedback.record(page(1, 0)));
    CY_CHECK_EQ(feedback.dropped(), 1U);
}

CY_TEST_CASE("a million samples of one page become one request") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(4096));

    // The RATIO is the property, not the count: a page asked for by every sample still leaves the
    // scheduler one request. The loop is sized for the Debug profile's 1 ms unit budget, and the
    // per-iteration result is accumulated rather than asserted — a doctest assertion costs more
    // than the call it is checking, and a thousand of them are what the budget would measure.
    constexpr u32 kSamples = 1'000;
    bool every_sample_accepted = true;
    for (u32 index = 0; index < kSamples; ++index) {
        every_sample_accepted = feedback.record(page(1, 0)) && every_sample_accepted;
    }
    CY_CHECK(every_sample_accepted);
    feedback.swap();

    cy::Array<FeedbackRequest> requests;
    CY_REQUIRE(feedback.resolve(requests));
    CY_REQUIRE_EQ(requests.size(), 1U);
    CY_CHECK_EQ(requests[0].address, page(1, 0));
    CY_CHECK_EQ(requests[0].samples, kSamples);
    CY_CHECK_EQ(feedback.recorded(), kSamples);
    CY_CHECK_EQ(feedback.dropped(), 0U);
}

CY_TEST_CASE("distinct pages stay distinct, and their sample counts are kept apart") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(1024));
    for (u32 index = 0; index < 100; ++index) {
        CY_REQUIRE(feedback.record(page(1, 0)));
    }
    for (u32 index = 0; index < 10; ++index) {
        CY_REQUIRE(feedback.record(page(1, 1)));
    }
    CY_REQUIRE(feedback.record(page(2, 0)));
    feedback.swap();

    cy::Array<FeedbackRequest> requests;
    CY_REQUIRE(feedback.resolve(requests));
    CY_REQUIRE_EQ(requests.size(), 3U);
    u32 total = 0;
    for (const FeedbackRequest& request : requests) {
        total += request.samples;
    }
    CY_CHECK_EQ(total, 111U);
}

CY_TEST_CASE("a full buffer drops and returns; it never waits") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(4));

    for (u32 index = 0; index < 4; ++index) {
        CY_CHECK(feedback.record(page(1, index)));
    }
    // FULL IS NOT A STALL. The alternative to dropping a page request is stalling the frame that
    // produced it, and a dropped request costs a blurrier surface for one frame while a stall costs
    // the frame. `record` returns false and says so in the counter.
    for (u32 index = 4; index < 100; ++index) {
        CY_CHECK_FALSE(feedback.record(page(1, index)));
    }
    CY_CHECK_EQ(feedback.recorded(), 4U);
    CY_CHECK_EQ(feedback.dropped(), 96U);

    feedback.swap();
    cy::Array<FeedbackRequest> requests;
    CY_REQUIRE(feedback.resolve(requests));
    CY_CHECK_EQ(requests.size(), 4U);
}

CY_TEST_CASE("the frame writes one bank while the resolver reads the other") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(64));
    CY_REQUIRE(feedback.record(page(1, 0)));
    feedback.swap();

    // The new frame's samples go somewhere the resolver is not reading.
    CY_REQUIRE(feedback.record(page(2, 0)));

    cy::Array<FeedbackRequest> requests;
    CY_REQUIRE(feedback.resolve(requests));
    CY_REQUIRE_EQ(requests.size(), 1U);
    CY_CHECK_EQ(requests[0].address, page(1, 0));

    feedback.swap();
    CY_REQUIRE(feedback.resolve(requests));
    CY_REQUIRE_EQ(requests.size(), 1U);
    CY_CHECK_EQ(requests[0].address, page(2, 0));
    CY_CHECK_EQ(feedback.swaps(), 2U);
    // With one thread there is never an in-flight writer to wait for.
    CY_CHECK_EQ(feedback.drain_spins(), 0U);
}

CY_TEST_CASE("density is a lever, clamped at both ends") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(16));
    CY_CHECK_EQ(feedback.density(), kMinFeedbackDensity);
    CY_CHECK(feedback.samples_pixel(0));
    CY_CHECK(feedback.samples_pixel(1));

    feedback.set_density(4);  // one sample per four-pixel block
    CY_CHECK_EQ(feedback.density(), 4U);
    CY_CHECK(feedback.samples_pixel(0));
    CY_CHECK_FALSE(feedback.samples_pixel(1));
    CY_CHECK(feedback.samples_pixel(8));

    feedback.set_density(0);
    CY_CHECK_EQ(feedback.density(), kMinFeedbackDensity);
    feedback.set_density(100000);
    CY_CHECK_EQ(feedback.density(), kMaxFeedbackDensity);
}

CY_TEST_CASE("reset returns the buffer to a state a frame can start from") {
    FeedbackBuffer feedback;
    CY_REQUIRE(feedback.configure(8));
    for (u32 index = 0; index < 20; ++index) {
        feedback.record(page(1, index));
    }
    CY_CHECK_GT(feedback.dropped(), 0U);

    feedback.reset();
    CY_CHECK_EQ(feedback.recorded(), 0U);
    CY_CHECK_EQ(feedback.dropped(), 0U);
    CY_CHECK_EQ(feedback.swaps(), 0U);
    CY_CHECK(feedback.record(page(1, 0)));
}
