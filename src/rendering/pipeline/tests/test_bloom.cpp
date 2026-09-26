// SPDX-License-Identifier: MIT
// Bloom in the layer, with no GPU: what is declared, what is attached, what is recorded, and what
// is refused. What the passes DRAW is `render.bloom`'s.

#include "frame_scene.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

class NullFixture {
public:
    NullFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_pipeline_bloom";
        device_ = rhi::create_device(allocator_, "null", description, selection_);
    }

    ~NullFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    NullFixture(const NullFixture&) = delete;
    NullFixture& operator=(const NullFixture&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

bool no_geometry(const render::DrawItem& /*item*/, const GpuDrawInstance& /*instance*/,
                 void* /*user*/, DrawGeometry& /*out*/) noexcept {
    return false;
}

}  // namespace

CY_TEST_CASE("bloom in the post chain is declared, attached and recorded, one step a pass") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    BloomSettings settings;
    settings.mip_count = 5;
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device(), &settings).has_value());

    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK(report.executed);

    // The chain has the stage, in the specification's place, and the frame declared its passes.
    bool staged = false;
    for (u32 index = 0; index < report.post_stages; ++index) {
        staged = staged || report.post_stage[index] == PostStage::Bloom;
    }
    CY_CHECK(staged);
    const BloomChain& chain = scene.assembly().frame().bloom();
    CY_CHECK_EQ(chain.levels, 5U);
    CY_CHECK_EQ(chain.step_count, 10U);
    // Every step ran through the renderer, and none was skipped.
    CY_CHECK_EQ(scene.bloom().report().steps, chain.step_count);
    CY_CHECK_EQ(scene.bloom().report().skipped, 0U);
    // The post-process read the bloomed colour, which is what "wired into the chain" means.
    CY_CHECK_EQ(scene.assembly().resources().post_source, chain.output);
    // The settings are the assembly's, handed over at bind: one description, not two.
    CY_CHECK_EQ(scene.bloom().settings().mip_count, 5U);
}

CY_TEST_CASE("without bloom the frame declares none of it and the renderer is never reached") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    for (u32 index = 0; index < report.post_stages; ++index) {
        CY_CHECK_NE(report.post_stage[index], PostStage::Bloom);
    }
    CY_CHECK_EQ(scene.assembly().frame().pass_of(FramePassKind::Bloom), kInvalidPass);
    CY_CHECK_EQ(scene.assembly().frame().bloom().levels, 0U);
    CY_CHECK_EQ(scene.bloom().report().steps, 0U);
    // The temporal history, exactly as the post-process read it before bloom existed.
    CY_CHECK_EQ(scene.assembly().resources().post_source,
                scene.assembly().resources().temporal_history);
}

CY_TEST_CASE("the recorder refuses a frame with bloom in its chain and no renderer for it") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    rhi::Device& device = fixture.device();

    assembly::AssemblyDescription description;
    description.width = 64;
    description.height = 64;
    description.far_plane = 100.0F;
    description.gpu_culling = false;
    description.post.bloom = true;
    assembly::FrameAssembly frame(allocator());
    CY_REQUIRE(frame.initialize(description).has_value());

    PipelineSetup setup;
    FramePipelines pipelines;
    CY_REQUIRE(pipelines.initialize(device, setup).has_value());
    FrameBindings bindings;
    CY_REQUIRE(bindings.initialize(device, pipelines, BindingCapacity{}).has_value());
    FrameRecorder recorder;
    CY_REQUIRE(recorder.initialize(pipelines, bindings).has_value());

    rhi::BufferDescription stream;
    stream.size = 256;
    stream.usage = rhi::BufferUsage::Vertex;
    GeometrySource geometry;
    for (rhi::BufferHandle& buffer : geometry.streams) {
        Expected<rhi::BufferHandle, Error> made = device.create_buffer(stream);
        CY_REQUIRE(made.has_value());
        buffer = *made;
    }
    geometry.geometry = &no_geometry;
    recorder.set_geometry(geometry);

    // Declared passes with nothing to record them would leave the post-process reading a target
    // nothing wrote: a frame that compiles and shows garbage. Refused instead, by name.
    const Status refused = recorder.bind(frame);
    CY_CHECK_FALSE(refused.has_value());

    BloomRenderer bloom;
    CY_REQUIRE(bloom.initialize(device, pipelines).has_value());
    recorder.set_bloom(&bloom);
    CY_CHECK(recorder.bind(frame).has_value());
    const rendering::assembly::FrameSinks sinks = recorder.sinks();
    CY_CHECK(sinks.passes[static_cast<usize>(FramePassKind::Bloom)].record != nullptr);

    bloom.shutdown();
    bindings.shutdown();
    pipelines.shutdown();
    for (const rhi::BufferHandle buffer : geometry.streams) {
        device.destroy_buffer(buffer);
    }
}

CY_TEST_CASE("the push block carries the processor's own threshold, knee and weights") {
    BloomStep step;
    step.source_width = 1920;
    step.source_height = 1080;
    step.target_width = 960;
    step.target_height = 540;
    step.detail_width = 480;
    step.detail_height = 270;
    BloomSettings settings;
    settings.threshold = 2048.0F;
    settings.knee = 0.5F;
    settings.intensity = 1.5F;  // clamped: more than all of the scattered energy is not a setting
    settings.scatter = 0.6F;
    settings.anamorphic = 2.0F;
    settings.firefly_suppression = false;
    const BloomPush push = bloom_push(step, settings);
    CY_CHECK_NEAR(push.extents[0], 1.0F / 1920.0F, 1e-9F);
    CY_CHECK_NEAR(push.extents[3], 1.0F / 540.0F, 1e-9F);
    CY_CHECK_NEAR(push.detail[1], 1.0F / 270.0F, 1e-9F);
    CY_CHECK_EQ(push.threshold[0], 2048.0F);
    // The knee is a FRACTION of the threshold in the settings and an absolute width in the shader,
    // exactly as `bloom_prefilter` derives it.
    CY_CHECK_EQ(push.threshold[1], 1024.0F);
    CY_CHECK_NEAR(push.threshold[2], 1.0F / 2048.0F, 1e-12F);
    CY_CHECK_EQ(push.threshold[3], 0.0F);
    CY_CHECK_EQ(push.blend[0], 1.0F);
    CY_CHECK_NEAR(push.blend[1], 0.6F, 1e-7F);
    CY_CHECK_EQ(push.blend[2], 2.0F);
}
