// M6 EXIT CRITERION: **residency and activation are provably separate — a test holds bytes resident
// with simulation off.** Task 4.3.
//
// docs/ROADMAP.md lists it among the seven things M6 must be able to demonstrate, and design.md §3
// says why it is a criterion rather than a detail: "If residency and activation cannot be
// separated, streaming becomes an all-or-nothing operation and the frame budget goes with it." §5
// adds it to the table of invariants that cannot be retrofitted, because "every later system that
// streams assumes it".
//
// --- WHAT "SIMULATION OFF" MEANS IN THIS FILE
// -------------------------------------------------------
//
// Nothing here simulates anything, and that is not a claim about the test — it is a claim about the
// interface. `ResidencyServer` has no tick, no world, no delta time and no "running" flag; the only
// clock it takes is a wall time passed to `end_frame`, and every one of these cases would behave
// identically if that clock never advanced. There is no mode to switch off, because there was never
// a mode to switch on.
//
// The cases below assert both directions, because only one of them is the interesting one:
//
//   1. Bytes stay resident across hundreds of frames with nothing activated and nothing simulated.
//   2. Activating and deactivating changes no byte of residency, in either direction.
//   3. A hold survives Critical memory pressure and a budget it exceeds — it is a fact, not a
//      weight another frame can outbid.
//   4. Releasing the HOLD is what makes the page evictable. That is the control: it proves the hold
//      was doing the work, and not some incidental property of a test that never evicted anything.
//   5. A page can be held before it arrives, and the hold applies at arrival — which is what a
//      streaming pin actually looks like, and the case a design that conflated the two facts would
//      have no answer for.

#include <cy/servers/residency/server.h>
#include <cy/test/test.h>

using namespace cy::residency;
using cy::f64;
using cy::u32;
using cy::u64;

namespace {

constexpr u64 kPageBytes = 4096;
constexpr u64 kResidentPages = 8;

[[nodiscard]] SubsystemPolicy policy(u64 budget) noexcept {
    SubsystemPolicy value;
    value.domain = cy::MemoryDomain::Gpu;
    value.budget_bytes = budget;
    value.budget_kind = cy::BudgetKind::Hard;
    value.min_residency_frames = 0;
    return value;
}

void make_resident(ResidencyServer& server, u64 page, f64 now) {
    ResidentReport report;
    report.key = PageKey{Subsystem::Texture, page};
    report.bytes = kPageBytes;
    CY_REQUIRE(server.note_resident(report, now));
}

}  // namespace

CY_TEST_CASE("bytes stay resident with nothing activated and nothing simulated") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kResidentPages * kPageBytes)));

    cy::Array<HoldId> holds;
    for (u64 page = 0; page < kResidentPages; ++page) {
        make_resident(server, page, 0.0);
        auto held = server.hold(PageKey{Subsystem::Texture, page}, HoldReason::Streaming);
        CY_REQUIRE(held);
        CY_REQUIRE(holds.push_back(*held));
    }
    CY_REQUIRE_EQ(server.resident_bytes(Subsystem::Texture), kResidentPages * kPageBytes);

    // Two hundred and forty frames. No `set_active`, no importance published, no request submitted,
    // no entity, no world, no tick — the clock advances and nothing else does.
    Schedule frame;
    f64 now = 0.0;
    for (u32 index = 0; index < 240; ++index) {
        now += 1.0 / 60.0;
        CY_REQUIRE(server.schedule(ScheduleOptions{now, 0}, frame));
        server.end_frame(now);
    }

    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), kResidentPages * kPageBytes);
    CY_CHECK_EQ(server.stats(Subsystem::Texture).resident_pages, kResidentPages);
    CY_CHECK_EQ(server.stats(Subsystem::Texture).evictions, 0U);
    // And nothing became active by being resident, which is the other half of the separation.
    CY_CHECK_EQ(server.active_pages(Subsystem::Texture), 0U);
    CY_CHECK_EQ(server.frame(), 240U);
}

CY_TEST_CASE("activation changes no byte of residency, in either direction") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kResidentPages * kPageBytes)));
    for (u64 page = 0; page < kResidentPages; ++page) {
        make_resident(server, page, 0.0);
    }
    const u64 before = server.resident_bytes(Subsystem::Texture);

    for (u32 pass = 0; pass < 16; ++pass) {
        for (u64 page = 0; page < kResidentPages; ++page) {
            CY_REQUIRE(server.set_active(PageKey{Subsystem::Texture, page}, (pass % 2) == 0));
        }
        CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), before);
        for (u64 page = 0; page < kResidentPages; ++page) {
            CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, page}));
        }
    }

    CY_CHECK_EQ(server.active_pages(Subsystem::Texture), 0U);  // the last pass deactivated
    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), before);
}

CY_TEST_CASE("activating a page that is not resident is not an error and does not make it one") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(0)));

    // Activation is reported ABOUT residency; it never causes it. A design where `set_active` could
    // pull bytes in is a design where activation and residency are the same operation.
    CY_CHECK_FALSE(server.set_active(PageKey{Subsystem::Texture, 1}, true));
    CY_CHECK_FALSE(server.is_resident(PageKey{Subsystem::Texture, 1}));
    CY_CHECK_FALSE(server.is_active(PageKey{Subsystem::Texture, 1}));
    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), 0U);
}

CY_TEST_CASE("a hold survives Critical pressure and a budget the page exceeds") {
    ResidencyServer server;
    // A budget of ONE page, with two already resident and both held. Nothing about this is a
    // comfortable configuration, and that is the point.
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kPageBytes)));
    make_resident(server, 1, 0.0);
    make_resident(server, 2, 0.0);
    CY_REQUIRE(server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Gameplay));
    CY_REQUIRE(server.hold(PageKey{Subsystem::Texture, 2}, HoldReason::Save));

    server.on_pressure(cy::PressureLevel::Critical, cy::PressureLevel::Normal);

    Schedule frame;
    for (u64 page = 10; page < 20; ++page) {
        Request request;
        request.key = PageKey{Subsystem::Texture, page};
        request.bytes = kPageBytes;
        request.inputs.importance = 1.0F;
        request.inputs.screen_coverage = 1.0F;
        CY_REQUIRE(server.request(request));
    }
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));

    // Ten maximally important requests, a budget already exceeded, and pressure at Critical. The
    // held pages do not move: the new requests are refused instead.
    CY_CHECK(frame.evictions.empty());
    CY_CHECK(frame.admissions.empty());
    CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, 1}));
    CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, 2}));
    CY_CHECK_EQ(server.stats(Subsystem::Texture).budget_blocked, 10U);
}

CY_TEST_CASE("releasing the hold is what makes the page evictable: the control") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kPageBytes)));
    make_resident(server, 1, 0.0);
    const auto held = server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Gameplay);
    CY_REQUIRE(held);
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 1U);

    Request incoming;
    incoming.key = PageKey{Subsystem::Texture, 2};
    incoming.bytes = kPageBytes;
    incoming.inputs.importance = 1.0F;

    Schedule frame;
    CY_REQUIRE(server.request(incoming));
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));
    CY_CHECK(frame.evictions.empty());

    // The ONLY thing that changes is the hold. Activation is never touched in this case.
    CY_CHECK(server.release(*held));
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 0U);

    server.end_frame(1.0);  // clears the same-frame hysteresis
    CY_REQUIRE(server.request(incoming));
    CY_REQUIRE(server.schedule(ScheduleOptions{2.0, 0}, frame));
    CY_REQUIRE_EQ(frame.evictions.size(), 1U);
    CY_CHECK_EQ(frame.evictions[0].key.page, 1U);
    CY_CHECK_EQ(frame.admissions.size(), 1U);
}

CY_TEST_CASE("an active page with no hold is still evictable: activation is a preference") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kPageBytes)));
    make_resident(server, 1, 0.0);
    CY_REQUIRE(server.set_active(PageKey{Subsystem::Texture, 1}, true));

    Request incoming;
    incoming.key = PageKey{Subsystem::Texture, 2};
    incoming.bytes = kPageBytes;
    incoming.inputs.importance = 1.0F;
    Schedule frame;
    CY_REQUIRE(server.request(incoming));
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));

    // Being active makes a page a worse candidate — `eviction_score` gives it a bonus — but it is
    // not a pin. Only a hold or a guarantee is.
    CY_REQUIRE_EQ(frame.evictions.size(), 1U);
    CY_CHECK_EQ(frame.evictions[0].key.page, 1U);
}

CY_TEST_CASE("a page can be held before it arrives, and the hold applies at arrival") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(kPageBytes)));

    // A streaming pin: the caller knows it will need the bytes and says so before the fetch
    // completes. A design that required residency first would have nowhere to put this.
    const auto held = server.hold(PageKey{Subsystem::Texture, 5}, HoldReason::Streaming);
    CY_REQUIRE(held);
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 5}), 1U);
    CY_CHECK_FALSE(server.is_resident(PageKey{Subsystem::Texture, 5}));

    make_resident(server, 5, 0.0);

    Request incoming;
    incoming.key = PageKey{Subsystem::Texture, 6};
    incoming.bytes = kPageBytes;
    incoming.inputs.importance = 1.0F;
    Schedule frame;
    CY_REQUIRE(server.request(incoming));
    CY_REQUIRE(server.schedule(ScheduleOptions{1.0, 0}, frame));
    CY_CHECK(frame.evictions.empty());
    CY_CHECK(server.is_resident(PageKey{Subsystem::Texture, 5}));

    CY_CHECK(server.release(*held));
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 5}), 0U);
}

CY_TEST_CASE("holds nest, and the last one released is the one that frees the page") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy(0)));
    make_resident(server, 1, 0.0);

    const auto first = server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Gameplay);
    const auto second = server.hold(PageKey{Subsystem::Texture, 1}, HoldReason::Editor);
    CY_REQUIRE(first);
    CY_REQUIRE(second);
    CY_CHECK_NE(first->value, second->value);
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 2U);

    CY_CHECK(server.release(*first));
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 1U);
    CY_CHECK_FALSE(server.release(*first));  // twice is refused, not double-counted
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 1U);
    CY_CHECK(server.release(*second));
    CY_CHECK_EQ(server.holds(PageKey{Subsystem::Texture, 1}), 0U);
    CY_CHECK_EQ(server.outstanding_holds(), 0U);
}
