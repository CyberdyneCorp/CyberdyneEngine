// Frame stage and memory category names, and the frame statistics accumulator. See
// cy/servers/render/statistics.h.

#include <cy/servers/render/statistics.h>

namespace cy::render {
namespace {

constexpr const char* kFrameStageNames[] = {
    "Extract", "Prepare", "Cull", "BuildGraph", "CompileGraph", "Execute", "Present",
};
static_assert(sizeof(kFrameStageNames) / sizeof(kFrameStageNames[0]) == kFrameStageCount);

constexpr const char* kMemoryCategoryNames[] = {
    "Textures", "Meshes", "RenderTargets", "Buffers", "AccelerationStructures",
};
static_assert(sizeof(kMemoryCategoryNames) / sizeof(kMemoryCategoryNames[0]) ==
              kMemoryCategoryCount);

}  // namespace

const char* frame_stage_name(FrameStage stage) noexcept {
    const auto index = static_cast<usize>(stage);
    return (index < kFrameStageCount) ? kFrameStageNames[index] : "<invalid>";
}

const char* memory_category_name(MemoryCategory category) noexcept {
    const auto index = static_cast<usize>(category);
    return (index < kMemoryCategoryCount) ? kMemoryCategoryNames[index] : "<invalid>";
}

void FrameStatistics::reset() noexcept {
    const u64 index = frame_index;
    *this = FrameStatistics{};
    frame_index = index;
}

void FrameStatistics::add_pass_timing(const char* name, u64 gpu_nanoseconds,
                                      bool measured) noexcept {
    if (pass_timing_count >= kMaxTimedPasses) {
        ++pass_timings_dropped;
        return;
    }
    pass_timings[pass_timing_count] =
        PassTiming{(name != nullptr) ? name : "", gpu_nanoseconds, measured};
    ++pass_timing_count;
}

f32 FrameStatistics::total_cpu_milliseconds() const noexcept {
    f32 total = 0.0F;
    for (const f32 milliseconds : stage_milliseconds) {
        total += milliseconds;
    }
    return total;
}

u64 FrameStatistics::total_memory_bytes() const noexcept {
    u64 total = 0;
    for (const u64 bytes : memory_bytes) {
        total += bytes;
    }
    return total;
}

void accumulate(FrameStatistics& frame, const ViewStatistics& view) noexcept {
    ++frame.views;
    frame.instances_visible += view.instances_visible;
    frame.draw_calls += view.draw_calls;
    frame.draws_merged += view.draws_merged;
    frame.triangles += view.triangles;
    frame.passes += view.passes;
}

}  // namespace cy::render
