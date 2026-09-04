#include "probe.h"

#include <cy/backends/rhi/access.h>

namespace cy::render_test {
namespace {

using rendering::PassContext;
using rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

}  // namespace

/// The probe draws one triangle per draw — each carries its own colour in the push block — so the
/// ceiling is small and the scratch is a fixed array rather than an allocation.
inline constexpr u32 kMaxProbeTriangles = 64;
inline constexpr u32 kMaxProbeVertices = kMaxProbeTriangles * 3;

/// What the record callbacks need. A struct rather than captures, because `RecordFn` is a plain
/// function pointer — the engine has no exceptions and no per-frame allocation on this path.
struct ProbeFixture::PassState {
    rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::BufferHandle vertices;
    rhi::BufferHandle color_readback;
    rhi::BufferHandle depth_readback;
    ResourceId color = rendering::kInvalidResource;
    ResourceId depth = rendering::kInvalidResource;
    ResourceId color_out = rendering::kInvalidResource;
    ResourceId depth_out = rendering::kInvalidResource;
    ProbePush push{};
    u32 vertex_count = 0;
    /// One colour per triangle. Carried here rather than re-read from the caller's span because
    /// `RecordFn` is a plain function pointer and the span's lifetime ends at the record call.
    f32 colors[kMaxProbeTriangles][4] = {};
};

namespace {

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ProbeFixture::PassState*>(user);

    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    color.clear.color[0] = 0.0F;
    color.clear.color[1] = 0.0F;
    color.clear.color[2] = 0.0F;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kProbeExtent, kProbeExtent};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    // REVERSED-Z, AND THE CLEAR IS THE HALF THAT IS EASY TO GET WRONG. A pass that cleared to 1.0
    // would look correct until something intersected, which is design.md §3's own warning.
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    const rhi::Viewport viewport{
        0.0F, 0.0F, static_cast<f32>(kProbeExtent), static_cast<f32>(kProbeExtent), 0.0F, 1.0F};
    context.commands->set_viewport(viewport);
    context.commands->set_scissor(rhi::Rect2D{0, 0, kProbeExtent, kProbeExtent});
    context.commands->bind_graphics_pipeline(state->pipeline);

    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                          Span<const u64>(&offset, 1));
    // One draw per triangle, because each carries its own colour in the push block. The probe is
    // about conventions rather than about batching.
    for (u32 triangle = 0; triangle < state->vertex_count; ++triangle) {
        ProbePush push = state->push;
        const u32 base = triangle * 3;
        for (u32 channel = 0; channel < 4; ++channel) {
            push.color[channel] = state->colors[triangle][channel];
        }
        context.commands->push_constants(
            state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(ProbePush)));
        context.commands->draw(3, 1, base, 0);
    }
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ProbeFixture::PassState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kProbeExtent, kProbeExtent, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->color_readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
    context.commands->copy_texture_to_buffer(state->executor->texture(state->depth),
                                             state->depth_readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace

Status ProbeFixture::prepare() noexcept {
    if (!have_vulkan()) {
        return fail(ErrorCode::Unavailable, "no Vulkan device");
    }
    rhi::Device& device = *device_.value();

    rhi::ShaderModuleDescription vertex;
    vertex.name = "convention vertex";
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.entry_point = "main";
    vertex.spirv =
        Span<const u32>(kConventionVertexSpirv, sizeof(kConventionVertexSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> vertex_module = device.create_shader_module(vertex);
    if (!vertex_module.has_value()) {
        return make_unexpected(vertex_module.error());
    }
    vertex_shader_ = *vertex_module;

    rhi::ShaderModuleDescription fragment;
    fragment.name = "convention fragment";
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.entry_point = "main";
    fragment.spirv =
        Span<const u32>(kConventionFragmentSpirv, sizeof(kConventionFragmentSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> fragment_module =
        device.create_shader_module(fragment);
    if (!fragment_module.has_value()) {
        return make_unexpected(fragment_module.error());
    }
    fragment_shader_ = *fragment_module;

    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(ProbePush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "convention layout";
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
        device.create_pipeline_layout(layout);
    if (!layout_handle.has_value()) {
        return make_unexpected(layout_handle.error());
    }
    layout_ = *layout_handle;

    const rhi::VertexBinding binding{0, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attribute{0, 0, rhi::Format::Rgb32Sfloat, 0};
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba8Unorm;

    rhi::GraphicsPipelineDescription pipeline;
    pipeline.name = "convention probe";
    pipeline.layout = layout_;
    pipeline.vertex_shader = vertex_shader_;
    pipeline.fragment_shader = fragment_shader_;
    pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(&attribute, 1);
    pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    // NO CULLING. The probe's triangles are authored for the cases rather than for a winding rule,
    // and a case about handedness that silently culled its own geometry would report a convention
    // error as an empty image.
    pipeline.rasterisation.cull_mode = rhi::CullMode::None;
    pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
    pipeline.depth_stencil.depth_test_enable = true;
    pipeline.depth_stencil.depth_write_enable = true;
    // The default is GreaterOrEqual and it is left at the default deliberately:
    // `rhi::DepthStencilState` documents that a pipeline wanting `Less` has to say so, and this
    // suite is what makes that default a measured fact rather than a comment.
    Expected<rhi::GraphicsPipelineHandle, Error> pipeline_handle =
        device.create_graphics_pipeline(pipeline);
    if (!pipeline_handle.has_value()) {
        return make_unexpected(pipeline_handle.error());
    }
    pipeline_ = *pipeline_handle;

    rhi::BufferDescription vertices;
    vertices.name = "probe vertices";
    vertices.size = static_cast<u64>(kMaxProbeVertices) * sizeof(f32) * 3;
    vertices.usage = rhi::BufferUsage::Vertex;
    vertices.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> vertex_buffer = device.create_buffer(vertices);
    if (!vertex_buffer.has_value()) {
        return make_unexpected(vertex_buffer.error());
    }
    vertices_ = *vertex_buffer;

    rhi::BufferDescription readback;
    readback.name = "probe colour readback";
    readback.size = static_cast<u64>(kProbeTexels) * sizeof(u32);
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> color_buffer = device.create_buffer(readback);
    if (!color_buffer.has_value()) {
        return make_unexpected(color_buffer.error());
    }
    color_readback_ = *color_buffer;

    readback.name = "probe depth readback";
    Expected<rhi::BufferHandle, Error> depth_buffer = device.create_buffer(readback);
    if (!depth_buffer.has_value()) {
        return make_unexpected(depth_buffer.error());
    }
    depth_readback_ = *depth_buffer;
    return ok();
}

Status ProbeFixture::render(Span<const ProbeTriangle> triangles, const Mat4& relative_to_clip,
                            ProbeImage& out) noexcept {
    if (pipeline_.is_null()) {
        return fail(ErrorCode::Unavailable, "probe: prepare() was not called");
    }
    if (triangles.size() * 3 > kMaxProbeVertices) {
        return fail(ErrorCode::InvalidArgument, "probe: too many triangles");
    }
    rhi::Device& device = *device_.value();
    // `begin_frame` answers with the frame's index; a test wants only whether it succeeded.
    if (Expected<u32, Error> began = device.begin_frame(); !began.has_value()) {
        return make_unexpected(began.error());
    }

    auto* vertex_bytes = static_cast<f32*>(device.buffer_mapped_pointer(vertices_));
    if (vertex_bytes == nullptr) {
        return fail(ErrorCode::Internal, "probe: the vertex buffer is not mapped");
    }
    u32 written = 0;
    for (const ProbeTriangle& triangle : triangles) {
        for (const Vec3& vertex : triangle.vertices) {
            vertex_bytes[written++] = vertex.x;
            vertex_bytes[written++] = vertex.y;
            vertex_bytes[written++] = vertex.z;
        }
    }

    rendering::RenderGraph graph(allocator_);
    rendering::GraphExecutor executor(allocator_, device);

    rendering::TextureRequest color_request;
    color_request.name = "probe colour";
    color_request.format = rhi::Format::Rgba8Unorm;
    color_request.width = kProbeExtent;
    color_request.height = kProbeExtent;
    const ResourceId color = graph.create_texture(color_request);

    rendering::TextureRequest depth_request;
    depth_request.name = "probe depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kProbeExtent;
    depth_request.height = kProbeExtent;
    const ResourceId depth = graph.create_texture(depth_request);

    rendering::BufferRequest readback_request;
    readback_request.name = "probe readback";
    readback_request.size = static_cast<u64>(kProbeTexels) * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, color_readback_);
    const ResourceId depth_out = graph.import_buffer(readback_request, depth_readback_);

    PassState state;
    state.executor = &executor;
    state.pipeline = pipeline_;
    state.layout = layout_;
    state.vertices = vertices_;
    state.color_readback = color_readback_;
    state.depth_readback = depth_readback_;
    state.color = color;
    state.depth = depth;
    state.color_out = color_out;
    state.depth_out = depth_out;
    state.vertex_count = static_cast<u32>(triangles.size());
    for (usize index = 0; index < triangles.size(); ++index) {
        for (u32 channel = 0; channel < 4; ++channel) {
            state.colors[index][channel] = triangles[index].color[channel];
        }
    }
    set_matrix(state.push, relative_to_clip);

    // The draw pass. Note what it declares and what it does not: a colour write, a depth write and
    // nothing else. Every barrier around it — including the depth image's transition out of
    // UNDEFINED and the colour image's transition to TRANSFER_SRC for the copy — is derived.
    graph.add_pass("probe draw", QueueKind::Graphics)
        .write(color, Access::ColorAttachmentWrite)
        .write(depth, Access::DepthStencilAttachmentWrite)
        .record(&record_draw, &state);
    graph.add_pass("probe readback", QueueKind::Graphics)
        .read(color, Access::TransferRead)
        .read(depth, Access::TransferRead)
        .write(color_out, Access::TransferWrite)
        .write(depth_out, Access::TransferWrite)
        .record(&record_readback, &state);
    // The host boundary declared as a dependency, so the graph emits the transfer-to-host barrier
    // rather than the test relying on coherent memory and a fence. (M3's spike, gotcha 6f.)
    graph.add_pass("probe host", QueueKind::Graphics)
        .read(color_out, Access::HostRead)
        .read(depth_out, Access::HostRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }

    Expected<rendering::ExecutionResult, Error> result =
        executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }
    if (Status idle = device.wait_idle(); !idle) {
        return idle;
    }
    if (Status ended = device.end_frame(); !ended) {
        return ended;
    }

    if (Status sized = out.color.resize(kProbeTexels); !sized) {
        return sized;
    }
    if (Status sized = out.depth.resize(kProbeTexels); !sized) {
        return sized;
    }
    const auto* color_bytes =
        static_cast<const u32*>(device.buffer_mapped_pointer(color_readback_));
    const auto* depth_bytes =
        static_cast<const f32*>(device.buffer_mapped_pointer(depth_readback_));
    if (color_bytes == nullptr || depth_bytes == nullptr) {
        return fail(ErrorCode::Internal, "probe: a readback buffer is not mapped");
    }
    for (u32 index = 0; index < kProbeTexels; ++index) {
        out.color[index] = color_bytes[index];
        out.depth[index] = depth_bytes[index];
    }
    executor.release();
    return ok();
}

}  // namespace cy::render_test
