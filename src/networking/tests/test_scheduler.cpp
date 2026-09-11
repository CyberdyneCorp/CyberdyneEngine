// M9 TASK 4.4 — THE DEGRADATION ORDER, BOUNDED STALENESS, AND THE CURVE.
//
// `networking-and-replication` — "Bandwidth management": "Budget enforcement SHALL degrade in a
// defined order: reduce update frequency for low-priority bands, reduce encoding precision, then
// mark entities dormant — before omitting updates entirely."
//
// A test that asserted "the total fitted the budget" would pass over a scheduler that simply
// dropped the tail, which is the one thing that order exists to prevent. So each case below asserts
// WHICH step of the order absorbed the pressure, by the count the report carries for it.
//
// The last case is the exit criterion's own shape: "Bandwidth stays within budget **as entity count
// scales**". It is the same property `src/networking/tools/cy_net_bandwidth_curve` measures across
// eight populations up to twenty thousand and draws in `docs/design/images/m9-bandwidth-curve.png`;
// here it is four populations, which is what the integration tier can afford.
//
// `integration`, because every case builds a population and drives ticks over it.

#include "fixture.h"

#include <cy/networking/scheduler.h>

#include <algorithm>

using namespace cy::net_test;
using cy::i64;
using cy::u32;
using cy::u64;

namespace {

constexpr PeerId kPeer = PeerId::make(1, 1);
constexpr u32 kBytesPerUpdate = 30;

u32 estimate(void* /*user*/, NetworkId /*id*/, u32 precision_percent) noexcept {
    const u32 scaled =
        (kBytesPerUpdate * (precision_percent == 0 ? 100U : precision_percent)) / 100U;
    return scaled == 0 ? 1U : scaled;
}

[[nodiscard]] Candidate near_candidate(NetworkId id, RelevanceRule rule, i64 distance) noexcept {
    Candidate candidate;
    candidate.id = id;
    candidate.rule = rule;
    candidate.distance_squared = distance * distance;
    return candidate;
}

}  // namespace

CY_TEST_CASE("networking: pressure becomes deferral before it becomes omission") {
    cy::Allocator& memory = allocator();
    PriorityScheduler scheduler(memory);
    scheduler.set_distance_bands(300LL * 300LL, 800LL * 800LL);
    NetworkIdMinter minter(1);

    cy::Array<Candidate> candidates(memory);
    for (u32 index = 0; index < 100; ++index) {
        CY_REQUIRE(
            candidates.push_back(near_candidate(minter.mint(), RelevanceRule::CellMembership, 100))
                .has_value());
    }

    BandwidthBudget budget;
    budget.bytes_per_tick = 300;  // ten updates of thirty bytes
    budget.dormant_after_ticks = 0;

    cy::Array<ScheduledEntry> scheduled(memory);
    const auto report =
        scheduler.select(kPeer, candidates.span(), 1, budget, estimate, nullptr, scheduled);
    CY_REQUIRE(report.has_value());

    CY_CHECK_EQ(report.value().candidates, 100U);
    CY_CHECK_LE(report.value().bytes_planned, u64{300});
    CY_CHECK_EQ(report.value().selected, 10U);
    // The pressure went into step one of the order, not into an unexplained drop.
    CY_CHECK_EQ(report.value().frequency_reduced, 90U);
    CY_CHECK_EQ(report.value().deferred, 90U);
    CY_CHECK_EQ(report.value().precision_reduced, 0U);
    CY_CHECK_EQ(scheduled.size(), cy::usize{10});
}

CY_TEST_CASE("networking: an owned entity loses precision rather than being dropped") {
    cy::Allocator& memory = allocator();
    PriorityScheduler scheduler(memory);
    NetworkIdMinter minter(1);

    // Two entities the peer owns. Owned is the band that may not simply be deferred, so step two of
    // the order — reduce encoding precision — is what has to absorb the shortfall.
    cy::Array<Candidate> candidates(memory);
    CY_REQUIRE(candidates.push_back(near_candidate(minter.mint(), RelevanceRule::Ownership, 10))
                   .has_value());
    CY_REQUIRE(candidates.push_back(near_candidate(minter.mint(), RelevanceRule::Ownership, 20))
                   .has_value());

    BandwidthBudget budget;
    budget.bytes_per_tick = 45;  // one full update and half of another
    budget.dormant_after_ticks = 0;

    cy::Array<ScheduledEntry> scheduled(memory);
    const auto report =
        scheduler.select(kPeer, candidates.span(), 1, budget, estimate, nullptr, scheduled);
    CY_REQUIRE(report.has_value());

    CY_CHECK_EQ(report.value().selected, 2U);
    CY_CHECK_EQ(report.value().precision_reduced, 1U);
    CY_CHECK_EQ(report.value().frequency_reduced, 0U);
    CY_CHECK_EQ(report.value().deferred, 0U);
    CY_CHECK_LE(report.value().bytes_planned, u64{45});
    CY_REQUIRE_EQ(scheduled.size(), cy::usize{2});
    CY_CHECK_EQ(scheduled[0].precision_percent, 100U);
    CY_CHECK_LT(scheduled[1].precision_percent, 100U);
}

CY_TEST_CASE("networking: a low-priority entity is never starved") {
    cy::Allocator& memory = allocator();
    PriorityScheduler scheduler(memory);
    scheduler.set_distance_bands(300LL * 300LL, 800LL * 800LL);
    NetworkIdMinter minter(1);

    // One distant entity and ten owned ones that exactly consume the budget between them. Without
    // bounded staleness the distant one is last for ever, because its score never approaches an
    // owned entity's — which the case checks by running four hundred ticks of exactly that.
    //
    // Ten and not fifty, deliberately: at fifty the owned entities cannot meet their OWN two-tick
    // guarantee, every one of them is forced every tick, and the case would then be measuring an
    // over-subscribed budget rather than the staleness guarantee. A guarantee is only a guarantee
    // where the budget can meet it, and saying which case is which is the point of the number.
    cy::Array<Candidate> candidates(memory);
    const NetworkId distant = minter.mint();
    CY_REQUIRE(candidates.push_back(near_candidate(distant, RelevanceRule::CellMembership, 700))
                   .has_value());
    for (u32 index = 0; index < 10; ++index) {
        CY_REQUIRE(candidates.push_back(near_candidate(minter.mint(), RelevanceRule::Ownership, 10))
                       .has_value());
    }

    BandwidthBudget budget;
    budget.bytes_per_tick = 300;
    budget.dormant_after_ticks = 0;

    u64 last_seen = 0;
    u64 worst_gap = 0;
    bool ever_sent = false;
    cy::Array<ScheduledEntry> scheduled(memory);
    for (u64 tick = 1; tick <= 400; ++tick) {
        scheduled.clear();
        for (auto& candidate : candidates) {
            CY_REQUIRE(scheduler.note_changed(candidate.id, tick).has_value());
        }
        const auto report =
            scheduler.select(kPeer, candidates.span(), tick, budget, estimate, nullptr, scheduled);
        CY_REQUIRE(report.has_value());
        CY_REQUIRE(report.value().bytes_planned <= u64{300});
        for (auto& entry : scheduled) {
            CY_REQUIRE(scheduler.note_sent(kPeer, entry.id, tick).has_value());
            if (entry.id == distant) {
                if (ever_sent && tick - last_seen > worst_gap) {
                    worst_gap = tick - last_seen;
                }
                last_seen = tick;
                ever_sent = true;
            }
        }
    }

    CY_CHECK(ever_sent);
    // The distant entity falls in `Far`, whose guaranteed interval is 120 ticks. It was sent inside
    // that guarantee, every time, for four hundred ticks of saturation — and it was never sent
    // BECAUSE of its score, which stays far below the ten owned entities competing with it.
    const u64 guaranteed = scheduler.bands().of(NetworkLodBand::Far).guaranteed_interval_ticks;
    CY_CHECK_LE(worst_gap, guaranteed);
    CY_CHECK_GT(worst_gap, u64{0});
}

CY_TEST_CASE("networking: dormancy is signalled once, and re-armed when the entity changes") {
    cy::Allocator& memory = allocator();
    PriorityScheduler scheduler(memory);
    NetworkIdMinter minter(1);
    const NetworkId still = minter.mint();

    cy::Array<Candidate> candidates(memory);
    CY_REQUIRE(candidates.push_back(near_candidate(still, RelevanceRule::CellMembership, 100))
                   .has_value());

    BandwidthBudget budget;
    budget.bytes_per_tick = 4096;
    budget.dormant_after_ticks = 10;

    CY_REQUIRE(scheduler.note_changed(still, 1).has_value());

    u32 signals = 0;
    cy::Array<ScheduledEntry> scheduled(memory);
    for (u64 tick = 1; tick <= 60; ++tick) {
        scheduled.clear();
        const auto report =
            scheduler.select(kPeer, candidates.span(), tick, budget, estimate, nullptr, scheduled);
        CY_REQUIRE(report.has_value());
        for (auto& entry : scheduled) {
            if (entry.dormancy_signal) {
                ++signals;
                CY_CHECK(entry.band == NetworkLodBand::Dormant);
            }
            CY_REQUIRE(scheduler.note_sent(kPeer, entry.id, tick).has_value());
        }
    }
    // "dormancy explicitly signalled so the client does not treat them as lost" — once, not once a
    // tick for fifty ticks.
    CY_CHECK_EQ(signals, 1U);

    // It moves again: the entity wakes, and the next time it settles the client is told again.
    CY_REQUIRE(scheduler.note_changed(still, 61).has_value());
    signals = 0;
    for (u64 tick = 61; tick <= 120; ++tick) {
        scheduled.clear();
        const auto report =
            scheduler.select(kPeer, candidates.span(), tick, budget, estimate, nullptr, scheduled);
        CY_REQUIRE(report.has_value());
        for (auto& entry : scheduled) {
            if (entry.dormancy_signal) {
                ++signals;
            }
            CY_REQUIRE(scheduler.note_sent(kPeer, entry.id, tick).has_value());
        }
    }
    CY_CHECK_EQ(signals, 1U);
}

CY_TEST_CASE("networking: the budget holds as entity count scales") {
    // THE EXIT CRITERION'S OWN SHAPE. Four populations rather than one, because a budget asserted
    // at a single population is a budget nobody has scaled. The full curve — eight populations to
    // twenty thousand — is `src/networking/tools/cy_net_bandwidth_curve` and the figure it draws.
    static constexpr u32 kPopulations[] = {64, 256, 1024, 4096};
    cy::Allocator& memory = allocator();

    u32 previous_candidates = 0;
    u64 previous_peak = 0;
    for (const u32 population : kPopulations) {
        PriorityScheduler scheduler(memory);
        scheduler.set_distance_bands(300LL * 300LL, 800LL * 800LL);
        NetworkIdMinter minter(1);

        cy::Array<Candidate> candidates(memory);
        for (u32 index = 0; index < population; ++index) {
            CY_REQUIRE(candidates
                           .push_back(near_candidate(minter.mint(), RelevanceRule::CellMembership,
                                                     100 + static_cast<i64>(index % 900)))
                           .has_value());
        }

        BandwidthBudget budget;
        budget.bytes_per_tick = 4096;
        budget.dormant_after_ticks = 0;

        u64 peak = 0;
        cy::Array<ScheduledEntry> scheduled(memory);
        for (u64 tick = 1; tick <= 30; ++tick) {
            scheduled.clear();
            for (auto& candidate : candidates) {
                CY_REQUIRE(scheduler.note_changed(candidate.id, tick).has_value());
            }
            const auto report = scheduler.select(kPeer, candidates.span(), tick, budget, estimate,
                                                 nullptr, scheduled);
            CY_REQUIRE(report.has_value());
            // The criterion, at every tick of every population rather than at the end of one.
            CY_REQUIRE(report.value().bytes_planned <= u64{4096});
            peak = std::max(report.value().bytes_planned, peak);
            for (auto& entry : scheduled) {
                CY_REQUIRE(scheduler.note_sent(kPeer, entry.id, tick).has_value());
            }
        }

        // The candidate set grows with the world and the bytes do not grow with it. Both halves are
        // needed: a scheduler that sent nothing would satisfy the budget at every population.
        CY_CHECK_GT(population, previous_candidates);
        CY_CHECK_GT(peak, u64{0});
        if (previous_peak != 0) {
            CY_CHECK_LE(peak, u64{4096});
        }
        previous_candidates = population;
        previous_peak = peak;
    }
}
