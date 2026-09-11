// M9 TASK 2.5 — chaos scheduling: the same commands under deliberately different execution.
//
// `integration` rather than `unit` because every case runs a scenario several times over; the unit
// tier's millisecond is for arithmetic, not for a validation pass.
//
// ================================================================================================
// THE WORKLOAD IS AN ORDERING DEPENDENCY, NOT A SIMULATION OF ONE
// ================================================================================================
//
// The reduction below sums values whose magnitudes differ by fifteen orders. Floating-point
// addition is not associative, so summing them in a different order gives a **different number** —
// that is the whole of what an undeclared ordering dependency is, and it is real arithmetic rather
// than a flag the test sets to make itself fail.
//
// The stable variant reduces partitions in partition-index order, which is
// `simulation-and-determinism`'s "Ordering keys SHALL be built from stable logical identity —
// system, partition, local sequence — and SHALL NOT include worker or thread identity". The
// unstable variant reduces them in the order the scheduler happened to finish them, which is what
// `ExecutionConditions::order_seed` stands in for. One of them survives chaos and the other does
// not, and both are run so that "the validator found nothing" is distinguishable from "the
// validator cannot find anything".

#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/validator.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <algorithm>

namespace {

using namespace cy;
using namespace cy::determinism;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr u32 kValues = 64;
constexpr u64 kTicks = 24;

/// **Partitions are a fixed logical identity, not a worker count.** That is the requirement itself:
/// "Ordering keys SHALL be built from stable logical identity — system, partition, local sequence —
/// and SHALL NOT include worker or thread identity, since with work stealing a worker's identity is
/// a function of timing." So the work is always split eight ways whatever the machine is, and what
/// the worker count and the order seed decide is only which worker finishes which partition first.
///
/// The first version of this file partitioned by `worker_count`, and it was wrong in an instructive
/// way: the *stable* variant diverged too, because regrouping the values changes which of them are
/// added together and floating-point addition is not associative. That is a real defect and it is
/// the one this arrangement is designed to make impossible.
constexpr u32 kPartitions = 8;

struct Workload {
    /// True: commit partitions by stable partition index. False: commit them in completion order.
    bool stable_order = true;
};

[[nodiscard]] u64 shuffle(u64 seed, u32 index) noexcept {
    u64 value = seed + (static_cast<u64>(index) * 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    return value ^ (value >> 31U);
}

/// One run. The *inputs* are identical in every run — the requirement's "with identical commands";
/// only the execution environment differs.
Status run_scenario(void* user, const ExecutionConditions& conditions,
                    Array<u64>& hashes) noexcept {
    const auto& workload = *static_cast<const Workload*>(user);

    for (u64 tick = 0; tick < kTicks; ++tick) {
        // Per-partition accumulation. Identical in every run: a value's partition is a function of
        // its own index, so no amount of chaos changes which values are added together.
        f64 partial[kPartitions] = {};
        for (u32 index = 0; index < kValues; ++index) {
            const u32 partition = index % kPartitions;
            // ONE HUGE POSITIVE PARTITION, ONE HUGE NEGATIVE, AND SIX ORDINARY ONES.
            //
            // The first version of this workload used magnitudes fifteen orders apart and did NOT
            // diverge, and the reason is worth keeping: every partial was an integer below 2^53 and
            // every partial sum stayed on a representable grid, so the order genuinely did not
            // matter. Nothing was wrong with the validator; the workload had no ordering dependency
            // to find.
            //
            // At 1e300 the loss is unavoidable. Summing the two huge partials first cancels them
            // exactly and leaves the six ordinary ones; summing an ordinary one first and the huge
            // pair afterwards throws it away, because 1e300's ulp is about 1e284. Two orders, two
            // different numbers, and no flag anywhere.
            f64 value = 1.0 + static_cast<f64>(tick);
            if (partition == 0) {
                value = 1.0e300;
            } else if (partition == 1) {
                value = -1.0e300;
            }
            partial[partition] += value;
        }

        // Completion order: which worker got there first, seeded by the schedule. This is the half
        // that must not decide an authoritative result.
        u32 order[kPartitions] = {};
        for (u32 index = 0; index < kPartitions; ++index) {
            order[index] = index;
        }
        if (!workload.stable_order) {
            const u64 seed =
                conditions.order_seed ^ (static_cast<u64>(conditions.worker_count) << 8U);
            for (u32 index = kPartitions; index > 1; --index) {
                const u32 swap = static_cast<u32>(shuffle(seed, index) % index);
                const u32 held = order[index - 1];
                order[index - 1] = order[swap];
                order[swap] = held;
            }
        }
        f64 total = 0.0;
        for (const u32 partition : order) {
            total += partial[partition];
        }

        StateHashTree tree(allocator());
        if (Status opened = tree.begin(HashLevel::World, 0, "reduction"); !opened) {
            return opened;
        }
        tree.mix_f64(total);
        if (Status closed = tree.end(); !closed) {
            return closed;
        }
        if (Status added = hashes.push_back(tree.root_hash()); !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status chaos_set(Array<ExecutionConditions>& out, u32 count, u64 seed) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (Status added = out.push_back(chaos_conditions(index, seed, 8)); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

CY_TEST_CASE("determinism: chaos scheduling finds an ordering dependency and names its tick") {
    Array<ExecutionConditions> conditions(allocator());
    CY_REQUIRE(chaos_set(conditions, 6, 0xD15EA5EULL).has_value());

    Workload unstable{false};
    ValidationOutcome outcome;
    CY_REQUIRE(validate_scenario(allocator(), &run_scenario, &unstable, conditions.span(), outcome)
                   .has_value());
    // "WHEN a system's result depends on execution order THEN running under chaos scheduling SHALL
    // produce differing hashes and identify the first diverging tick."
    CY_CHECK(outcome.diverged);
    CY_CHECK_NE(outcome.first_diverging_tick, kNoTick);
    CY_CHECK_LT(outcome.first_diverging_tick, kTicks);
    CY_CHECK_GE(outcome.conditions_varied, 1U);
    CY_CHECK_NE(outcome.left_run, outcome.right_run);
}

CY_TEST_CASE("determinism: a stable ordering key survives the same chaos set") {
    // The control. Without it, the case above would pass on a validator that reported a divergence
    // for everything — which is the same defect as one that reports a divergence for nothing.
    Array<ExecutionConditions> conditions(allocator());
    CY_REQUIRE(chaos_set(conditions, 6, 0xD15EA5EULL).has_value());

    Workload stable{true};
    ValidationOutcome outcome;
    CY_REQUIRE(validate_scenario(allocator(), &run_scenario, &stable, conditions.span(), outcome)
                   .has_value());
    CY_CHECK_FALSE(outcome.diverged);
    CY_CHECK_EQ(outcome.runs, 6U);
    CY_CHECK_EQ(outcome.ticks_compared, static_cast<u32>(kTicks));
    // "Determinism is not serialisation": the pass ran at several worker counts and none of them
    // was forced to one.
    u32 highest = 0;
    for (const ExecutionConditions& one : conditions.span()) {
        highest = std::max(one.worker_count, highest);
    }
    CY_CHECK_GT(highest, 1U);
}

CY_TEST_CASE("determinism: a validation pass that varies nothing is refused, not reported green") {
    // A pass over identical conditions proves the scenario is a function of its input. It does not
    // prove the scenario is independent of execution order, which is the only thing this validator
    // exists to find.
    Array<ExecutionConditions> identical(allocator());
    ExecutionConditions one;
    one.worker_count = 4;
    one.order_seed = 7;
    CY_REQUIRE(identical.push_back(one).has_value());
    CY_REQUIRE(identical.push_back(one).has_value());
    CY_REQUIRE(identical.push_back(one).has_value());

    Workload unstable{false};
    ValidationOutcome outcome;
    CY_CHECK_FALSE(
        validate_scenario(allocator(), &run_scenario, &unstable, identical.span(), outcome)
            .has_value());
    CY_CHECK_EQ(outcome.conditions_varied, 0U);
    CY_CHECK_FALSE(outcome.diverged);

    // And one condition is not a comparison at all.
    Array<ExecutionConditions> single(allocator());
    CY_REQUIRE(single.push_back(one).has_value());
    CY_CHECK_FALSE(validate_scenario(allocator(), &run_scenario, &unstable, single.span(), outcome)
                       .has_value());
}
