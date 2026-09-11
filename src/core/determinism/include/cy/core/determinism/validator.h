#pragma once
// The determinism validator: two runs in, one field on one entity out. M9 task 2.5.
//
// `simulation-and-determinism` — "The determinism validator": the engine provides a validator that
// "runs a scenario more than once under deliberately different execution conditions — worker
// counts, task ordering, chunk assignment, allocator layout — with identical commands, and compares
// hashes per tick"; a "chaos scheduling mode" randomises permitted execution order "so that
// undeclared ordering dependencies surface rather than remaining latent"; and on divergence it
// "captures the window".
//
// ================================================================================================
// TWO HALVES, AND THEY ARE DIFFERENT SHAPES
// ================================================================================================
//
//   WHEN   `TickHashComparison` is a per-tick root hash from each side. Cheap — one `u64` per tick
//          per run — so a session can carry it and a lockstep peer can send it. It answers "which
//          tick first disagreed", and it is deliberately incapable of answering anything else,
//          because the thing that makes it cheap is that it throws the structure away.
//
//   WHERE  `localise()` takes two **full** hash trees, taken at the tick the first half named, and
//          descends them to a named field on a named entity. That is the whole reason the hash is
//          hierarchical: the descent is O(depth), so paying for the full tree at one tick costs
//          nothing like paying for it at every tick.
//
// The split is what makes the expensive half affordable: a session hashes roots every tick and
// trees on demand, and the validator tells it which tick to ask about.
//
// ================================================================================================
// THE WINDOW IS PART OF THE ANSWER, NOT A COURTESY
// ================================================================================================
//
// "On divergence, the validator SHALL capture the window: the last agreeing snapshot, the commands
// in between, the random trace, the systems that wrote the differing state, and the divergent
// values." `TickHashComparison` reports the tick boundary of that window — the last agreeing tick
// and the first disagreeing one — because that is what it can know. The *contents* of the window
// are the command log's, and `cy::replay::DivergenceWindow` (src/replay/divergence.h) assembles
// them: this is layer 0 and cannot name a command. Two files rather than one because the layering
// is real, and neither half is useful alone.
//
// ================================================================================================
// WHAT CHAOS SCHEDULING IS NOT
// ================================================================================================
//
// It is not a random number fed to the job system. `ExecutionConditions` below is a **description
// of a run's execution environment** that a scenario is obliged to honour, and the validator's job
// is to produce a set of them that differ in every dimension the requirement names while holding
// the commands identical. A scenario that ignores the conditions it is handed will agree with
// itself and prove nothing — so `ValidationOutcome::conditions_varied` counts how many dimensions
// actually differed across the runs, and `validate_scenario()` refuses a set that varies none.
// That refusal is the difference between this and a check that cannot fail.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// No tick. `0` is a real tick, so the absence has to be spelled.
inline constexpr u64 kNoTick = ~0ULL;

/// Which of the two runs a hash came from.
enum class RunSide : u8 {
    Left = 0,
    Right,
};

/// Per-tick root hashes from two runs, and the first tick they disagree at.
///
/// Records in tick order and refuses a tick out of order, because a comparison that accepted them
/// in any order would report the first disagreement it happened to be told about rather than the
/// first that happened.
class TickHashComparison {
public:
    explicit TickHashComparison(Allocator& allocator) noexcept
        : left_(allocator), right_(allocator) {}

    TickHashComparison(const TickHashComparison&) = delete;
    TickHashComparison& operator=(const TickHashComparison&) = delete;

    /// Refuses a tick at or before the last one recorded for that side.
    [[nodiscard]] Status observe(RunSide side, u64 tick, u64 root_hash) noexcept;

    [[nodiscard]] u32 ticks_observed(RunSide side) const noexcept;
    /// Ticks both sides recorded. Only these can disagree; a tick one side never reached is a
    /// different fact and `truncated()` reports it.
    [[nodiscard]] u32 ticks_compared() const noexcept;
    /// True when one run stopped before the other. Not a divergence, and not a clean agreement
    /// either — a run that crashed at tick 40 agrees with everything up to 40.
    [[nodiscard]] bool truncated() const noexcept;

    [[nodiscard]] bool diverged() const noexcept { return first_diverging_ != kNoTick; }
    [[nodiscard]] u64 first_diverging_tick() const noexcept { return first_diverging_; }
    /// The last tick both sides agreed on, or `kNoTick` when they disagreed at the first.
    [[nodiscard]] u64 last_agreeing_tick() const noexcept { return last_agreeing_; }

    void clear() noexcept;

private:
    struct Sample {
        u64 tick = 0;
        u64 hash = 0;
    };

    void recompare() noexcept;

    Array<Sample> left_;
    Array<Sample> right_;
    u64 first_diverging_ = kNoTick;
    u64 last_agreeing_ = kNoTick;
};

/// A divergence narrowed as far as the two trees allow.
///
/// Every identifier is reported beside its name: the name is what a person reads and the number is
/// what the report is *about*, because a component can be renamed without the state changing.
struct FieldDivergence {
    bool diverged = false;
    /// The descent stopped because the two trees have different shapes here — an entity in one run
    /// and not in the other. A completely different cause from a value mismatch, so it is a
    /// separate flag rather than a special value.
    bool shape_mismatch = false;

    /// Zero when the path carried no level of that kind. A subsystem's state has no entity.
    u64 entity = 0;
    u64 component = 0;
    const char* component_name = "";
    u64 field = 0;
    const char* field_name = "";

    /// The two hashes at the deepest node that could be compared.
    u64 left = 0;
    u64 right = 0;

    /// How deep the descent got. `kHashDepth` levels at most.
    u32 depth = 0;
    /// The raw descent, for a caller that wants the whole path rather than the four levels above.
    Divergence path;
};

/// Descend two trees taken at the same tick and name the field they disagree about.
void localise(const StateHashTree& left, const StateHashTree& right, FieldDivergence& out) noexcept;

// --- Chaos scheduling ---------------------------------------------------------------------------

/// The execution environment one validation run is obliged to honour.
///
/// Every field is one of the four dimensions `simulation-and-determinism` names. A scenario reads
/// them and arranges itself accordingly; one that does not will agree with itself, which is what
/// `conditions_varied` exists to catch.
struct ExecutionConditions {
    /// Workers the scenario runs its parallel work on. Never fewer than one.
    u32 worker_count = 1;
    /// Seeds the permitted reordering of independent work. Two runs with the same seed schedule
    /// identically; two with different seeds do not.
    u64 order_seed = 0;
    /// Assign rows to chunks differently. Allocator history that no reference can observe, and
    /// therefore state the hash must be indifferent to.
    bool perturb_chunk_assignment = false;
    /// Vary allocation addresses and reuse order.
    bool perturb_allocator_layout = false;

    friend constexpr bool operator==(const ExecutionConditions&,
                                     const ExecutionConditions&) noexcept = default;
};

/// `count` conditions that differ from one another in every dimension the requirement names.
///
/// Deterministic in `seed`: the same seed gives the same set, so a divergence found by a chaos run
/// is reproducible by quoting the seed and the index rather than by luck.
[[nodiscard]] ExecutionConditions chaos_conditions(u32 index, u64 seed, u32 max_workers) noexcept;

/// One run of the scenario. `hashes` is appended with one root hash per tick, in tick order.
///
/// A function pointer plus a user pointer rather than a template, so the validator is a compiled
/// function and a scenario can be chosen at run time — which is what a chaos mode driven by a
/// command-line seed needs.
using ScenarioFn = Status (*)(void* user, const ExecutionConditions& conditions,
                              Array<u64>& hashes) noexcept;

/// What one validation pass found.
struct ValidationOutcome {
    u32 runs = 0;
    u32 ticks_compared = 0;
    /// How many of the four dimensions actually differed across the conditions given. Zero means
    /// the pass could not have found an ordering dependency, and `validate_scenario()` refuses it.
    u32 conditions_varied = 0;
    bool diverged = false;
    u64 first_diverging_tick = kNoTick;
    /// The two runs that disagreed, as indices into the conditions given.
    u32 left_run = 0;
    u32 right_run = 0;
};

/// Run `scenario` once per condition and compare per-tick hashes across every pair.
///
/// Refuses a set of fewer than two conditions, and a set whose conditions are all identical: a pass
/// that ran the same environment twice proves the scenario is a function, not that it is
/// order-independent.
[[nodiscard]] Status validate_scenario(Allocator& allocator, ScenarioFn scenario, void* user,
                                       Span<const ExecutionConditions> conditions,
                                       ValidationOutcome& out) noexcept;

}  // namespace cy::determinism
