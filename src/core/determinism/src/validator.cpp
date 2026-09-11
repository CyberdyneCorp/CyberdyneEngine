// The determinism validator: two runs in, one field on one entity out. M9 task 2.5.

#include <cy/core/determinism/validator.h>

#include <cy/core/memory/allocator.h>

#include <algorithm>

namespace cy::determinism {
namespace {

/// SplitMix64. A fixed constant rather than `cy::hash_bytes`, which is seeded per process in
/// development builds: a chaos set that differed between two runs of the same binary could not be
/// quoted in a bug report.
[[nodiscard]] u64 mix64(u64 value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

}  // namespace

Status TickHashComparison::observe(RunSide side, u64 tick, u64 root_hash) noexcept {
    Array<Sample>& samples = side == RunSide::Left ? left_ : right_;
    if (!samples.empty() && samples[samples.size() - 1].tick >= tick) {
        return fail(ErrorCode::InvalidArgument,
                    "determinism: tick hashes are observed in tick order; out of order, the first "
                    "disagreement reported would be the first one heard about rather than the "
                    "first that happened");
    }
    if (Status added = samples.push_back(Sample{tick, root_hash}); !added) {
        return added;
    }
    recompare();
    return ok();
}

void TickHashComparison::recompare() noexcept {
    first_diverging_ = kNoTick;
    last_agreeing_ = kNoTick;
    const usize count = std::min(left_.size(), right_.size());
    for (usize index = 0; index < count; ++index) {
        if (left_[index].tick != right_[index].tick) {
            // Two runs that reached different ticks are not comparable at this index. Reported as a
            // divergence at the earlier of the two, which is where the timelines parted.
            first_diverging_ =
                left_[index].tick < right_[index].tick ? left_[index].tick : right_[index].tick;
            return;
        }
        if (left_[index].hash != right_[index].hash) {
            first_diverging_ = left_[index].tick;
            return;
        }
        last_agreeing_ = left_[index].tick;
    }
}

u32 TickHashComparison::ticks_observed(RunSide side) const noexcept {
    return static_cast<u32>(side == RunSide::Left ? left_.size() : right_.size());
}

u32 TickHashComparison::ticks_compared() const noexcept {
    return static_cast<u32>(std::min(left_.size(), right_.size()));
}

bool TickHashComparison::truncated() const noexcept {
    return left_.size() != right_.size();
}

void TickHashComparison::clear() noexcept {
    left_.clear();
    right_.clear();
    first_diverging_ = kNoTick;
    last_agreeing_ = kNoTick;
}

void localise(const StateHashTree& left, const StateHashTree& right,
              FieldDivergence& out) noexcept {
    out = FieldDivergence{};
    StateHashTree::compare(left, right, out.path);
    out.diverged = out.path.diverged;
    out.shape_mismatch = out.path.shape_mismatch;
    out.left = out.path.left;
    out.right = out.path.right;
    out.depth = out.path.depth;
    if (!out.diverged) {
        return;
    }
    // A shape mismatch names the child that is missing, which the path cannot: the deepest node the
    // two trees share is that child's parent. Filled first so that the path's own levels, read
    // below, do not overwrite it with a shallower identity.
    if (out.path.missing_identified) {
        switch (out.path.missing_level) {
            case HashLevel::Entity:
                out.entity = out.path.missing_id;
                break;
            case HashLevel::Component:
                out.component = out.path.missing_id;
                out.component_name = out.path.missing_name;
                break;
            case HashLevel::Field:
                out.field = out.path.missing_id;
                out.field_name = out.path.missing_name;
                break;
            default:
                break;
        }
    }
    // The descent already stopped at the deepest node that could be compared. Reading the three
    // levels the report names out of the path is a lookup rather than a second walk.
    for (u32 index = 0; index < out.path.depth && index < kHashDepth; ++index) {
        switch (out.path.levels[index]) {
            case HashLevel::Entity:
                out.entity = out.path.ids[index];
                break;
            case HashLevel::Component:
                out.component = out.path.ids[index];
                out.component_name = out.path.names[index];
                break;
            case HashLevel::Field:
                out.field = out.path.ids[index];
                out.field_name = out.path.names[index];
                break;
            case HashLevel::World:
            case HashLevel::Subsystem:
            case HashLevel::Archetype:
            case HashLevel::Chunk:
                break;
        }
    }
}

ExecutionConditions chaos_conditions(u32 index, u64 seed, u32 max_workers) noexcept {
    const u64 draw = mix64(seed ^ (static_cast<u64>(index) * 0x2545F4914F6CDD1DULL));
    ExecutionConditions conditions;
    const u32 span = max_workers == 0 ? 1 : max_workers;
    conditions.worker_count = 1 + static_cast<u32>(draw % span);
    conditions.order_seed = mix64(draw);
    conditions.perturb_chunk_assignment = ((draw >> 40U) & 1U) != 0;
    conditions.perturb_allocator_layout = ((draw >> 41U) & 1U) != 0;
    return conditions;
}

namespace {

/// How many of the four dimensions differ across the set. The number `validate_scenario()` refuses
/// a zero of.
[[nodiscard]] u32 dimensions_varied(Span<const ExecutionConditions> conditions) noexcept {
    if (conditions.size() < 2) {
        return 0;
    }
    const ExecutionConditions& first = conditions[0];
    bool workers = false;
    bool order = false;
    bool chunks = false;
    bool layout = false;
    for (usize index = 1; index < conditions.size(); ++index) {
        workers = workers || conditions[index].worker_count != first.worker_count;
        order = order || conditions[index].order_seed != first.order_seed;
        chunks =
            chunks || conditions[index].perturb_chunk_assignment != first.perturb_chunk_assignment;
        layout =
            layout || conditions[index].perturb_allocator_layout != first.perturb_allocator_layout;
    }
    return static_cast<u32>(workers) + static_cast<u32>(order) + static_cast<u32>(chunks) +
           static_cast<u32>(layout);
}

}  // namespace

namespace {

/// Run the scenario once per condition, collecting each run's per-tick hashes.
[[nodiscard]] Status collect_runs(Allocator& allocator, ScenarioFn scenario, void* user,
                                  Span<const ExecutionConditions> conditions,
                                  Array<Array<u64>>& runs) noexcept {
    for (const ExecutionConditions& condition : conditions) {
        Array<u64> hashes(allocator);
        if (Status ran = scenario(user, condition, hashes); !ran) {
            return ran;
        }
        if (Status added = runs.push_back(static_cast<Array<u64>&&>(hashes)); !added) {
            return added;
        }
    }
    return ok();
}

/// Compare one pair of runs tick by tick, reporting the first disagreement.
[[nodiscard]] Status compare_pair(TickHashComparison& comparison, const Array<u64>& left,
                                  const Array<u64>& right, u32& ticks_compared) noexcept {
    comparison.clear();
    const usize count = std::min(left.size(), right.size());
    for (usize tick = 0; tick < count; ++tick) {
        if (Status observed = comparison.observe(RunSide::Left, tick, left[tick]); !observed) {
            return observed;
        }
        if (Status observed = comparison.observe(RunSide::Right, tick, right[tick]); !observed) {
            return observed;
        }
    }
    ticks_compared = std::max(static_cast<u32>(count), ticks_compared);
    return ok();
}

}  // namespace

Status validate_scenario(Allocator& allocator, ScenarioFn scenario, void* user,
                         Span<const ExecutionConditions> conditions,
                         ValidationOutcome& out) noexcept {
    out = ValidationOutcome{};
    if (scenario == nullptr) {
        return fail(ErrorCode::InvalidArgument, "determinism: no scenario to validate");
    }
    if (conditions.size() < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "determinism: a validation pass compares runs, so it needs at least two "
                    "execution conditions");
    }
    out.conditions_varied = dimensions_varied(conditions);
    if (out.conditions_varied == 0) {
        // A pass over identical conditions proves the scenario is a function of its input. It does
        // not prove the scenario is independent of execution order, which is the only thing this
        // validator exists to find — so it is refused rather than reported green.
        return fail(ErrorCode::InvalidArgument,
                    "determinism: every execution condition in this set is identical; such a pass "
                    "cannot surface an ordering dependency and must not report that it did");
    }

    Array<Array<u64>> runs(allocator);
    if (Status collected = collect_runs(allocator, scenario, user, conditions, runs); !collected) {
        return collected;
    }
    out.runs = static_cast<u32>(runs.size());

    TickHashComparison comparison(allocator);
    for (usize left = 0; left + 1 < runs.size(); ++left) {
        for (usize right = left + 1; right < runs.size(); ++right) {
            if (Status compared =
                    compare_pair(comparison, runs[left], runs[right], out.ticks_compared);
                !compared) {
                return compared;
            }
            if (comparison.diverged()) {
                out.diverged = true;
                out.first_diverging_tick = comparison.first_diverging_tick();
                out.left_run = static_cast<u32>(left);
                out.right_run = static_cast<u32>(right);
                return ok();
            }
        }
    }
    return ok();
}

}  // namespace cy::determinism
