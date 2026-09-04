#pragma once
// Dumping and self-checking a compiled graph. Task 2.2.8.
//
// `rhi-and-render-graph`: "The graph SHALL be able to dump its structure — passes, resources,
// lifetimes, barriers, aliasing decisions — as text or a Graphviz diagram."
//
// Both dumps are written into a caller-owned buffer rather than returning a string: the engine has
// no owning string type in the containers M1 shipped, and a dump is produced at a moment — a crash
// artefact, a `--dump-graph` run, a failing test's message — where the caller already knows where
// the bytes should go.
//
// validate_plan() is the other half of "validation and debugging", and it is the half that earns
// its keep. M3's spike established that a validation layer will not catch a missing queue-family
// ownership transfer and will not catch a missing alias barrier: both produced zero errors and
// correct pixels on the device, and both are silently wrong on hardware that compresses. Those two
// properties have to be structural, so this function checks them against the plan itself — before a
// device ever sees it, and on the null backend in continuous integration.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering {

/// Append a human-readable dump of the plan to `out`, NUL-terminated. Returns the number of
/// characters written, not counting the terminator.
///
/// The format is the one M3's spike printed, because it is the one that was read a hundred times
/// while the model was being got right: submits, their waits, each pass and the barriers before it,
/// the ownership releases at the end of a submit, then the memory plan and the culled passes.
Expected<usize, Error> dump_text(const RenderGraph& graph, const CompiledGraph& plan,
                                 Array<char>& out) noexcept;

/// The same plan as a Graphviz digraph: passes as nodes clustered by submit, resource dependencies
/// as solid edges, cross-queue semaphores as bold edges, alias edges as dashed ones.
Expected<usize, Error> dump_graphviz(const RenderGraph& graph, const CompiledGraph& plan,
                                     Array<char>& out) noexcept;

/// What a plan check found. Every field is a count so that a test can assert on the shape of a plan
/// as well as on its correctness.
struct PlanAudit {
    u32 submits = 0;
    u32 passes = 0;
    u32 semaphore_waits = 0;
    u32 image_barriers = 0;
    u32 buffer_barriers = 0;
    u32 memory_barriers = 0;
    u32 ownership_releases = 0;
    u32 ownership_acquires = 0;
    u32 layout_transitions = 0;
    u32 alias_barriers = 0;
    u64 transient_peak_bytes = 0;
    u64 transient_naive_bytes = 0;
};

/// Check the plan against the invariants a validation layer does not check.
///
///   * every ownership RELEASE has exactly one matching ACQUIRE, with identical layouts, families
///     and subresource range — the two halves are derived from one hazard, and a mismatch means the
///     derivation drifted;
///   * every cross-queue dependency is a semaphore wait that points at an EARLIER submit;
///   * no wait names a value its queue never signals;
///   * every transient's placement stays inside the reserved pool and no two placements with
///     overlapping lifetimes overlap in memory;
///   * a transient whose memory is reused is either preceded by an alias barrier or ordered by a
///     semaphore.
///
/// Returns the audit on success and names the first violated invariant on failure.
Expected<PlanAudit, Error> validate_plan(const RenderGraph& graph,
                                         const CompiledGraph& plan) noexcept;

}  // namespace cy::rendering
