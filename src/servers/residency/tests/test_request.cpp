// Request priority and compaction. M6 task 4.1.
//
// `residency` — "Request priority". Two scenarios sit under that requirement and both are cases
// here: "Comparable across subsystems" — a texture page and a shadow page scored by the same
// function — and "Requests are compacted": **a million pixels request the same page, one request
// reaches the scheduler.**

#include <cy/servers/residency/request.h>
#include <cy/test/test.h>

using namespace cy::residency;
using cy::f32;
using cy::u32;

namespace {

[[nodiscard]] Request page_request(u32 page, f32 importance) noexcept {
    Request request;
    request.key = PageKey{Subsystem::Texture, page};
    request.inputs.importance = importance;
    request.inputs.screen_coverage = importance;
    request.bytes = 64ULL * 1024ULL;
    return request;
}

}  // namespace

CY_TEST_CASE("a page key survives its own encoding, and cannot collide across subsystems") {
    const PageKey texture{Subsystem::Texture, 0x00FF'FFFF'FFFF'FFULL};
    const PageKey shadow{Subsystem::Shadow, 0x00FF'FFFF'FFFF'FFULL};
    CY_CHECK_NE(texture.packed(), shadow.packed());
    CY_CHECK(PageKey::unpack(texture.packed()) == texture);
    CY_CHECK(PageKey::unpack(shadow.packed()) == shadow);
}

CY_TEST_CASE("scoring is monotonic in each of its seven terms") {
    RequestInputs base;
    base.importance = 0.5F;
    base.screen_coverage = 0.5F;
    base.prediction_confidence = 0.5F;
    const f32 reference = score_request(base);

    RequestInputs more = base;
    more.importance = 0.9F;
    CY_CHECK_GT(score_request(more), reference);

    more = base;
    more.screen_coverage = 0.9F;
    CY_CHECK_GT(score_request(more), reference);

    more = base;
    more.detail_deficit = 3;
    CY_CHECK_GT(score_request(more), reference);

    more = base;
    more.prediction_confidence = 1.0F;
    CY_CHECK_GT(score_request(more), reference);

    more = base;
    more.age_seconds = 2.0;
    CY_CHECK_GT(score_request(more), reference);

    more = base;
    more.seconds_until_needed = 0.01;
    CY_CHECK_GT(score_request(more), reference);
}

CY_TEST_CASE("urgency is slack: cost and deadline are read together, not separately") {
    // THE PROPERTY THAT A DEADLINE-ONLY SCORE GETS WRONG. Two pages are needed in 30 ms. One
    // renders in 40 ms and is already late; the other streams in 2 ms and is comfortable. Scoring
    // on the deadline alone orders them identically.
    RequestInputs expensive;
    expensive.importance = 0.5F;
    expensive.seconds_until_needed = 0.030;
    expensive.production_cost_ms = 40.0F;
    expensive.cost = CostClass::Rendered;

    RequestInputs cheap = expensive;
    cheap.production_cost_ms = 2.0F;
    cheap.cost = CostClass::Streamed;

    CY_CHECK_GT(score_request(expensive), score_request(cheap));
}

CY_TEST_CASE("an unmeasured shadow page is not treated as free") {
    RequestInputs unmeasured;
    unmeasured.importance = 0.5F;
    unmeasured.seconds_until_needed = 0.020;
    unmeasured.production_cost_ms = 0.0F;

    RequestInputs streamed = unmeasured;
    streamed.cost = CostClass::Streamed;
    RequestInputs rendered = unmeasured;
    rendered.cost = CostClass::Rendered;

    CY_CHECK_GT(score_request(rendered), score_request(streamed));
}

CY_TEST_CASE("a texture page and a shadow page are comparable, and the decision is explicable") {
    // `score_request` takes no subsystem argument at all, so a texture page and a shadow page with
    // the same numbers cannot reach different answers. What a case can check is the consequence:
    // when the two compete, the ordering follows the terms rather than which subsystem asked.
    RequestInputs texture_page;
    texture_page.importance = 0.4F;
    texture_page.screen_coverage = 0.9F;
    texture_page.cost = CostClass::Streamed;

    RequestInputs shadow_page;
    shadow_page.importance = 0.4F;
    shadow_page.screen_coverage = 0.9F;
    shadow_page.cost = CostClass::Rendered;

    // With no deadline, cost does not enter the score: the two are indistinguishable, which is what
    // "scored by the same policy" means.
    CY_CHECK_EQ(score_request(texture_page), score_request(shadow_page));

    // Give them a deadline and the expensive one leads, because it has less slack.
    texture_page.seconds_until_needed = 0.020;
    shadow_page.seconds_until_needed = 0.020;
    CY_CHECK_GT(score_request(shadow_page), score_request(texture_page));

    // Raise the texture page's own claim and it leads again. Nothing here consulted a subsystem.
    texture_page.detail_deficit = 4;
    CY_CHECK_GT(score_request(texture_page), score_request(shadow_page));
}

CY_TEST_CASE("a million pixels request the same page and one request reaches the scheduler") {
    RequestQueue queue;
    // The ratio is the property, not the count. Sized for the Debug profile's 1 ms unit budget,
    // and the per-iteration result is accumulated rather than asserted: a doctest assertion costs
    // more than the call it is checking, and a thousand of them are what the budget would measure.
    constexpr u32 kSubmissions = 1'000;
    bool every_submission_accepted = true;
    for (u32 index = 0; index < kSubmissions; ++index) {
        every_submission_accepted =
            queue.submit(page_request(42, 0.5F)).has_value() && every_submission_accepted;
    }
    CY_CHECK(every_submission_accepted);
    queue.compact();

    CY_CHECK_EQ(queue.size(), 1U);
    CY_CHECK_EQ(queue.submissions(), kSubmissions);
    CY_CHECK_EQ(queue.entries()[0].submissions, kSubmissions);
}

CY_TEST_CASE("merging takes the strongest claim, not the first or the last") {
    RequestQueue queue;
    Request weak = page_request(9, 0.1F);
    weak.inputs.detail_deficit = 1;
    weak.inputs.seconds_until_needed = 5.0;
    weak.inputs.prediction_confidence = 0.2F;
    weak.bytes = 1024;

    Request urgent = page_request(9, 0.9F);
    urgent.inputs.detail_deficit = 4;
    urgent.inputs.seconds_until_needed = 0.05;
    urgent.inputs.prediction_confidence = 1.0F;
    urgent.bytes = 4096;
    urgent.guaranteed = true;

    CY_REQUIRE(queue.submit(weak));
    CY_REQUIRE(queue.submit(urgent));
    CY_REQUIRE(queue.submit(weak));
    queue.compact();

    CY_REQUIRE_EQ(queue.size(), 1U);
    const ScoredRequest& merged = queue.entries()[0];
    CY_CHECK_NEAR(merged.inputs.importance, 0.9F, 1e-5F);
    CY_CHECK_EQ(merged.inputs.detail_deficit, 4U);
    CY_CHECK_NEAR(merged.inputs.seconds_until_needed, 0.05, 1e-9);
    CY_CHECK_NEAR(merged.inputs.prediction_confidence, 1.0F, 1e-5F);
    CY_CHECK_EQ(merged.bytes, 4096U);
    CY_CHECK(merged.guaranteed);
    CY_CHECK_EQ(merged.submissions, 3U);
}

CY_TEST_CASE("compaction orders by score, and breaks ties by key rather than by arrival") {
    RequestQueue first;
    RequestQueue second;
    // Identical requests, submitted in opposite orders. Without the key tiebreak the admitted set
    // would depend on extraction order, which is a hash iteration and a thread interleaving away
    // from differing every run.
    for (u32 page = 0; page < 8; ++page) {
        CY_REQUIRE(first.submit(page_request(page, 0.5F)));
        CY_REQUIRE(second.submit(page_request(7 - page, 0.5F)));
    }
    first.compact();
    second.compact();

    CY_REQUIRE_EQ(first.size(), second.size());
    for (cy::usize index = 0; index < first.size(); ++index) {
        CY_CHECK_EQ(first.entries()[index].key.packed(), second.entries()[index].key.packed());
    }

    RequestQueue ordered;
    CY_REQUIRE(ordered.submit(page_request(1, 0.1F)));
    CY_REQUIRE(ordered.submit(page_request(2, 0.9F)));
    ordered.compact();
    CY_REQUIRE_EQ(ordered.size(), 2U);
    CY_CHECK_EQ(ordered.entries()[0].key.page, 2U);
    CY_CHECK_GT(ordered.entries()[0].score, ordered.entries()[1].score);
}

CY_TEST_CASE("clearing a queue clears its compaction accounting too") {
    RequestQueue queue;
    CY_REQUIRE(queue.submit(page_request(1, 0.5F)));
    CY_REQUIRE(queue.submit(page_request(1, 0.5F)));
    queue.clear();
    CY_CHECK(queue.empty());
    CY_CHECK_EQ(queue.submissions(), 0U);

    // And the page can be requested again, merging from scratch rather than into a stale slot.
    CY_REQUIRE(queue.submit(page_request(1, 0.5F)));
    queue.compact();
    CY_CHECK_EQ(queue.entries()[0].submissions, 1U);
}
