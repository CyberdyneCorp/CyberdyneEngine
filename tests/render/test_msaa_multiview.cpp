// SPDX-License-Identifier: MIT
// MSAA and multi-view through the render graph, on a Vulkan device. M11.d tasks 5.1 and 5.2.
//
// ================================================================================================
// WHAT A PICTURE CAN SAY THAT A PLAN CANNOT
// ================================================================================================
//
// `unit.render_graph` proves the twin, the resolve's placement and the refusals, and
// `integration.render_graph_scale` proves the executor chose the recording by capability. None of
// that shows a resolve RESOLVED anything, or that the baseline put each view in its own layer. So
// this suite draws one triangle whose edges are all oblique and reads the pixels back:
//
//   * 4x has partially covered edge pixels and 1x has none, while both cover the same area — the
//     statistical form of "edges are smoother", which no re-generated reference can launder;
//   * a resolve the executor skipped leaves the target at its clear colour, and the 4x case then
//     reads an empty image — that is the mutation `m11d:msaa-through-the-graph` is proven with;
//   * 1x through the graph's model (`multisample(1)` declared) is BYTE-IDENTICAL to the same pass
//     declared the way every pass was before M11.d;
//   * a device WITH multi-view and one created WITHOUT it render the same two-layer target byte
//     for byte, each layer carrying only its own view's colour.
//
// Every case builds its own device and asserts zero validation errors with synchronisation
// validation on: a view-masked pipeline on a device that never enabled the multiview feature is
// invalid usage, and it was exactly that until M11.d — the capability was reported unconditionally.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#include "device.h"
#include "shaders/view_probe_spirv.h"

#include <cstdio>

namespace {

using cy::u16;
using cy::u32;
using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;

constexpr u32 kSide = 64;
constexpr u32 kTexels = kSide * kSide;
constexpr u32 kMaxViews = 2;

/// Matches `ViewPush` in shaders/view_probe.slang.
struct ViewPush {
    float colors[2][4] = {{1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}};
    u32 view = 0;
    u32 padding[3] = {0, 0, 0};
};
static_assert(sizeof(ViewPush) == 48, "the push block must match view_probe.slang");

/// How one render is declared.
struct Request {
    u16 samples = 1;
    u32 views = 1;
    /// Declare `multisample(1)` explicitly rather than leaving the pass as it was before M11.d.
    bool say_one_sample = false;
    ViewPush push{};
};

/// The pipelines the probe needs, created once per device. The multi-view one only where the
/// device reports the capability: its vertex module carries SPIR-V `MultiView`, which a device
/// created without the feature must refuse.
class ViewProbe {
public:
    explicit ViewProbe(bool request_multiview) noexcept
        : fixture_("vulkan", "cy_test_render_msaa_multiview", request_multiview) {}

    ~ViewProbe() {
        if (!fixture_.is(cy::rhi::BackendKind::Vulkan)) {
            return;
        }
        cy::rhi::Device& device = fixture_.device();
        (void)device.wait_idle();
        for (cy::rhi::GraphicsPipelineHandle pipeline : {single_, multisampled_, multiview_}) {
            if (!pipeline.is_null()) {
                device.destroy_graphics_pipeline(pipeline);
            }
        }
        for (cy::rhi::ShaderModuleHandle module :
             {baseline_vertex_, multiview_vertex_, fragment_}) {
            if (!module.is_null()) {
                device.destroy_shader_module(module);
            }
        }
        if (!layout_.is_null()) {
            device.destroy_pipeline_layout(layout_);
        }
        if (!readback_.is_null()) {
            device.destroy_buffer(readback_);
        }
    }

    ViewProbe(const ViewProbe&) = delete;
    ViewProbe& operator=(const ViewProbe&) = delete;

    [[nodiscard]] bool have_vulkan() const noexcept {
        return fixture_.is(cy::rhi::BackendKind::Vulkan);
    }
    void report_skip() const noexcept { fixture_.report_skip(); }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return fixture_.device(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return fixture_.validation_errors(); }

    [[nodiscard]] cy::Status prepare() noexcept;
    /// Render one request through the graph and read every layer back, RGBA8, layer after layer.
    [[nodiscard]] cy::Status render(const Request& request, cy::Array<u32>& out) noexcept;

    struct PassState {
        ViewProbe* probe = nullptr;
        const Request* request = nullptr;
        ResourceId colour = cy::rendering::kInvalidResource;
        ResourceId readback = cy::rendering::kInvalidResource;
        cy::rendering::GraphExecutor* executor = nullptr;
    };

    [[nodiscard]] cy::rhi::GraphicsPipelineHandle pipeline_for(u16 samples,
                                                               u32 view_mask) const noexcept {
        if (view_mask != 0) {
            return multiview_;
        }
        return samples > 1 ? multisampled_ : single_;
    }
    [[nodiscard]] cy::rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] cy::rhi::BufferHandle readback() const noexcept { return readback_; }

private:
    [[nodiscard]] cy::Expected<cy::rhi::ShaderModuleHandle, cy::Error> module(
        const char* name, cy::rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept {
        cy::rhi::ShaderModuleDescription description;
        description.name = name;
        description.stage = stage;
        description.entry_point = "main";
        description.spirv = spirv;
        return fixture_.device().create_shader_module(description);
    }
    [[nodiscard]] cy::Expected<cy::rhi::GraphicsPipelineHandle, cy::Error> pipeline(
        const char* name, cy::rhi::ShaderModuleHandle vertex, u32 samples, u32 view_mask) noexcept {
        cy::rhi::ColorAttachmentState colour;
        colour.format = cy::rhi::Format::Rgba8Unorm;
        cy::rhi::GraphicsPipelineDescription description;
        description.name = name;
        description.layout = layout_;
        description.vertex_shader = vertex;
        description.fragment_shader = fragment_;
        description.color_attachments = cy::Span<const cy::rhi::ColorAttachmentState>(&colour, 1);
        description.rasterisation.cull_mode = cy::rhi::CullMode::None;
        description.sample_count = samples;
        description.view_mask = view_mask;
        return fixture_.device().create_graphics_pipeline(description);
    }

    cy::render_test::DeviceFixture fixture_;
    cy::rhi::ShaderModuleHandle baseline_vertex_;
    cy::rhi::ShaderModuleHandle multiview_vertex_;
    cy::rhi::ShaderModuleHandle fragment_;
    cy::rhi::PipelineLayoutHandle layout_;
    cy::rhi::GraphicsPipelineHandle single_;
    cy::rhi::GraphicsPipelineHandle multisampled_;
    cy::rhi::GraphicsPipelineHandle multiview_;
    cy::rhi::BufferHandle readback_;
};

cy::Status ViewProbe::prepare() noexcept {
    cy::rhi::Device& device = fixture_.device();
    const auto words = [](const auto& array) noexcept {
        return cy::Span<const u32>(array, sizeof(array) / sizeof(u32));
    };
    cy::Expected<cy::rhi::ShaderModuleHandle, cy::Error> vertex =
        module("view probe vertex", cy::rhi::ShaderStage::Vertex,
               words(cy::render_test::kViewProbeBaselineVertexSpirv));
    if (!vertex) {
        return cy::make_unexpected(vertex.error());
    }
    baseline_vertex_ = *vertex;
    cy::Expected<cy::rhi::ShaderModuleHandle, cy::Error> fragment =
        module("view probe fragment", cy::rhi::ShaderStage::Fragment,
               words(cy::render_test::kViewProbeFragmentSpirv));
    if (!fragment) {
        return cy::make_unexpected(fragment.error());
    }
    fragment_ = *fragment;

    const cy::rhi::PushConstantRange range{
        cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment, 0, sizeof(ViewPush)};
    cy::rhi::PipelineLayoutDescription layout;
    layout.name = "view probe layout";
    layout.push_constants = cy::Span<const cy::rhi::PushConstantRange>(&range, 1);
    cy::Expected<cy::rhi::PipelineLayoutHandle, cy::Error> layout_handle =
        device.create_pipeline_layout(layout);
    if (!layout_handle) {
        return cy::make_unexpected(layout_handle.error());
    }
    layout_ = *layout_handle;

    cy::Expected<cy::rhi::GraphicsPipelineHandle, cy::Error> single =
        pipeline("view probe 1x", baseline_vertex_, 1, 0);
    if (!single) {
        return cy::make_unexpected(single.error());
    }
    single_ = *single;
    cy::Expected<cy::rhi::GraphicsPipelineHandle, cy::Error> multisampled =
        pipeline("view probe 4x", baseline_vertex_, 4, 0);
    if (!multisampled) {
        return cy::make_unexpected(multisampled.error());
    }
    multisampled_ = *multisampled;

    if (device.capabilities().has(cy::rhi::Capability::Multiview)) {
        cy::Expected<cy::rhi::ShaderModuleHandle, cy::Error> view_vertex =
            module("view probe multi-view vertex", cy::rhi::ShaderStage::Vertex,
                   words(cy::render_test::kViewProbeMultiviewVertexSpirv));
        if (!view_vertex) {
            return cy::make_unexpected(view_vertex.error());
        }
        multiview_vertex_ = *view_vertex;
        cy::Expected<cy::rhi::GraphicsPipelineHandle, cy::Error> multiview =
            pipeline("view probe multi-view", multiview_vertex_, 1, (1U << kMaxViews) - 1U);
        if (!multiview) {
            return cy::make_unexpected(multiview.error());
        }
        multiview_ = *multiview;
    }

    cy::rhi::BufferDescription readback;
    readback.name = "view probe readback";
    readback.size = static_cast<cy::u64>(kMaxViews) * kTexels * sizeof(u32);
    readback.usage = cy::rhi::BufferUsage::TransferDestination;
    readback.memory = cy::rhi::MemoryUse::Readback;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> buffer = device.create_buffer(readback);
    if (!buffer) {
        return cy::make_unexpected(buffer.error());
    }
    readback_ = *buffer;
    return cy::ok();
}

void record_draw(const PassContext& context, void* user) noexcept {
    const auto* state = static_cast<const ViewProbe::PassState*>(user);
    cy::rhi::RenderAttachment colour;
    // The graph's choice, not the pass's: the 4x twin where the pass is multisampled, this view's
    // layer where multi-view is emulated, the target itself otherwise.
    colour.view = context.attachment_view(state->colour);
    colour.load = cy::rhi::LoadOp::Clear;
    colour.store = cy::rhi::StoreOp::Store;
    colour.clear.color[3] = 1.0F;

    cy::rhi::RenderingInfo info;
    info.render_area = cy::rhi::Rect2D{0, 0, kSide, kSide};
    info.color_attachments = cy::Span<const cy::rhi::RenderAttachment>(&colour, 1);
    info.view_mask = context.view_mask;
    context.commands->begin_rendering(info);
    context.commands->set_viewport(cy::rhi::Viewport{0.0F, 0.0F, static_cast<float>(kSide),
                                                     static_cast<float>(kSide), 0.0F, 1.0F});
    context.commands->set_scissor(cy::rhi::Rect2D{0, 0, kSide, kSide});
    context.commands->bind_graphics_pipeline(
        state->probe->pipeline_for(context.sample_count, context.view_mask));
    ViewPush push = state->request->push;
    push.view = context.view_index;
    context.commands->push_constants(
        state->probe->layout(), cy::rhi::ShaderStage::Vertex | cy::rhi::ShaderStage::Fragment, 0,
        cy::Span<const cy::u8>(reinterpret_cast<const cy::u8*>(&push), sizeof(push)));
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    const auto* state = static_cast<const ViewProbe::PassState*>(user);
    cy::rhi::BufferTextureCopy regions[kMaxViews];
    const u32 views = state->request->views;
    for (u32 layer = 0; layer < views; ++layer) {
        regions[layer] = cy::rhi::BufferTextureCopy{};
        regions[layer].buffer_offset = static_cast<cy::u64>(layer) * kTexels * sizeof(u32);
        regions[layer].base_layer = static_cast<u16>(layer);
        regions[layer].layer_count = 1;
        regions[layer].texture_extent = cy::rhi::Extent3D{kSide, kSide, 1};
    }
    context.commands->copy_texture_to_buffer(
        state->executor->texture(state->colour), state->probe->readback(),
        cy::Span<const cy::rhi::BufferTextureCopy>(regions, views));
}

cy::Status ViewProbe::render(const Request& request, cy::Array<u32>& out) noexcept {
    cy::rhi::Device& device = fixture_.device();
    if (cy::Expected<u32, cy::Error> began = device.begin_frame(); !began) {
        return cy::make_unexpected(began.error());
    }
    cy::rendering::RenderGraph graph(fixture_.allocator());
    cy::rendering::GraphExecutor executor(fixture_.allocator(), device);

    cy::rendering::TextureRequest texture;
    texture.name = "view probe colour";
    texture.format = cy::rhi::Format::Rgba8Unorm;
    texture.width = kSide;
    texture.height = kSide;
    texture.array_layers = static_cast<u16>(request.views);
    const ResourceId colour = graph.create_texture(texture);

    cy::rendering::BufferRequest readback;
    readback.name = "view probe readback";
    readback.size = static_cast<cy::u64>(kMaxViews) * kTexels * sizeof(u32);
    readback.extra_usage = cy::rhi::BufferUsage::TransferDestination;
    const ResourceId out_buffer = graph.import_buffer(readback, readback_);

    PassState state{this, &request, colour, out_buffer, &executor};
    cy::rendering::PassBuilder draw = graph.add_pass("view probe draw", QueueKind::Graphics);
    if (request.samples > 1 || request.say_one_sample) {
        draw.multisample(request.samples);
    }
    if (request.views > 1) {
        draw.views(request.views);
    }
    draw.write(colour, Access::ColorAttachmentWrite).record(&record_draw, &state);
    if (request.samples > 1) {
        draw.resolve(colour);
    }
    graph.add_pass("view probe readback", QueueKind::Graphics)
        .read(colour, Access::TransferRead)
        .write(out_buffer, Access::TransferWrite)
        .record(&record_readback, &state);
    graph.add_pass("view probe host", QueueKind::Graphics)
        .read(out_buffer, Access::HostRead)
        .side_effect();

    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    if (!result) {
        return cy::make_unexpected(result.error());
    }
    if (cy::Status idle = device.wait_idle(); !idle) {
        return idle;
    }
    if (cy::Status ended = device.end_frame(); !ended) {
        return ended;
    }
    const u32 texels = kTexels * request.views;
    if (cy::Status sized = out.resize(texels); !sized) {
        return sized;
    }
    const auto* bytes = static_cast<const u32*>(device.buffer_mapped_pointer(readback_));
    if (bytes == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the readback buffer is not mapped");
    }
    for (u32 index = 0; index < texels; ++index) {
        out[index] = bytes[index];
    }
    executor.release();
    return cy::ok();
}

struct Coverage {
    /// Pixels neither empty nor full: an edge the rasteriser blended.
    u32 partial = 0;
    /// Pixels fully covered.
    u32 full = 0;
    /// The red channel summed over the image, in units of one full pixel.
    double area = 0.0;
};

Coverage measure(const cy::Array<u32>& pixels) noexcept {
    Coverage coverage;
    for (u32 index = 0; index < kTexels; ++index) {
        const u32 red = pixels[index] & 0xFFU;
        coverage.area += static_cast<double>(red) / 255.0;
        if (red == 255U) {
            ++coverage.full;
        } else if (red != 0U) {
            ++coverage.partial;
        }
    }
    return coverage;
}

bool same_bytes(const cy::Array<u32>& a, const cy::Array<u32>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (cy::usize index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("4x edges are smoother than 1x, through the graph's twin and resolve") {
    ViewProbe probe(true);
    if (!probe.have_vulkan()) {
        probe.report_skip();
        return;
    }
    CY_REQUIRE(probe.prepare().has_value());
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::Array<u32> single(allocator);
    cy::Array<u32> multisampled(allocator);
    Request one;
    Request four;
    four.samples = 4;
    CY_REQUIRE(probe.render(one, single).has_value());
    CY_REQUIRE(probe.render(four, multisampled).has_value());

    const Coverage hard = measure(single);
    const Coverage soft = measure(multisampled);
    std::fprintf(stderr,
                 "1x: %u full, %u partial, area %.1f | 4x: %u full, %u partial, area %.1f\n",
                 hard.full, hard.partial, hard.area, soft.full, soft.partial, soft.area);

    // The triangle is there at both counts.
    CY_REQUIRE(hard.full > kTexels / 8);
    // 1x HAS NO PARTIAL PIXELS — one sample a pixel is covered or not.
    CY_CHECK_EQ(hard.partial, 0U);
    // 4x blends its edges: the triangle's perimeter is roughly 160 pixels at this size, and every
    // oblique edge crosses a pixel it does not fill. A resolve that never ran leaves the target at
    // its clear colour, and then neither this nor the area check below holds.
    CY_CHECK_GE(soft.partial, kSide);
    // AND THE SAME TRIANGLE: coverage is area, and averaging samples must not move it. Within 3%.
    CY_CHECK_GT(soft.area, hard.area * 0.97);
    CY_CHECK_LT(soft.area, hard.area * 1.03);
    CY_CHECK_EQ(probe.validation_errors(), 0U);
}

CY_TEST_CASE("1x declared through the attachment model is byte-identical to the pass before it") {
    ViewProbe probe(true);
    if (!probe.have_vulkan()) {
        probe.report_skip();
        return;
    }
    CY_REQUIRE(probe.prepare().has_value());
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::Array<u32> before(allocator);
    cy::Array<u32> after(allocator);
    Request legacy;
    Request declared;
    declared.say_one_sample = true;
    CY_REQUIRE(probe.render(legacy, before).has_value());
    CY_REQUIRE(probe.render(declared, after).has_value());
    CY_CHECK(same_bytes(before, after));
    CY_CHECK_GT(measure(before).full, 0U);
    CY_CHECK_EQ(probe.validation_errors(), 0U);
}

CY_TEST_CASE("multi-view and its per-view baseline render the same layers, chosen by capability") {
    ViewProbe with(true);
    if (!with.have_vulkan()) {
        with.report_skip();
        return;
    }
    // THE REGRESSION: the capability used to be reported with the feature never enabled, so the
    // view-masked pipeline below was invalid usage. It is now the feature's own answer.
    CY_REQUIRE(with.device().capabilities().has(cy::rhi::Capability::Multiview));
    CY_REQUIRE(with.prepare().has_value());

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    Request stereo;
    stereo.views = 2;
    stereo.push.colors[0][1] = 0.0F;  // view 0: magenta
    stereo.push.colors[1][0] = 0.0F;  // view 1: cyan
    cy::Array<u32> native(allocator);
    CY_REQUIRE(with.render(stereo, native).has_value());
    CY_CHECK_EQ(with.validation_errors(), 0U);

    // Each layer holds its own view's colour and none of the other's.
    const u32 centre = ((kSide / 2) * kSide) + (kSide / 2);
    CY_CHECK_EQ(native[centre], 0xFFFF00FFU);            // RGBA8 magenta, little-endian
    CY_CHECK_EQ(native[kTexels + centre], 0xFFFFFF00U);  // cyan
    CY_CHECK_EQ(native[0], 0xFF000000U);                 // the clear, outside the triangle

    // The baseline: a device created WITHOUT multi-view, so the capability is absent and the
    // executor records the pass once per view into single-layer views.
    ViewProbe without(false);
    CY_REQUIRE(without.have_vulkan());
    CY_REQUIRE_FALSE(without.device().capabilities().has(cy::rhi::Capability::Multiview));
    CY_REQUIRE(without.prepare().has_value());
    cy::Array<u32> emulated(allocator);
    CY_REQUIRE(without.render(stereo, emulated).has_value());
    CY_CHECK_EQ(without.validation_errors(), 0U);
    const bool agree = same_bytes(native, emulated);
    CY_CHECK(agree);
    // Printed only after both devices rendered and read back, so a skipping run cannot print it —
    // `m11d:multiview-by-capability` requires this line.
    std::fprintf(stderr, "multi-view: %u texels over %u layers, native and per-view baseline %s\n",
                 kTexels * stereo.views, stereo.views, agree ? "agree" : "DIFFER");
}
