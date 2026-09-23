// SPDX-License-Identifier: MIT
#include <cy/rendering/virtual_geometry/forward_visibility.h>

#include <cy/rendering/graph/executor.h>

#include <cstring>

#include "../shaders/vg_visbuffer_spirv.h"
#include "visbuffer_internal.h"

namespace cy::rendering::vg {

using namespace detail;

namespace {

/// The target clears to zero, and zero is the complement of the "no surface" word — see
/// `vgVisHwVertex`. So the clear value is the ordinary one and no NaN bit pattern has to survive a
/// float round trip through `ClearValue`.
[[nodiscard]] rhi::RenderAttachment visibility_attachment(rhi::TextureViewHandle view) noexcept {
    rhi::RenderAttachment attachment;
    attachment.view = view;
    attachment.load = rhi::LoadOp::Clear;
    attachment.store = rhi::StoreOp::Store;
    return attachment;
}

[[nodiscard]] rhi::RenderAttachment depth_attachment(rhi::TextureViewHandle view,
                                                     bool clear) noexcept {
    rhi::RenderAttachment attachment;
    attachment.view = view;
    attachment.load = clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
    attachment.store = rhi::StoreOp::Store;
    attachment.clear = rhi::reversed_z_depth_clear();
    return attachment;
}

}  // namespace

ForwardVisibility::ForwardVisibility(rhi::Device& device) noexcept : device_(device) {}

ForwardVisibility::~ForwardVisibility() {
    // A null handle is not destroyed, for the reason `~VisbufferPass` gives.
    if (!pipeline_.is_null()) {
        device_.destroy_graphics_pipeline(pipeline_);
    }
    if (!layout_.is_null()) {
        device_.destroy_pipeline_layout(layout_);
    }
    for (const rhi::ShaderModuleHandle module : {vertex_, fragment_}) {
        if (!module.is_null()) {
            device_.destroy_shader_module(module);
        }
    }
    for (const rhi::BufferHandle buffer : {indices_, index_staging_}) {
        if (!buffer.is_null()) {
            device_.destroy_buffer(buffer);
        }
    }
}

Status ForwardVisibility::initialise(VisbufferPass& visbuffer, rhi::Format depth_format) noexcept {
    if (visbuffer_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "ForwardVisibility::initialise: already initialised");
    }
    if (!visbuffer.initialised_) {
        return fail(ErrorCode::Unavailable,
                    "ForwardVisibility::initialise: the visibility pass has not been initialised");
    }
    // THE ONE PAYLOAD THE COMPLEMENT CANNOT CARRY. All ones is identity 2^24 - 1 with triangle 255,
    // and its complement is the cleared texel. `VisbufferPass::initialise` admits an identity space
    // of exactly 2^24; this path admits one fewer, so a covered pixel never reads as empty.
    if (visbuffer.identity_count_ >= kMaxSurfaceIdentity) {
        return fail(
            ErrorCode::InvalidArgument,
            "ForwardVisibility::initialise: the scene fills the surface identity space, and "
            "the hardware target stores the payload complemented — identity 2^24 - 1 with "
            "triangle 255 would read back as an empty pixel");
    }
    if (visbuffer.max_cluster_indices_ == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "ForwardVisibility::initialise: the scene has no triangles to draw");
    }

    // One index per corner of the widest cluster. The vertex shader reads the cluster's OWN index
    // list out of the payload; this buffer only numbers the corners.
    Array<u32> corners(visbuffer.allocator_);
    if (Status sized = corners.resize(visbuffer.max_cluster_indices_); !sized) {
        return sized;
    }
    for (u32 index = 0; index < corners.size(); ++index) {
        corners[index] = index;
    }
    index_bytes_ = corners.size() * sizeof(u32);
    Expected<rhi::BufferHandle, Error> indices =
        visbuffer.make_buffer("vg.vis.hw-indices", index_bytes_,
                              rhi::BufferUsage::Index | rhi::BufferUsage::TransferDestination,
                              rhi::MemoryUse::DeviceLocal);
    if (!indices) {
        return make_unexpected(indices.error());
    }
    indices_ = *indices;
    // STAGED HERE, COPIED IN THE GRAPH. `VisbufferPass::upload` copies in a submit of its own and
    // waits on the host, and synchronisation validation reported the first draw's index fetch as
    // `SYNC-HAZARD-READ-AFTER-WRITE` against that copy: a host wait proves the copy COMPLETED and
    // does not make it VISIBLE to the index-input stage. The compute passes that read the uploads
    // beside it are covered by a transfer-to-compute barrier the graph emits anyway; nothing emits
    // one to index input. So the copy is a pass in the first graph that draws, and the graph
    // derives the barrier from the stage's declared `IndexRead` — the traversal's own rule, and the
    // README's third defect, applied here.
    Expected<rhi::BufferHandle, Error> staging =
        visbuffer.make_buffer("vg.vis.hw-indices-staging", index_bytes_,
                              rhi::BufferUsage::TransferSource, rhi::MemoryUse::Upload);
    if (!staging) {
        return make_unexpected(staging.error());
    }
    index_staging_ = *staging;
    void* mapped = device_.buffer_mapped_pointer(index_staging_);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal,
                    "ForwardVisibility::initialise: the index staging buffer is not mapped");
    }
    std::memcpy(mapped, corners.data(), index_bytes_);
    indices_uploaded_ = false;

    // THE SAME SET, A SECOND LAYOUT. The descriptor set layout is `VisbufferPass`'s, whose bindings
    // are visible to the vertex stage for exactly this pipeline; the push range differs because
    // Vulkan requires a push to name every stage of the range it writes, and the compute passes
    // push with `Compute` alone.
    rhi::PushConstantRange push;
    push.stages = rhi::ShaderStage::Vertex;
    push.size = sizeof(VisPush);
    rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "vg.vis.hw";
    layout_description.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&visbuffer.set_layout_, 1);
    layout_description.push_constants = Span<const rhi::PushConstantRange>(&push, 1);
    Expected<rhi::PipelineLayoutHandle, Error> layout =
        device_.create_pipeline_layout(layout_description);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    layout_ = *layout;

    const struct {
        rhi::ShaderModuleHandle* target;
        const char* name;
        rhi::ShaderStage stage;
        Span<const u32> spirv;
    } modules[] = {
        {&vertex_, "vg.vis.hw-vertex", rhi::ShaderStage::Vertex,
         Span<const u32>(kVgVisHwVertexSpirv)},
        {&fragment_, "vg.vis.hw-fragment", rhi::ShaderStage::Fragment,
         Span<const u32>(kVgVisHwFragmentSpirv)},
    };
    for (const auto& entry : modules) {
        rhi::ShaderModuleDescription description;
        description.name = entry.name;
        description.stage = entry.stage;
        description.spirv = entry.spirv;
        Expected<rhi::ShaderModuleHandle, Error> module = device_.create_shader_module(description);
        if (!module) {
            return make_unexpected(module.error());
        }
        *entry.target = *module;
    }

    rhi::ColorAttachmentState visibility;
    visibility.format = rhi::Format::R32Uint;
    rhi::GraphicsPipelineDescription description;
    description.name = "vg.vis.hw";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    description.topology = rhi::PrimitiveTopology::TriangleList;
    // BOTH WINDINGS, as `vgVisRaster` accepts both: the vertex shader negates y to keep the two
    // rasterisers' buffers row-compatible, which flips every triangle's winding.
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = depth_format;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    description.depth_stencil.depth_compare = rhi::CompareOp::GreaterOrEqual;
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&visibility, 1);
    Expected<rhi::GraphicsPipelineHandle, Error> pipeline =
        device_.create_graphics_pipeline(description);
    if (!pipeline) {
        return make_unexpected(pipeline.error());
    }
    pipeline_ = *pipeline;
    visbuffer_ = &visbuffer;
    return ok();
}

Status ForwardVisibility::build(RenderGraph& graph, const GpuTraversal& traversal,
                                const Mat4& world_to_clip, ForwardFrame& frame,
                                const FrameDescription& description) noexcept {
    if (visbuffer_ == nullptr) {
        return fail(ErrorCode::Unavailable, "ForwardVisibility::build: initialise() has not run");
    }
    const GpuTraversal::GraphResources& traversed = traversal.graph_resources();
    // BY POINTER AND BY CONTENT. A graph destroyed after the traversal recorded into it can be
    // followed by a new one at the same address, so the pointer alone could accept ids that name
    // nothing in this graph; the ids must also name the traversal's own buffers here.
    const bool same_graph =
        traversed.graph == &graph && traversed.visible < graph.resource_count() &&
        graph.resource(traversed.visible).imported_buffer == traversal.visible_buffer();
    if (!same_graph) {
        return fail(ErrorCode::InvalidArgument,
                    "ForwardVisibility::build: the traversal was not recorded into this graph. "
                    "Record it first: the stage's vertex shader reads the visible list that "
                    "recording writes, and the graph derives that barrier from the traversal's own "
                    "resources and from nothing else");
    }
    const VisbufferOptions& options = visbuffer_->options_;
    if (description.width != options.width || description.height != options.height) {
        return fail(ErrorCode::InvalidArgument,
                    "ForwardVisibility::build: the frame and the visibility pass disagree about "
                    "the viewport, and the gather copies one into the other texel for texel");
    }
    const auto stage = static_cast<usize>(FramePassKind::VirtualGeometry);
    if (description.callbacks[stage].record != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "ForwardVisibility::build: the description already records the virtual "
                    "geometry stage");
    }
    if (Status bound = visbuffer_->bind_frame(traversal, world_to_clip); !bound) {
        return bound;
    }

    VisbufferPass& visbuffer = *visbuffer_;
    imports_ = visbuffer.import_frame(graph, traversal);
    BufferRequest payload;
    payload.name = "vg.vis.hw-payload";
    payload.size = static_cast<u64>(options.width) * options.height * sizeof(u32);
    payload.extra_usage = rhi::BufferUsage::Storage;
    imports_.hw_payload = graph.import_buffer(payload, visbuffer.hw_payload_);

    visbuffer.states_.clear();
    if (Status reserved = visbuffer.states_.reserve(kPassCount); !reserved) {
        return reserved;
    }
    if (Status head = visbuffer.declare_head(graph, imports_, kPassHwPrepare, traversed.counters);
        !head) {
        return head;
    }

    // THE CORNER INDICES, copied by the first graph that draws and read by every one. See
    // `initialise` for why the copy is a pass rather than a submit of its own.
    BufferRequest index_request;
    index_request.name = "vg.vis.hw-indices";
    index_request.size = index_bytes_;
    index_request.extra_usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDestination;
    const ResourceId indices = graph.import_buffer(index_request, indices_);
    if (!indices_uploaded_) {
        graph.add_pass("vg.vis.hw-indices", rhi::QueueKind::Graphics)
            .write(indices, rhi::Access::TransferWrite)
            .record(&record_index_upload, this);
    }

    // THE FRAME, with the stage turned on and recorded here. The reads are what make the graph put
    // a compute-to-vertex barrier between the traversal and the draw, and a compute-to-indirect one
    // between `vgVisHwPrepare` and the draw's arguments.
    reads_[0] = FrameResourceRead{imports_.args, rhi::Access::IndirectCommandRead};
    reads_[1] = FrameResourceRead{traversed.visible, rhi::Access::VertexStorageRead};
    reads_[2] = FrameResourceRead{traversed.counters, rhi::Access::VertexStorageRead};
    reads_[3] = FrameResourceRead{traversed.clusters, rhi::Access::VertexStorageRead};
    reads_[4] = FrameResourceRead{traversed.instances, rhi::Access::VertexStorageRead};
    reads_[5] = FrameResourceRead{traversed.assets, rhi::Access::VertexStorageRead};
    reads_[6] = FrameResourceRead{indices, rhi::Access::IndexRead};
    description_ = description;
    description_.features.virtual_geometry = true;
    description_.callbacks[stage] = FramePassCallback(&record_stage, this);
    description_.callbacks[stage].reads = Span<const FrameResourceRead>(reads_, kStageReads);
    const auto prepass = static_cast<usize>(FramePassKind::DepthPrepass);
    clear_depth_ =
        !description_.features.depth_prepass || description_.callbacks[prepass].record == nullptr;
    frame_ = &frame;
    if (Status built = frame.build(graph, description_); !built) {
        return built;
    }

    // THE GATHER. The stage's target into the buffer `vgVisHwUnpack` reads, one word a texel; after
    // it, the resolve chain is the compute path's, declared by the same function.
    graph.add_pass("vg.vis.gather", rhi::QueueKind::Graphics)
        .read(frame.resources().visibility, rhi::Access::TransferRead)
        .write(imports_.hw_payload, rhi::Access::TransferWrite)
        .record(&record_gather, this);
    const u64 pixels = static_cast<u64>(options.width) * options.height;
    VisbufferPass::PassState* unpack = visbuffer.next_state(
        kPassHwUnpack, static_cast<u32>((pixels + kGroupSize - 1U) / kGroupSize));
    if (unpack == nullptr) {
        return fail(ErrorCode::OutOfMemory, "ForwardVisibility: the pass state table is full");
    }
    graph.add_pass("vg.vis.hw-unpack", rhi::QueueKind::Graphics)
        .read(imports_.hw_payload, rhi::Access::ComputeStorageRead)
        .write(imports_.visbuffer, rhi::Access::ComputeStorageWrite)
        .record(&VisbufferPass::record_pass, unpack);
    return visbuffer.declare_resolve_chain(graph, imports_);
}

void ForwardVisibility::record_stage(const PassContext& context, void* user) noexcept {
    auto& self = *static_cast<ForwardVisibility*>(user);
    const VisbufferPass& visbuffer = *self.visbuffer_;
    const FrameResources& resources = self.frame_->resources();
    const u32 width = self.description_.width;
    const u32 height = self.description_.height;
    rhi::CommandBuffer& commands = *context.commands;

    const rhi::RenderAttachment visibility =
        visibility_attachment(context.executor->view(resources.visibility));
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, width, height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&visibility, 1);
    info.depth_attachment =
        depth_attachment(context.executor->view(resources.depth), self.clear_depth_);
    commands.begin_rendering(info);
    commands.set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(width), static_cast<f32>(height), 0.0F, 1.0F});
    commands.set_scissor(rhi::Rect2D{0, 0, width, height});
    commands.bind_graphics_pipeline(self.pipeline_);
    commands.bind_descriptor_sets(self.layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&visbuffer.descriptors_, 1));
    commands.push_constants(self.layout_, rhi::ShaderStage::Vertex, 0,
                            Span<const u8>(visbuffer.push_, sizeof(VisPush)));
    commands.bind_index_buffer(self.indices_, 0, true);
    commands.draw_indexed_indirect(visbuffer.vis_args_, kDrawArgsOffset, 1, kDrawArgsStride);
    commands.end_rendering();

    ++self.report_.stages_recorded;
    ++self.report_.draws;
    self.report_.loaded_prepass_depth = !self.clear_depth_;
}

void ForwardVisibility::record_index_upload(const PassContext& context, void* user) noexcept {
    auto& self = *static_cast<ForwardVisibility*>(user);
    rhi::BufferCopy region;
    region.size = self.index_bytes_;
    context.commands->copy_buffer(self.index_staging_, self.indices_,
                                  Span<const rhi::BufferCopy>(&region, 1));
    self.indices_uploaded_ = true;
}

void ForwardVisibility::record_gather(const PassContext& context, void* user) noexcept {
    auto& self = *static_cast<ForwardVisibility*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{self.description_.width, self.description_.height, 1};
    context.commands->copy_texture_to_buffer(
        context.executor->texture(self.frame_->resources().visibility),
        self.visbuffer_->hw_payload_, Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace cy::rendering::vg
