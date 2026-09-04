#pragma once
// Deterministic parallel primitives. Task 3.2.10.
//
// `core-jobs-and-concurrency`: "The engine SHALL provide parallel primitives whose results do not
// depend on execution order: parallel_for, parallel_reduce, parallel_scan, and parallel_sort.
// Reductions SHALL use a fixed combination order derived from the partitioning, not from completion
// order, so floating-point results are reproducible."
//
// THE ONE IDEA THESE FOUR SHARE. The partitioning is a pure function of the element count and the
// grain — never of the worker count, never of how many partitions happened to finish — and every
// combination is performed in partition-index order. A partition's *work* runs wherever a worker
// picks it up; a partition's *result* is combined exactly where its index says. Floating-point
// addition is not associative, so a reduction that folded results in completion order would give a
// different last bit on a busy machine than on an idle one, and `simulation-and-determinism` would
// have nothing to build on.
//
// These are the blocking forms: they submit and then wait, and the waiting thread runs other ready
// tasks rather than idling. That is what a stage wants. A caller that needs the work in flight
// while it does something else submits through `JobSystem::submit_parallel_for` directly.

#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/types.h>

#include <algorithm>
#include <type_traits>

namespace cy::jobs {

/// The most partitions a reduction, scan or sort is split into.
///
/// Lower than `kMaxParallelPartitions` because each of the three keeps one accumulator per
/// partition on the stack, and 256 of anything reasonable is a few kilobytes rather than tens.
inline constexpr u64 kMaxReductionPartitions = 256;

/// The grain a reduction actually uses: the caller's, raised until the partition count fits the
/// accumulator array. A pure function of the count and the grain, so the partitioning — and
/// therefore the combination order — is the same on every machine.
constexpr u64 reduction_grain(u64 count, u64 grain) noexcept {
    const u64 minimum = (count + kMaxReductionPartitions - 1) / kMaxReductionPartitions;
    const u64 requested = grain == 0 ? 1 : grain;
    const u64 chosen = requested > minimum ? requested : minimum;
    return chosen == 0 ? 1 : chosen;
}

/// An indexed parallel loop over a callable.
///
/// `fn` is invoked as `fn(const TaskContext&, u64 begin, u64 end)` once per partition, with a
/// half-open range. It must be safe to call from several workers at once — which is exactly what
/// the access declarations in access.h exist to make checkable.
template <class Fn>
Status parallel_for(JobSystem& jobs, u64 count, u64 grain, Fn& fn,
                    const char* name = "parallel_for",
                    Priority priority = Priority::Normal) noexcept {
    if (count == 0) {
        return ok();
    }
    // A captureless lambda converts to a function pointer, which is what JobBody is: a
    // std::function would allocate, and scheduling must not.
    const ParallelForBody body = [](const TaskContext& context, u64 begin, u64 end,
                                    void* user) noexcept {
        (*static_cast<Fn*>(user))(context, begin, end);
    };
    auto handle = jobs.submit_parallel_for(count, grain, body, &fn, name, priority);
    if (!handle) {
        return fail(handle.error().code, handle.error().message, handle.error().system_code);
    }
    jobs.wait(handle.value());
    return ok();
}

/// A reduction whose result is bit-identical across runs and across worker counts.
///
/// `map(u64 index) -> T` produces one element's contribution; `combine(T, T) -> T` folds two.
/// Partitions are folded left-to-right *by index*, and each partition folds its own range
/// left-to-right, so the whole combination order is fixed by the partitioning alone.
template <class T, class Map, class Combine>
Expected<T, cy::Error> parallel_reduce(JobSystem& jobs, u64 count, u64 grain, T identity, Map& map,
                                       Combine& combine, const char* name = "parallel_reduce",
                                       Priority priority = Priority::Normal) noexcept {
    static_assert(std::is_trivially_destructible_v<T>,
                  "a reduction's accumulators live in a fixed array and are never destructed");
    if (count == 0) {
        return identity;
    }

    const u64 effective_grain = reduction_grain(count, grain);
    const u64 partitions = JobSystem::partition_count(count, effective_grain);
    const u64 chunk = (count + partitions - 1) / partitions;

    T partials[kMaxReductionPartitions];
    for (u64 i = 0; i < partitions; ++i) {
        partials[i] = identity;
    }

    // Which partition a range belongs to is recovered from its first index rather than carried:
    // the partitioning is a pure function of the count and the grain, so `begin / chunk` is exact.
    auto partition_body = [&](const TaskContext&, u64 begin, u64 end) noexcept {
        T local = identity;
        for (u64 i = begin; i < end; ++i) {
            local = combine(local, map(i));
        }
        partials[begin / chunk] = local;
    };

    if (auto status = parallel_for(jobs, count, effective_grain, partition_body, name, priority);
        !status) {
        return fail(status.error().code, status.error().message, status.error().system_code);
    }

    // The fixed combination order. Left to right by partition index, on this thread, after every
    // partition has finished — never in completion order.
    T result = identity;
    for (u64 i = 0; i < partitions; ++i) {
        result = combine(result, partials[i]);
    }
    return result;
}

/// An exclusive scan: `output[i]` is the combination of `input[0 .. i)`, with `identity` at zero.
///
/// Three passes — per-partition totals, a sequential scan of those totals in index order, then each
/// partition's own scan from its offset. The middle pass is the deterministic one: it is what makes
/// the result independent of which partition finished first.
template <class T, class Combine>
Status parallel_exclusive_scan(JobSystem& jobs, const T* input, T* output, u64 count, u64 grain,
                               T identity, Combine& combine, const char* name = "parallel_scan",
                               Priority priority = Priority::Normal) noexcept {
    static_assert(std::is_trivially_destructible_v<T>,
                  "a scan's partition totals live in a fixed array and are never destructed");
    if (count == 0) {
        return ok();
    }
    if (input == nullptr || output == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a scan needs an input and an output");
    }

    const u64 effective_grain = reduction_grain(count, grain);
    const u64 partitions = JobSystem::partition_count(count, effective_grain);
    const u64 chunk = (count + partitions - 1) / partitions;

    T totals[kMaxReductionPartitions];
    T offsets[kMaxReductionPartitions];
    for (u64 i = 0; i < partitions; ++i) {
        totals[i] = identity;
        offsets[i] = identity;
    }

    auto sum_partition = [&](const TaskContext&, u64 begin, u64 end) noexcept {
        T local = identity;
        for (u64 i = begin; i < end; ++i) {
            local = combine(local, input[i]);
        }
        totals[begin / chunk] = local;
    };
    if (auto status = parallel_for(jobs, count, effective_grain, sum_partition, name, priority);
        !status) {
        return status;
    }

    T running = identity;
    for (u64 i = 0; i < partitions; ++i) {
        offsets[i] = running;
        running = combine(running, totals[i]);
    }

    auto scan_partition = [&](const TaskContext&, u64 begin, u64 end) noexcept {
        T running_local = offsets[begin / chunk];
        for (u64 i = begin; i < end; ++i) {
            output[i] = running_local;
            running_local = combine(running_local, input[i]);
        }
    };
    return parallel_for(jobs, count, effective_grain, scan_partition, name, priority);
}

/// A sort whose result does not depend on execution order.
///
/// Each partition is sorted in parallel, then the sorted runs are merged pairwise on the calling
/// thread. `scratch` must hold `count` elements; the caller supplies it so that sorting allocates
/// nothing — which is also why the per-partition sort is `std::sort` rather than
/// `std::stable_sort`, since the latter allocates a temporary buffer of its own.
///
/// The merge takes from the left run when two elements compare equal, so the *merge* is stable. The
/// per-partition sort is not, so two elements a comparator cannot distinguish may swap within a
/// partition. The result is still identical on every run of a given build; a caller that needs it
/// identical across builds, or that carries a payload the comparator ignores, puts a tie-break in
/// the comparator.
template <class T, class Less>
Status parallel_sort(JobSystem& jobs, T* data, T* scratch, u64 count, u64 grain, Less& less,
                     const char* name = "parallel_sort",
                     Priority priority = Priority::Normal) noexcept {
    if (count < 2) {
        return ok();
    }
    if (data == nullptr || scratch == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a parallel sort needs its data and its scratch");
    }

    const u64 effective_grain = reduction_grain(count, grain);
    const u64 partitions = JobSystem::partition_count(count, effective_grain);

    auto sort_partition = [&](const TaskContext&, u64 begin, u64 end) noexcept {
        std::sort(data + begin, data + end, less);
    };
    if (auto status = parallel_for(jobs, count, effective_grain, sort_partition, name, priority);
        !status) {
        return status;
    }

    // Merge the runs pairwise, in index order. Sequential on purpose: it is O(n log p) over a small
    // p, and a parallel merge would reintroduce exactly the ordering question this exists to avoid.
    const u64 chunk = (count + partitions - 1) / partitions;
    u64 width = chunk;
    T* source = data;
    T* target = scratch;
    while (width < count) {
        for (u64 begin = 0; begin < count; begin += 2 * width) {
            const u64 middle = begin + width < count ? begin + width : count;
            const u64 end = begin + (2 * width) < count ? begin + (2 * width) : count;
            u64 left = begin;
            u64 right = middle;
            u64 out = begin;
            while (left < middle && right < end) {
                // `!less(right, left)` rather than `less(left, right)`: equal elements take the
                // left run, which is what keeps the merge stable.
                target[out++] =
                    !less(source[right], source[left]) ? source[left++] : source[right++];
            }
            while (left < middle) {
                target[out++] = source[left++];
            }
            while (right < end) {
                target[out++] = source[right++];
            }
        }
        T* swap = source;
        source = target;
        target = swap;
        width *= 2;
    }

    if (source != data) {
        for (u64 i = 0; i < count; ++i) {
            data[i] = source[i];
        }
    }
    return ok();
}

}  // namespace cy::jobs
