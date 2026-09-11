// The per-frame uploads and the descriptor sets that name them. M8.c task 1b.1.

#include <cy/rendering/pipeline/frame_bindings.h>

#include <cstring>

namespace cy::rendering::pipeline {
namespace {

/// One buffer of the ring. Every one of them is `Upload` memory and persistently mapped, which is
/// what `rhi::Device::buffer_mapped_pointer` documents: mapping is not free and a per-frame ring is
/// written every frame.
[[nodiscard]] Expected<rhi::BufferHandle, Error> make_buffer(rhi::Device& device, const char* name,
                                                             u64 bytes,
                                                             rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    // Never zero: a device refuses a zero-size buffer, and a frame with no lights is an ordinary
    // frame rather than an error. One element's worth is the floor.
    description.size = bytes == 0 ? 16U : bytes;
    description.usage = usage;
    description.memory = rhi::MemoryUse::Upload;
    return device.create_buffer(description);
}

template <typename T>
[[nodiscard]] Status copy_span(rhi::Device& device, rhi::BufferHandle buffer, Span<const T> source,
                               u32 capacity, const char* what) noexcept {
    if (source.size() > capacity) {
        return fail(ErrorCode::OutOfRange, what);
    }
    if (source.empty()) {
        return ok();
    }
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "frame bindings: a ring buffer is not mapped");
    }
    std::memcpy(mapped, source.data(), source.size() * sizeof(T));
    return ok();
}

template <typename T>
[[nodiscard]] Status copy_value(rhi::Device& device, rhi::BufferHandle buffer,
                                const T& value) noexcept {
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "frame bindings: a constant buffer is not mapped");
    }
    std::memcpy(mapped, &value, sizeof(T));
    return ok();
}

[[nodiscard]] rhi::DescriptorWrite buffer_write(u32 binding, rhi::DescriptorKind kind,
                                                rhi::BufferHandle buffer) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = kind;
    write.buffer = buffer;
    write.buffer_offset = 0;
    write.buffer_range = 0;  // the rest of the buffer
    return write;
}

}  // namespace

BindingCapacity BindingCapacity::for_grid(const ClusterGrid& grid, u32 max_draws, u32 max_instances,
                                          u32 material_capacity, u32 max_lights) noexcept {
    BindingCapacity capacity;
    capacity.lights = max_lights;
    // One header per (cluster, element type) and one index per element a cluster may hold. Both
    // come off the grid rather than off a guess, so the ring cannot be smaller than the assignment
    // that fills it.
    const u64 clusters = grid.cluster_count();
    capacity.cluster_headers = static_cast<u32>(clusters * kClusterElementTypeCount) + 1U;
    capacity.cluster_indices =
        static_cast<u32>(clusters * grid.max_elements_per_cluster * kClusterElementTypeCount) + 1U;
    capacity.draws = max_draws;
    capacity.instances = max_instances;
    capacity.material_bytes = material_capacity * kMaterialBlockBytes;
    return capacity;
}

FrameBindings::~FrameBindings() {
    shutdown();
}

Span<const rhi::DescriptorSetHandle> FrameBindings::sets() const noexcept {
    return {sets_, kSetCount};
}

Status FrameBindings::create_slot(rhi::Device& device, u32 index) noexcept {
    Slot& slot = slots_[index];
    struct Request {
        const char* name;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"cy frame globals", sizeof(GlobalsData), rhi::BufferUsage::Uniform, &slot.globals},
        {"cy frame view", sizeof(FrameViewData), rhi::BufferUsage::Uniform, &slot.view},
        {"cy frame lights", u64{capacity_.lights} * sizeof(GpuLight),
         rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource, &slot.lights},
        {"cy frame cluster headers", u64{capacity_.cluster_headers} * sizeof(ClusterHeader),
         rhi::BufferUsage::Storage, &slot.cluster_headers},
        {"cy frame cluster indices", u64{capacity_.cluster_indices} * sizeof(u32),
         rhi::BufferUsage::Storage, &slot.cluster_indices},
        {"cy frame draws", u64{capacity_.draws} * sizeof(GpuDrawInstance),
         rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource, &slot.draws},
        {"cy frame instances", u64{capacity_.instances} * sizeof(InstanceTransform),
         rhi::BufferUsage::Storage, &slot.instances},
        {"cy frame materials", u64{capacity_.material_bytes}, rhi::BufferUsage::Storage,
         &slot.materials},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> made =
            make_buffer(device, request.name, request.bytes, request.usage);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    (void)index;
    return ok();
}

Status FrameBindings::initialize(rhi::Device& device, const FramePipelines& pipelines,
                                 const BindingCapacity& capacity) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "frame bindings: already initialized");
    }
    if (!pipelines.ready()) {
        return fail(ErrorCode::InvalidArgument,
                    "frame bindings: the pipelines must be initialized first");
    }
    device_ = &device;
    pipelines_ = &pipelines;
    capacity_ = capacity;
    slot_count_ = device.frames_in_flight();
    if (slot_count_ == 0 || slot_count_ > rhi::kMaxFramesInFlight) {
        device_ = nullptr;
        return fail(ErrorCode::InvalidArgument,
                    "frame bindings: the device reported an impossible frames-in-flight");
    }
    for (u32 index = 0; index < slot_count_; ++index) {
        if (Status made = create_slot(device, index); !made) {
            shutdown();
            return made;
        }
    }
    ready_ = true;
    return ok();
}

Status FrameBindings::write_sets(u32 frame_slot) noexcept {
    rhi::Device& device = *device_;
    const Slot& slot = slots_[frame_slot];

    for (u32 index = 0; index < kSetCount; ++index) {
        // PER-FRAME, DELIBERATELY. See the header comment: a set the caller asked to be persistent
        // and a set allocated from the frame pool are the same object until the frame comes round,
        // which is the defect M4's suite exists for.
        Expected<rhi::DescriptorSetHandle, Error> allocated =
            device.allocate_descriptor_set(pipelines_->set_layout(index), true);
        if (!allocated.has_value()) {
            return make_unexpected(allocated.error());
        }
        sets_[index] = *allocated;
    }

    const rhi::DescriptorWrite globals[] = {
        buffer_write(0, rhi::DescriptorKind::UniformBuffer, slot.globals),
    };
    if (Status written = device.update_descriptor_set(sets_[kGlobalSet],
                                                      Span<const rhi::DescriptorWrite>(globals, 1));
        !written) {
        return written;
    }

    const rhi::DescriptorWrite view[] = {
        buffer_write(kViewBindingFrame, rhi::DescriptorKind::UniformBuffer, slot.view),
        buffer_write(kViewBindingLights, rhi::DescriptorKind::StorageBuffer, slot.lights),
        buffer_write(kViewBindingClusterHeaders, rhi::DescriptorKind::StorageBuffer,
                     slot.cluster_headers),
        buffer_write(kViewBindingClusterIndices, rhi::DescriptorKind::StorageBuffer,
                     slot.cluster_indices),
        buffer_write(kViewBindingDrawInstances, rhi::DescriptorKind::StorageBuffer, slot.draws),
        buffer_write(kViewBindingInstances, rhi::DescriptorKind::StorageBuffer, slot.instances),
        buffer_write(kViewBindingMaterials, rhi::DescriptorKind::StorageBuffer, slot.materials),
    };
    return device.update_descriptor_set(sets_[kViewSet],
                                        Span<const rhi::DescriptorWrite>(view, kViewBindingCount));
}

Status FrameBindings::upload(u32 frame_slot, const FrameUpload& upload) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "frame bindings: not initialized");
    }
    if (frame_slot >= slot_count_) {
        return fail(ErrorCode::OutOfRange, "frame bindings: the frame slot is past the ring");
    }
    rhi::Device& device = *device_;
    const Slot& slot = slots_[frame_slot];

    if (Status written = copy_value(device, slot.globals, upload.globals); !written) {
        return written;
    }
    if (Status written = copy_value(device, slot.view, upload.view); !written) {
        return written;
    }
    if (Status written = copy_span(device, slot.lights, upload.lights, capacity_.lights,
                                   "frame bindings: more lights than the ring was sized for");
        !written) {
        return written;
    }
    if (Status written = copy_span(
            device, slot.cluster_headers, upload.cluster_headers, capacity_.cluster_headers,
            "frame bindings: more cluster headers than the ring was sized for");
        !written) {
        return written;
    }
    if (Status written = copy_span(
            device, slot.cluster_indices, upload.cluster_indices, capacity_.cluster_indices,
            "frame bindings: more cluster indices than the ring was sized for");
        !written) {
        return written;
    }
    if (Status written = copy_span(device, slot.draws, upload.draws, capacity_.draws,
                                   "frame bindings: more draws than the ring was sized for");
        !written) {
        return written;
    }
    if (Status written = copy_span(device, slot.instances, upload.instances, capacity_.instances,
                                   "frame bindings: more instances than the ring was sized for");
        !written) {
        return written;
    }
    if (Status written =
            copy_span(device, slot.materials, upload.materials, capacity_.material_bytes,
                      "frame bindings: the material table is larger than the ring");
        !written) {
        return written;
    }
    if (Status written = write_sets(frame_slot); !written) {
        return written;
    }
    current_slot_ = frame_slot;
    staged_light_bytes_ = upload.lights.size() * sizeof(GpuLight);
    staged_draw_bytes_ = upload.draws.size() * sizeof(GpuDrawInstance);
    ++uploads_;
    return ok();
}

rhi::BufferHandle FrameBindings::staged_lights() const noexcept {
    return ready_ ? slots_[current_slot_].lights : rhi::BufferHandle{};
}

rhi::BufferHandle FrameBindings::staged_draws() const noexcept {
    return ready_ ? slots_[current_slot_].draws : rhi::BufferHandle{};
}

Status FrameBindings::bind_scene_color(rhi::TextureViewHandle view) noexcept {
    if (!ready_ || sets_[kPassSet].is_null()) {
        return fail(ErrorCode::Unavailable, "frame bindings: no pass set to write");
    }
    rhi::DescriptorWrite writes[2];
    writes[0].binding = kPassBindingSceneColor;
    writes[0].kind = rhi::DescriptorKind::SampledTexture;
    writes[0].texture_view = view;
    writes[0].layout = rhi::ImageLayout::ShaderReadOnly;
    writes[1].binding = kPassBindingSampler;
    writes[1].kind = rhi::DescriptorKind::Sampler;
    writes[1].sampler = pipelines_->linear_clamp();
    return device_->update_descriptor_set(sets_[kPassSet],
                                          Span<const rhi::DescriptorWrite>(writes, 2));
}

void FrameBindings::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (Slot& slot : slots_) {
        rhi::BufferHandle* handles[] = {
            &slot.globals,         &slot.view,  &slot.lights,    &slot.cluster_headers,
            &slot.cluster_indices, &slot.draws, &slot.instances, &slot.materials};
        for (rhi::BufferHandle* handle : handles) {
            if (!handle->is_null()) {
                device.destroy_buffer(*handle);
                *handle = rhi::BufferHandle{};
            }
        }
    }
    for (rhi::DescriptorSetHandle& set : sets_) {
        set = rhi::DescriptorSetHandle{};
    }
    device_ = nullptr;
    pipelines_ = nullptr;
    slot_count_ = 0;
    current_slot_ = 0;
    staged_light_bytes_ = 0;
    staged_draw_bytes_ = 0;
    uploads_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::pipeline
