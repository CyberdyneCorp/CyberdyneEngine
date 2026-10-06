#include <cy/rendering/skinning/skinned_scene.h>

#include <cstring>

#include "skin_pipeline.h"

namespace cy::rendering::skinning {
namespace {

using render::PackedNormalTangent;
using render::geometry::GpuActiveBlendShape;
using render::geometry::GpuBoneDualQuaternion;
using render::geometry::GpuBoneMatrix;
using render::geometry::GpuSkinConstants;
using render::geometry::GpuSkinInfluence;
using render::geometry::InfluenceCount;
using render::geometry::SkinningMethod;

using detail::at_least_one;
using detail::kBindingCount;

[[nodiscard]] u64 encode_window(u32 first, u32 vertices) noexcept {
    return (static_cast<u64>(first) << 32U) | static_cast<u64>(vertices);
}

[[nodiscard]] u32 window_first(u64 encoded) noexcept {
    return static_cast<u32>(encoded >> 32U);
}

[[nodiscard]] u32 window_size(u64 encoded) noexcept {
    return static_cast<u32>(encoded);
}

[[nodiscard]] u32 blocks_of(InfluenceCount influences) noexcept {
    return influences == InfluenceCount::Eight ? 2U : 1U;
}

template <typename T>
[[nodiscard]] Status copy_at(rhi::Device& device, rhi::BufferHandle handle, usize first,
                             Span<const T> source) noexcept {
    if (source.empty()) {
        return ok();
    }
    auto* target = static_cast<T*>(device.buffer_mapped_pointer(handle));
    if (target == nullptr) {
        return fail(ErrorCode::Internal, "skinned scene: an input buffer is not mapped");
    }
    std::memcpy(target + first, source.data(), source.size() * sizeof(T));
    return ok();
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> make_buffer(rhi::Device& device, const char* name,
                                                             u64 size, rhi::BufferUsage usage,
                                                             rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size;
    description.usage = usage;
    description.memory = memory;
    return device.create_buffer(description);
}

}  // namespace

SkinnedScene::SkinnedScene(Allocator& allocator) noexcept
    : allocator_(&allocator),
      meshes_(allocator),
      instances_(allocator),
      free_instances_(allocator),
      free_windows_(allocator),
      dispatched_(allocator) {}

SkinnedScene::~SkinnedScene() {
    destroy();
}

bool SkinnedScene::supported(const rhi::Device& device) noexcept {
    return detail::skin_supported(device);
}

Status SkinnedScene::create(rhi::Device& device, const SkinnedSceneDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "skinned scene: already created");
    }
    if (!supported(device)) {
        return detail::skin_unsupported(device);
    }
    if (desc.max_mesh_vertices == 0 || desc.max_instance_vertices == 0 ||
        desc.max_pose_matrices == 0 || desc.max_meshes == 0 || desc.max_instances == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "SkinnedSceneDescription: every maximum must be non-zero; the buffers are "
                    "sized once and cannot grow on the device");
    }
    device_ = &device;
    desc_ = desc;
    if (Status created = pipeline_.create(device); !created) {
        destroy();
        return created;
    }
    if (Status created = create_buffers(); !created) {
        destroy();
        return created;
    }
    Expected<rhi::DescriptorSetHandle, Error> set =
        device.allocate_descriptor_set(pipeline_.set_layout, false);
    if (!set.has_value()) {
        destroy();
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;
    const rhi::BufferHandle handles[kBindingCount] = {
        buffers_.bones,
        buffers_.in_positions,
        buffers_.in_frames,
        buffers_.influences,
        buffers_.positions,
        buffers_.frames,
        buffers_.bone_dual_quaternions,
        buffers_.blend_shape_deltas,
        buffers_.active_blend_shapes,
    };
    if (Status written = detail::write_skin_set(device, descriptor_set_, handles); !written) {
        destroy();
        return written;
    }
    return ok();
}

Status SkinnedScene::create_buffers() noexcept {
    // The pose and the inputs are host-visible and written in place: the pose because a frame
    // writes the changed range of it, the inputs because they are written once per mesh. The output
    // is device-local and a vertex buffer, because the frame binds it as one.
    const auto storage = rhi::BufferUsage::Storage;
    const auto vertex_out =
        rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferSource;
    const u64 output_vertices = static_cast<u64>(desc_.max_instance_vertices) * 2U;
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"skinned scene bones", at_least_one(desc_.max_pose_matrices, sizeof(GpuBoneMatrix)),
         storage, rhi::MemoryUse::Upload, &buffers_.bones},
        {"skinned scene bone dual quaternions",
         at_least_one(desc_.max_pose_matrices, sizeof(GpuBoneDualQuaternion)), storage,
         rhi::MemoryUse::Upload, &buffers_.bone_dual_quaternions},
        {"skinned scene blend shape deltas",
         at_least_one(0, sizeof(render::geometry::BlendShapeDelta)), storage,
         rhi::MemoryUse::Upload, &buffers_.blend_shape_deltas},
        {"skinned scene active blend shapes", at_least_one(0, sizeof(GpuActiveBlendShape)), storage,
         rhi::MemoryUse::Upload, &buffers_.active_blend_shapes},
        {"skinned scene input positions", at_least_one(desc_.max_mesh_vertices, sizeof(Vec3)),
         storage, rhi::MemoryUse::Upload, &buffers_.in_positions},
        {"skinned scene input frames",
         at_least_one(desc_.max_mesh_vertices, sizeof(PackedNormalTangent)), storage,
         rhi::MemoryUse::Upload, &buffers_.in_frames},
        {"skinned scene influences",
         at_least_one(static_cast<u64>(desc_.max_mesh_vertices) * 2U, sizeof(GpuSkinInfluence)),
         storage, rhi::MemoryUse::Upload, &buffers_.influences},
        {"skinned scene output positions", at_least_one(output_vertices, sizeof(Vec3)), vertex_out,
         rhi::MemoryUse::DeviceLocal, &buffers_.positions},
        {"skinned scene output frames", at_least_one(output_vertices, sizeof(PackedNormalTangent)),
         vertex_out, rhi::MemoryUse::DeviceLocal, &buffers_.frames},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> made =
            make_buffer(*device_, request.name, request.size, request.usage, request.memory);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    if (!desc_.read_back) {
        return ok();
    }
    const Request host[] = {
        {"skinned scene positions readback", at_least_one(output_vertices, sizeof(Vec3)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.positions_readback},
        {"skinned scene frames readback",
         at_least_one(output_vertices, sizeof(PackedNormalTangent)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.frames_readback},
    };
    for (const Request& request : host) {
        Expected<rhi::BufferHandle, Error> made =
            make_buffer(*device_, request.name, request.size, request.usage, request.memory);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    return ok();
}

void SkinnedScene::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::BufferHandle* buffers[] = {
        &buffers_.bones,
        &buffers_.bone_dual_quaternions,
        &buffers_.blend_shape_deltas,
        &buffers_.active_blend_shapes,
        &buffers_.in_positions,
        &buffers_.in_frames,
        &buffers_.influences,
        &buffers_.positions,
        &buffers_.frames,
        &buffers_.positions_readback,
        &buffers_.frames_readback,
    };
    for (rhi::BufferHandle* handle : buffers) {
        if (*handle) {
            device_->destroy_buffer(*handle);
        }
        *handle = rhi::BufferHandle{};
    }
    pipeline_.destroy(*device_);
    descriptor_set_ = {};
    meshes_.clear();
    instances_.clear();
    free_instances_.clear();
    free_windows_.clear();
    dispatched_.clear();
    next_vertex_ = 0;
    next_record_ = 0;
    next_output_ = 0;
    read_count_ = 0;
    stats_ = SkinnedSceneStats{};
    device_ = nullptr;
}

// --- The table
// ------------------------------------------------------------------------------------

Expected<SkinnedMeshId, Error> SkinnedScene::add_mesh(const SkinnedMeshDescription& mesh) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "skinned scene: not created");
    }
    const auto vertices = static_cast<u32>(mesh.positions.size());
    const u32 blocks = blocks_of(mesh.influence_count);
    if (vertices == 0 || mesh.bone_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "skinned scene: a mesh needs vertices and a skeleton with bones");
    }
    if (mesh.frames.size() != vertices ||
        mesh.influences.size() != static_cast<usize>(vertices) * blocks) {
        return fail(ErrorCode::InvalidArgument,
                    "skinned scene: a mesh needs one frame per vertex and one influence record per "
                    "vertex per four influences");
    }
    if (meshes_.size() >= desc_.max_meshes) {
        return fail(ErrorCode::OutOfRange, "skinned scene: more meshes than it was sized for");
    }
    // THE DISPATCH ADDRESSES INFLUENCES BY `vertex * blocks`, so an eight-influence mesh's records
    // start at twice its first vertex. The first vertex is chosen so that neither its vertices nor
    // its records overlap anything already added.
    u32 first = next_vertex_;
    const u32 record_first = (next_record_ + blocks - 1U) / blocks;
    first = first > record_first ? first : record_first;
    const u64 end_vertex = static_cast<u64>(first) + vertices;
    const u64 end_record = (static_cast<u64>(first) + vertices) * blocks;
    if (end_vertex > desc_.max_mesh_vertices ||
        end_record > static_cast<u64>(desc_.max_mesh_vertices) * 2U) {
        return fail(ErrorCode::OutOfRange,
                    "skinned scene: the mesh does not fit in the input buffers it was sized for");
    }
    if (Status copied = copy_at(*device_, buffers_.in_positions, first, mesh.positions); !copied) {
        return make_unexpected(copied.error());
    }
    if (Status copied = copy_at(*device_, buffers_.in_frames, first, mesh.frames); !copied) {
        return make_unexpected(copied.error());
    }
    if (Status copied = copy_at(*device_, buffers_.influences, static_cast<usize>(first) * blocks,
                                mesh.influences);
        !copied) {
        return make_unexpected(copied.error());
    }
    if (Status pushed =
            meshes_.push_back(Mesh{first, vertices, mesh.bone_count, mesh.influence_count});
        !pushed) {
        return make_unexpected(pushed.error());
    }
    next_vertex_ = static_cast<u32>(end_vertex);
    next_record_ = static_cast<u32>(end_record);
    stats_.meshes = static_cast<u32>(meshes_.size());
    return static_cast<SkinnedMeshId>(meshes_.size() - 1U);
}

Expected<u32, Error> SkinnedScene::take_output_window(u32 vertices) noexcept {
    const u32 doubled = vertices * 2U;
    for (usize index = 0; index < free_windows_.size(); ++index) {
        if (window_size(free_windows_[index]) == doubled) {
            const u32 first = window_first(free_windows_[index]);
            free_windows_.remove_unordered(index);
            return first;
        }
    }
    if (static_cast<u64>(next_output_) + doubled >
        static_cast<u64>(desc_.max_instance_vertices) * 2U) {
        return fail(ErrorCode::OutOfRange,
                    "skinned scene: more skinned vertices than the output was sized for");
    }
    const u32 first = next_output_;
    next_output_ += doubled;
    return first;
}

Expected<SkinnedInstance, Error> SkinnedScene::add_instance(SkinnedMeshId mesh,
                                                            SkinningMethod method) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "skinned scene: not created");
    }
    if (mesh >= meshes_.size()) {
        return fail(ErrorCode::NotFound, "skinned scene: no such mesh");
    }
    if (stats_.instances >= desc_.max_instances) {
        return fail(ErrorCode::OutOfRange, "skinned scene: more instances than it was sized for");
    }
    Expected<u32, Error> window = take_output_window(meshes_[mesh].vertex_count);
    if (!window.has_value()) {
        return make_unexpected(window.error());
    }
    u32 index = 0;
    if (!free_instances_.empty()) {
        index = free_instances_.back();
        free_instances_.pop_back();
    } else {
        index = static_cast<u32>(instances_.size());
        if (Status pushed = instances_.push_back(Instance{}); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    Instance& slot = instances_[index];
    const u32 generation = slot.generation;
    slot = Instance{};
    slot.generation = generation;
    slot.mesh = mesh;
    slot.first_output = *window;
    slot.method = method;
    slot.live = true;
    ++stats_.instances;
    return SkinnedInstance{index, generation};
}

const SkinnedScene::Instance* SkinnedScene::instance_of(SkinnedInstance instance) const noexcept {
    if (!instance.valid() || instance.index >= instances_.size()) {
        return nullptr;
    }
    const Instance& slot = instances_[instance.index];
    return slot.live && slot.generation == instance.generation ? &slot : nullptr;
}

bool SkinnedScene::live(SkinnedInstance instance) const noexcept {
    return instance_of(instance) != nullptr;
}

Status SkinnedScene::remove_instance(SkinnedInstance instance) noexcept {
    if (instance_of(instance) == nullptr) {
        return fail(ErrorCode::NotFound, "skinned scene: this handle names no live instance");
    }
    Instance& slot = instances_[instance.index];
    const u32 doubled = meshes_[slot.mesh].vertex_count * 2U;
    if (Status pushed = free_windows_.push_back(encode_window(slot.first_output, doubled));
        !pushed) {
        return pushed;
    }
    if (Status pushed = free_instances_.push_back(instance.index); !pushed) {
        return pushed;
    }
    slot.live = false;
    slot.skinned = false;
    ++slot.generation;
    --stats_.instances;
    return ok();
}

// --- The pose
// -------------------------------------------------------------------------------------

Status SkinnedScene::upload_poses(Span<const Mat4> matrices, u32 first, u32 count) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "skinned scene: not created");
    }
    stats_.uploaded_matrices = 0;
    if (count == 0) {
        return ok();
    }
    const u64 end = static_cast<u64>(first) + count;
    if (end > matrices.size()) {
        return fail(ErrorCode::OutOfRange,
                    "skinned scene: the upload range runs past the end of the pose world");
    }
    if (end > desc_.max_pose_matrices) {
        return fail(ErrorCode::OutOfRange,
                    "skinned scene: the pose world is larger than the pose buffer was sized for");
    }
    auto* bones = static_cast<GpuBoneMatrix*>(device_->buffer_mapped_pointer(buffers_.bones));
    auto* duals = static_cast<GpuBoneDualQuaternion*>(
        device_->buffer_mapped_pointer(buffers_.bone_dual_quaternions));
    if (bones == nullptr || duals == nullptr) {
        return fail(ErrorCode::Internal, "skinned scene: the pose buffer is not mapped");
    }
    // BOTH REPRESENTATIONS, per changed bone: the method is an instance's, and an instance reading
    // the other representation of a bone this range did not touch would read a stale pose.
    for (u32 index = first; index < end; ++index) {
        bones[index] = render::geometry::pack_bone_matrix(matrices[index]);
        duals[index] = render::geometry::pack_bone_dual_quaternion(matrices[index]);
    }
    stats_.uploaded_matrices = count;
    stats_.uploaded_matrices_total += count;
    return ok();
}

Status SkinnedScene::set_pose(SkinnedInstance instance, u32 pose_offset) noexcept {
    if (instance_of(instance) == nullptr) {
        return fail(ErrorCode::NotFound, "skinned scene: this handle names no live instance");
    }
    Instance& slot = instances_[instance.index];
    if (static_cast<u64>(pose_offset) + meshes_[slot.mesh].bone_count > desc_.max_pose_matrices) {
        return fail(ErrorCode::OutOfRange,
                    "skinned scene: the instance's bones run past the end of the pose buffer; "
                    "PoseWorld::matrix_offset moves every publish, so read it every frame");
    }
    slot.pose_offset = pose_offset;
    slot.posed = true;
    return ok();
}

// --- The pass
// -------------------------------------------------------------------------------------

Status SkinnedScene::prepare_dispatches(u64 frame_index) noexcept {
    dispatched_.clear();
    const render::geometry::SkinnedBuffers halves =
        render::geometry::SkinnedBuffers::for_frame(frame_index);
    for (u32 index = 0; index < instances_.size(); ++index) {
        Instance& slot = instances_[index];
        slot.skinned = false;
        if (!slot.live || !slot.posed) {
            continue;
        }
        const Mesh& mesh = meshes_[slot.mesh];
        render::geometry::SkinningDescriptor descriptor;
        descriptor.vertex_count = mesh.vertex_count;
        descriptor.bone_count = mesh.bone_count;
        descriptor.retained_bones = mesh.bone_count;
        descriptor.pose_offset = slot.pose_offset;
        descriptor.influences = mesh.influences;
        descriptor.method = slot.method;
        descriptor.source = render::geometry::PoseSource::GpuPoseWorld;
        Expected<GpuSkinConstants, Error> constants = render::geometry::make_skin_constants(
            descriptor, true, mesh.first_vertex,
            slot.first_output + (halves.current * mesh.vertex_count));
        if (!constants.has_value()) {
            return make_unexpected(constants.error());
        }
        slot.constants = *constants;
        if (Status pushed = dispatched_.push_back(index); !pushed) {
            return pushed;
        }
    }
    return ok();
}

void SkinnedScene::record_skin(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<SkinnedScene*>(user);
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(self->pipeline_.pipeline);
    commands.bind_descriptor_sets(self->pipeline_.layout, 0,
                                  Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    // ONE DISPATCH PER INSTANCE, ONE PASS FOR ALL OF THEM. Each dispatch writes its own window of
    // the output, so no two dispatches of the pass touch the same vertex and none needs a barrier
    // against another.
    for (const u32 index : self->dispatched_) {
        const GpuSkinConstants& constants = self->instances_[index].constants;
        commands.push_constants(
            self->pipeline_.layout, rhi::ShaderStage::Compute, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&constants), sizeof(GpuSkinConstants)));
        commands.dispatch(detail::skin_groups(constants.vertex_count), 1, 1);
    }
}

void SkinnedScene::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<SkinnedScene*>(user);
    const u64 vertices = static_cast<u64>(self->desc_.max_instance_vertices) * 2U;
    const rhi::BufferCopy positions{0, 0, at_least_one(vertices, sizeof(Vec3))};
    context.commands->copy_buffer(self->buffers_.positions, self->buffers_.positions_readback,
                                  Span<const rhi::BufferCopy>(&positions, 1));
    const rhi::BufferCopy frames{0, 0, at_least_one(vertices, sizeof(PackedNormalTangent))};
    context.commands->copy_buffer(self->buffers_.frames, self->buffers_.frames_readback,
                                  Span<const rhi::BufferCopy>(&frames, 1));
}

Status SkinnedScene::declare(RenderGraph& graph, u64 frame_index) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "skinned scene: not created");
    }
    read_count_ = 0;
    stats_.dispatches = 0;
    stats_.passes = 0;
    if (Status prepared = prepare_dispatches(frame_index); !prepared) {
        return prepared;
    }
    if (dispatched_.empty()) {
        return ok();
    }
    const auto import = [&graph, this](const char* name, rhi::BufferHandle handle,
                                       rhi::BufferUsage extra) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = extra;
        return graph.import_buffer(request, handle);
    };
    const ResourceId bones =
        import("skinned scene bones", buffers_.bones, rhi::BufferUsage::Storage);
    const ResourceId duals = import("skinned scene bone dual quaternions",
                                    buffers_.bone_dual_quaternions, rhi::BufferUsage::Storage);
    const ResourceId in_positions =
        import("skinned scene input positions", buffers_.in_positions, rhi::BufferUsage::Storage);
    const ResourceId in_frames =
        import("skinned scene input frames", buffers_.in_frames, rhi::BufferUsage::Storage);
    const ResourceId influences =
        import("skinned scene influences", buffers_.influences, rhi::BufferUsage::Storage);
    reads_[0] = import("skinned scene output positions", buffers_.positions,
                       rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    reads_[1] = import("skinned scene output frames", buffers_.frames,
                       rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    read_count_ = 2;

    using rhi::Access;
    graph.add_pass("skin scene", rhi::QueueKind::Graphics)
        .read(bones, Access::ComputeStorageRead)
        .read(duals, Access::ComputeStorageRead)
        .read(in_positions, Access::ComputeStorageRead)
        .read(in_frames, Access::ComputeStorageRead)
        .read(influences, Access::ComputeStorageRead)
        .write(reads_[0], Access::ComputeStorageWrite)
        .write(reads_[1], Access::ComputeStorageWrite)
        .record(&record_skin, this);
    stats_.passes = 1;
    stats_.dispatches = static_cast<u32>(dispatched_.size());

    // THE OTHER HALF IS LAST FRAME'S only when the instance was skinned in the frame before this
    // one; on its first frame, or after a frame it sat out, the other half is stale.
    for (const u32 index : dispatched_) {
        Instance& slot = instances_[index];
        slot.has_previous = slot.skinned_once && slot.skinned_frame + 1U == frame_index;
        slot.skinned = true;
        slot.skinned_once = true;
        slot.skinned_frame = frame_index;
    }

    if (desc_.read_back) {
        const ResourceId positions_out =
            import("skinned scene positions readback", buffers_.positions_readback,
                   rhi::BufferUsage::TransferDestination);
        const ResourceId frames_out =
            import("skinned scene frames readback", buffers_.frames_readback,
                   rhi::BufferUsage::TransferDestination);
        graph.add_pass("skin scene readback", rhi::QueueKind::Graphics)
            .read(reads_[0], Access::TransferRead)
            .read(reads_[1], Access::TransferRead)
            .write(positions_out, Access::TransferWrite)
            .write(frames_out, Access::TransferWrite)
            .record(&record_readback, this);
        graph.add_pass("skin scene host read", rhi::QueueKind::Graphics)
            .read(positions_out, Access::HostRead)
            .read(frames_out, Access::HostRead)
            .side_effect();
        stats_.passes += 2U;
    }
    return graph.status();
}

Span<const ResourceId> SkinnedScene::vertex_reads() const noexcept {
    return {reads_, read_count_};
}

bool SkinnedScene::output(SkinnedInstance instance, SkinnedOutput& out) const noexcept {
    const Instance* slot = instance_of(instance);
    if (slot == nullptr || !slot->skinned) {
        return false;
    }
    const u32 vertices = meshes_[slot->mesh].vertex_count;
    const u32 current = slot->constants.first_output_vertex;
    out.positions = buffers_.positions;
    out.frames = buffers_.frames;
    out.current_vertex = current;
    out.previous_vertex = current == slot->first_output ? current + vertices : slot->first_output;
    out.has_previous = slot->has_previous;
    out.vertex_count = vertices;
    return true;
}

Expected<GpuSkinConstants, Error> SkinnedScene::constants(SkinnedInstance instance) const noexcept {
    const Instance* slot = instance_of(instance);
    if (slot == nullptr || !slot->skinned) {
        return fail(ErrorCode::NotFound, "skinned scene: the instance was not skinned this frame");
    }
    return slot->constants;
}

Expected<Span<const Vec3>, Error> SkinnedScene::read_back_positions() const noexcept {
    const auto* positions =
        device_ == nullptr || !buffers_.positions_readback
            ? nullptr
            : static_cast<const Vec3*>(device_->buffer_mapped_pointer(buffers_.positions_readback));
    if (positions == nullptr) {
        return fail(
            ErrorCode::InvalidArgument,
            "skinned scene: no read-back; create it with SkinnedSceneDescription::read_back");
    }
    return Span<const Vec3>(positions, static_cast<usize>(desc_.max_instance_vertices) * 2U);
}

Expected<Span<const PackedNormalTangent>, Error> SkinnedScene::read_back_frames() const noexcept {
    const auto* frames = device_ == nullptr || !buffers_.frames_readback
                             ? nullptr
                             : static_cast<const PackedNormalTangent*>(
                                   device_->buffer_mapped_pointer(buffers_.frames_readback));
    if (frames == nullptr) {
        return fail(
            ErrorCode::InvalidArgument,
            "skinned scene: no read-back; create it with SkinnedSceneDescription::read_back");
    }
    return Span<const PackedNormalTangent>(frames,
                                           static_cast<usize>(desc_.max_instance_vertices) * 2U);
}

Span<const GpuBoneMatrix> SkinnedScene::pose_buffer() const noexcept {
    if (device_ == nullptr) {
        return {};
    }
    const auto* bones =
        static_cast<const GpuBoneMatrix*>(device_->buffer_mapped_pointer(buffers_.bones));
    return bones == nullptr ? Span<const GpuBoneMatrix>{}
                            : Span<const GpuBoneMatrix>(bones, desc_.max_pose_matrices);
}

}  // namespace cy::rendering::skinning
