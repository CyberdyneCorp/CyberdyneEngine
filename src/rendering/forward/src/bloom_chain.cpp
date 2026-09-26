// SPDX-License-Identifier: MIT
// Bloom's graph passes. See the header for the chain and why each level is its own texture.

#include <cy/rendering/forward/bloom_chain.h>

#include <cy/rendering/forward/frame.h>

namespace cy::rendering {
namespace {

using rhi::Access;

// Graph pass names must outlive the graph, so they are literals rather than formatted strings.
constexpr const char* kDownsampleNames[kMaxBloomLevels] = {
    "bloom prefilter",    "bloom downsample 1", "bloom downsample 2", "bloom downsample 3",
    "bloom downsample 4", "bloom downsample 5", "bloom downsample 6", "bloom downsample 7",
};
constexpr const char* kUpsampleNames[kMaxBloomLevels] = {
    "bloom upsample 0", "bloom upsample 1", "bloom upsample 2", "bloom upsample 3",
    "bloom upsample 4", "bloom upsample 5", "bloom upsample 6", "bloom upsample 7",
};
constexpr const char* kDownNames[kMaxBloomLevels] = {
    "bloom down 0", "bloom down 1", "bloom down 2", "bloom down 3",
    "bloom down 4", "bloom down 5", "bloom down 6", "bloom down 7",
};
constexpr const char* kUpNames[kMaxBloomLevels] = {
    "bloom up 0", "bloom up 1", "bloom up 2", "bloom up 3",
    "bloom up 4", "bloom up 5", "bloom up 6", "bloom up 7",
};

/// Declare one pass: its reads, its one write, and the caller's callback.
void declare_step(RenderGraph& graph, const char* name, BloomStep& step,
                  const FramePassCallback& callback) noexcept {
    PassBuilder builder = graph.add_pass(name, rhi::QueueKind::Graphics);
    builder.read(step.source, Access::FragmentSampledRead);
    if (step.detail != kInvalidResource) {
        builder.read(step.detail, Access::FragmentSampledRead);
    }
    builder.write(step.target, Access::ColorAttachmentWrite);
    if (callback.record != nullptr) {
        builder.record(callback.record, callback.user);
    }
    step.pass = builder.id();
}

}  // namespace

const char* bloom_step_name(BloomStepKind kind) noexcept {
    switch (kind) {
        case BloomStepKind::Prefilter:
            return "prefilter";
        case BloomStepKind::Downsample:
            return "downsample";
        case BloomStepKind::Upsample:
            return "upsample";
        case BloomStepKind::Composite:
            return "composite";
        case BloomStepKind::Count:
            break;
    }
    return "unknown";
}

const BloomStep* BloomChain::step_of(PassId pass) const noexcept {
    for (u32 index = 0; index < step_count; ++index) {
        if (steps[index].pass == pass) {
            return &steps[index];
        }
    }
    return nullptr;
}

u32 bloom_level_extent(u32 full, u32 level) noexcept {
    const u32 shifted = full >> (level + 1U);
    return shifted == 0 ? 1U : shifted;
}

u32 bloom_level_count(u32 width, u32 height, u32 requested) noexcept {
    const u32 shorter = width < height ? width : height;
    u32 levels = requested < kMaxBloomLevels ? requested : kMaxBloomLevels;
    while (levels > 1U && (shorter >> levels) < 2U) {
        --levels;
    }
    return levels == 0 ? 1U : levels;
}

Status declare_bloom_chain(RenderGraph& graph, ResourceId source, u32 width, u32 height,
                           rhi::Format format, u32 requested_levels,
                           const FramePassCallback& callback, BloomChain& out) noexcept {
    out = BloomChain{};
    if (width == 0 || height == 0 || source == kInvalidResource) {
        return fail(ErrorCode::InvalidArgument, "bloom: the chain needs a source and an extent");
    }
    if (format == rhi::Format::Undefined) {
        return fail(ErrorCode::InvalidArgument, "bloom: the chain's levels need a colour format");
    }
    const u32 levels = bloom_level_count(width, height, requested_levels);
    out.levels = levels;

    TextureRequest request;
    request.format = format;
    for (u32 level = 0; level < levels; ++level) {
        request.width = bloom_level_extent(width, level);
        request.height = bloom_level_extent(height, level);
        request.name = kDownNames[level];
        out.down[level] = graph.create_texture(request);
        // The coarsest level has nothing beneath it to upsample, so it has no `up` of its own.
        if (level + 1U < levels) {
            request.name = kUpNames[level];
            out.up[level] = graph.create_texture(request);
        }
    }
    request.width = width;
    request.height = height;
    request.name = "bloomed colour";
    out.output = graph.create_texture(request);

    // Down the chain: the prefilter from the scene, then each level from the one above it.
    for (u32 level = 0; level < levels; ++level) {
        BloomStep& step = out.steps[out.step_count++];
        step.kind = level == 0 ? BloomStepKind::Prefilter : BloomStepKind::Downsample;
        step.level = level;
        step.source = level == 0 ? source : out.down[level - 1U];
        step.source_width = level == 0 ? width : bloom_level_extent(width, level - 1U);
        step.source_height = level == 0 ? height : bloom_level_extent(height, level - 1U);
        step.target = out.down[level];
        step.target_width = bloom_level_extent(width, level);
        step.target_height = bloom_level_extent(height, level);
        declare_step(graph, kDownsampleNames[level], step, callback);
    }

    // Back up it, coarsest first: each level blends the tent-filtered level beneath it over its
    // own downsample. The coarsest's "upsample" is its downsample, so it is read directly.
    for (u32 level = levels - 1U; level-- > 0U;) {
        BloomStep& step = out.steps[out.step_count++];
        step.kind = BloomStepKind::Upsample;
        step.level = level;
        step.source = out.down[level];
        step.source_width = bloom_level_extent(width, level);
        step.source_height = bloom_level_extent(height, level);
        const bool coarsest_below = level + 2U == levels;
        step.detail = coarsest_below ? out.down[level + 1U] : out.up[level + 1U];
        step.detail_width = bloom_level_extent(width, level + 1U);
        step.detail_height = bloom_level_extent(height, level + 1U);
        step.target = out.up[level];
        step.target_width = step.source_width;
        step.target_height = step.source_height;
        declare_step(graph, kUpsampleNames[level], step, callback);
    }

    BloomStep& composite = out.steps[out.step_count++];
    composite.kind = BloomStepKind::Composite;
    composite.level = 0;
    composite.source = source;
    composite.source_width = width;
    composite.source_height = height;
    composite.detail = levels > 1U ? out.up[0] : out.down[0];
    composite.detail_width = bloom_level_extent(width, 0);
    composite.detail_height = bloom_level_extent(height, 0);
    composite.target = out.output;
    composite.target_width = width;
    composite.target_height = height;
    declare_step(graph, "bloom composite", composite, callback);

    return graph.status();
}

}  // namespace cy::rendering
