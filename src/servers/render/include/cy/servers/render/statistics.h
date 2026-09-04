#pragma once
// Frame structure and render statistics. Tasks 4.1.3 and 4.1.6.
//
// `rendering-architecture` — "Frame structure" names seven stages, in order, and "Render
// statistics" requires the renderer to accumulate "per view and per frame: visible instance count,
// draw call count, triangle count, pass count, GPU time per pass (via timestamps), CPU time per
// stage, and memory usage by resource category".
//
// --- THE STAGES ARE AN ENUM, NOT A COMMENT
// --------------------------------------------------------
//
// The seven stages exist as a type for the same reason `determinism::TickPhase` does: they are the
// vocabulary a timing report is broken down by and the vocabulary an ordering constraint names. A
// renderer that ran them as seven function calls in a row with a comment above each would have no
// way to answer "which stage was slow", and the first person to need that answer would add a
// seventh timer with its own spelling.
//
// `FrameStage` does NOT schedule anything. The frame driver (`cy::rendering::FrameLoop`, layer 4)
// runs the stages; this enumeration only names them.
//
// --- WHY "MEASURED" IS A FIELD ON EVERY TIMING
// ----------------------------------------------------
//
// A device without timestamp queries reports no GPU time. Zero is not the same answer as "not
// measured", and a report that cannot tell them apart is a report that says a pass is free. Every
// timing below therefore carries the flag, and the printer prints "—" rather than "0.00 ms".

#include <cy/core/base/types.h>
#include <cy/core/values/name.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/types.h>

namespace cy::render {

/// `rendering-architecture`'s seven, in its order.
enum class FrameStage : u8 {
    Extract = 0,   // publish the render snapshot
    Prepare,       // update GPU resources from it: instance, light and material buffers
    Cull,          // per view, produce visible instance and shadow caster lists
    BuildGraph,    // declare passes, resources and dependencies for every view
    CompileGraph,  // cull, schedule, alias, synchronise
    Execute,       // record and submit, potentially in parallel
    Present,       // swap chain presentation and end-of-frame callbacks
    Count,
};

inline constexpr u32 kFrameStageCount = static_cast<u32>(FrameStage::Count);

[[nodiscard]] const char* frame_stage_name(FrameStage stage) noexcept;

/// GPU memory by category. `rendering-geometry-and-resources`: "The engine SHALL report GPU memory
/// by category (textures, meshes, render targets, buffers, acceleration structures)".
enum class MemoryCategory : u8 {
    Textures = 0,
    Meshes,
    RenderTargets,
    Buffers,
    AccelerationStructures,
    Count,
};

inline constexpr u32 kMemoryCategoryCount = static_cast<u32>(MemoryCategory::Count);

[[nodiscard]] const char* memory_category_name(MemoryCategory category) noexcept;

/// The most passes one frame reports timings for. Fixed so a frame's statistics allocate nothing:
/// a report that allocates is a report that perturbs what it measures, and this one runs every
/// frame in a development build.
inline constexpr u32 kMaxTimedPasses = 64;

/// One render graph pass's GPU duration, attributable by name.
///
/// `name` is the pass's own literal, borrowed and never owned — a pass name outlives the frame that
/// recorded it, because it is a string literal in the code that declared the pass.
struct PassTiming {
    const char* name = "";
    u64 gpu_nanoseconds = 0;
    bool measured = false;
};

/// What one view cost and drew.
struct ViewStatistics {
    Name name;
    ViewPurpose purpose = ViewPurpose::Primary;
    /// Instances the cull kept, out of those it looked at. Both, because the ratio is the number
    /// that tells you whether the spatial index is doing anything.
    u32 instances_considered = 0;
    u32 instances_visible = 0;
    u32 draw_calls = 0;
    /// After automatic instancing: how many of `draw_calls` were merged away. `draw_calls` is what
    /// the device saw and this is what it saved, which is the pair that makes instancing's benefit
    /// a measurement.
    u32 draws_merged = 0;
    u64 triangles = 0;
    u32 passes = 0;
    f32 cpu_milliseconds = 0.0F;
    u64 gpu_nanoseconds = 0;
    bool gpu_measured = false;
};

/// What a whole frame cost.
struct FrameStatistics {
    u64 frame_index = 0;

    /// CPU time per stage. Indexed by `FrameStage`, so a report iterates rather than naming seven
    /// fields — which is what makes adding a stage a one-line change here.
    f32 stage_milliseconds[kFrameStageCount] = {};

    u32 views = 0;
    u32 instances_visible = 0;
    u32 draw_calls = 0;
    u32 draws_merged = 0;
    u64 triangles = 0;
    u32 passes = 0;

    u32 pass_timing_count = 0;
    PassTiming pass_timings[kMaxTimedPasses];

    /// Bytes resident on the device, by category.
    u64 memory_bytes[kMemoryCategoryCount] = {};

    /// GPU scene occupancy, which is the number that says whether an instance capacity is right.
    u32 gpu_scene_capacity = 0;
    u32 gpu_scene_reserved = 0;
    u32 gpu_scene_uploaded_slots = 0;

    /// design.md §6's assertion, carried on the frame: `submission_fingerprint()` over the sorted
    /// draw list. Two runs of the same snapshot produce the same number, and a golden test records
    /// one number rather than a draw list.
    u64 submission_fingerprint = 0;

    void reset() noexcept;

    /// Record a pass's GPU time. Silently drops the timing past `kMaxTimedPasses` rather than
    /// growing — and `pass_timings_dropped` says how many, so a frame with more passes than the
    /// report can hold says so instead of quietly reporting a subset.
    void add_pass_timing(const char* name, u64 gpu_nanoseconds, bool measured) noexcept;

    u32 pass_timings_dropped = 0;

    [[nodiscard]] f32 total_cpu_milliseconds() const noexcept;
    [[nodiscard]] u64 total_memory_bytes() const noexcept;
};

/// Accumulate one view's numbers into the frame's. Written once, here, so a frame total cannot
/// drift from the views it is the sum of.
void accumulate(FrameStatistics& frame, const ViewStatistics& view) noexcept;

}  // namespace cy::render
