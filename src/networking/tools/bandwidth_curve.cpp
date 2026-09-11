// The bandwidth curve: measured against entity count, not asserted at one population.
// M9 task 4.4.
//
// Prints CSV to standard output. `curve.py` draws `docs/design/images/m9-bandwidth-curve.png` from
// it and `src/networking/README.md` pastes the run, so the committed figure and the numbers behind
// it come from the same execution.
//
// ================================================================================================
// WHAT IS VARIED AND WHAT IS HELD
// ================================================================================================
//
// The world's population grows by a factor of two hundred across the run — 100 entities to 20 000.
// The peer's budget, its interest cells, its relevance distance and its band table do not move.
// That is the question the exit criterion asks ("Bandwidth stays within budget **as entity count
// scales**") and it is only a question if the peer's own configuration is the constant.
//
// The entities are laid out on a square lattice spanning the same two kilometres whatever the
// population, so a larger population is a **denser** world rather than a bigger one: the peer's
// sixteen cells of sixty-four hold a quarter of it at every size, and the candidate set grows with
// the world while the budget does not. That is the pressure the degradation order exists for.
//
// ================================================================================================
// THE MEASUREMENT IS THE MAXIMUM, NOT THE LAST TICK
// ================================================================================================
//
// A per-tick budget is a claim about every tick. Reporting the last one would report whichever
// phase of the band intervals tick sixty happened to land in — an honest-looking zero, at the
// population where the budget is under the most pressure. So the run takes the **maximum** bytes
// planned across sixty ticks and the tool exits non-zero if any tick exceeded the budget.

#include <cy/core/memory/system_allocator.h>
#include <cy/networking/interest.h>
#include <cy/networking/scheduler.h>

#include <algorithm>
#include <cstdio>

namespace {

using namespace cy;
using namespace cy::net;

/// Every entity's update is estimated at the same size, so the curve measures the scheduler's
/// selection rather than a schema's compression. Thirty bytes is `CompiledSchema::bits_for()` on
/// the sample schema in `src/networking/tests/fixture.h` with every field present: 234 bits.
constexpr u32 kBytesPerUpdate = 30;
constexpr u32 kTicks = 60;
constexpr i64 kWorldHalfExtent = 1000;
constexpr u32 kCellsAcross = 8;

u32 estimate(void* /*user*/, NetworkId /*id*/, u32 precision_percent) noexcept {
    const u32 scaled =
        (kBytesPerUpdate * (precision_percent == 0 ? 100U : precision_percent)) / 100U;
    return scaled == 0 ? 1U : scaled;
}

/// A square lattice over a fixed extent. No random number generator, so two runs of this tool print
/// the same numbers — which is the same property `LocalNetwork`'s condition simulator has and for
/// the same reason.
[[nodiscard]] u32 lattice_side(u32 population) noexcept {
    u32 side = 1;
    while (side * side < population) {
        ++side;
    }
    return side;
}

struct Sample {
    u32 population = 0;
    u32 candidates = 0;
    u32 examined = 0;
    u32 peak_selected = 0;
    u64 selected_total = 0;
    u64 deferred_total = 0;
    u64 frequency_reduced_total = 0;
    u64 precision_reduced_total = 0;
    u64 forced_by_staleness_total = 0;
    u64 peak_bytes = 0;
    u64 bytes_budget = 0;
    /// The longest any candidate waited between two sends, in ticks. Bounded staleness as a number:
    /// a scheduler that starved its tail would show it here rather than in a player's report.
    u64 worst_gap_ticks = 0;
};

[[nodiscard]] bool measure(u32 population, Sample& out) noexcept {
    Allocator& allocator = system_allocator(MemoryDomain::World);
    InterestSet interest(allocator);
    PriorityScheduler scheduler(allocator);
    scheduler.set_distance_bands(300LL * 300LL, 800LL * 800LL);

    const u32 side = lattice_side(population);
    const i64 step = (2 * kWorldHalfExtent) / static_cast<i64>(side == 1 ? 1 : side - 1);

    NetworkIdMinter minter(1);
    for (u32 index = 0; index < population; ++index) {
        const i64 column = static_cast<i64>(index % side);
        const i64 row = static_cast<i64>(index / side);
        RelevanceSubject subject;
        subject.id = minter.mint();
        subject.position_x = -kWorldHalfExtent + (column * step);
        subject.position_y = -kWorldHalfExtent + (row * step);
        const u64 cell_x =
            static_cast<u64>((subject.position_x + kWorldHalfExtent) *
                             static_cast<i64>(kCellsAcross) / ((2 * kWorldHalfExtent) + 1));
        const u64 cell_y =
            static_cast<u64>((subject.position_y + kWorldHalfExtent) *
                             static_cast<i64>(kCellsAcross) / ((2 * kWorldHalfExtent) + 1));
        subject.cell = cell_x + (cell_y * kCellsAcross);
        subject.importance = index % 7;
        if (!interest.place(subject)) {
            return false;
        }
    }

    const PeerId peer = PeerId::make(1, 1);
    // The central four-by-four block: sixteen cells of sixty-four, held constant while the world
    // grows denser inside them.
    CellId cells[16] = {};
    u32 written = 0;
    for (u64 y = 2; y < 6; ++y) {
        for (u64 x = 2; x < 6; ++x) {
            cells[written++] = x + (y * kCellsAcross);
        }
    }
    if (!interest.set_interest_cells(peer, Span<const CellId>(cells, 16))) {
        return false;
    }

    PeerInterest view;
    view.peer = peer;
    view.relevance_distance_squared = 1200LL * 1200LL;

    BandwidthBudget budget;
    budget.bytes_per_tick = 8192;
    budget.dormant_after_ticks = 0;  // Dormancy is the other measurement; hold it out of this one.

    Array<Candidate> candidates(allocator);
    Array<NetworkId> left(allocator);
    Array<ScheduledEntry> scheduled(allocator);
    HashMap<u64, u64> last_sent(allocator);

    out = Sample{};
    out.population = population;
    out.bytes_budget = budget.bytes_per_tick;

    for (u64 tick = 1; tick <= kTicks; ++tick) {
        candidates.clear();
        left.clear();
        scheduled.clear();
        Expected<RelevanceDelta, Error> delta = interest.evaluate(view, candidates, left);
        if (!delta) {
            return false;
        }
        // Everything moves every tick, which is the worst case for a scheduler: nothing is idle and
        // nothing goes dormant, so every candidate is genuinely owed an update.
        for (auto& candidate : candidates) {
            if (!scheduler.note_changed(candidate.id, tick)) {
                return false;
            }
        }
        Expected<SchedulerReport, Error> selected =
            scheduler.select(peer, candidates.span(), tick, budget, estimate, nullptr, scheduled);
        if (!selected) {
            return false;
        }
        const SchedulerReport& report = selected.value();
        for (auto& entry : scheduled) {
            const NetworkId id = entry.id;
            if (!scheduler.note_sent(peer, id, tick)) {
                return false;
            }
            if (u64* previous = last_sent.find(id.value()); previous != nullptr) {
                const u64 gap = tick - *previous;
                out.worst_gap_ticks = std::max(gap, out.worst_gap_ticks);
                *previous = tick;
            } else if (!last_sent.insert(id.value(), tick)) {
                return false;
            }
        }

        out.candidates = report.candidates;
        out.examined = static_cast<u32>(interest.examined_last());
        out.selected_total += report.selected;
        out.deferred_total += report.deferred;
        out.frequency_reduced_total += report.frequency_reduced;
        out.precision_reduced_total += report.precision_reduced;
        out.forced_by_staleness_total += report.forced_by_staleness;
        out.peak_selected = std::max(report.selected, out.peak_selected);
        out.peak_bytes = std::max(report.bytes_planned, out.peak_bytes);
        if (report.bytes_planned > report.bytes_budget) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    static constexpr cy::u32 kPopulations[] = {100, 250, 500, 1000, 2500, 5000, 10000, 20000};

    std::printf(
        "population,candidates,examined,peak_selected,selected_total,deferred_total,"
        "frequency_reduced_total,precision_reduced_total,forced_by_staleness_total,peak_bytes,"
        "bytes_budget,worst_gap_ticks\n");
    for (const cy::u32 population : kPopulations) {
        Sample sample;
        if (!measure(population, sample)) {
            // The instrument reports the failure rather than printing a curve that looks fine.
            std::fprintf(stderr,
                         "bandwidth_curve: the measurement failed at %u entities (the budget was "
                         "exceeded, or an allocation failed)\n",
                         population);
            return 1;
        }
        std::printf("%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", sample.population,
                    sample.candidates, sample.examined, sample.peak_selected,
                    static_cast<unsigned long long>(sample.selected_total),
                    static_cast<unsigned long long>(sample.deferred_total),
                    static_cast<unsigned long long>(sample.frequency_reduced_total),
                    static_cast<unsigned long long>(sample.precision_reduced_total),
                    static_cast<unsigned long long>(sample.forced_by_staleness_total),
                    static_cast<unsigned long long>(sample.peak_bytes),
                    static_cast<unsigned long long>(sample.bytes_budget),
                    static_cast<unsigned long long>(sample.worst_gap_ticks));
    }
    return 0;
}
