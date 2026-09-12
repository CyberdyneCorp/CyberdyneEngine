#include <cy/rendering/skinning/skin_pass.h>

#include <cstring>

#include "skin_spirv.h"

namespace cy::rendering::skinning {
namespace {

using render::PackedNormalTangent;
using render::geometry::GpuBoneMatrix;
using render::geometry::GpuSkinConstants;
using render::geometry::GpuSkinInfluence;
using render::geometry::pack_bone_matrix;
using render::geometry::SkinnedBuffers;
using render::geometry::SkinningDescriptor;

/// `skin_vertices`'s `[numthreads(64, 1, 1)]`. One place, so that a change to the shader that is
/// not reflected here covers the wrong number of vertices rather than a comment.
constexpr u32 kSkinGroupSize = 64;

/// The six bindings the shader declares, in the order it declares them.
enum Binding : u32 {
    kBindingBones = 0,
    kBindingInPositions = 1,
    kBindingInFrames = 2,
    kBindingInfluences = 3,
    kBindingOutPositions = 4,
    kBindingOutFrames = 5,
    kBindingCount = 6,
};

/// Vulkan has no zero-length buffer, and a descriptor must name something even when the stream
/// behind it is empty — a pass created without frames still binds bindings 2 and 5, and the flag in
/// the constant block is what tells the shader not to read them.
[[nodiscard]] u64 at_least_one(u64 count, u64 stride) noexcept {
    return (count == 0 ? 1 : count) * stride;
}

template <typename T>
[[nodiscard]] Status copy_into(rhi::Device& device, rhi::BufferHandle handle,
                               Span<const T> source) noexcept {
    if (source.empty()) {
        return ok();
    }
    void* target = device.buffer_mapped_pointer(handle);
    if (target == nullptr) {
        return fail(ErrorCode::Internal, "a skinning input buffer is not mapped");
    }
    std::memcpy(target, source.data(), source.size() * sizeof(T));
    return ok();
}

}  // namespace

// --- Creation -----------------------------------------------------------------------------------

SkinPass::~SkinPass() {
    destroy();
}

bool SkinPass::supported(const rhi::Device& device) noexcept {
    return device.capabilities().has(rhi::Capability::ComputeShaders);
}

Status SkinPass::create(Allocator& allocator, rhi::Device& device,
                        const SkinPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the skinning pass has already been created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "skinning needs Capability::ComputeShaders; cpu_reference_skin computes the "
                    "same answer and is what a device without one would have to run");
    }
    if (desc.max_vertices == 0 || desc.max_bones == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "SkinPassDescription::max_vertices and ::max_bones must both be non-zero; the "
                    "buffers cannot grow on the device and are sized once");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;

    if (Status created = create_pipeline(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_buffers(); !created) {
        destroy();
        return created;
    }
    if (Status written = write_descriptors(); !written) {
        destroy();
        return written;
    }
    return ok();
}

Status SkinPass::create_pipeline() noexcept {
    rhi::ShaderModuleDescription module;
    module.name = "skin vertices";
    module.stage = rhi::ShaderStage::Compute;
    // "main", not "skin_vertices": slangc names a single-entry SPIR-V module's entry point `main`
    // whatever `-entry` said, and the name the RHI passes is the one in the module.
    module.entry_point = "main";
    module.spirv = Span<const u32>(kSkinVerticesSpirv, sizeof(kSkinVerticesSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> created = device_->create_shader_module(module);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    shader_ = *created;

    rhi::DescriptorBinding bindings[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "skin set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(GpuSkinConstants)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "skin layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> pipeline_layout_handle =
        device_->create_pipeline_layout(pipeline_layout);
    if (!pipeline_layout_handle.has_value()) {
        return make_unexpected(pipeline_layout_handle.error());
    }
    pipeline_layout_ = *pipeline_layout_handle;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = "skin vertices";
    pipeline.layout = pipeline_layout_;
    pipeline.shader = shader_;
    Expected<rhi::ComputePipelineHandle, Error> pipeline_handle =
        device_->create_compute_pipeline(pipeline);
    if (!pipeline_handle.has_value()) {
        return make_unexpected(pipeline_handle.error());
    }
    pipeline_ = *pipeline_handle;
    return ok();
}

Status SkinPass::create_buffers() noexcept {
    // WHICH MEMORY EACH BUFFER LIVES IN, AND WHY.
    //
    // The bind-pose streams are Upload and written once. They could be device-local with a staging
    // copy, and for a shipped mesh residency path they should be — this pass is sized for one skin
    // and uploads the mesh once, so the copy would be a copy of a copy.
    //
    // The pose is Upload and rewritten every frame, which is what a pose is.
    //
    // The OUTPUT is DeviceLocal and carries `Vertex` as well as `Storage`, because it is a vertex
    // buffer: a draw binds it and a vertex fetch reads it. It is twice `max_vertices` because the
    // output is double buffered.
    //
    // The read-back pair is host memory the transfer pass copies into. It exists so the dispatch
    // can be compared against `cpu_reference_skin`; a frame that only draws never touches it.
    const auto storage = rhi::BufferUsage::Storage;
    const auto vertex_out =
        rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferSource;
    const u64 output_vertices = static_cast<u64>(desc_.max_vertices) * 2U;
    const u64 frame_vertices = desc_.with_frames ? output_vertices : 0;
    const u64 frame_inputs = desc_.with_frames ? desc_.max_vertices : 0;

    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"skin bones", at_least_one(desc_.max_bones, sizeof(GpuBoneMatrix)), storage,
         rhi::MemoryUse::Upload, &buffers_.bones},
        {"skin input positions", at_least_one(desc_.max_vertices, sizeof(Vec3)), storage,
         rhi::MemoryUse::Upload, &buffers_.in_positions},
        {"skin input frames", at_least_one(frame_inputs, sizeof(PackedNormalTangent)), storage,
         rhi::MemoryUse::Upload, &buffers_.in_frames},
        // Two records per vertex, because an eight-influence mesh needs them and a four-influence
        // one costs eight bytes a vertex to keep one sizing rule instead of two.
        {"skin influences",
         at_least_one(static_cast<u64>(desc_.max_vertices) * 2U, sizeof(GpuSkinInfluence)), storage,
         rhi::MemoryUse::Upload, &buffers_.influences},
        {"skin output positions", at_least_one(output_vertices, sizeof(Vec3)), vertex_out,
         rhi::MemoryUse::DeviceLocal, &buffers_.positions},
        {"skin output frames", at_least_one(frame_vertices, sizeof(PackedNormalTangent)),
         vertex_out, rhi::MemoryUse::DeviceLocal, &buffers_.frames},
    };
    for (const Request& request : requests) {
        rhi::BufferDescription description;
        description.name = request.name;
        description.size = request.size;
        description.usage = request.usage;
        description.memory = request.memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    if (!desc_.read_back) {
        return ok();
    }
    const Request host_requests[] = {
        {"skin positions readback", at_least_one(output_vertices, sizeof(Vec3)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.positions_readback},
        {"skin frames readback", at_least_one(frame_vertices, sizeof(PackedNormalTangent)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.frames_readback},
    };
    for (const Request& request : host_requests) {
        rhi::BufferDescription description;
        description.name = request.name;
        description.size = request.size;
        description.usage = request.usage;
        description.memory = request.memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    return ok();
}

Status SkinPass::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    const rhi::BufferHandle handles[kBindingCount] = {
        buffers_.bones,      buffers_.in_positions, buffers_.in_frames,
        buffers_.influences, buffers_.positions,    buffers_.frames,
    };
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = handles[index];
        writes[index].buffer_range = 0;  // the rest of the buffer
    }
    return device_->update_descriptor_set(descriptor_set_,
                                          Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

void SkinPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    const rhi::BufferHandle buffers[] = {
        buffers_.bones,
        buffers_.in_positions,
        buffers_.in_frames,
        buffers_.influences,
        buffers_.positions,
        buffers_.frames,
        buffers_.positions_readback,
        buffers_.frames_readback,
    };
    for (rhi::BufferHandle handle : buffers) {
        device_->destroy_buffer(handle);
    }
    buffers_ = Buffers{};
    device_->destroy_compute_pipeline(pipeline_);
    device_->destroy_pipeline_layout(pipeline_layout_);
    device_->destroy_descriptor_set_layout(set_layout_);
    device_->destroy_shader_module(shader_);
    pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    shader_ = {};
    descriptor_set_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
    mesh_uploaded_ = false;
}

// --- Upload -------------------------------------------------------------------------------------

Status SkinPass::upload_mesh(Span<const Vec3> positions, Span<const PackedNormalTangent> frames,
                             Span<const GpuSkinInfluence> influences) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the skinning pass has not been created");
    }
    if (positions.size() > desc_.max_vertices) {
        return fail(ErrorCode::OutOfRange,
                    "the mesh has more vertices than the skinning pass was sized for");
    }
    if (desc_.with_frames && frames.size() != positions.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "this pass carries the normal-tangent stream, so it needs one frame per "
                    "vertex; a pass created with with_frames false takes an empty span");
    }
    if (!desc_.with_frames && !frames.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "this pass was created without the normal-tangent stream and has no buffer to "
                    "put one in");
    }
    if (influences.size() < positions.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "every vertex needs at least one influence record; an eight-influence mesh "
                    "needs two");
    }
    if (influences.size() > static_cast<usize>(desc_.max_vertices) * 2U) {
        return fail(ErrorCode::OutOfRange, "more influence records than the pass was sized for");
    }
    if (Status copied = copy_into(*device_, buffers_.in_positions, positions); !copied) {
        return copied;
    }
    if (Status copied = copy_into(*device_, buffers_.in_frames, frames); !copied) {
        return copied;
    }
    if (Status copied = copy_into(*device_, buffers_.influences, influences); !copied) {
        return copied;
    }
    mesh_uploaded_ = true;
    return ok();
}

Status SkinPass::upload(const SkinningDescriptor& descriptor, Span<const Mat4> skinning_matrices,
                        u64 frame_index) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the skinning pass has not been created");
    }
    if (!mesh_uploaded_) {
        return fail(ErrorCode::InvalidArgument,
                    "upload_mesh() has not been called; a dispatch over an unwritten bind pose "
                    "would skin whatever the allocator last left in the buffer");
    }
    // The parity picks the half of the output buffer this frame writes. The other half still holds
    // the previous frame's positions, which is what motion vectors read.
    const SkinnedBuffers halves = SkinnedBuffers::for_frame(frame_index);
    Expected<GpuSkinConstants, Error> constants = render::geometry::make_skin_constants(
        descriptor, desc_.with_frames, 0, halves.current * desc_.max_vertices);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    if (constants->vertex_count > desc_.max_vertices) {
        return fail(ErrorCode::OutOfRange,
                    "the descriptor covers more vertices than the skinning pass was sized for");
    }
    if (static_cast<usize>(descriptor.pose_offset) + descriptor.bone_count >
        skinning_matrices.size()) {
        return fail(ErrorCode::OutOfRange,
                    "the pose does not hold this skin's bones at the offset the descriptor gives; "
                    "PoseWorld::matrix_offset moves every publish because the world is double "
                    "buffered, so a cached offset reads the previous frame or past the end");
    }
    if (skinning_matrices.size() > desc_.max_bones) {
        return fail(ErrorCode::OutOfRange,
                    "the pose holds more bones than the skinning pass was sized for");
    }
    constants_ = *constants;
    previous_offset_ = halves.previous * desc_.max_vertices;

    // THE ONE TRANSPOSITION IN THE PATH. `Mat4` is column-major and the dispatch reads three rows;
    // `pack_bone_matrix` is where the convention changes and it is the only place it does.
    auto* target = static_cast<GpuBoneMatrix*>(device_->buffer_mapped_pointer(buffers_.bones));
    if (target == nullptr) {
        return fail(ErrorCode::Internal, "the skinning pose buffer is not mapped");
    }
    for (usize bone = 0; bone < skinning_matrices.size(); ++bone) {
        target[bone] = pack_bone_matrix(skinning_matrices[bone]);
    }
    return ok();
}

// --- Declaration --------------------------------------------------------------------------------

void SkinPass::record_skin(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<SkinPass*>(user);
    context.commands->bind_compute_pipeline(self->pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    context.commands->push_constants(
        self->pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&self->constants_), sizeof(GpuSkinConstants)));
    const u32 groups = (self->constants_.vertex_count + kSkinGroupSize - 1U) / kSkinGroupSize;
    context.commands->dispatch(groups == 0 ? 1 : groups, 1, 1);
}

void SkinPass::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<SkinPass*>(user);
    const u64 vertices = static_cast<u64>(self->desc_.max_vertices) * 2U;
    const rhi::BufferCopy positions{0, 0, at_least_one(vertices, sizeof(Vec3))};
    context.commands->copy_buffer(self->buffers_.positions, self->buffers_.positions_readback,
                                  Span<const rhi::BufferCopy>(&positions, 1));
    if (!self->desc_.with_frames) {
        return;
    }
    const rhi::BufferCopy frames{0, 0, at_least_one(vertices, sizeof(PackedNormalTangent))};
    context.commands->copy_buffer(self->buffers_.frames, self->buffers_.frames_readback,
                                  Span<const rhi::BufferCopy>(&frames, 1));
}

Status SkinPass::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the skinning pass has not been created");
    }
    if (constants_.vertex_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "upload() has not been called for this frame; a dispatch declared with no "
                    "constant block would cover no vertices and report success");
    }

    // Every resource is imported rather than graph-owned: the buffers outlive the frame, the
    // descriptor set names them once, and a transient's handle changes every frame.
    const auto import = [&graph, this](const char* name, rhi::BufferHandle handle,
                                       rhi::BufferUsage extra) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = extra;
        return graph.import_buffer(request, handle);
    };

    const ResourceId bones = import("skin bones", buffers_.bones, rhi::BufferUsage::Storage);
    const ResourceId in_positions =
        import("skin input positions", buffers_.in_positions, rhi::BufferUsage::Storage);
    const ResourceId in_frames =
        import("skin input frames", buffers_.in_frames, rhi::BufferUsage::Storage);
    const ResourceId influences =
        import("skin influences", buffers_.influences, rhi::BufferUsage::Storage);
    positions_id_ = import("skin output positions", buffers_.positions,
                           rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    frames_id_ = import("skin output frames", buffers_.frames,
                        rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);

    using rhi::Access;
    using rhi::QueueKind;

    // ONE PASS, and it declares the output as WRITTEN rather than read-written. The dispatch covers
    // one half of the double-buffered output and does not read the other, so a read-write
    // declaration would claim a dependency on the previous frame's contents that does not exist and
    // would serialise two frames that need not be.
    graph.add_pass("skin vertices", QueueKind::Graphics)
        .read(bones, Access::ComputeStorageRead)
        .read(in_positions, Access::ComputeStorageRead)
        .read(in_frames, Access::ComputeStorageRead)
        .read(influences, Access::ComputeStorageRead)
        .write(positions_id_, Access::ComputeStorageWrite)
        .write(frames_id_, Access::ComputeStorageWrite)
        .record(&record_skin, this);

    if (!desc_.read_back) {
        return graph.status();
    }
    const ResourceId positions_out = import("skin positions readback", buffers_.positions_readback,
                                            rhi::BufferUsage::TransferDestination);
    const ResourceId frames_out = import("skin frames readback", buffers_.frames_readback,
                                         rhi::BufferUsage::TransferDestination);
    graph.add_pass("skin readback", QueueKind::Graphics)
        .read(positions_id_, Access::TransferRead)
        .read(frames_id_, Access::TransferRead)
        .write(positions_out, Access::TransferWrite)
        .write(frames_out, Access::TransferWrite)
        .record(&record_readback, this);
    // The host boundary is a dependency like any other: declaring it is what makes the graph emit
    // the transfer-to-host barrier, rather than this module relying on coherent memory and a fence.
    graph.add_pass("skin host read", QueueKind::Graphics)
        .read(positions_out, Access::HostRead)
        .read(frames_out, Access::HostRead)
        .side_effect();

    return graph.status();
}

// --- Read-back ----------------------------------------------------------------------------------

Expected<Span<const Vec3>, Error> SkinPass::read_back_positions() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the skinning pass has not been created"});
    }
    const auto* positions =
        static_cast<const Vec3*>(device_->buffer_mapped_pointer(buffers_.positions_readback));
    if (positions == nullptr) {
        return make_unexpected(Error{ErrorCode::Internal,
                                     "the skinning read-back buffer is not mapped; a pass created "
                                     "without SkinPassDescription::read_back has none"});
    }
    return Span<const Vec3>(positions, static_cast<usize>(desc_.max_vertices) * 2U);
}

Expected<Span<const PackedNormalTangent>, Error> SkinPass::read_back_frames() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the skinning pass has not been created"});
    }
    if (!desc_.with_frames) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "this pass was created without the normal-tangent stream"});
    }
    const auto* frames = static_cast<const PackedNormalTangent*>(
        device_->buffer_mapped_pointer(buffers_.frames_readback));
    if (frames == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "the skinning read-back buffer is not mapped"});
    }
    return Span<const PackedNormalTangent>(frames, static_cast<usize>(desc_.max_vertices) * 2U);
}

}  // namespace cy::rendering::skinning
