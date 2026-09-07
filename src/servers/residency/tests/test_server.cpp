// `ResidencyServer`: the frame, the budget, the coordinated reduction, the diagnostics, and the
// teardown that happens while work is outstanding. M6 tasks 4.1 and 4.2.

#include <cy/servers/residency/server.h>
#include <cy/test/test.h>

using namespace cy::residency;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;

namespace {

constexpr u64 kPageBytes = 1000;

[[nodiscard]] SubsystemPolicy texture_policy(u64 budget) noexcept {
    SubsystemPolicy policy;
    policy.domain = cy::MemoryDomain::Gpu;
    policy.budget_bytes = budget;
    policy.budget_kind = cy::BudgetKind::Hard;
    policy.min_residency_frames = 0;  // the anti-oscillation guard has its own case
    policy.default_cost = CostClass::Streamed;
    return policy;
}

[[nodiscard]] Request texture_request(u64 page, f32 importance) noexcept {
    Request request;
    request.key = PageKey{Subsystem::Texture, page};
    request.bytes = kPageBytes;
    request.inputs.importance = importance;
    request.inputs.screen_coverage = importance;
    return request;
}

void make_resident(ResidencyServer& server, u64 page, f64 now) {
    ResidentReport report;
    report.key = PageKey{Subsystem::Texture, page};
    report.bytes = kPageBytes;
    CY_REQUIRE(server.note_resident(report, now));
}

[[nodiscard]] LeverSchedule schedule(f32 normal, f32 elevated, f32 critical) noexcept {
    LeverSchedule lever;
    lever.declared = true;
    lever.normal = normal;
    lever.elevated = elevated;
    lever.critical = critical;
    return lever;
}

}  // namespace

CY_TEST_CASE("a request for an unregistered subsystem is refused rather than queued") {
    ResidencyServer server;
    CY_CHECK_FALSE(server.request(texture_request(1, 0.5F)));
    CY_CHECK_EQ(server.pending_requests(), 0U);

    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(0)));
    CY_CHECK(server.request(texture_request(1, 0.5F)));
    CY_CHECK_EQ(server.pending_requests(), 1U);
}

CY_TEST_CASE("the frame admits what fits and blocks what does not") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(3 * kPageBytes)));

    Schedule frame;
    for (u64 page = 0; page < 5; ++page) {
        CY_REQUIRE(server.request(texture_request(page, 0.9F - (0.1F * static_cast<f32>(page)))));
    }
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));

    // Three fit in the hard budget; the other two are budget-blocked, not silently dropped.
    CY_CHECK_EQ(frame.admissions.size(), 3U);
    for (const Admission& admission : frame.admissions) {
        make_resident(server, admission.key.page, 0.0);
    }
    const ResidencyStats stats = server.stats(Subsystem::Texture);
    CY_CHECK_EQ(stats.requests, 5U);
    CY_CHECK_EQ(stats.admissions, 3U);
    CY_CHECK_EQ(stats.budget_blocked, 2U);
    CY_CHECK_EQ(stats.resident_bytes, 3 * kPageBytes);
    CY_CHECK_EQ(stats.resident_pages, 3U);
}

CY_TEST_CASE("a soft budget admits and a hard budget refuses") {
    SubsystemPolicy soft = texture_policy(kPageBytes);
    soft.budget_kind = cy::BudgetKind::Soft;

    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, soft));
    Schedule frame;
    for (u64 page = 0; page < 4; ++page) {
        CY_REQUIRE(server.request(texture_request(page, 0.5F)));
    }
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));
    // `cy/core/memory/budget.h`: "A soft budget always admits — crossing it is what raises
    // pressure, not what stops the growth." Disagreeing here would give the engine two meanings.
    CY_CHECK_EQ(frame.admissions.size(), 4U);
}

CY_TEST_CASE("a frame's admission limit defers rather than drops") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(0)));
    Schedule frame;
    for (u64 page = 0; page < 6; ++page) {
        CY_REQUIRE(server.request(texture_request(page, 0.5F)));
    }
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 2}, frame));
    CY_CHECK_EQ(frame.admissions.size(), 2U);
    CY_CHECK_EQ(server.stats(Subsystem::Texture).outscored, 4U);
    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 5}).reason == QualityReason::Outscored);
}

CY_TEST_CASE("a resident page is a hit and is not admitted twice") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(0)));
    make_resident(server, 1, 0.0);

    Schedule frame;
    CY_REQUIRE(server.request(texture_request(1, 0.5F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));
    CY_CHECK(frame.admissions.empty());
    CY_CHECK_EQ(server.stats(Subsystem::Texture).hits, 1U);
    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 1}).reason == QualityReason::Resident);
}

CY_TEST_CASE("making room evicts the cheapest to lose, and never a held or guaranteed page") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(3 * kPageBytes)));

    make_resident(server, 1, 0.0);
    make_resident(server, 2, 0.0);
    ResidentReport tail;
    tail.key = PageKey{Subsystem::Texture, 3};
    tail.bytes = kPageBytes;
    tail.guaranteed = true;
    CY_REQUIRE(server.note_resident(tail, 0.0));

    // Page 1 is held; page 3 is guaranteed. Page 2 is the only candidate.
    const auto held = server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Gameplay);
    CY_REQUIRE(held);

    Schedule frame;
    CY_REQUIRE(server.request(texture_request(9, 1.0F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));

    CY_REQUIRE_EQ(frame.evictions.size(), 1U);
    CY_CHECK_EQ(frame.evictions[0].key.page, 2U);
    CY_CHECK_EQ(frame.admissions.size(), 1U);
    CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, 1}));
    CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, 3}));
    CY_CHECK_FALSE(server.is_resident(PageKey{Subsystem::Texture, 2}));
}

CY_TEST_CASE("a page evicted this frame is not admitted again this frame") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(kPageBytes)));
    make_resident(server, 1, 0.0);

    Schedule frame;
    CY_REQUIRE(server.request(texture_request(2, 1.0F)));   // evicts page 1
    CY_REQUIRE(server.request(texture_request(1, 0.05F)));  // and asks for it straight back
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));

    CY_CHECK_EQ(frame.evictions.size(), 1U);
    CY_REQUIRE_EQ(frame.admissions.size(), 1U);
    CY_CHECK_EQ(frame.admissions[0].key.page, 2U);
    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 1}).reason == QualityReason::Evicted);
    // And the re-request inside the window is what churn measures.
    CY_CHECK_EQ(server.stats(Subsystem::Texture).churn.evictions, 1U);
}

CY_TEST_CASE("the minimum residency age stops a page leaving the frame it arrived in") {
    SubsystemPolicy policy = texture_policy(kPageBytes);
    policy.min_residency_frames = 2;
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy));
    make_resident(server, 1, 0.0);

    Schedule frame;
    CY_REQUIRE(server.request(texture_request(2, 1.0F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));
    CY_CHECK(frame.evictions.empty());
    CY_CHECK(frame.admissions.empty());  // budget-blocked rather than thrashing

    server.end_frame(0.016);
    server.end_frame(0.032);
    CY_REQUIRE(server.request(texture_request(2, 1.0F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{0.048, 0}, frame));
    CY_CHECK_EQ(frame.evictions.size(), 1U);
}

CY_TEST_CASE("pressure applies the declared plan, and pinned mode stops it") {
    ResidencyServer server;
    SubsystemPolicy policy = texture_policy(0);
    policy.levers[static_cast<u32>(Lever::TextureMipBias)] = schedule(0.0F, 1.0F, 2.0F);
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy));

    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 0.0F, 1e-6F);
    server.on_pressure(cy::PressureLevel::Elevated, cy::PressureLevel::Normal);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 1.0F, 1e-6F);
    server.on_pressure(cy::PressureLevel::Critical, cy::PressureLevel::Elevated);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 2.0F, 1e-6F);

    cy::Array<ReductionStep> plan;
    CY_REQUIRE(server.last_reduction(plan));
    CY_REQUIRE_EQ(plan.size(), 1U);
    CY_CHECK(plan[0].lever == Lever::TextureMipBias);
    CY_CHECK_NEAR(plan[0].from, 1.0F, 1e-6F);

    // `residency`: pinned mode disables coordinated adjustment. The level is still recorded.
    server.set_pinned(true);
    server.on_pressure(cy::PressureLevel::Normal, cy::PressureLevel::Critical);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 2.0F, 1e-6F);
    CY_CHECK(server.level() == cy::PressureLevel::Normal);
}

CY_TEST_CASE("relaxing waits out the dwell; tightening does not") {
    ResidencyServer server;
    SubsystemPolicy policy = texture_policy(0);
    policy.levers[static_cast<u32>(Lever::TextureMipBias)] = schedule(0.0F, 1.0F, 2.0F);
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy));
    server.set_relax_dwell(1.0);

    server.end_frame(10.0);  // establishes the clock
    server.on_pressure(cy::PressureLevel::Elevated, cy::PressureLevel::Normal);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 1.0F, 1e-6F);

    // Pressure falls immediately. The lever does NOT: refilling the caches that were just trimmed
    // is the churn the next requirement measures.
    server.on_pressure(cy::PressureLevel::Normal, cy::PressureLevel::Elevated);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 1.0F, 1e-6F);

    server.end_frame(10.5);
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 1.0F, 1e-6F);
    server.end_frame(11.5);  // the dwell has elapsed
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 0.0F, 1e-6F);
}

CY_TEST_CASE("a cycle of hard residency dependencies is a configuration error") {
    ResidencyServer server;
    CY_CHECK(server.validate_dependencies());

    // Shadows need an opacity texture; textures do not need shadows. Legal.
    CY_REQUIRE(
        server.declare_dependency(Subsystem::Shadow, Subsystem::Texture, DependencyKind::Hard));
    CY_CHECK(server.validate_dependencies());

    // A cycle of COARSE dependencies is a description of the engine, not a fault: each is satisfied
    // by a guaranteed representation that is already resident.
    CY_REQUIRE(server.declare_dependency(Subsystem::Texture, Subsystem::Illumination,
                                         DependencyKind::Coarse));
    CY_REQUIRE(server.declare_dependency(Subsystem::Illumination, Subsystem::Shadow,
                                         DependencyKind::Coarse));
    CY_CHECK(server.validate_dependencies());

    // Close the cycle with hard edges and it becomes one.
    CY_REQUIRE(server.declare_dependency(Subsystem::Texture, Subsystem::Illumination,
                                         DependencyKind::Hard));
    CY_REQUIRE(server.declare_dependency(Subsystem::Illumination, Subsystem::Shadow,
                                         DependencyKind::Hard));
    CY_CHECK_FALSE(server.validate_dependencies());

    CY_CHECK_FALSE(
        server.declare_dependency(Subsystem::Shadow, Subsystem::Shadow, DependencyKind::Hard));
}

CY_TEST_CASE("why is this blurry: every cause is a value rather than a guess") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(kPageBytes)));

    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 1}).reason ==
             QualityReason::NeverRequested);

    Schedule frame;
    CY_REQUIRE(server.request(texture_request(1, 0.9F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));
    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 1}).reason ==
             QualityReason::AwaitingProduction);

    make_resident(server, 1, 0.0);
    const Explanation resident = server.explain(PageKey{Subsystem::Texture, 1});
    CY_CHECK(resident.reason == QualityReason::Resident);
    CY_CHECK_FALSE(resident.active);
    CY_CHECK_EQ(resident.holds, 0U);

    // With the budget full and the only resident page HELD, there is no room to be made — which is
    // what `BudgetBlocked` means, as distinct from `Outscored`. Without the hold the policy would
    // evict page 1 for page 2 and the answer would be `AwaitingProduction`, which is a different
    // and equally correct thing to report.
    CY_REQUIRE(server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Gameplay));
    CY_REQUIRE(server.request(texture_request(2, 0.1F)));
    CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));
    CY_CHECK(server.explain(PageKey{Subsystem::Texture, 2}).reason == QualityReason::BudgetBlocked);
}

CY_TEST_CASE("unregistering a subsystem mid-flight leaves no bytes and no live holds") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, texture_policy(0)));
    CY_REQUIRE(server.register_subsystem(Subsystem::Geometry, texture_policy(0)));

    for (u64 page = 0; page < 8; ++page) {
        make_resident(server, page, 0.0);
    }
    const auto held = server.hold(PageKey{Subsystem::Texture, 3}, HoldReason::Streaming);
    CY_REQUIRE(held);
    ResidentReport geometry;
    geometry.key = PageKey{Subsystem::Geometry, 1};
    geometry.bytes = kPageBytes;
    CY_REQUIRE(server.note_resident(geometry, 0.0));

    CY_CHECK(server.unregister_subsystem(Subsystem::Texture));

    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), 0U);
    CY_CHECK_EQ(server.outstanding_holds(), 0U);
    CY_CHECK_FALSE(server.registered(Subsystem::Texture));
    // Releasing the invalidated hold is refused rather than double-counted.
    CY_CHECK_FALSE(server.release(*held));
    // The other subsystem is untouched.
    CY_CHECK(server.is_resident(PageKey{Subsystem::Geometry, 1}));
    CY_CHECK_EQ(server.resident_bytes(Subsystem::Geometry), kPageBytes);
}

CY_TEST_CASE("reset drops the pages and keeps the configuration") {
    ResidencyServer server;
    SubsystemPolicy policy = texture_policy(0);
    policy.levers[static_cast<u32>(Lever::TextureMipBias)] = schedule(0.0F, 1.0F, 2.0F);
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy));
    server.on_pressure(cy::PressureLevel::Critical, cy::PressureLevel::Normal);
    make_resident(server, 1, 0.0);
    CY_REQUIRE(server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Editor));

    server.reset();

    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), 0U);
    CY_CHECK_EQ(server.outstanding_holds(), 0U);
    CY_CHECK_EQ(server.frame(), 0U);
    CY_CHECK(server.registered(Subsystem::Texture));
    CY_CHECK_NEAR(server.lever(Subsystem::Texture, Lever::TextureMipBias), 2.0F, 1e-6F);
}
