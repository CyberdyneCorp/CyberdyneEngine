#pragma once
// Pipeline diagnostics: what the frame did, per pass and per view. Task 4.3.4.
//
// `rendering-forward-clustered` — "Pipeline diagnostics": "The renderer SHALL report per frame:
// cluster occupancy statistics (average and maximum elements per cluster, overflow count), draw
// calls and triangles per pass, sort key distribution, pipeline cache hits and misses, and per-pass
// GPU time."
//
// ================================================================================================
// THE SORT KEY DISTRIBUTION IS THE ONE WORTH EXPLAINING
// ================================================================================================
//
// "Sort key distribution" sounds like a histogram, and a histogram of 64-bit keys tells nobody
// anything. What a reader actually wants to know is whether the sort DID ITS JOB, and the sort's
// job is stated as a scenario: "WHEN the opaque list is sorted and submitted THEN all draws sharing
// a pipeline SHALL be adjacent, and within them all sharing a material."
//
// That is measurable exactly: count the pipeline CHANGES along the submitted order and compare
// against the number of DISTINCT pipelines. They are equal if and only if every pipeline's draws
// are contiguous. So `analyse_sort_keys` reports both numbers, `SortKeyDistribution::grouped()` is
// the comparison, and a test asserts it — which turns a scenario into an assertion rather than into
// a screenshot of a profiler.
//
// The same pair for materials, and the layer histogram beside them, because "how many draws were
// transparent" is the first question anyone asks about a frame that is slow.
//
// ================================================================================================
// EVERY GPU TIMING CARRIES WHETHER IT WAS MEASURED
// ================================================================================================
//
// Copied deliberately from `render::PassTiming`, which explains it: a device without timestamp
// queries reports no GPU time, and zero is not the same answer as "not measured". A report that
// cannot tell them apart is a report that says a pass is free.

#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/cull.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/lighting/shadow_atlas.h>
#include <cy/servers/render/sort.h>
#include <cy/servers/render/statistics.h>
#include <cy/servers/render/types.h>

namespace cy::rendering {

/// What one pass cost and drew.
struct PassDiagnostics {
    FramePassKind kind = FramePassKind::Count;
    /// The pass's own literal, borrowed. A pass name outlives the frame that recorded it because it
    /// is a string literal in the code that declared the pass.
    const char* name = "";
    u32 draw_calls = 0;
    /// How many draws automatic instancing merged away. The pair `(draw_calls, draws_merged)` is
    /// what makes instancing's benefit a measurement rather than a claim.
    u32 draws_merged = 0;
    u64 triangles = 0;
    u64 gpu_nanoseconds = 0;
    bool gpu_measured = false;
};

/// What the sort achieved. See the header comment for why these are the numbers.
struct SortKeyDistribution {
    u32 draws = 0;
    u32 layer_draws[render::kSortLayerCount] = {};
    /// Contiguous runs of one pipeline, and of one material within a pipeline, along the submitted
    /// order. On a grouped list these ARE the distinct counts; on an ungrouped one they are larger,
    /// and the difference is how many extra state changes the frame paid for.
    u32 pipeline_runs = 0;
    u32 material_runs = 0;
    /// Whether the keys are non-decreasing — that is, whether the list is in sort-key order.
    ///
    /// THIS IS THE EXACT TEST, and it is exact because the key's fields are ordered: layer, then
    /// pipeline, then material, then mesh. A non-decreasing key sequence therefore has every
    /// pipeline's draws contiguous and, inside each, every material's — which is precisely the
    /// scenario. Counting distinct values instead would need a set, and a frame has a hundred
    /// thousand draws.
    bool keys_non_decreasing = false;

    /// Whether the list is grouped as the sort promises: pipelines contiguous, and materials
    /// contiguous within them.
    [[nodiscard]] constexpr bool grouped() const noexcept { return keys_non_decreasing; }
};

/// Counted over a SORTED list. Over an unsorted one the numbers are still correct and `grouped()`
/// is simply false, which is the honest answer rather than an assertion failure.
///
/// One pass, no allocation and no set: `keys_non_decreasing` is the exact answer to the question
/// the scenario asks, and the run counts are what a report prints beside it.
[[nodiscard]] SortKeyDistribution analyse_sort_keys(Span<const render::DrawItem> draws) noexcept;

/// `shader-system`'s pipeline cache, as the frame report sees it.
struct PipelineCacheDiagnostics {
    u32 hits = 0;
    u32 misses = 0;
    u32 builds = 0;
    /// Requests served by the fallback material while a real pipeline compiled. A frame with a
    /// non-zero count here is a frame that showed the wrong thing somewhere, which is worth knowing
    /// even though nothing crashed.
    u32 fallbacks = 0;
};

/// One frame's report for one view.
struct FrameDiagnostics {
    explicit FrameDiagnostics(Allocator& allocator) noexcept : passes(allocator) {}

    FrameDiagnostics(const FrameDiagnostics&) = delete;
    FrameDiagnostics& operator=(const FrameDiagnostics&) = delete;

    ClusterStatistics clusters{};
    CullStatistics cull{};
    ShadowAtlasStatistics shadows{};
    SortKeyDistribution sort{};
    PipelineCacheDiagnostics pipelines{};
    Array<PassDiagnostics> passes;

    /// The prepass mode the feature set produced, reported because "which targets exist" is the
    /// first thing to check when a screen-space effect is reading nothing.
    PrepassMode prepass_mode = PrepassMode::DepthOnly;

    void clear() noexcept;

    [[nodiscard]] u32 total_draw_calls() const noexcept;
    [[nodiscard]] u64 total_triangles() const noexcept;
    /// Zero when nothing was measured, which `any_gpu_measured()` distinguishes from a fast frame.
    [[nodiscard]] u64 total_gpu_nanoseconds() const noexcept;
    [[nodiscard]] bool any_gpu_measured() const noexcept;
};

/// Add a pass's numbers to the report. One function so that a report cannot end up with two entries
/// for one pass through two call sites that both looked reasonable.
[[nodiscard]] Status record_pass(FrameDiagnostics& diagnostics,
                                 const PassDiagnostics& pass) noexcept;

/// Fold a view's report into `render::FrameStatistics`, which is what the engine-wide report reads.
/// Written here rather than in the server because the mapping is this module's vocabulary, and a
/// second place that knew it would be a second place to keep in step.
void accumulate_into(render::FrameStatistics& frame, const FrameDiagnostics& diagnostics) noexcept;

}  // namespace cy::rendering
