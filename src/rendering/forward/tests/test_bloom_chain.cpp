// Bloom's graph passes: what the chain declares, in what order, and that a frame without bloom
// declares nothing of it.
//
// Device-free, like the rest of this suite: the chain is declarations into a real `RenderGraph`
// compiled against `synthetic_memory_query`. What the passes DRAW is `render.bloom`'s.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/forward/bloom_chain.h>
#include <cy/rendering/forward/diagnostics.h>
#include <cy/rendering/forward/frame.h>

#include <cstring>

namespace {

using cy::rendering::BloomChain;
using cy::rendering::BloomStep;
using cy::rendering::BloomStepKind;
using cy::rendering::ForwardFrame;
using cy::rendering::FrameDescription;
using cy::rendering::FramePassCallback;
using cy::rendering::FramePassKind;
using cy::rendering::kInvalidPass;
using cy::rendering::kInvalidResource;
using cy::rendering::RenderGraph;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

FrameDescription frame_description(bool bloom) noexcept {
    FrameDescription description;
    description.width = 320;
    description.height = 180;
    description.light_count = 4;
    description.draw_instance_count = 16;
    const cy::rendering::ClusterGridConfig config{32, 8, 16};
    const cy::Expected<cy::rendering::ClusterGrid, cy::Error> grid =
        cy::rendering::make_cluster_grid(config, 320, 180, 0.1F, 100.0F);
    if (grid.has_value()) {
        description.cluster_grid = *grid;
    }
    description.features.bloom = bloom;
    return description;
}

[[nodiscard]] bool compiles(RenderGraph& graph) noexcept {
    cy::rendering::CompileOptions options;
    options.query_memory = &cy::rendering::synthetic_memory_query;
    return graph.compile(options).has_value();
}

[[nodiscard]] cy::u32 names_starting(const RenderGraph& graph, const char* prefix) noexcept {
    cy::u32 count = 0;
    for (cy::u32 pass = 0; pass < graph.pass_count(); ++pass) {
        count += std::strncmp(graph.pass_name(pass), prefix, std::strlen(prefix)) == 0 ? 1U : 0U;
    }
    for (cy::u32 resource = 0; resource < graph.resource_count(); ++resource) {
        const char* name = graph.resource(resource).name;
        count += std::strncmp(name, prefix, std::strlen(prefix)) == 0 ? 1U : 0U;
    }
    return count;
}

/// Whether `pass` declares a read of `resource`.
[[nodiscard]] bool reads(const RenderGraph& graph, cy::u32 pass,
                         cy::rendering::ResourceId resource) {
    for (const cy::rendering::Use& use : graph.pass_uses(pass)) {
        if (use.resource == resource && !cy::rhi::is_write(use.access)) {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("the level count fits the extent, never exceeds the maximum and is never zero") {
    CY_CHECK_EQ(cy::rendering::bloom_level_count(1920, 1080, 6), 6U);
    CY_CHECK_EQ(cy::rendering::bloom_level_count(3840, 2160, 99), cy::rendering::kMaxBloomLevels);
    // 180 rows: the sixth level is 2 texels tall and a seventh would be one, which a 13-tap
    // footprint would read mostly as the clamp at the edge.
    CY_CHECK_EQ(cy::rendering::bloom_level_count(320, 180, 8), 6U);
    CY_CHECK_EQ(cy::rendering::bloom_level_count(4, 4, 6), 1U);
    CY_CHECK_EQ(cy::rendering::bloom_level_count(1920, 1080, 0), 1U);
    CY_CHECK_EQ(cy::rendering::bloom_level_extent(1080, 0), 540U);
    CY_CHECK_EQ(cy::rendering::bloom_level_extent(3, 4), 1U);
}

CY_TEST_CASE("the chain is a progressive downsample, an upsample back up it, and a composite") {
    RenderGraph graph(allocator());
    cy::rendering::TextureRequest request;
    request.name = "scene";
    request.format = cy::rhi::Format::Rgba16Sfloat;
    request.width = 1920;
    request.height = 1080;
    const cy::rendering::ResourceId scene = graph.create_texture(request);
    BloomChain chain;
    CY_REQUIRE(cy::rendering::declare_bloom_chain(graph, scene, 1920, 1080,
                                                  cy::rhi::Format::Rgba16Sfloat, 6,
                                                  FramePassCallback{}, chain)
                   .has_value());

    CY_REQUIRE_EQ(chain.levels, 6U);
    CY_REQUIRE_EQ(chain.step_count, 12U);
    // Down: the prefilter reads the scene, every later level reads the one above it and halves.
    CY_CHECK_EQ(chain.steps[0].kind, BloomStepKind::Prefilter);
    CY_CHECK_EQ(chain.steps[0].source, scene);
    CY_CHECK_EQ(chain.steps[0].target_width, 960U);
    for (cy::u32 level = 1; level < 6; ++level) {
        const BloomStep& step = chain.steps[level];
        CY_CHECK_EQ(step.kind, BloomStepKind::Downsample);
        CY_CHECK_EQ(step.source, chain.down[level - 1]);
        CY_CHECK_EQ(step.target, chain.down[level]);
        CY_CHECK_EQ(step.target_width * 2U, step.source_width);
    }
    // Up: coarsest first, each blending the level beneath over its own downsample. The first reads
    // the coarsest DOWNSAMPLE, because the coarsest level has nothing beneath it to upsample.
    CY_CHECK_EQ(chain.steps[6].kind, BloomStepKind::Upsample);
    CY_CHECK_EQ(chain.steps[6].level, 4U);
    CY_CHECK_EQ(chain.steps[6].detail, chain.down[5]);
    for (cy::u32 index = 7; index < 11; ++index) {
        const BloomStep& step = chain.steps[index];
        CY_CHECK_EQ(step.kind, BloomStepKind::Upsample);
        CY_CHECK_EQ(step.source, chain.down[step.level]);
        CY_CHECK_EQ(step.detail, chain.up[step.level + 1U]);
        CY_CHECK_EQ(step.target, chain.up[step.level]);
    }
    CY_CHECK_EQ(chain.up[5], kInvalidResource);
    // The composite reads the scene and the finest upsample, and writes a new full-size target.
    const BloomStep& composite = chain.steps[11];
    CY_CHECK_EQ(composite.kind, BloomStepKind::Composite);
    CY_CHECK_EQ(composite.source, scene);
    CY_CHECK_EQ(composite.detail, chain.up[0]);
    CY_CHECK_EQ(composite.target, chain.output);
    CY_CHECK_EQ(composite.target_width, 1920U);
    CY_CHECK(reads(graph, composite.pass, scene));
    CY_CHECK(reads(graph, composite.pass, chain.up[0]));

    // Every pass is found by its id, which is how one callback records all of them.
    for (cy::u32 index = 0; index < chain.step_count; ++index) {
        CY_CHECK_EQ(chain.step_of(chain.steps[index].pass), &chain.steps[index]);
    }
    CY_CHECK(chain.step_of(kInvalidPass) == nullptr);
}

CY_TEST_CASE("a one-level chain composites straight from its only downsample") {
    RenderGraph graph(allocator());
    cy::rendering::TextureRequest request;
    request.format = cy::rhi::Format::Rgba16Sfloat;
    request.width = 8;
    request.height = 8;
    const cy::rendering::ResourceId scene = graph.create_texture(request);
    BloomChain chain;
    CY_REQUIRE(cy::rendering::declare_bloom_chain(graph, scene, 8, 8, cy::rhi::Format::Rgba16Sfloat,
                                                  1, FramePassCallback{}, chain)
                   .has_value());
    CY_CHECK_EQ(chain.step_count, 2U);
    CY_CHECK_EQ(chain.steps[1].kind, BloomStepKind::Composite);
    CY_CHECK_EQ(chain.steps[1].detail, chain.down[0]);
}

CY_TEST_CASE("the chain refuses what would declare a frame that renders nothing") {
    RenderGraph graph(allocator());
    BloomChain chain;
    CY_CHECK_FALSE(cy::rendering::declare_bloom_chain(graph, kInvalidResource, 8, 8,
                                                      cy::rhi::Format::Rgba16Sfloat, 6,
                                                      FramePassCallback{}, chain)
                       .has_value());
    cy::rendering::TextureRequest request;
    request.format = cy::rhi::Format::Rgba16Sfloat;
    request.width = 8;
    request.height = 8;
    const cy::rendering::ResourceId scene = graph.create_texture(request);
    CY_CHECK_FALSE(cy::rendering::declare_bloom_chain(graph, scene, 0, 8,
                                                      cy::rhi::Format::Rgba16Sfloat, 6,
                                                      FramePassCallback{}, chain)
                       .has_value());
    CY_CHECK_FALSE(cy::rendering::declare_bloom_chain(graph, scene, 8, 8,
                                                      cy::rhi::Format::Undefined, 6,
                                                      FramePassCallback{}, chain)
                       .has_value());
}

CY_TEST_CASE("bloom sits between the temporal stage and the post-process, which reads its output") {
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = frame_description(true);
    description.features.temporal = true;
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(compiles(graph));

    const BloomChain& chain = frame.bloom();
    CY_REQUIRE(chain.levels > 0U);
    // The prefilter and the composite both read what the temporal stage wrote: bloom is after the
    // resolve and before exposure, step 9 of the specification's chain.
    CY_CHECK_EQ(chain.steps[0].source, frame.resources().temporal_history);
    CY_CHECK_EQ(frame.resources().post_source, chain.output);
    const cy::rendering::PassId post = frame.pass_of(FramePassKind::PostProcess);
    CY_REQUIRE_NE(post, kInvalidPass);
    CY_CHECK(reads(graph, post, chain.output));
    CY_CHECK_LT(frame.pass_of(FramePassKind::Temporal), frame.pass_of(FramePassKind::Bloom));
    CY_CHECK_LT(chain.steps[chain.step_count - 1U].pass, post);

    // Every step is staged under the one stage kind, in declaration order.
    cy::u32 staged = 0;
    for (const cy::rendering::FramePass& pass : frame.passes()) {
        staged += pass.kind == FramePassKind::Bloom ? 1U : 0U;
    }
    CY_CHECK_EQ(staged, chain.step_count);
}

CY_TEST_CASE("with bloom off, nothing of it is declared and the post-process reads the scene") {
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    const FrameDescription description = frame_description(false);
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(compiles(graph));

    // ABSENT, NOT SKIPPED: no pass, no target, no stage.
    CY_CHECK_EQ(names_starting(graph, "bloom"), 0U);
    CY_CHECK_EQ(frame.pass_of(FramePassKind::Bloom), kInvalidPass);
    CY_CHECK_EQ(frame.bloom().levels, 0U);
    CY_CHECK_EQ(frame.bloom().output, kInvalidResource);
    // The frame it was before bloom existed: the post-process reads the shading target.
    CY_CHECK_EQ(frame.resources().post_source, frame.resources().color);
    CY_CHECK(reads(graph, frame.pass_of(FramePassKind::PostProcess), frame.resources().color));
}
