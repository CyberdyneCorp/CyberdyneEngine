// SPDX-License-Identifier: MIT
// Acceleration structures on Vulkan: `VK_KHR_acceleration_structure`, traversed by
// `VK_KHR_ray_query`. Issue #35, stage 3. See cy/backends/rhi/acceleration.h for the vocabulary.
//
// ================================================================================================
// WHAT A STRUCTURE OWNS, AND WHY EACH PIECE IS SIZED AT CREATION
// ================================================================================================
//
//   storage    the buffer the driver places the structure in, registered in the device's buffer
//              pool so a render graph pass can declare its access against it;
//   scratch    the larger of the build and the update scratch the driver states, aligned to
//              `minAccelerationStructureScratchOffsetAlignment` — one per structure, so the builds
//              of one `build_acceleration_structures` call never share a region;
//   instances  a top level's `VkAccelerationStructureInstanceKHR` records, host-visible, written
//              at record time from the caller's engine-owned instances.
//
// `vkGetAccelerationStructureBuildSizesKHR` is asked once, at the description's maxima, and a build
// that carries more than the description is refused by name rather than overrunning a buffer.
//
// ================================================================================================
// THE HOST-VISIBLE INSTANCE RECORDS ARE WRITTEN IN PLACE
// ================================================================================================
//
// Right for a renderer that drains each frame, which is every suite that builds one today, and the
// same deliberate shortcut `cy::rendering-gi-gpu` documents for its scene buffers: a top level
// rebuilt while the frame before it still traverses it needs per-frame instance staging, and that
// arrives with the frame composite (issue #35 stage 4).

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cstring>

#include "vulkan_common.h"
#include "vulkan_device.h"

namespace cy::rhi::vulkan {
namespace {

[[nodiscard]] VkBuildAccelerationStructureFlagsKHR build_flags(
    const AccelerationStructureDescription& desc) noexcept {
    VkBuildAccelerationStructureFlagsKHR flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (desc.allow_update) {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    return flags;
}

[[nodiscard]] VkAccelerationStructureTypeKHR structure_type(
    AccelerationStructureKind kind) noexcept {
    return kind == AccelerationStructureKind::TopLevel
               ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
               : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
}

[[nodiscard]] u64 align_up(u64 value, u64 alignment) noexcept {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

}  // namespace

VkDeviceAddress VulkanDevice::buffer_address(BufferHandle handle) noexcept {
    const VulkanBuffer* stored = buffers_.resolve(handle);
    if (stored == nullptr) {
        return 0;
    }
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = stored->buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

Expected<VkBuffer, Error> VulkanDevice::create_raw_buffer(u64 size, VkBufferUsageFlags usage,
                                                          bool host_visible, u64 alignment,
                                                          VmaAllocation& allocation,
                                                          void** mapped) noexcept {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo create{};
    create.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) {
        create.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocated{};
    CY_VK_TRY(vmaCreateBufferWithAlignment(vma_, &info, &create, alignment, &buffer, &allocation,
                                           &allocated),
              "vmaCreateBufferWithAlignment (acceleration structure)");
    if (mapped != nullptr) {
        *mapped = allocated.pMappedData;
    }
    return buffer;
}

Status VulkanDevice::size_acceleration_structure(
    const AccelerationStructureDescription& desc,
    VkAccelerationStructureBuildSizesInfoKHR& sizes) const noexcept {
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    u32 primitives = 0;
    if (desc.kind == AccelerationStructureKind::TopLevel) {
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        primitives = desc.max_instances;
    } else {
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexStride = 12;
        triangles.maxVertex = desc.max_vertices - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        primitives = desc.max_triangles;
    }
    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = structure_type(desc.kind);
    build.flags = build_flags(desc);
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    sizes = VkAccelerationStructureBuildSizesInfoKHR{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);
    if (sizes.accelerationStructureSize == 0) {
        return fail(ErrorCode::Internal,
                    "vkGetAccelerationStructureBuildSizesKHR reported a zero-sized structure");
    }
    return ok();
}

Expected<AccelerationStructureHandle, Error> VulkanDevice::create_acceleration_structure(
    const AccelerationStructureDescription& desc) {
    if (!ray_tracing_.enabled_on_the_device) {
        return Device::create_acceleration_structure(desc);
    }
    const bool top = desc.kind == AccelerationStructureKind::TopLevel;
    if ((top && desc.max_instances == 0) ||
        (!top && (desc.max_triangles == 0 || desc.max_vertices == 0))) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "an acceleration structure is sized at creation: a top level "
                                     "needs max_instances, a bottom level max_triangles and "
                                     "max_vertices, none of them zero"});
    }
    if (scratch_alignment_ == 0) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 all{};
        all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        all.pNext = &properties;
        vkGetPhysicalDeviceProperties2(physical_, &all);
        scratch_alignment_ =
            std::max<u64>(properties.minAccelerationStructureScratchOffsetAlignment, 1);
    }

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    if (Status sized = size_acceleration_structure(desc, sizes); !sized) {
        return make_unexpected(sized.error());
    }

    VulkanAccelerationStructure record;
    record.desc = desc;
    record.name.assign(desc.name);
    record.desc.name = record.name.text;
    record.structure_bytes = sizes.accelerationStructureSize;
    record.scratch_bytes = std::max(sizes.buildScratchSize, sizes.updateScratchSize);

    // The storage: a buffer in the device's own pool, so it has a handle a graph can import.
    VulkanBuffer storage;
    storage.desc.name = "acceleration structure storage";
    storage.desc.size = record.structure_bytes;
    storage.desc.memory = MemoryUse::DeviceLocal;
    storage.name.assign(desc.name);
    storage.desc.name = storage.name.text;
    storage.bytes = record.structure_bytes;
    storage.category = GpuMemoryCategory::Persistent;
    Expected<VkBuffer, Error> made =
        create_raw_buffer(record.structure_bytes,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, 256, storage.allocation, nullptr);
    if (!made) {
        return make_unexpected(made.error());
    }
    storage.buffer = *made;
    Expected<BufferHandle, Error> storage_handle = buffers_.create(storage);
    if (!storage_handle) {
        vmaDestroyBuffer(vma_, storage.buffer, storage.allocation);
        return make_unexpected(storage_handle.error());
    }
    record.storage = *storage_handle;
    charge(GpuMemoryCategory::Persistent, record.structure_bytes);

    // Everything below that fails releases what was made above through the one destroy path.
    const auto abandon = [this, &record](const Error& error) {
        if (record.scratch != VK_NULL_HANDLE) {
            vmaDestroyBuffer(vma_, record.scratch, record.scratch_allocation);
        }
        if (record.instances != VK_NULL_HANDLE) {
            vmaDestroyBuffer(vma_, record.instances, record.instance_allocation);
        }
        if (record.structure != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(device_, record.structure, nullptr);
        }
        destroy_buffer(record.storage);
        return make_unexpected(error);
    };

    Expected<VkBuffer, Error> scratch = create_raw_buffer(
        record.scratch_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
        scratch_alignment_, record.scratch_allocation, nullptr);
    if (!scratch) {
        return abandon(scratch.error());
    }
    record.scratch = *scratch;
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = record.scratch;
    record.scratch_address =
        align_up(vkGetBufferDeviceAddress(device_, &address), scratch_alignment_);

    if (top) {
        record.instance_bytes =
            static_cast<u64>(desc.max_instances) * sizeof(VkAccelerationStructureInstanceKHR);
        Expected<VkBuffer, Error> instances =
            create_raw_buffer(record.instance_bytes,
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              true, 16, record.instance_allocation, &record.instance_mapped);
        if (!instances || record.instance_mapped == nullptr) {
            return abandon(instances ? Error{ErrorCode::Internal,
                                             "the top level's instance records are not mapped"}
                                     : instances.error());
        }
        record.instances = *instances;
        address.buffer = record.instances;
        record.instance_address = vkGetBufferDeviceAddress(device_, &address);
    }

    VkAccelerationStructureCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create.buffer = storage.buffer;
    create.size = record.structure_bytes;
    create.type = structure_type(desc.kind);
    if (VkResult result =
            vkCreateAccelerationStructureKHR(device_, &create, nullptr, &record.structure);
        result != VK_SUCCESS) {
        return abandon(error_from(result, "vkCreateAccelerationStructureKHR"));
    }
    VkAccelerationStructureDeviceAddressInfoKHR structure_address{};
    structure_address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    structure_address.accelerationStructure = record.structure;
    record.address = vkGetAccelerationStructureDeviceAddressKHR(device_, &structure_address);

    Expected<AccelerationStructureHandle, Error> handle = acceleration_structures_.create(record);
    if (!handle) {
        return abandon(handle.error());
    }
    if (const VulkanAccelerationStructure* stored = acceleration_structures_.resolve(*handle);
        stored != nullptr) {
        name_object(reinterpret_cast<u64>(stored->structure),
                    VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, stored->name.text);
    }
    charge(GpuMemoryCategory::Persistent, record.scratch_bytes + record.instance_bytes);
    return handle;
}

void VulkanDevice::destroy_acceleration_structure(AccelerationStructureHandle handle) noexcept {
    VulkanAccelerationStructure* record = acceleration_structures_.resolve(handle);
    if (record == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "destroy_acceleration_structure() on a stale or never-issued handle");
        return;
    }
    // The structure first, then its memory: both deferred to the frames that could still use them.
    retire(Retirement::Kind::AccelerationStructure, record->structure, nullptr);
    retire(Retirement::Kind::Buffer, record->scratch, record->scratch_allocation);
    if (record->instances != VK_NULL_HANDLE) {
        retire(Retirement::Kind::Buffer, record->instances, record->instance_allocation);
    }
    discharge(GpuMemoryCategory::Persistent, record->scratch_bytes + record->instance_bytes);
    destroy_buffer(record->storage);
    (void)acceleration_structures_.destroy(handle);
}

bool VulkanDevice::acceleration_structure_valid(AccelerationStructureHandle handle) const noexcept {
    return acceleration_structures_.resolve(handle) != nullptr;
}

AccelerationStructureInfo VulkanDevice::acceleration_structure_info(
    AccelerationStructureHandle handle) const noexcept {
    AccelerationStructureInfo info;
    const VulkanAccelerationStructure* record = acceleration_structures_.resolve(handle);
    if (record == nullptr) {
        return info;
    }
    info.storage = record->storage;
    info.structure_bytes = record->structure_bytes;
    info.scratch_bytes = record->scratch_bytes;
    info.instance_bytes = record->instance_bytes;
    info.kind = record->desc.kind;
    info.allow_update = record->desc.allow_update;
    return info;
}

// --- Recording -----------------------------------------------------------------------------------

namespace {

/// Everything one build's `VkAccelerationStructureBuildGeometryInfoKHR` points at. Kept per build
/// in a fixed array because the call takes them all at once.
struct PendingBuild {
    VkAccelerationStructureGeometryKHR geometry{};
    VkAccelerationStructureBuildRangeInfoKHR range{};
};

/// Write a top level's instances into its own records, in Vulkan's form. False when one names a
/// bottom level that does not exist, which records nothing rather than a dangling address.
[[nodiscard]] bool write_instances(VulkanDevice& device, VulkanAccelerationStructure& target,
                                   Span<const AccelerationStructureInstance> instances) noexcept {
    auto* records = static_cast<VkAccelerationStructureInstanceKHR*>(target.instance_mapped);
    for (usize index = 0; index < instances.size(); ++index) {
        const AccelerationStructureInstance& source = instances[index];
        const VulkanAccelerationStructure* bottom = device.acceleration_structure(source.bottom);
        if (bottom == nullptr || bottom->desc.kind != AccelerationStructureKind::BottomLevel) {
            return false;
        }
        VkAccelerationStructureInstanceKHR record{};
        std::memcpy(&record.transform.matrix[0][0], source.transform, sizeof(source.transform));
        record.instanceCustomIndex = source.custom_index & 0xFFFFFFU;
        record.mask = source.mask;
        record.instanceShaderBindingTableRecordOffset = 0;
        // Both faces: a GI ray that leaves a surface and meets the back of a wall has met the
        // wall, which is what the service's own trace answers too.
        record.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        record.accelerationStructureReference = bottom->address;
        records[index] = record;
    }
    return true;
}

}  // namespace

void VulkanCommandBuffer::build_acceleration_structures(
    Span<const AccelerationStructureBuild> builds) noexcept {
    constexpr usize kMaxBuilds = 64;
    if (builds.empty() || !device_->ray_tracing_enabled()) {
        return;
    }
    if (builds.size() > kMaxBuilds) {
        device_->report_validation(ValidationSeverity::Error,
                                   "build_acceleration_structures(): more than 64 builds in one "
                                   "call; split them across passes");
        return;
    }
    PendingBuild pending[kMaxBuilds] = {};
    VkAccelerationStructureBuildGeometryInfoKHR infos[kMaxBuilds] = {};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[kMaxBuilds] = {};
    u32 count = 0;

    for (const AccelerationStructureBuild& build : builds) {
        VulkanAccelerationStructure* target = device_->acceleration_structure(build.target);
        if (target == nullptr) {
            device_->report_validation(ValidationSeverity::Error,
                                       "build_acceleration_structures(): a stale target handle");
            continue;
        }
        const bool update = build.mode == AccelerationStructureBuildMode::Update;
        if (update && !target->desc.allow_update) {
            device_->report_validation(ValidationSeverity::Error,
                                       "build_acceleration_structures(): an update of a structure "
                                       "created without allow_update");
            continue;
        }
        PendingBuild& slot = pending[count];
        slot.geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        slot.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        if (target->desc.kind == AccelerationStructureKind::TopLevel) {
            if (build.instances.size() > target->desc.max_instances ||
                !write_instances(*device_, *target, build.instances)) {
                device_->report_validation(ValidationSeverity::Error,
                                           "build_acceleration_structures(): a top level with more "
                                           "instances than it was created for, or an instance of "
                                           "a stale bottom level");
                continue;
            }
            slot.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            slot.geometry.geometry.instances.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            slot.geometry.geometry.instances.arrayOfPointers = VK_FALSE;
            slot.geometry.geometry.instances.data.deviceAddress = target->instance_address;
            slot.range.primitiveCount = static_cast<u32>(build.instances.size());
        } else {
            const AccelerationStructureTriangles& source = build.triangles;
            const VkDeviceAddress vertices = device_->buffer_address(source.vertices);
            const VkDeviceAddress indices = device_->buffer_address(source.indices);
            if (vertices == 0 || indices == 0 ||
                source.triangle_count > target->desc.max_triangles ||
                source.vertex_count > target->desc.max_vertices || source.vertex_count == 0) {
                device_->report_validation(ValidationSeverity::Error,
                                           "build_acceleration_structures(): a bottom level whose "
                                           "inputs are stale or larger than it was created for");
                continue;
            }
            VkAccelerationStructureGeometryTrianglesDataKHR& triangles =
                slot.geometry.geometry.triangles;
            slot.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = vertices + source.vertex_offset;
            triangles.vertexStride = source.vertex_stride;
            triangles.maxVertex = source.vertex_count - 1;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = indices + source.index_offset;
            slot.range.primitiveCount = source.triangle_count;
        }
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[count];
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = structure_type(target->desc.kind);
        info.flags = build_flags(target->desc);
        info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                           : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = update ? target->structure : VK_NULL_HANDLE;
        info.dstAccelerationStructure = target->structure;
        info.geometryCount = 1;
        info.pGeometries = &slot.geometry;
        info.scratchData.deviceAddress = target->scratch_address;
        ranges[count] = &slot.range;
        ++count;
    }
    if (count != 0) {
        vkCmdBuildAccelerationStructuresKHR(commands_, count, infos, ranges);
    }
}

}  // namespace cy::rhi::vulkan
