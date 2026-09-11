// The per-frame budget, the deferral by priority, and the staleness a caller reads. M8.c task 4.3.
//
// UNIT, and it runs no job system: every case here pumps inline, which is the scheduler's declared
// second mode. The job-system path, its teardown under load, and everything that starts a thread
// are `integration.ml_runtime`'s — a unit case that starts a worker pool is a unit case measuring
// the machine.

#include <cy/core/memory/system_allocator.h>
#include <cy/ml/schedule.h>
#include <cy/test/test.h>

#include "fake_backend.h"

namespace {

using namespace cy;
using namespace cy::ml;
using cy::ml::test::FakeBackend;
using cy::ml::test::make_fake_asset;

/// One backend, one asset, and as many sessions as a case asks for.
struct Fixture {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend{BackendKind::OnnxRuntime};
    BackendRegistry registry;
    Expected<ModelAsset, Error> asset = make_fake_asset(system_allocator(MemoryDomain::Engine));

    Fixture() { (void)registry.add(&backend); }

    [[nodiscard]] Expected<InferenceSession, Error> session() {
        SessionDesc desc;
        SelectionReport report;
        return InferenceSession::create(allocator, registry, asset.value(), desc, report);
    }
};

CY_TEST_CASE("ml.schedule: an unbudgeted frame dispatches everything it was given") {
    Fixture fixture;
    Expected<InferenceSession, Error> first = fixture.session();
    Expected<InferenceSession, Error> second = fixture.session();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{0, 0});  // unlimited
    scheduler.begin_frame();
    CY_CHECK(scheduler.submit(first.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.submit(second.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.dispatch().has_value());
    CY_CHECK_EQ(scheduler.pump(), 2U);
    CY_CHECK_EQ(scheduler.report().submitted, 2U);
    CY_CHECK_EQ(scheduler.report().dispatched, 2U);
    CY_CHECK_EQ(scheduler.report().deferred, 0U);
    CY_CHECK_EQ(first.value().stats().invocations, 1U);
}

CY_TEST_CASE("ml.schedule: the invocation budget defers, and it defers the lowest priority") {
    Fixture fixture;
    Expected<InferenceSession, Error> low = fixture.session();
    Expected<InferenceSession, Error> normal = fixture.session();
    Expected<InferenceSession, Error> critical = fixture.session();
    CY_REQUIRE(low.has_value());
    CY_REQUIRE(normal.has_value());
    CY_REQUIRE(critical.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{1, 0});  // one invocation, no time limit
    scheduler.begin_frame();
    // Submitted lowest first on purpose: a scheduler that dispatched in submission order would pass
    // a test that submitted them in priority order, which is the test this one replaces.
    CY_CHECK(scheduler.submit(low.value(), InferencePriority::Low).has_value());
    CY_CHECK(scheduler.submit(normal.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.submit(critical.value(), InferencePriority::Critical).has_value());
    CY_CHECK(scheduler.dispatch().has_value());

    CY_CHECK_EQ(scheduler.report().dispatched, 1U);
    CY_CHECK_EQ(scheduler.report().deferred, 2U);
    CY_CHECK_EQ(scheduler.deferred_count(), 2U);
    // "the deferral reported": which priority was starved, not only how many.
    CY_CHECK_EQ(scheduler.report().highest_deferred, InferencePriority::Normal);

    CY_CHECK_EQ(scheduler.pump(), 1U);
    CY_CHECK_EQ(critical.value().stats().invocations, 1U);
    CY_CHECK_EQ(normal.value().stats().invocations, 0U);
    CY_CHECK_EQ(low.value().stats().invocations, 0U);
    // Utilisation over the count limit: one of one.
    CY_CHECK_GE(scheduler.report().utilisation, 1.0F);
}

CY_TEST_CASE("ml.schedule: a tie is broken by submission order, not by memory address") {
    Fixture fixture;
    Expected<InferenceSession, Error> first = fixture.session();
    Expected<InferenceSession, Error> second = fixture.session();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{1, 0});
    scheduler.begin_frame();
    CY_CHECK(scheduler.submit(second.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.submit(first.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.dispatch().has_value());
    CY_CHECK_EQ(scheduler.pump(), 1U);
    // `second` was submitted first, so `second` runs. The rule is about the order of submission.
    CY_CHECK_EQ(second.value().stats().invocations, 1U);
    CY_CHECK_EQ(first.value().stats().invocations, 0U);
}

CY_TEST_CASE("ml.schedule: a result is fresh the frame it completed and stale afterwards") {
    Fixture fixture;
    Expected<InferenceSession, Error> session = fixture.session();
    CY_REQUIRE(session.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{0, 0});

    // Before anything has run, a caller reads "never completed" and the frame is not blocked.
    scheduler.begin_frame();
    AsyncResult result = scheduler.result(session.value());
    CY_CHECK(result.never_completed);
    CY_CHECK_FALSE(result.fresh);
    CY_CHECK_EQ(result.session, nullptr);

    CY_CHECK(scheduler.submit(session.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.dispatch().has_value());
    CY_CHECK_EQ(scheduler.pump(), 1U);
    result = scheduler.result(session.value());
    CY_CHECK(result.fresh);
    CY_CHECK_FALSE(result.never_completed);
    CY_CHECK_EQ(result.frames_stale, 0U);
    CY_CHECK_EQ(result.session, &session.value());

    // Two frames in which nothing was submitted: the caller still has an answer, and it knows how
    // old it is. That is "SHALL yield the previous result ... with the staleness visible".
    scheduler.begin_frame();
    result = scheduler.result(session.value());
    CY_CHECK_FALSE(result.fresh);
    CY_CHECK_EQ(result.frames_stale, 1U);
    CY_CHECK_NE(result.session, nullptr);

    scheduler.begin_frame();
    CY_CHECK_EQ(scheduler.result(session.value()).frames_stale, 2U);

    // And a frame that runs it again resets the age.
    CY_CHECK(scheduler.submit(session.value(), InferencePriority::Normal).has_value());
    CY_CHECK(scheduler.dispatch().has_value());
    CY_CHECK_EQ(scheduler.pump(), 1U);
    CY_CHECK_EQ(scheduler.result(session.value()).frames_stale, 0U);
}

CY_TEST_CASE("ml.schedule: dispatch does not run a model, and pump is where the time goes") {
    Fixture fixture;
    Expected<InferenceSession, Error> session = fixture.session();
    CY_REQUIRE(session.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{0, 0});
    scheduler.begin_frame();
    CY_CHECK(scheduler.submit(session.value(), InferencePriority::High).has_value());
    CY_CHECK(scheduler.dispatch().has_value());
    // The frame has been dispatched and nothing has run: "asynchronous inference SHALL never stall
    // the frame" is a property of this line.
    CY_CHECK_EQ(session.value().stats().invocations, 0U);
    CY_CHECK_EQ(scheduler.pump(), 1U);
    CY_CHECK_EQ(session.value().stats().invocations, 1U);
}

CY_TEST_CASE("ml.schedule: a frame's queue does not survive into the next one") {
    Fixture fixture;
    Expected<InferenceSession, Error> session = fixture.session();
    CY_REQUIRE(session.has_value());

    InferenceScheduler scheduler;
    scheduler.set_budget(InferenceBudget{0, 0});
    scheduler.begin_frame();
    CY_CHECK(scheduler.submit(session.value(), InferencePriority::Background).has_value());
    // Deliberately never dispatched or pumped.
    scheduler.begin_frame();
    CY_CHECK_EQ(scheduler.report().submitted, 0U);
    CY_CHECK_EQ(scheduler.deferred_count(), 0U);
    CY_CHECK_EQ(scheduler.pump(), 0U);
    CY_CHECK_EQ(session.value().stats().invocations, 0U);
}

CY_TEST_CASE("ml.schedule: the queue's ceiling is reported rather than exceeded") {
    Fixture fixture;
    Expected<InferenceSession, Error> session = fixture.session();
    CY_REQUIRE(session.has_value());

    InferenceScheduler scheduler;
    scheduler.begin_frame();
    for (u32 index = 0; index < InferenceScheduler::kMaxRequests; ++index) {
        CY_REQUIRE(scheduler.submit(session.value(), InferencePriority::Normal).has_value());
    }
    Expected<RequestId, Error> overflow =
        scheduler.submit(session.value(), InferencePriority::Critical);
    CY_REQUIRE_FALSE(overflow.has_value());
    CY_CHECK_EQ(overflow.error().code, ErrorCode::OutOfRange);
}

}  // namespace
