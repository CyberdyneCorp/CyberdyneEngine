// The jitter sequence: applied once, absent when nobody wants it, and reproducible when pinned.
// Task 8.3.

#include <cy/test/test.h>

#include <cy/rendering/temporal/jitter.h>

#include <cmath>

namespace {

using cy::rendering::JitterConfig;
using cy::rendering::JitterSequence;

}  // namespace

CY_TEST_CASE("the Halton sequence is the one everybody else's is") {
    // The first few radical inverses in base 2 and 3, which is the check that says the
    // implementation is the sequence and not merely a low-discrepancy-looking thing.
    CY_CHECK_NEAR(cy::rendering::halton(1, 2), 0.5F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(2, 2), 0.25F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(3, 2), 0.75F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(1, 3), 1.0F / 3.0F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(2, 3), 2.0F / 3.0F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(3, 3), 1.0F / 9.0F, 1e-6F);
    CY_CHECK_NEAR(cy::rendering::halton(0, 2), 0.0F, 1e-6F);
    // A degenerate base answers zero rather than looping forever.
    CY_CHECK_NEAR(cy::rendering::halton(5, 1), 0.0F, 1e-6F);
}

CY_TEST_CASE("the sequence stays inside a pixel, spreads over one, and repeats at its length") {
    JitterSequence sequence;
    JitterConfig config;
    config.length = 8;
    sequence.configure(config);

    cy::Vec2 first[8];
    for (cy::Vec2& sample : first) {
        sequence.advance(true);
        sample = sequence.current();
        CY_CHECK(std::fabs(sample.x) <= 0.5F);
        CY_CHECK(std::fabs(sample.y) <= 0.5F);
    }
    // Eight distinct samples, not eight copies of one.
    for (cy::u32 a = 0; a < 8; ++a) {
        for (cy::u32 b = a + 1; b < 8; ++b) {
            CY_CHECK(std::fabs(first[a].x - first[b].x) + std::fabs(first[a].y - first[b].y) >
                     1e-4F);
        }
    }
    // And it repeats: the ninth frame is the first sample again.
    sequence.advance(true);
    CY_CHECK_NEAR(sequence.current().x, first[0].x, 1e-6F);
    CY_CHECK_NEAR(sequence.current().y, first[0].y, 1e-6F);
    // The previous offset is exposed too, because reprojection needs both.
    CY_CHECK_NEAR(sequence.previous().x, first[7].x, 1e-6F);
}

CY_TEST_CASE("no consumer, no jitter — and the sequence does not advance while it is off") {
    JitterSequence sequence;
    sequence.configure(JitterConfig{});

    sequence.advance(true);
    sequence.advance(true);
    const cy::u32 index_before = sequence.index();
    CY_CHECK(sequence.enabled());

    for (cy::u32 frame = 0; frame < 5; ++frame) {
        sequence.advance(false);
        CY_CHECK_FALSE(sequence.enabled());
        CY_CHECK_EQ(sequence.current().x, 0.0F);
        CY_CHECK_EQ(sequence.current().y, 0.0F);
    }
    CY_CHECK_EQ(sequence.index(), index_before);

    // Turning it back on resumes the sequence rather than jumping to wherever a free-running frame
    // counter had reached.
    sequence.advance(true);
    CY_CHECK_EQ(sequence.index(), (index_before + 1U) % sequence.config().length);
}

CY_TEST_CASE("pinned, the sequence is reproducible whatever frame the capture started on") {
    JitterSequence early;
    JitterSequence late;
    early.configure(JitterConfig{});
    late.configure(JitterConfig{});

    // One has been running for a while; the other has not. That difference is exactly what makes an
    // unpinned golden image irreproducible.
    for (cy::u32 frame = 0; frame < 37; ++frame) {
        early.advance(true);
    }
    early.pin(3);
    late.pin(3);

    for (cy::u32 frame = 0; frame < 10; ++frame) {
        early.advance(true);
        late.advance(true);
        CY_CHECK(early.pinned());
        CY_CHECK_EQ(early.current().x, late.current().x);
        CY_CHECK_EQ(early.current().y, late.current().y);
    }

    early.unpin();
    CY_CHECK_FALSE(early.pinned());
    early.advance(true);
    CY_CHECK_NE(early.index(), late.index());
}

CY_TEST_CASE("the NDC offset spans two units of clip space per pixel, not one") {
    JitterSequence sequence;
    JitterConfig config;
    config.length = 4;
    sequence.configure(config);
    sequence.advance(true);

    const cy::Vec2 pixels = sequence.current();
    const cy::Vec2 ndc = sequence.ndc_offset(1920, 1080);
    // The missing factor of two halves the sample spread and looks exactly like a sequence that is
    // too short, which is why it is asserted rather than trusted.
    CY_CHECK_NEAR(ndc.x, pixels.x * 2.0F / 1920.0F, 1e-9F);
    CY_CHECK_NEAR(ndc.y, pixels.y * 2.0F / 1080.0F, 1e-9F);
    // A zero-sized target does not divide by zero.
    const cy::Vec2 degenerate = sequence.ndc_offset(0, 0);
    CY_CHECK(std::isfinite(degenerate.x));
}
