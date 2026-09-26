// SPDX-License-Identifier: MIT
// Multi-view selected by capability, and MSAA recorded through the attachment model — on the null
// backend, with no GPU. M11.d tasks 5.1 and 5.2.
//
// `rhi-and-render-graph`: "The renderer SHALL branch on capabilities, never on backend identity."
// These cases put ONE declaration in front of a null device twice — once reporting
// `Capability::Multiview`, once with it forced absent — and assert that the executor chose the
// recording by the capability alone, that the baseline records every view into its own layer, and
// that the plan is identical either way. `render.msaa_multiview` makes the same comparison on a
// Vulkan device, in pixels.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::GraphExecutor;
using cy::rendering::PassContext;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;

namespace {

constexpr cy::u32 kViews = 2;
constexpr cy::u32 kMaxRecordings = 8;

struct ViewLog {
    cy::u32 recordings = 0;
    cy::u32 view_count[kMaxRecordings] = {};
    cy::u32 view_mask[kMaxRecordings] = {};
    cy::u32 view_index[kMaxRecordings] = {};
    cy::rhi::TextureViewHandle attachment[kMaxRecordings] = {};
    ResourceId target = cy::rendering::kInvalidResource;
};

void record_views(const PassContext& context, void* user) noexcept {
    auto* log = static_cast<ViewLog*>(user);
    if (log->recordings < kMaxRecordings) {
        const cy::u32 at = log->recordings;
        log->view_count[at] = context.view_count;
        log->view_mask[at] = context.view_mask;
        log->view_index[at] = context.view_index;
        log->attachment[at] = context.attachment_view(log->target);
    }
    ++log->recordings;
    cy::rhi::RenderAttachment colour;
    colour.view = context.attachment_view(log->target);
    colour.load = cy::rhi::LoadOp::Clear;
    cy::rhi::RenderingInfo info;
    info.render_area = cy::rhi::Rect2D{0, 0, 16, 16};
    info.color_attachments = cy::Span<const cy::rhi::RenderAttachment>(&colour, 1);
    info.view_mask = context.view_mask;
    context.commands->begin_rendering(info);
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

/// A null device whose `Capability::Multiview` is whatever the case says.
class NullDevice {
public:
    explicit NullDevice(bool multiview) noexcept
        : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)),
          device_(cy::rhi::null::create_null_device(allocator_, description(multiview))) {}
    ~NullDevice() {
        if (device_.has_value()) {
            cy::rhi::null::destroy_null_device(allocator_, device_.value());
        }
    }
    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;
    NullDevice(NullDevice&&) = delete;
    NullDevice& operator=(NullDevice&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] cy::Allocator& allocator() const noexcept { return allocator_; }

private:
    static cy::rhi::DeviceDescription description(bool multiview) noexcept {
        cy::rhi::DeviceDescription description;
        description.enable_validation = true;
        description.request_multiview = multiview;
        return description;
    }

    cy::Allocator& allocator_;
    cy::Expected<cy::rhi::Device*, cy::Error> device_;
};

struct Run {
    ViewLog log;
    cy::u64 plan_hash = 0;
    cy::u32 passes = 0;
};

/// One stereo pass into a two-layer target, then a readback so the pass is not culled.
bool run_stereo(cy::rhi::Device& device, cy::Allocator& allocator, Run& run) noexcept {
    RenderGraph graph(allocator);
    GraphExecutor executor(allocator, device);
    cy::rendering::TextureRequest request = cy::rendering::test::colour_target("eyes");
    request.array_layers = kViews;
    const ResourceId eyes = graph.create_texture(request);
    run.log.target = eyes;
    graph.add_pass("stereo", QueueKind::Graphics)
        .views(kViews)
        .write(eyes, Access::ColorAttachmentWrite)
        .record(&record_views, &run.log);
    const ResourceId out =
        graph.create_buffer(cy::rendering::test::storage_buffer("readback", 16ULL * 16 * 4 * 2));
    graph.add_pass("readback", QueueKind::Graphics)
        .read(eyes, Access::TransferRead)
        .write(out, Access::TransferWrite)
        .side_effect();
    if (!device.begin_frame().has_value()) {
        return false;
    }
    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    if (!result.has_value()) {
        return false;
    }
    run.plan_hash = result->plan_hash;
    run.passes = result->passes_recorded;
    executor.release();
    return device.end_frame().has_value();
}

}  // namespace

CY_TEST_CASE("multi-view is recorded once with a view mask where the device reports it") {
    NullDevice device(true);
    CY_REQUIRE(device.ok());
    CY_REQUIRE(device.device().capabilities().has(cy::rhi::Capability::Multiview));
    Run run;
    CY_REQUIRE(run_stereo(device.device(), device.allocator(), run));

    CY_CHECK_EQ(run.log.recordings, 1U);
    CY_CHECK_EQ(run.log.view_count[0], kViews);
    CY_CHECK_EQ(run.log.view_mask[0], 0b11U);
    CY_CHECK_FALSE(run.log.attachment[0].is_null());
}

CY_TEST_CASE("without the capability every view is recorded into its own layer") {
    NullDevice device(false);
    CY_REQUIRE(device.ok());
    // FORCED ABSENT through the device's own capability, not a graph option: the executor has
    // nothing else to read.
    CY_REQUIRE_FALSE(device.device().capabilities().has(cy::rhi::Capability::Multiview));
    Run run;
    CY_REQUIRE(run_stereo(device.device(), device.allocator(), run));

    CY_REQUIRE_EQ(run.log.recordings, kViews);
    for (cy::u32 view = 0; view < kViews; ++view) {
        CY_CHECK_EQ(run.log.view_count[view], kViews);
        CY_CHECK_EQ(run.log.view_mask[view], 0U);
        CY_CHECK_EQ(run.log.view_index[view], view);
        CY_CHECK_FALSE(run.log.attachment[view].is_null());
    }
    // Two DIFFERENT single-layer views: rendering both recordings through one view would put both
    // eyes in layer 0, which is the defect the baseline exists not to have.
    CY_CHECK_NE(run.log.attachment[0], run.log.attachment[1]);
}

CY_TEST_CASE("the capability changes the recording and not the plan") {
    NullDevice with(true);
    NullDevice without(false);
    CY_REQUIRE(with.ok());
    CY_REQUIRE(without.ok());
    Run multiview;
    Run baseline;
    CY_REQUIRE(run_stereo(with.device(), with.allocator(), multiview));
    CY_REQUIRE(run_stereo(without.device(), without.allocator(), baseline));
    CY_CHECK_EQ(multiview.plan_hash, baseline.plan_hash);
    CY_CHECK_EQ(multiview.passes, baseline.passes);

    // The same answer through `override_capability`, the null backend's test hook — so neither
    // case above depends on `request_multiview` being the only way to reach the baseline.
    NullDevice overridden(true);
    CY_REQUIRE(overridden.ok());
    cy::rhi::null::override_capability(overridden.device(), cy::rhi::Capability::Multiview, false);
    Run forced;
    CY_REQUIRE(run_stereo(overridden.device(), overridden.allocator(), forced));
    CY_CHECK_EQ(forced.log.recordings, kViews);
    CY_CHECK_EQ(forced.plan_hash, multiview.plan_hash);
}

namespace {

struct MsaaLog {
    ResourceId colour = cy::rendering::kInvalidResource;
    cy::rhi::TextureViewHandle rendered_through;
    cy::u16 samples = 0;
};

void record_msaa(const PassContext& context, void* user) noexcept {
    auto* log = static_cast<MsaaLog*>(user);
    log->rendered_through = context.attachment_view(log->colour);
    log->samples = context.sample_count;
    context.commands->draw(3, 1, 0, 0);
}

}  // namespace

CY_TEST_CASE("a multisampled pass is handed its twin's view, and the resolve is recorded") {
    NullDevice device(true);
    CY_REQUIRE(device.ok());
    RenderGraph graph(device.allocator());
    GraphExecutor executor(device.allocator(), device.device());
    const ResourceId colour = graph.create_texture(cy::rendering::test::colour_target("colour"));
    MsaaLog log;
    log.colour = colour;
    graph.add_pass("draw", QueueKind::Graphics)
        .multisample(4)
        .write(colour, Access::ColorAttachmentWrite)
        .resolve(colour)
        .record(&record_msaa, &log);
    const ResourceId out =
        graph.create_buffer(cy::rendering::test::storage_buffer("readback", 16ULL * 16 * 4));
    graph.add_pass("readback", QueueKind::Graphics)
        .read(colour, Access::TransferRead)
        .write(out, Access::TransferWrite)
        .side_effect();
    CY_REQUIRE(device.device().begin_frame().has_value());
    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    CY_REQUIRE(result.has_value());
    // Draw, the graph's resolve, the readback.
    CY_CHECK_EQ(result->passes_recorded, 3U);
    CY_CHECK_EQ(log.samples, 4U);
    const ResourceId twin = graph.resource(colour).multisampled;
    CY_REQUIRE_NE(twin, cy::rendering::kInvalidResource);
    CY_CHECK_EQ(log.rendered_through, executor.view(twin));
    CY_CHECK_NE(log.rendered_through, executor.view(colour));
    CY_CHECK_FALSE(executor.texture(twin).is_null());
    executor.release();
    CY_CHECK(device.device().end_frame().has_value());
}
