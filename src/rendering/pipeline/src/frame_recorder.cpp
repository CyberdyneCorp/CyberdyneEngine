// The record callbacks `FrameSinks` has been waiting for. M8.c tasks 1b.1 and 1b.2.

#include <cy/rendering/pipeline/frame_recorder.h>

#include <cy/core/math/matrix.h>

namespace cy::rendering::pipeline {
namespace {

/// Copy a `Mat4` out as four ROWS. `cy::Mat4` is column-major with column vectors, and
/// `cy/frame.slang` takes four rows and dots them — see that file's header for why it is rows and
/// not a matrix type. This is the one place the two conventions meet, which is the point.
void write_rows(const Mat4& matrix, f32 out[16]) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[(row * 4U) + column] = matrix.columns[column][row];
        }
    }
}

[[nodiscard]] FrameRecorder* recorder_of(void* user) noexcept {
    return static_cast<FrameRecorder*>(user);
}

/// The attachment the shading target is. MSAA is refused at `bind()`, so the single-sample colour
/// is the only case that reaches here.
[[nodiscard]] rhi::RenderAttachment color_attachment(const GraphExecutor& executor,
                                                     ResourceId resource, bool clear) noexcept {
    rhi::RenderAttachment attachment;
    attachment.view = executor.view(resource);
    attachment.load = clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
    attachment.store = rhi::StoreOp::Store;
    // A dark neutral rather than black: a frame that failed to shade and a frame that cleared are
    // then different pictures, which is the whole reason a capture is worth taking.
    attachment.clear.color[0] = 0.02F;
    attachment.clear.color[1] = 0.025F;
    attachment.clear.color[2] = 0.035F;
    attachment.clear.color[3] = 1.0F;
    return attachment;
}

[[nodiscard]] rhi::RenderAttachment depth_attachment(const GraphExecutor& executor,
                                                     ResourceId resource, bool clear) noexcept {
    rhi::RenderAttachment attachment;
    attachment.view = executor.view(resource);
    attachment.load = clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
    attachment.store = rhi::StoreOp::Store;
    // REVERSED-Z. A pass that cleared to 1.0 would look correct until something intersected, which
    // is design.md §3's own warning and is why this is `reversed_z_depth_clear()` and not a
    // literal.
    attachment.clear = rhi::reversed_z_depth_clear();
    return attachment;
}

void set_full_viewport(rhi::CommandBuffer& commands, u32 width, u32 height) noexcept {
    const rhi::Viewport viewport{0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height),
                                 0.0F, 1.0F};
    commands.set_viewport(viewport);
    commands.set_scissor(rhi::Rect2D{0, 0, width, height});
}

/// Bind the frame's three sets, ONCE PER PASS SCOPE rather than once per layer that has draws.
///
/// A pass whose own layer is empty still has to leave the sets bound: an extension records after it
/// — the particle renderer binds only its own set 2 and reads the frame block out of set 1 — and a
/// transparent stage with no transparent meshes is the ordinary case rather than a rare one. The
/// first version of this bound only inside `draw_layer`, and the symptom was seventeen
/// "uses set #1 but that set is not bound" on a frame whose transparent layer happened to be empty.
void bind_frame_sets(rhi::CommandBuffer& commands, const FramePipelines& pipelines,
                     const FrameBindings& bindings) noexcept {
    commands.bind_descriptor_sets(pipelines.layout(), 0, bindings.sets());
}

/// The draws of one sort layer, and where they begin in the whole list — which is what the push
/// constant needs, because `cyDrawInstances` is uploaded in the list's own order.
struct LayerRange {
    const render::DrawItem* items = nullptr;
    const GpuDrawInstance* instances = nullptr;
    u32 first = 0;
    u32 count = 0;
};

[[nodiscard]] LayerRange layer_range(const FrameAssembly& assembly,
                                     render::SortLayer layer) noexcept {
    const DrawList& list = assembly.draws();
    const Span<const render::DrawItem> slice = assembly.layer(layer);
    LayerRange range;
    if (slice.empty() || list.items.empty()) {
        return range;
    }
    range.first = static_cast<u32>(slice.data() - list.items.data());
    range.count = static_cast<u32>(slice.size());
    range.items = list.items.data();
    range.instances = list.instances.data();
    return range;
}

void record_extensions(FrameRecorder& recorder, const PassContext& context, FramePassKind kind,
                       u32 width, u32 height, bool inside_rendering) noexcept {
    for (const PassExtension& extension : recorder.extensions()) {
        if (extension.kind != kind || extension.record == nullptr) {
            continue;
        }
        ExtensionContext arguments;
        arguments.commands = context.commands;
        arguments.executor = context.executor;
        arguments.recorder = &recorder;
        arguments.kind = kind;
        arguments.width = width;
        arguments.height = height;
        arguments.inside_rendering = inside_rendering;
        extension.record(arguments, extension.user);
        ++recorder.mutable_report().extensions_run;
    }
}

/// One geometry pass: bind the state, walk the layer, draw what has geometry.
void draw_layer(FrameRecorder& recorder, const PassContext& context, FramePipelineKind pipeline,
                render::SortLayer layer, u32& counter) noexcept {
    FrameAssembly& assembly = *recorder.assembly();
    const GeometrySource& geometry = recorder.geometry();
    const LayerRange range = layer_range(assembly, layer);
    if (range.count == 0) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_graphics_pipeline(recorder.pipelines()->pipeline(pipeline));

    const bool depth_only = pipeline == FramePipelineKind::Depth;
    const u64 offsets[3] = {0, 0, 0};
    commands.bind_vertex_buffers(
        0, Span<const rhi::BufferHandle>(geometry.streams, depth_only ? 1U : 3U),
        Span<const u64>(offsets, depth_only ? 1U : 3U));

    rhi::BufferHandle bound_indices;
    for (u32 offset = 0; offset < range.count; ++offset) {
        const u32 index = range.first + offset;
        DrawGeometry draw;
        if (!geometry.geometry(range.items[index], range.instances[index], geometry.user, draw)) {
            ++recorder.mutable_report().skipped_draws;
            continue;
        }
        const DrawPush push{index};
        commands.push_constants(recorder.pipelines()->layout(),
                                rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(push)));
        if (draw.indices.is_null()) {
            commands.draw(draw.vertex_count, 1, 0, 0);
        } else {
            if (!(draw.indices == bound_indices)) {
                commands.bind_index_buffer(draw.indices, 0, draw.wide_indices);
                bound_indices = draw.indices;
            }
            commands.draw_indexed(draw.index_count, 1, draw.first_index, draw.vertex_offset, 0);
        }
        ++counter;
    }
}

// --- The callbacks -------------------------------------------------------------------------

/// Stage 1. The transfer `ForwardFrame` declared and nobody recorded.
///
/// The frame's own `lights` and `draw_instances` are graph-owned, device-local and declared
/// `Access::TransferWrite` here; the ring is host-visible. So this is the copy, and the barrier the
/// graph derived around it now guards a transfer that happens.
void record_prepare(const PassContext& context, void* user) noexcept {
    FrameRecorder& recorder = *recorder_of(user);
    const FrameResources& resources = recorder.assembly()->resources();
    const FrameBindings& bindings = *recorder.bindings();

    struct Transfer {
        ResourceId destination = kInvalidResource;
        rhi::BufferHandle source;
        u64 bytes = 0;
    };
    const Transfer transfers[] = {
        {resources.lights, bindings.staged_lights(), bindings.staged_light_bytes()},
        {resources.draw_instances, bindings.staged_draws(), bindings.staged_draw_bytes()},
    };
    for (const Transfer& transfer : transfers) {
        if (transfer.destination == kInvalidResource || transfer.bytes == 0 ||
            transfer.source.is_null()) {
            continue;
        }
        const rhi::BufferHandle destination = context.executor->buffer(transfer.destination);
        if (destination.is_null()) {
            continue;
        }
        rhi::BufferCopy region;
        region.size = transfer.bytes;
        context.commands->copy_buffer(transfer.source, destination,
                                      Span<const rhi::BufferCopy>(&region, 1));
        recorder.mutable_report().uploaded_bytes += transfer.bytes;
    }
    record_extensions(recorder, context, FramePassKind::Prepare, 0, 0, false);
    ++recorder.mutable_report().passes;
}

void record_depth_prepass(const PassContext& context, void* user) noexcept {
    FrameRecorder& recorder = *recorder_of(user);
    FrameAssembly& assembly = *recorder.assembly();
    const FrameResources& resources = assembly.resources();
    const AssemblyDescription& description = assembly.description();

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, description.width, description.height};
    info.depth_attachment = depth_attachment(*context.executor, resources.depth, true);
    context.commands->begin_rendering(info);
    set_full_viewport(*context.commands, description.width, description.height);
    bind_frame_sets(*context.commands, *recorder.pipelines(), *recorder.bindings());
    draw_layer(recorder, context, FramePipelineKind::Depth, render::SortLayer::Opaque,
               recorder.mutable_report().prepass_draws);
    record_extensions(recorder, context, FramePassKind::DepthPrepass, description.width,
                      description.height, true);
    context.commands->end_rendering();
    ++recorder.mutable_report().passes;
}

void record_opaque(const PassContext& context, void* user) noexcept {
    FrameRecorder& recorder = *recorder_of(user);
    FrameAssembly& assembly = *recorder.assembly();
    const FrameResources& resources = assembly.resources();
    const AssemblyDescription& description = assembly.description();

    const rhi::RenderAttachment color = color_attachment(*context.executor, resources.color, true);
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, description.width, description.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    // Loaded, not cleared: the prepass wrote it and the opaque pass compares EQUAL against it.
    info.depth_attachment = depth_attachment(*context.executor, resources.depth, false);
    context.commands->begin_rendering(info);
    set_full_viewport(*context.commands, description.width, description.height);
    bind_frame_sets(*context.commands, *recorder.pipelines(), *recorder.bindings());
    draw_layer(recorder, context, FramePipelineKind::Opaque, render::SortLayer::Opaque,
               recorder.mutable_report().opaque_draws);
    record_extensions(recorder, context, FramePassKind::Opaque, description.width,
                      description.height, true);
    context.commands->end_rendering();
    ++recorder.mutable_report().passes;
}

void record_transparent(const PassContext& context, void* user) noexcept {
    FrameRecorder& recorder = *recorder_of(user);
    FrameAssembly& assembly = *recorder.assembly();
    const FrameResources& resources = assembly.resources();
    const AssemblyDescription& description = assembly.description();

    const rhi::RenderAttachment color = color_attachment(*context.executor, resources.color, false);
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, description.width, description.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment = depth_attachment(*context.executor, resources.depth, false);
    context.commands->begin_rendering(info);
    set_full_viewport(*context.commands, description.width, description.height);
    bind_frame_sets(*context.commands, *recorder.pipelines(), *recorder.bindings());
    if (!recorder.pipelines()->pipeline(FramePipelineKind::Transparent).is_null()) {
        draw_layer(recorder, context, FramePipelineKind::Transparent,
                   render::SortLayer::Transparent, recorder.mutable_report().transparent_draws);
    }
    // THE PARTICLE RENDERER'S SEAM. `vfx-system` wants particles composited after the transparent
    // draws and before the tonemap, which is exactly here.
    record_extensions(recorder, context, FramePassKind::Transparent, description.width,
                      description.height, true);
    context.commands->end_rendering();
    ++recorder.mutable_report().passes;
}

/// Stage 11. `cy/fullscreen.slang`'s own resolve, straight into the frame's output.
void record_post_process(const PassContext& context, void* user) noexcept {
    FrameRecorder& recorder = *recorder_of(user);
    FrameAssembly& assembly = *recorder.assembly();
    const FrameResources& resources = assembly.resources();
    const AssemblyDescription& description = assembly.description();
    FrameBindings& bindings = *recorder.bindings();

    // The scene colour is a transient the graph realised a moment ago, so its view cannot be named
    // before this point. That is why the pass set is written here and the view set is written in
    // `upload`.
    if (Status bound = bindings.bind_scene_color(context.executor->view(resources.color)); !bound) {
        return;
    }
    const rhi::RenderAttachment color = color_attachment(*context.executor, resources.output, true);
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, description.width, description.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    context.commands->begin_rendering(info);
    set_full_viewport(*context.commands, description.width, description.height);
    bind_frame_sets(*context.commands, *recorder.pipelines(), bindings);
    context.commands->bind_graphics_pipeline(
        recorder.pipelines()->pipeline(FramePipelineKind::Resolve));
    // One oversized triangle, its positions derived from `SV_VertexID`. No vertex buffer, which is
    // why this pipeline has no vertex bindings at all.
    context.commands->draw(3, 1, 0, 0);
    record_extensions(recorder, context, FramePassKind::PostProcess, description.width,
                      description.height, true);
    context.commands->end_rendering();
    ++recorder.mutable_report().passes;
}

}  // namespace

Status FrameRecorder::initialize(FramePipelines& pipelines, FrameBindings& bindings) noexcept {
    if (!pipelines.ready()) {
        return fail(ErrorCode::InvalidArgument, "frame recorder: the pipelines are not ready");
    }
    if (!bindings.ready()) {
        return fail(ErrorCode::InvalidArgument, "frame recorder: the bindings are not ready");
    }
    pipelines_ = &pipelines;
    bindings_ = &bindings;
    return ok();
}

Status FrameRecorder::add_extension(const PassExtension& extension) noexcept {
    if (extension.record == nullptr || extension.kind == FramePassKind::Count) {
        return fail(ErrorCode::InvalidArgument,
                    "frame recorder: an extension needs a stage and a "
                    "callback");
    }
    if (extension_count_ >= kMaxPassExtensions) {
        return fail(ErrorCode::OutOfRange, "frame recorder: too many pass extensions");
    }
    extensions_[extension_count_++] = extension;
    return ok();
}

Status FrameRecorder::bind(FrameAssembly& assembly) noexcept {
    if (pipelines_ == nullptr || bindings_ == nullptr) {
        return fail(ErrorCode::Unavailable, "frame recorder: not initialized");
    }
    if (!geometry_.complete()) {
        return fail(ErrorCode::InvalidArgument,
                    "frame recorder: the geometry source has no streams or no lookup — the mesh "
                    "table is the caller's and this layer holds no copy of it");
    }
    const AssemblyDescription& description = assembly.description();
    if (description.color_format != pipelines_->setup().color_format ||
        description.depth_format != pipelines_->setup().depth_format) {
        return fail(ErrorCode::InvalidArgument,
                    "frame recorder: the pipelines were created for different attachment formats "
                    "than the frame declares");
    }
    if (pipelines_->setup().sample_count != 1) {
        return fail(ErrorCode::NotImplemented,
                    "frame recorder: multisampled frames are not recorded by this layer — the "
                    "shading target would be `color_multisampled` and the resolve a second pass");
    }
    assembly_ = &assembly;
    reset_report();
    return ok();
}

FrameSinks FrameRecorder::sinks() noexcept {
    FrameSinks sinks;
    const auto attach = [&](FramePassKind kind, RecordFn function) noexcept {
        sinks.passes[static_cast<usize>(kind)] = FramePassCallback{function, this};
    };
    attach(FramePassKind::Prepare, &record_prepare);
    attach(FramePassKind::DepthPrepass, &record_depth_prepass);
    attach(FramePassKind::Opaque, &record_opaque);
    attach(FramePassKind::Transparent, &record_transparent);
    attach(FramePassKind::PostProcess, &record_post_process);
    return sinks;
}

FrameUpload upload_for(const FrameAssembly& assembly, const AssemblyReport& report,
                       const Mat4& relative_to_clip, const Mat4& relative_to_view,
                       Span<const InstanceTransform> instances, const GlobalsData& globals,
                       const u32 material_offsets[4]) noexcept {
    const AssemblyDescription& description = assembly.description();
    const ClusterAssignment& clusters = assembly.clusters();

    FrameUpload upload;
    upload.globals = globals;
    write_rows(relative_to_clip, upload.view.relative_to_clip);
    write_rows(relative_to_view, upload.view.relative_to_view);
    const Vec3 ambient = assembly.sky_irradiance();
    upload.view.ambient_and_occlusion[0] = ambient.x;
    upload.view.ambient_and_occlusion[1] = ambient.y;
    upload.view.ambient_and_occlusion[2] = ambient.z;
    upload.view.ambient_and_occlusion[3] = 1.0F;
    upload.view.extent_and_inverse[0] = static_cast<f32>(description.width);
    upload.view.extent_and_inverse[1] = static_cast<f32>(description.height);
    upload.view.extent_and_inverse[2] =
        description.width == 0 ? 0.0F : 1.0F / static_cast<f32>(description.width);
    upload.view.extent_and_inverse[3] =
        description.height == 0 ? 0.0F : 1.0F / static_cast<f32>(description.height);

    // The grid is copied field for field rather than translated: `ClusterGrid` is asserted in
    // forward/cluster.h to BE `cy/cluster.slang`'s struct, so a translation here would be a second
    // description of one layout. It is rebuilt from the description rather than read off the
    // assembly because the assembly keeps its grid private — and `make_cluster_grid` is a pure
    // function of the same four numbers, so the two cannot disagree.
    ClusterGrid grid;
    if (Expected<ClusterGrid, Error> made =
            make_cluster_grid(description.clusters, description.width, description.height,
                              description.near_plane, description.far_plane);
        made.has_value()) {
        grid = *made;
    }
    upload.view.cluster_dimensions[0] = grid.dimensions[0];
    upload.view.cluster_dimensions[1] = grid.dimensions[1];
    upload.view.cluster_dimensions[2] = grid.dimensions[2];
    upload.view.max_elements_per_cluster = grid.max_elements_per_cluster;
    upload.view.near_plane = grid.near_plane;
    upload.view.far_plane = grid.far_plane;
    upload.view.slice_scale = grid.slice_scale;
    upload.view.slice_bias = grid.slice_bias;

    upload.view.counts[0] = report.lights;
    upload.view.counts[1] = kMaterialBlockBytes / 4U;
    upload.view.counts[2] = kClusterElementTypeCount;
    upload.view.counts[3] = clusters.headers.empty() ? 0U : 1U;
    for (u32 index = 0; index < 4U; ++index) {
        upload.view.material_offsets[index] = material_offsets[index];
    }

    upload.lights = assembly.lights();
    upload.cluster_headers = clusters.headers.span();
    upload.cluster_indices = clusters.indices.span();
    upload.draws = assembly.draws().instances.span();
    upload.instances = instances;
    upload.materials = assembly.materials().bytes();
    return upload;
}

}  // namespace cy::rendering::pipeline
