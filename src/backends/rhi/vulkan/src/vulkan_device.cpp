// Resources, pipelines, frames and submission on a real device. Task 2.3.1.
//
// Every creation call runs the same cy::rhi::validate_* the null backend runs before it touches a
// Vulkan entry point, so a pipeline refused in continuous integration is refused here for the same
// reason and with the same message.

#include <cy/core/memory/pressure.h>

#include "vulkan_device.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace cy::rhi::vulkan {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

u64 hash_bytes(u64 seed, const void* data, usize size) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = seed;
    for (usize index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

GpuMemoryCategory category_for(MemoryUse memory) noexcept {
    switch (memory) {
        case MemoryUse::Upload:
            return GpuMemoryCategory::Upload;
        case MemoryUse::Readback:
            return GpuMemoryCategory::Readback;
        case MemoryUse::DeviceLocal:
        case MemoryUse::HostVisibleDeviceLocal:
            break;
    }
    return GpuMemoryCategory::Persistent;
}

/// The VMA request one of the engine's memory uses becomes.
///
/// HostVisibleDeviceLocal is the unified-memory and resizable-BAR case `rhi-and-render-graph`
/// singles out: per-frame instance data is written straight to device-local memory and the staging
/// copy is skipped. VMA answers with host-visible device-local where the device has it and falls
/// back on its own where it does not, which is why this is one branch rather than a capability
/// test.
VmaAllocationCreateInfo allocation_for(MemoryUse memory) noexcept {
    VmaAllocationCreateInfo info{};
    switch (memory) {
        case MemoryUse::Upload:
            info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUse::Readback:
            info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            info.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUse::HostVisibleDeviceLocal:
            info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUse::DeviceLocal:
            info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            break;
    }
    return info;
}

u64 texture_byte_size(const TextureDescription& desc) noexcept {
    u64 total = 0;
    for (u16 mip = 0; mip < desc.mip_levels; ++mip) {
        const u32 width = desc.extent.width >> mip;
        const u32 height = desc.extent.height >> mip;
        const u32 depth = desc.extent.depth >> mip;
        total += format_byte_size(desc.format, width != 0 ? width : 1, height != 0 ? height : 1) *
                 (depth != 0 ? depth : 1);
    }
    return total * desc.array_layers;
}

VkImageCreateInfo image_info_for(const TextureDescription& desc) noexcept {
    // Vulkan's flag-bit enums have no zero enumerator, and zero-initialising the structure before
    // filling in what matters is the API's own idiom.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = to_vulkan_image_type(desc.dimension);
    info.format = to_vulkan(desc.format);
    info.extent = VkExtent3D{desc.extent.width, desc.extent.height, desc.extent.depth};
    info.mipLevels = desc.mip_levels;
    info.arrayLayers = desc.array_layers;
    info.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = to_vulkan(desc.usage);
    // EXCLUSIVE, always. `rhi-and-render-graph` has the graph derive queue-family ownership
    // transfers; CONCURRENT would make them unnecessary and would cost bandwidth on every access,
    // on every device that compresses. The transfers are the cheaper answer and the graph emits
    // them whether or not a validation layer would have caught their absence — it does not.
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.dimension == TextureDimension::Cube) {
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    return info;
}

}  // namespace

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(device_);
        release_retirements(~0ULL);
        release_transient_resources();

        for (u32 slot = 0; slot < frames_in_flight_; ++slot) {
            FrameContext& frame = frames_[slot];
            for (const auto& per_queue : frame.command_pools) {
                for (VkCommandPool pool : per_queue) {
                    if (pool != VK_NULL_HANDLE) {
                        vkDestroyCommandPool(device_, pool, nullptr);
                    }
                }
            }
            if (frame.descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, frame.descriptor_pool, nullptr);
            }
            if (frame.fence != VK_NULL_HANDLE) {
                vkDestroyFence(device_, frame.fence, nullptr);
            }
        }
        for (VkSemaphore semaphore : timelines_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        if (persistent_descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, persistent_descriptor_pool_, nullptr);
        }
        if (bindless_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, bindless_pool_, nullptr);
        }
        if (bindless_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, bindless_layout_, nullptr);
        }
        if (breadcrumb_buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(vma_, breadcrumb_buffer_, breadcrumb_allocation_);
        }
        if (transient_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, transient_memory_, nullptr);
        }
        if (pipeline_cache_ != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        }
        if (vma_ != nullptr) {
            vmaDestroyAllocator(vma_);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

// --- Retirement
// ------------------------------------------------------------------------------------
//
// `rhi-and-render-graph`: "its memory SHALL be released only after frame N's fence has signalled".

void VulkanDevice::retire(Retirement::Kind kind, void* object, VmaAllocation allocation) noexcept {
    Retirement entry;
    entry.kind = kind;
    entry.frame = frame_index_;
    entry.object = object;
    entry.allocation = allocation;
    (void)retirements_.push_back(entry);
    ++stats_.resources_retired;
}

void VulkanDevice::release_retirements(u64 up_to_frame) noexcept {
    // Compacted in place: only elements at or before the read position are written, and Array's
    // iterator is a pointer, so this is well defined.
    usize kept = 0;
    for (const Retirement entry : retirements_) {
        if (entry.frame > up_to_frame) {
            retirements_[kept] = entry;
            ++kept;
            continue;
        }
        switch (entry.kind) {
            case Retirement::Kind::Buffer:
                vmaDestroyBuffer(vma_, static_cast<VkBuffer>(entry.object), entry.allocation);
                break;
            case Retirement::Kind::Image:
                if (entry.allocation != nullptr) {
                    vmaDestroyImage(vma_, static_cast<VkImage>(entry.object), entry.allocation);
                } else {
                    // A transient: its memory belongs to the graph's pool, not to VMA.
                    vkDestroyImage(device_, static_cast<VkImage>(entry.object), nullptr);
                }
                break;
            case Retirement::Kind::ImageView:
                vkDestroyImageView(device_, static_cast<VkImageView>(entry.object), nullptr);
                break;
            case Retirement::Kind::Sampler:
                vkDestroySampler(device_, static_cast<VkSampler>(entry.object), nullptr);
                break;
            case Retirement::Kind::Pipeline:
                vkDestroyPipeline(device_, static_cast<VkPipeline>(entry.object), nullptr);
                break;
            case Retirement::Kind::ShaderModule:
                vkDestroyShaderModule(device_, static_cast<VkShaderModule>(entry.object), nullptr);
                break;
            case Retirement::Kind::Framebuffer:
                break;
        }
        ++stats_.resources_freed;
    }
    while (retirements_.size() > kept) {
        retirements_.pop_back();
    }
}

void VulkanDevice::charge(GpuMemoryCategory category, u64 bytes) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kGpuMemoryCategoryCount) {
        return;
    }
    live_bytes_[index] += bytes;
    peak_bytes_[index] = std::max(peak_bytes_[index], live_bytes_[index]);
    ++allocation_count_;
}

void VulkanDevice::discharge(GpuMemoryCategory category, u64 bytes) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kGpuMemoryCategoryCount) {
        return;
    }
    live_bytes_[index] = live_bytes_[index] >= bytes ? live_bytes_[index] - bytes : 0;
}

// --- Buffers
// ---------------------------------------------------------------------------------------

Expected<BufferHandle, Error> VulkanDevice::create_buffer(const BufferDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_buffer(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = to_vulkan(desc.usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    const VmaAllocationCreateInfo allocation = allocation_for(desc.memory);
    VulkanBuffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    buffer.category = category_for(desc.memory);
    buffer.bytes = desc.size;

    VmaAllocationInfo allocated{};
    CY_VK_TRY(
        vmaCreateBuffer(vma_, &info, &allocation, &buffer.buffer, &buffer.allocation, &allocated),
        "vmaCreateBuffer");
    buffer.mapped = allocated.pMappedData;

    Expected<BufferHandle, Error> handle = buffers_.create(buffer);
    if (!handle) {
        vmaDestroyBuffer(vma_, buffer.buffer, buffer.allocation);
        return handle;
    }
    // Every resource is named for a debug tool, which `rhi-and-render-graph` requires; the pointer
    // is the stored copy, so it stays valid for the resource's life.
    name_object(reinterpret_cast<u64>(buffer.buffer), VK_OBJECT_TYPE_BUFFER,
                buffers_.resolve(*handle)->name.text);
    charge(buffer.category, desc.size);
    return handle;
}

void VulkanDevice::destroy_buffer(BufferHandle handle) noexcept {
    VulkanBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "destroy_buffer() on a stale or never-issued handle");
        return;
    }
    if (!buffer->transient) {
        discharge(buffer->category, buffer->bytes);
    }
    retire(Retirement::Kind::Buffer, buffer->buffer, buffer->allocation);
    (void)buffers_.destroy(handle);
}

bool VulkanDevice::is_valid(BufferHandle handle) const noexcept {
    return buffers_.resolve(handle) != nullptr;
}

void* VulkanDevice::buffer_mapped_pointer(BufferHandle handle) noexcept {
    VulkanBuffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? buffer->mapped : nullptr;
}

const BufferDescription* VulkanDevice::buffer_description(BufferHandle handle) const noexcept {
    const VulkanBuffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? &buffer->desc : nullptr;
}

// --- Textures
// --------------------------------------------------------------------------------------

Expected<TextureHandle, Error> VulkanDevice::create_texture(const TextureDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_texture(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    const VkImageCreateInfo info = image_info_for(desc);
    const VmaAllocationCreateInfo allocation = allocation_for(desc.memory);

    VulkanTexture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    texture.category = category_for(desc.memory);
    texture.bytes = texture_byte_size(desc);

    CY_VK_TRY(
        vmaCreateImage(vma_, &info, &allocation, &texture.image, &texture.allocation, nullptr),
        "vmaCreateImage");

    Expected<TextureHandle, Error> handle = textures_.create(texture);
    if (!handle) {
        vmaDestroyImage(vma_, texture.image, texture.allocation);
        return handle;
    }
    name_object(reinterpret_cast<u64>(texture.image), VK_OBJECT_TYPE_IMAGE,
                textures_.resolve(*handle)->name.text);
    charge(texture.category, texture.bytes);
    return handle;
}

void VulkanDevice::destroy_texture(TextureHandle handle) noexcept {
    VulkanTexture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "destroy_texture() on a stale or never-issued handle");
        return;
    }
    if (texture->owned_by_swapchain) {
        report_validation(ValidationSeverity::Error,
                          "destroy_texture() on a swapchain image; the swapchain owns it");
        return;
    }
    if (!texture->transient) {
        discharge(texture->category, texture->bytes);
    }
    retire(Retirement::Kind::Image, texture->image, texture->allocation);
    (void)textures_.destroy(handle);
}

bool VulkanDevice::is_valid(TextureHandle handle) const noexcept {
    return textures_.resolve(handle) != nullptr;
}

const TextureDescription* VulkanDevice::texture_description(TextureHandle handle) const noexcept {
    const VulkanTexture* texture = textures_.resolve(handle);
    return texture != nullptr ? &texture->desc : nullptr;
}

Expected<TextureViewHandle, Error> VulkanDevice::create_texture_view(
    const TextureViewDescription& desc) {
    VulkanTexture* texture = textures_.resolve(desc.texture);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "texture view: the texture handle is stale");
    }
    if (!texture->bound) {
        // A view needs bound memory. On the null backend this is reported; here it would be a
        // validation error and undefined behaviour, so it is refused before the driver sees it.
        report_validation(ValidationSeverity::Error,
                          "texture view created before its transient texture was bound");
        return fail(ErrorCode::Unavailable, "the texture has no memory bound yet");
    }

    ValidationMessage message;
    if (Status valid = validate_texture_view(desc, texture->desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    const SubresourceRange range =
        resolve_range(desc.range, texture->desc.mip_levels, texture->desc.array_layers);
    const Format format = desc.format == Format::Undefined ? texture->desc.format : desc.format;

    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = texture->image;
    info.viewType = to_vulkan_view_type(desc.dimension, range.layer_count);
    info.format = to_vulkan(format);
    info.subresourceRange.aspectMask = aspect_of(texture->desc.format);
    info.subresourceRange.baseMipLevel = range.base_mip;
    info.subresourceRange.levelCount = range.mip_count;
    info.subresourceRange.baseArrayLayer = range.base_layer;
    info.subresourceRange.layerCount = range.layer_count;

    VulkanTextureView view;
    view.texture = desc.texture;
    view.range = range;
    CY_VK_TRY(vkCreateImageView(device_, &info, nullptr, &view.view), "vkCreateImageView");

    Expected<TextureViewHandle, Error> handle = views_.create(view);
    if (!handle) {
        vkDestroyImageView(device_, view.view, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(view.view), VK_OBJECT_TYPE_IMAGE_VIEW, desc.name);
    return handle;
}

void VulkanDevice::destroy_texture_view(TextureViewHandle handle) noexcept {
    VulkanTextureView* view = views_.resolve(handle);
    if (view == nullptr) {
        return;
    }
    retire(Retirement::Kind::ImageView, view->view, nullptr);
    (void)views_.destroy(handle);
}

bool VulkanDevice::is_valid(TextureViewHandle handle) const noexcept {
    return views_.resolve(handle) != nullptr;
}

// --- Samplers and queries
// ---------------------------------------------------------------------------

Expected<SamplerHandle, Error> VulkanDevice::create_sampler(const SamplerDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_sampler(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = to_vulkan(desc.mag_filter);
    info.minFilter = to_vulkan(desc.min_filter);
    info.mipmapMode = to_vulkan(desc.mipmap_mode);
    info.addressModeU = to_vulkan(desc.address_u);
    info.addressModeV = to_vulkan(desc.address_v);
    info.addressModeW = to_vulkan(desc.address_w);
    info.mipLodBias = desc.mip_lod_bias;
    info.anisotropyEnable = desc.max_anisotropy > 1.0F ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = desc.max_anisotropy;
    info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
    // Reversed-Z again: a shadow comparison sampler compares GreaterEqual because near is 1.
    info.compareOp = to_vulkan(desc.compare_op);
    info.minLod = desc.min_lod;
    info.maxLod = desc.max_lod;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    VulkanSampler sampler;
    CY_VK_TRY(vkCreateSampler(device_, &info, nullptr, &sampler.sampler), "vkCreateSampler");
    Expected<SamplerHandle, Error> handle = samplers_.create(sampler);
    if (!handle) {
        vkDestroySampler(device_, sampler.sampler, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(sampler.sampler), VK_OBJECT_TYPE_SAMPLER, desc.name);
    return handle;
}

void VulkanDevice::destroy_sampler(SamplerHandle handle) noexcept {
    VulkanSampler* sampler = samplers_.resolve(handle);
    if (sampler == nullptr) {
        return;
    }
    retire(Retirement::Kind::Sampler, sampler->sampler, nullptr);
    (void)samplers_.destroy(handle);
}

Expected<QueryPoolHandle, Error> VulkanDevice::create_query_pool(const QueryPoolDescription& desc) {
    if (desc.count == 0) {
        return fail(ErrorCode::InvalidArgument, "a query pool of zero queries answers nothing");
    }
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryCount = desc.count;
    switch (desc.kind) {
        case QueryKind::Timestamp:
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            break;
        case QueryKind::Occlusion:
            info.queryType = VK_QUERY_TYPE_OCCLUSION;
            break;
        case QueryKind::PipelineStatistics:
            info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                      VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT;
            break;
    }

    VulkanQueryPool pool;
    pool.kind = desc.kind;
    pool.count = desc.count;
    CY_VK_TRY(vkCreateQueryPool(device_, &info, nullptr, &pool.pool), "vkCreateQueryPool");
    Expected<QueryPoolHandle, Error> handle = query_pools_.create(pool);
    if (!handle) {
        vkDestroyQueryPool(device_, pool.pool, nullptr);
    }
    return handle;
}

void VulkanDevice::destroy_query_pool(QueryPoolHandle handle) noexcept {
    VulkanQueryPool* pool = query_pools_.resolve(handle);
    if (pool == nullptr) {
        return;
    }
    vkDestroyQueryPool(device_, pool->pool, nullptr);
    (void)query_pools_.destroy(handle);
}

Expected<u32, Error> VulkanDevice::read_query_results(QueryPoolHandle pool, u32 first, u32 count,
                                                      Span<u64> out) {
    VulkanQueryPool* stored = query_pools_.resolve(pool);
    if (stored == nullptr) {
        return fail(ErrorCode::NotFound, "read_query_results(): stale query pool handle");
    }
    if (first + count > stored->count) {
        return fail(ErrorCode::OutOfRange, "read_query_results(): range outside the pool");
    }
    const u32 written = count < out.size() ? count : static_cast<u32>(out.size());
    if (written == 0) {
        return 0U;
    }
    const VkResult result = vkGetQueryPoolResults(device_, stored->pool, first, written,
                                                  static_cast<usize>(written) * sizeof(u64),
                                                  out.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        return fail(ErrorCode::Unavailable, "the queries have not resolved yet");
    }
    if (result != VK_SUCCESS) {
        return make_unexpected(error_from(result, "vkGetQueryPoolResults"));
    }
    if (stored->kind == QueryKind::Timestamp) {
        // Already in nanoseconds, so a caller never multiplies by a backend constant. That is what
        // makes a GPU timing comparable with the CPU timings on the shared trace.
        const u64 period = capabilities_.limits().timestamp_period_ns;
        for (u32 index = 0; index < written; ++index) {
            out[index] *= period != 0 ? period : 1;
        }
    }
    return written;
}

// --- Transients
// -----------------------------------------------------------------------------------------

Expected<TextureHandle, Error> VulkanDevice::create_transient_texture(
    const TextureDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_texture(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    // UNBOUND. The graph asks for the requirements, plans the whole frame's memory, reserves one
    // pool and only then binds — the order gotcha 6j names, and the order a view depends on.
    const VkImageCreateInfo info = image_info_for(desc);
    VulkanTexture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    texture.transient = true;
    texture.bound = false;
    texture.category = GpuMemoryCategory::Transient;
    texture.bytes = texture_byte_size(desc);
    CY_VK_TRY(vkCreateImage(device_, &info, nullptr, &texture.image), "vkCreateImage (transient)");

    Expected<TextureHandle, Error> handle = textures_.create(texture);
    if (!handle) {
        vkDestroyImage(device_, texture.image, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(texture.image), VK_OBJECT_TYPE_IMAGE,
                textures_.resolve(*handle)->name.text);
    if (Status pushed = live_transient_textures_.push_back(*handle); !pushed) {
        return make_unexpected(pushed.error());
    }
    return handle;
}

Expected<BufferHandle, Error> VulkanDevice::create_transient_buffer(const BufferDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_buffer(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = to_vulkan(desc.usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VulkanBuffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    buffer.transient = true;
    buffer.bound = false;
    buffer.category = GpuMemoryCategory::Transient;
    buffer.bytes = desc.size;
    CY_VK_TRY(vkCreateBuffer(device_, &info, nullptr, &buffer.buffer),
              "vkCreateBuffer (transient)");

    Expected<BufferHandle, Error> handle = buffers_.create(buffer);
    if (!handle) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        return handle;
    }
    if (Status pushed = live_transient_buffers_.push_back(*handle); !pushed) {
        return make_unexpected(pushed.error());
    }
    return handle;
}

Expected<MemoryRequirements, Error> VulkanDevice::texture_memory_requirements(
    TextureHandle handle) const {
    const VulkanTexture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "texture_memory_requirements(): stale handle");
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, texture->image, &requirements);
    return MemoryRequirements{requirements.size, requirements.alignment,
                              requirements.memoryTypeBits};
}

Expected<MemoryRequirements, Error> VulkanDevice::buffer_memory_requirements(
    BufferHandle handle) const {
    const VulkanBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "buffer_memory_requirements(): stale handle");
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer->buffer, &requirements);
    return MemoryRequirements{requirements.size, requirements.alignment,
                              requirements.memoryTypeBits};
}

Status VulkanDevice::reserve_transient_memory(u64 bytes, u32 memory_type_bits) {
    if (bytes == 0) {
        return ok();
    }
    if (bytes <= transient_bytes_ && (memory_type_bits & (1U << transient_memory_type_)) != 0) {
        // The pool is kept across frames, so a steady-state frame reserves nothing. That is what
        // makes the reported peak the plan's peak rather than the sum of every frame's plan.
        return ok();
    }

    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    u32 chosen = ~0U;
    for (u32 index = 0; index < properties.memoryTypeCount; ++index) {
        if ((memory_type_bits & (1U << index)) == 0) {
            continue;
        }
        if ((properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
            0) {
            chosen = index;
            break;
        }
    }
    if (chosen == ~0U) {
        return fail(ErrorCode::Unsupported,
                    "no device-local memory type satisfies every transient in the plan");
    }

    if (transient_memory_ != VK_NULL_HANDLE) {
        // Retired rather than freed: the previous frame may still be reading from it.
        (void)vkDeviceWaitIdle(device_);
        vkFreeMemory(device_, transient_memory_, nullptr);
        discharge(GpuMemoryCategory::Transient, transient_bytes_);
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = bytes;
    allocate.memoryTypeIndex = chosen;
    CY_VK_TRY(vkAllocateMemory(device_, &allocate, nullptr, &transient_memory_),
              "vkAllocateMemory (the render graph's transient pool)");
    transient_bytes_ = bytes;
    transient_memory_type_ = chosen;
    charge(GpuMemoryCategory::Transient, bytes);
    name_object(reinterpret_cast<u64>(transient_memory_), VK_OBJECT_TYPE_DEVICE_MEMORY,
                "cy.render-graph.transients");
    return ok();
}

Status VulkanDevice::bind_transient(TextureHandle handle, u64 offset) {
    VulkanTexture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "bind_transient(): stale texture handle");
    }
    if (!texture->transient) {
        return fail(ErrorCode::InvalidArgument,
                    "bind_transient() on a resource that is not graph-owned");
    }
    CY_VK_TRY(vkBindImageMemory(device_, texture->image, transient_memory_, offset),
              "vkBindImageMemory (transient)");
    texture->bound = true;
    return ok();
}

Status VulkanDevice::bind_transient(BufferHandle handle, u64 offset) {
    VulkanBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "bind_transient(): stale buffer handle");
    }
    if (!buffer->transient) {
        return fail(ErrorCode::InvalidArgument,
                    "bind_transient() on a resource that is not graph-owned");
    }
    CY_VK_TRY(vkBindBufferMemory(device_, buffer->buffer, transient_memory_, offset),
              "vkBindBufferMemory (transient)");
    buffer->bound = true;
    return ok();
}

void VulkanDevice::release_transient_resources() noexcept {
    for (const TextureHandle handle : live_transient_textures_) {
        VulkanTexture* texture = textures_.resolve(handle);
        if (texture != nullptr) {
            retire(Retirement::Kind::Image, texture->image, nullptr);
            (void)textures_.destroy(handle);
        }
    }
    for (const BufferHandle handle : live_transient_buffers_) {
        VulkanBuffer* buffer = buffers_.resolve(handle);
        if (buffer != nullptr) {
            retire(Retirement::Kind::Buffer, buffer->buffer, nullptr);
            (void)buffers_.destroy(handle);
        }
    }
    live_transient_textures_.clear();
    live_transient_buffers_.clear();
}

// --- Shaders, descriptors and pipelines
// ---------------------------------------------------------------

Expected<ShaderModuleHandle, Error> VulkanDevice::create_shader_module(
    const ShaderModuleDescription& desc) {
    if (desc.spirv.empty()) {
        return fail(ErrorCode::InvalidArgument, "shader module: no SPIR-V");
    }
    if (desc.spirv[0] != 0x07230203U) {
        return fail(ErrorCode::InvalidArgument,
                    "shader module: the first word is not SPIR-V's magic number");
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = desc.spirv.size() * sizeof(u32);
    info.pCode = desc.spirv.data();

    VulkanShaderModule module;
    module.stage = desc.stage;
    module.entry_point.assign(desc.entry_point);
    module.code_hash = hash_bytes(kFnvOffset, desc.spirv.data(), info.codeSize);
    CY_VK_TRY(vkCreateShaderModule(device_, &info, nullptr, &module.module),
              "vkCreateShaderModule");

    Expected<ShaderModuleHandle, Error> handle = shaders_.create(module);
    if (!handle) {
        vkDestroyShaderModule(device_, module.module, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(module.module), VK_OBJECT_TYPE_SHADER_MODULE, desc.name);
    return handle;
}

void VulkanDevice::destroy_shader_module(ShaderModuleHandle handle) noexcept {
    VulkanShaderModule* module = shaders_.resolve(handle);
    if (module == nullptr) {
        return;
    }
    retire(Retirement::Kind::ShaderModule, module->module, nullptr);
    (void)shaders_.destroy(handle);
}

Expected<DescriptorSetLayoutHandle, Error> VulkanDevice::create_descriptor_set_layout(
    const DescriptorSetLayoutDescription& desc) {
    Array<VkDescriptorSetLayoutBinding> bindings(*allocator_);
    Array<VkDescriptorBindingFlags> flags(*allocator_);
    bool any_flags = false;
    for (const DescriptorBinding& binding : desc.bindings) {
        if (binding.partially_bound && model_ != DescriptorModel::Bindless) {
            return fail(ErrorCode::Unsupported,
                        "a partially bound binding needs Capability::BindlessPartiallyBound; this "
                        "device is on the compatibility path");
        }
        VkDescriptorSetLayoutBinding out{};
        out.binding = binding.binding;
        out.descriptorType = to_vulkan(binding.kind);
        out.descriptorCount = binding.count;
        out.stageFlags = to_vulkan(binding.stages);
        if (Status pushed = bindings.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
        VkDescriptorBindingFlags binding_flags = 0;
        if (binding.partially_bound) {
            binding_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            any_flags = true;
        }
        if (Status pushed = flags.push_back(binding_flags); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{};
    flag_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flag_info.bindingCount = static_cast<u32>(flags.size());
    flag_info.pBindingFlags = flags.data();

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<u32>(bindings.size());
    info.pBindings = bindings.data();
    if (any_flags) {
        info.pNext = &flag_info;
    }

    VulkanDescriptorSetLayout layout;
    layout.binding_count = static_cast<u32>(bindings.size());
    CY_VK_TRY(vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout.layout),
              "vkCreateDescriptorSetLayout");
    Expected<DescriptorSetLayoutHandle, Error> handle = set_layouts_.create(layout);
    if (!handle) {
        vkDestroyDescriptorSetLayout(device_, layout.layout, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(layout.layout), VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                desc.name);
    return handle;
}

void VulkanDevice::destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept {
    VulkanDescriptorSetLayout* layout = set_layouts_.resolve(handle);
    if (layout == nullptr) {
        return;
    }
    vkDestroyDescriptorSetLayout(device_, layout->layout, nullptr);
    (void)set_layouts_.destroy(handle);
}

Expected<PipelineLayoutHandle, Error> VulkanDevice::create_pipeline_layout(
    const PipelineLayoutDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_pipeline_layout(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    Array<VkDescriptorSetLayout> layouts(*allocator_);
    for (const DescriptorSetLayoutHandle set : desc.set_layouts) {
        const VulkanDescriptorSetLayout* stored = set_layouts_.resolve(set);
        if (stored == nullptr) {
            return fail(ErrorCode::NotFound, "pipeline layout: a set layout handle is stale");
        }
        if (Status pushed = layouts.push_back(stored->layout); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    Array<VkPushConstantRange> ranges(*allocator_);
    for (const PushConstantRange& range : desc.push_constants) {
        VkPushConstantRange out{};
        out.stageFlags = to_vulkan(range.stages);
        out.offset = range.offset;
        out.size = range.size;
        if (Status pushed = ranges.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<u32>(layouts.size());
    info.pSetLayouts = layouts.data();
    info.pushConstantRangeCount = static_cast<u32>(ranges.size());
    info.pPushConstantRanges = ranges.data();

    VulkanPipelineLayout layout;
    CY_VK_TRY(vkCreatePipelineLayout(device_, &info, nullptr, &layout.layout),
              "vkCreatePipelineLayout");
    Expected<PipelineLayoutHandle, Error> handle = pipeline_layouts_.create(layout);
    if (!handle) {
        vkDestroyPipelineLayout(device_, layout.layout, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(layout.layout), VK_OBJECT_TYPE_PIPELINE_LAYOUT, desc.name);
    return handle;
}

void VulkanDevice::destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept {
    VulkanPipelineLayout* layout = pipeline_layouts_.resolve(handle);
    if (layout == nullptr) {
        return;
    }
    vkDestroyPipelineLayout(device_, layout->layout, nullptr);
    (void)pipeline_layouts_.destroy(handle);
}

Expected<DescriptorSetHandle, Error> VulkanDevice::allocate_descriptor_set(
    DescriptorSetLayoutHandle layout, bool per_frame) {
    const VulkanDescriptorSetLayout* stored = set_layouts_.resolve(layout);
    if (stored == nullptr) {
        return fail(ErrorCode::NotFound, "allocate_descriptor_set(): stale layout handle");
    }
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    // Per-frame sets come from the frame's pool and are freed wholesale when that frame comes round
    // again; a persistent set comes from the pool that is never reset. THE ARGUMENT IS READ HERE,
    // and it was not: every set came from the frame pool whatever the caller asked for, so a
    // persistent set was recycled after `frames_in_flight_` frames and every later bind named a
    // descriptor that no longer existed. The milestone's own artefact hit it from frame 3 —
    // 24 validation errors a frame — and no device suite noticed, because none renders three
    // frames. Getting this wrong is not a leak: it is a use-after-free the driver sees and the
    // application does not.
    info.descriptorPool =
        per_frame ? frames_[frame_slot_].descriptor_pool : persistent_descriptor_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &stored->layout;

    VulkanDescriptorSet set;
    set.per_frame = per_frame;
    set.frame_slot = frame_slot_;
    CY_VK_TRY(vkAllocateDescriptorSets(device_, &info, &set.set), "vkAllocateDescriptorSets");
    return descriptor_sets_.create(set);
}

Status VulkanDevice::update_descriptor_set(DescriptorSetHandle set,
                                           Span<const DescriptorWrite> writes) {
    VulkanDescriptorSet* stored = descriptor_sets_.resolve(set);
    if (stored == nullptr) {
        return fail(ErrorCode::NotFound, "update_descriptor_set(): stale set handle");
    }
    Array<VkWriteDescriptorSet> vulkan_writes(*allocator_);
    Array<VkDescriptorBufferInfo> buffer_infos(*allocator_);
    Array<VkDescriptorImageInfo> image_infos(*allocator_);
    if (Status reserved = buffer_infos.reserve(writes.size()); !reserved) {
        return reserved;
    }
    if (Status reserved = image_infos.reserve(writes.size()); !reserved) {
        return reserved;
    }

    for (const DescriptorWrite& write : writes) {
        VkWriteDescriptorSet out{};
        out.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        out.dstSet = stored->set;
        out.dstBinding = write.binding;
        out.dstArrayElement = write.array_index;
        out.descriptorCount = 1;
        out.descriptorType = to_vulkan(write.kind);

        if (!write.buffer.is_null()) {
            const VulkanBuffer* buffer = buffers_.resolve(write.buffer);
            if (buffer == nullptr) {
                return fail(ErrorCode::NotFound, "descriptor write names a stale buffer handle");
            }
            VkDescriptorBufferInfo info{};
            info.buffer = buffer->buffer;
            info.offset = write.buffer_offset;
            info.range = write.buffer_range == 0 ? VK_WHOLE_SIZE : write.buffer_range;
            if (Status pushed = buffer_infos.push_back(info); !pushed) {
                return pushed;
            }
            out.pBufferInfo = &buffer_infos[buffer_infos.size() - 1];
        } else {
            VkDescriptorImageInfo info{};
            if (!write.texture_view.is_null()) {
                const VulkanTextureView* view = views_.resolve(write.texture_view);
                if (view == nullptr) {
                    return fail(ErrorCode::NotFound,
                                "descriptor write names a stale texture view handle");
                }
                info.imageView = view->view;
            }
            if (!write.sampler.is_null()) {
                const VulkanSampler* sampler = samplers_.resolve(write.sampler);
                if (sampler == nullptr) {
                    return fail(ErrorCode::NotFound,
                                "descriptor write names a stale sampler handle");
                }
                info.sampler = sampler->sampler;
            }
            info.imageLayout = to_vulkan(write.layout);
            if (Status pushed = image_infos.push_back(info); !pushed) {
                return pushed;
            }
            out.pImageInfo = &image_infos[image_infos.size() - 1];
        }
        if (Status pushed = vulkan_writes.push_back(out); !pushed) {
            return pushed;
        }
    }
    if (!vulkan_writes.empty()) {
        vkUpdateDescriptorSets(device_, static_cast<u32>(vulkan_writes.size()),
                               vulkan_writes.data(), 0, nullptr);
    }
    return ok();
}

BindlessIndex VulkanDevice::bind_texture_globally(TextureViewHandle view,
                                                  SamplerHandle sampler) noexcept {
    if (model_ != DescriptorModel::Bindless) {
        return kInvalidBindlessIndex;
    }
    const VulkanTextureView* stored_view = views_.resolve(view);
    const VulkanSampler* stored_sampler = samplers_.resolve(sampler);
    if (stored_view == nullptr || stored_sampler == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "bind_texture_globally(): a stale view or sampler handle");
        return kInvalidBindlessIndex;
    }

    BindlessIndex index = kInvalidBindlessIndex;
    if (!bindless_free_.empty()) {
        index = bindless_free_[bindless_free_.size() - 1];
        bindless_free_.pop_back();
    } else if (bindless_next_ < kBindlessCapacity) {
        index = bindless_next_++;
    } else {
        report_validation(ValidationSeverity::Error, "the global descriptor table is full");
        return kInvalidBindlessIndex;
    }

    VkDescriptorImageInfo info{};
    info.imageView = stored_view->view;
    info.sampler = stored_sampler->sampler;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindless_set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return index;
}

void VulkanDevice::release_bindless_index(BindlessIndex index) noexcept {
    if (index == kInvalidBindlessIndex) {
        return;
    }
    (void)bindless_free_.push_back(index);
}

Expected<GraphicsPipelineHandle, Error> VulkanDevice::create_graphics_pipeline(
    const GraphicsPipelineDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_graphics_pipeline(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    const VulkanPipelineLayout* layout = pipeline_layouts_.resolve(desc.layout);
    const VulkanShaderModule* vertex = shaders_.resolve(desc.vertex_shader);
    if (layout == nullptr || vertex == nullptr) {
        return fail(ErrorCode::NotFound, "graphics pipeline: a stale layout or shader handle");
    }
    const VulkanShaderModule* fragment = shaders_.resolve(desc.fragment_shader);

    Array<VkSpecializationMapEntry> specialization_entries(*allocator_);
    Array<u32> specialization_values(*allocator_);
    for (const SpecializationConstant& constant : desc.specialization) {
        VkSpecializationMapEntry entry{};
        entry.constantID = constant.id;
        entry.offset = static_cast<u32>(specialization_values.size() * sizeof(u32));
        entry.size = sizeof(u32);
        if (Status pushed = specialization_entries.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = specialization_values.push_back(constant.value); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = static_cast<u32>(specialization_entries.size());
    specialization.pMapEntries = specialization_entries.data();
    specialization.dataSize = specialization_values.size() * sizeof(u32);
    specialization.pData = specialization_values.data();

    Array<VkPipelineShaderStageCreateInfo> stages(*allocator_);
    {
        // See image_info_for(): zero-initialising is Vulkan's own idiom.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vertex->module;
        stage.pName = vertex->entry_point.text;
        stage.pSpecializationInfo = specialization.mapEntryCount != 0 ? &specialization : nullptr;
        if (Status pushed = stages.push_back(stage); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (fragment != nullptr) {
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) — see above.
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = fragment->module;
        stage.pName = fragment->entry_point.text;
        stage.pSpecializationInfo = specialization.mapEntryCount != 0 ? &specialization : nullptr;
        if (Status pushed = stages.push_back(stage); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Array<VkVertexInputBindingDescription> vertex_bindings(*allocator_);
    for (const VertexBinding& binding : desc.vertex_bindings) {
        VkVertexInputBindingDescription out{};
        out.binding = binding.binding;
        out.stride = binding.stride;
        out.inputRate = binding.input_rate == VertexInputRate::PerInstance
                            ? VK_VERTEX_INPUT_RATE_INSTANCE
                            : VK_VERTEX_INPUT_RATE_VERTEX;
        if (Status pushed = vertex_bindings.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    Array<VkVertexInputAttributeDescription> vertex_attributes(*allocator_);
    for (const VertexAttribute& attribute : desc.vertex_attributes) {
        VkVertexInputAttributeDescription out{};
        out.location = attribute.location;
        out.binding = attribute.binding;
        out.format = to_vulkan(attribute.format);
        out.offset = attribute.offset;
        if (Status pushed = vertex_attributes.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size());
    vertex_input.pVertexBindingDescriptions = vertex_bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = to_vulkan(desc.topology);

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = to_vulkan(desc.rasterisation.polygon_mode);
    raster.cullMode = to_vulkan(desc.rasterisation.cull_mode);
    raster.frontFace = to_vulkan(desc.rasterisation.front_face);
    raster.depthClampEnable = desc.rasterisation.depth_clamp_enable ? VK_TRUE : VK_FALSE;
    raster.depthBiasEnable = (desc.rasterisation.depth_bias_constant != 0.0F ||
                              desc.rasterisation.depth_bias_slope != 0.0F)
                                 ? VK_TRUE
                                 : VK_FALSE;
    raster.depthBiasConstantFactor = desc.rasterisation.depth_bias_constant;
    raster.depthBiasSlopeFactor = desc.rasterisation.depth_bias_slope;
    raster.lineWidth = desc.rasterisation.line_width;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) — see above.
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sample_count);

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depth_stencil.depth_test_enable ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depth_stencil.depth_write_enable ? VK_TRUE : VK_FALSE;
    // GreaterOrEqual by default, which is reversed-Z's comparison. design.md §3: the depth buffer
    // is [0, 1], cleared to 0, compared GreaterEqual — and a scene looks right with the comparison
    // inverted until something intersects, which is why this is a default rather than a convention.
    depth.depthCompareOp = to_vulkan(desc.depth_stencil.depth_compare);
    depth.stencilTestEnable = desc.depth_stencil.stencil_test_enable ? VK_TRUE : VK_FALSE;
    depth.minDepthBounds = 0.0F;
    depth.maxDepthBounds = 1.0F;

    Array<VkPipelineColorBlendAttachmentState> blends(*allocator_);
    Array<VkFormat> colour_formats(*allocator_);
    for (const ColorAttachmentState& attachment : desc.color_attachments) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = attachment.blend_enable ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = to_vulkan(attachment.source_color);
        blend.dstColorBlendFactor = to_vulkan(attachment.destination_color);
        blend.colorBlendOp = to_vulkan(attachment.color_op);
        blend.srcAlphaBlendFactor = to_vulkan(attachment.source_alpha);
        blend.dstAlphaBlendFactor = to_vulkan(attachment.destination_alpha);
        blend.alphaBlendOp = to_vulkan(attachment.alpha_op);
        blend.colorWriteMask = to_vulkan(attachment.write_mask);
        if (Status pushed = blends.push_back(blend); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = colour_formats.push_back(to_vulkan(attachment.format)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = static_cast<u32>(blends.size());
    blend_state.pAttachments = blends.data();

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    // Dynamic rendering: no VkRenderPass and no VkFramebuffer to keep in step. Vulkan 1.3 is the
    // baseline precisely so that this is the only path.
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.viewMask = desc.view_mask;
    rendering.colorAttachmentCount = static_cast<u32>(colour_formats.size());
    rendering.pColorAttachmentFormats = colour_formats.data();
    rendering.depthAttachmentFormat = to_vulkan(desc.depth_stencil.format);
    if (format_info(desc.depth_stencil.format).has_stencil) {
        rendering.stencilAttachmentFormat = rendering.depthAttachmentFormat;
    }

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<u32>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = layout->layout;

    VulkanPipeline pipeline;
    pipeline.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pipeline.state_hash = hash_bytes(kFnvOffset, &vertex->code_hash, sizeof(vertex->code_hash));
    CY_VK_TRY(
        vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &pipeline.pipeline),
        "vkCreateGraphicsPipelines");
    ++stats_.pipeline_cache_misses;

    Expected<GraphicsPipelineHandle, Error> handle = graphics_pipelines_.create(pipeline);
    if (!handle) {
        vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(pipeline.pipeline), VK_OBJECT_TYPE_PIPELINE, desc.name);
    return handle;
}

void VulkanDevice::destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
    VulkanPipeline* pipeline = graphics_pipelines_.resolve(handle);
    if (pipeline == nullptr) {
        return;
    }
    retire(Retirement::Kind::Pipeline, pipeline->pipeline, nullptr);
    (void)graphics_pipelines_.destroy(handle);
}

Expected<ComputePipelineHandle, Error> VulkanDevice::create_compute_pipeline(
    const ComputePipelineDescription& desc) {
    const VulkanPipelineLayout* layout = pipeline_layouts_.resolve(desc.layout);
    const VulkanShaderModule* module = shaders_.resolve(desc.shader);
    if (layout == nullptr || module == nullptr) {
        return fail(ErrorCode::NotFound, "compute pipeline: a stale layout or shader handle");
    }

    Array<VkSpecializationMapEntry> entries(*allocator_);
    Array<u32> values(*allocator_);
    for (const SpecializationConstant& constant : desc.specialization) {
        VkSpecializationMapEntry entry{};
        entry.constantID = constant.id;
        entry.offset = static_cast<u32>(values.size() * sizeof(u32));
        entry.size = sizeof(u32);
        if (Status pushed = entries.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = values.push_back(constant.value); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = static_cast<u32>(entries.size());
    specialization.pMapEntries = entries.data();
    specialization.dataSize = values.size() * sizeof(u32);
    specialization.pData = values.data();

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) — see above.
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module->module;
    info.stage.pName = module->entry_point.text;
    info.stage.pSpecializationInfo = specialization.mapEntryCount != 0 ? &specialization : nullptr;
    info.layout = layout->layout;

    VulkanPipeline pipeline;
    pipeline.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    pipeline.state_hash = hash_bytes(kFnvOffset, &module->code_hash, sizeof(module->code_hash));
    CY_VK_TRY(
        vkCreateComputePipelines(device_, pipeline_cache_, 1, &info, nullptr, &pipeline.pipeline),
        "vkCreateComputePipelines");
    ++stats_.pipeline_cache_misses;

    Expected<ComputePipelineHandle, Error> handle = compute_pipelines_.create(pipeline);
    if (!handle) {
        vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
        return handle;
    }
    name_object(reinterpret_cast<u64>(pipeline.pipeline), VK_OBJECT_TYPE_PIPELINE, desc.name);
    return handle;
}

void VulkanDevice::destroy_compute_pipeline(ComputePipelineHandle handle) noexcept {
    VulkanPipeline* pipeline = compute_pipelines_.resolve(handle);
    if (pipeline == nullptr) {
        return;
    }
    retire(Retirement::Kind::Pipeline, pipeline->pipeline, nullptr);
    (void)compute_pipelines_.destroy(handle);
}

Expected<u64, Error> VulkanDevice::save_pipeline_cache(Span<u8> out) {
    // `rhi-and-render-graph`, "Cache invalidated by driver update": the cache header carries the
    // driver's own identity, so a driver change makes the blob unusable and the driver itself
    // rejects it. The engine does not have to hash the driver version into a key of its own.
    usize size = out.size();
    const VkResult result =
        vkGetPipelineCacheData(device_, pipeline_cache_, &size, out.empty() ? nullptr : out.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return make_unexpected(error_from(result, "vkGetPipelineCacheData"));
    }
    return static_cast<u64>(size);
}

Status VulkanDevice::load_pipeline_cache(Span<const u8> data) {
    if (data.empty()) {
        return ok();
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    // A blob from another driver is rejected by the driver itself rather than accepted and used.
    CY_VK_TRY(vkCreatePipelineCache(device_, &info, nullptr, &cache), "vkCreatePipelineCache");
    if (pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    }
    pipeline_cache_ = cache;
    ++stats_.pipeline_cache_hits;
    return ok();
}

// --- Frames and submission
// ---------------------------------------------------------------------------

Expected<u32, Error> VulkanDevice::begin_frame() {
    if (frame_open_) {
        return fail(ErrorCode::InvalidArgument, "begin_frame() while a frame is already open");
    }
    frame_slot_ = static_cast<u32>(frame_index_ % frames_in_flight_);
    FrameContext& frame = frames_[frame_slot_];

    // `rhi-and-render-graph`, "Frame pacing": when the CPU is frames_in_flight frames ahead it
    // waits on the oldest frame's fence before reusing that frame's pools. Expressed on the
    // timelines, because that is what the submissions signal.
    if (frame.used) {
        VkSemaphore semaphores[kQueueKindCount] = {};
        u64 values[kQueueKindCount] = {};
        u32 count = 0;
        for (u32 kind = 0; kind < kQueueKindCount; ++kind) {
            if (frame.timeline[kind] == 0) {
                continue;
            }
            semaphores[count] = timelines_[kind];
            values[count] = frame.timeline[kind];
            ++count;
        }
        if (count != 0) {
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = count;
            wait.pSemaphores = semaphores;
            wait.pValues = values;
            CY_VK_TRY(vkWaitSemaphores(device_, &wait, ~0ULL), "vkWaitSemaphores (frame pacing)");
        }
        for (const auto& per_queue : frame.command_pools) {
            for (VkCommandPool pool : per_queue) {
                if (pool != VK_NULL_HANDLE) {
                    CY_VK_TRY(vkResetCommandPool(device_, pool, 0), "vkResetCommandPool");
                }
            }
        }
        CY_VK_TRY(vkResetDescriptorPool(device_, frame.descriptor_pool, 0),
                  "vkResetDescriptorPool");
    }

    // The frame's command buffer handles are gone with the pool; free the slots so a stale handle
    // resolves to nothing rather than to whatever the pool hands out next.
    usize kept = 0;
    for (const CommandBufferHandle handle : live_command_buffers_) {
        const VulkanCommandBuffer* buffer = command_buffers_.resolve(handle);
        if (buffer != nullptr && buffer->frame_slot() == frame_slot_) {
            (void)command_buffers_.destroy(handle);
            continue;
        }
        live_command_buffers_[kept] = handle;
        ++kept;
    }
    while (live_command_buffers_.size() > kept) {
        live_command_buffers_.pop_back();
    }

    // Everything retired frames_in_flight frames ago can now be freed: every frame that could
    // reference it has completed.
    if (frame_index_ >= frames_in_flight_) {
        release_retirements(frame_index_ - frames_in_flight_);
    }

    frame.used = true;
    for (u64& value : frame.timeline) {
        value = 0;
    }
    frame_open_ = true;
    ++stats_.frames_begun;
    return frame_slot_;
}

Status VulkanDevice::end_frame() {
    if (!frame_open_) {
        return fail(ErrorCode::InvalidArgument, "end_frame() with no frame open");
    }
    frame_open_ = false;
    ++frame_index_;
    ++stats_.frames_completed;
    return ok();
}

namespace {

/// The recording slot this thread owns.
///
/// Assigned on the thread's first acquire and never reassigned: a command buffer must be recorded
/// on the thread that took it, and a slot that moved would put two threads on one pool. A thread
/// past kMaxRecordingThreads is reported rather than silently sharing, because sharing a pool is a
/// data race whose symptom is a corrupted command stream a week later.
u32 recording_slot() noexcept {
    static std::atomic<u32> next{0};
    thread_local const u32 slot = next.fetch_add(1, std::memory_order_relaxed);
    return slot;
}

}  // namespace

Expected<VkCommandPool, Error> VulkanDevice::pool_for_this_thread(QueueKind queue) noexcept {
    const u32 slot = recording_slot();
    if (slot >= kMaxRecordingThreads) {
        return fail(ErrorCode::OutOfRange,
                    "more threads have recorded command buffers than rhi::kMaxRecordingThreads "
                    "allows; a command pool may not be shared between threads");
    }
    VkCommandPool& pool = frames_[frame_slot_].command_pools[static_cast<u32>(queue)][slot];
    if (pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        create.queueFamilyIndex = queue_families_[static_cast<u32>(queue)];
        // TRANSIENT, and reset wholesale rather than per buffer: a frame's command buffers are
        // recycled together when that frame comes round again, which is the whole point of a pool
        // per frame.
        create.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        CY_VK_TRY(vkCreateCommandPool(device_, &create, nullptr, &pool), "vkCreateCommandPool");
    }
    return pool;
}

Expected<CommandBufferHandle, Error> VulkanDevice::acquire_command_buffer(QueueKind queue,
                                                                          bool secondary) {
    const auto queue_index = static_cast<u32>(queue);
    if (queue_index >= kQueueKindCount) {
        return fail(ErrorCode::InvalidArgument, "acquire_command_buffer(): no such queue");
    }
    // Called from job workers during parallel recording: the pool is this thread's, and the
    // bookkeeping around it is guarded.
    const std::lock_guard<std::mutex> lock(acquire_mutex_);
    Expected<VkCommandPool, Error> pool = pool_for_this_thread(queue);
    if (!pool) {
        return make_unexpected(pool.error());
    }

    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = *pool;
    info.level = secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    VkCommandBuffer commands = VK_NULL_HANDLE;
    CY_VK_TRY(vkAllocateCommandBuffers(device_, &info, &commands), "vkAllocateCommandBuffers");

    Expected<CommandBufferHandle, Error> handle =
        command_buffers_.create(this, commands, queue, secondary, frame_slot_);
    if (!handle) {
        return handle;
    }
    if (VulkanCommandBuffer* buffer = command_buffers_.resolve(*handle); buffer != nullptr) {
        buffer->set_handle(*handle);
    }
    if (Status pushed = live_command_buffers_.push_back(*handle); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++stats_.command_buffers_recorded;
    return handle;
}

CommandBuffer* VulkanDevice::command_buffer(CommandBufferHandle handle) noexcept {
    return command_buffers_.resolve(handle);
}

Status VulkanDevice::begin_command_buffer(CommandBufferHandle handle) {
    VulkanCommandBuffer* buffer = command_buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "begin_command_buffer(): stale handle");
    }
    VkCommandBufferInheritanceInfo inheritance{};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (buffer->secondary()) {
        // A secondary that begins its own dynamic-rendering scope inherits nothing, which is what
        // lets the render graph record a whole pass — its begin_rendering included — on a worker.
        info.pInheritanceInfo = &inheritance;
    }
    CY_VK_TRY(vkBeginCommandBuffer(buffer->raw(), &info), "vkBeginCommandBuffer");
    buffer->set_recording(true);
    return ok();
}

Status VulkanDevice::end_command_buffer(CommandBufferHandle handle) {
    VulkanCommandBuffer* buffer = command_buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "end_command_buffer(): stale handle");
    }
    CY_VK_TRY(vkEndCommandBuffer(buffer->raw()), "vkEndCommandBuffer");
    buffer->set_recording(false);
    return ok();
}

Status VulkanDevice::execute_secondary(CommandBufferHandle primary,
                                       Span<const CommandBufferHandle> secondaries) {
    VulkanCommandBuffer* parent = command_buffers_.resolve(primary);
    if (parent == nullptr || parent->secondary()) {
        return fail(ErrorCode::InvalidArgument,
                    "execute_secondary(): the first argument must be a primary command buffer");
    }
    Array<VkCommandBuffer> raw(*allocator_);
    for (const CommandBufferHandle handle : secondaries) {
        const VulkanCommandBuffer* child = command_buffers_.resolve(handle);
        if (child == nullptr || !child->secondary()) {
            return fail(ErrorCode::InvalidArgument,
                        "execute_secondary(): a named buffer is not a secondary");
        }
        if (Status pushed = raw.push_back(child->raw()); !pushed) {
            return pushed;
        }
    }
    if (!raw.empty()) {
        // In the order given, which is the graph's — never the order the workers happened to
        // finish in. That is what makes parallel recording deterministic.
        vkCmdExecuteCommands(parent->raw(), static_cast<u32>(raw.size()), raw.data());
    }
    return ok();
}

Expected<u64, Error> VulkanDevice::submit(const SubmitInfo& info) {
    const auto queue_index = static_cast<u32>(info.queue);
    if (queue_index >= kQueueKindCount) {
        return fail(ErrorCode::InvalidArgument, "submit(): queue outside the enumeration");
    }

    Array<VkCommandBufferSubmitInfo> commands(*allocator_);
    for (const CommandBufferHandle handle : info.command_buffers) {
        const VulkanCommandBuffer* buffer = command_buffers_.resolve(handle);
        if (buffer == nullptr) {
            return fail(ErrorCode::NotFound, "submit(): stale command buffer handle");
        }
        if (buffer->recording()) {
            return fail(ErrorCode::InvalidArgument,
                        "submit(): a command buffer is still recording");
        }
        VkCommandBufferSubmitInfo out{};
        out.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        out.commandBuffer = buffer->raw();
        if (Status pushed = commands.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Array<VkSemaphoreSubmitInfo> waits(*allocator_);
    for (const TimelineWait& wait : info.waits) {
        const auto index = static_cast<u32>(wait.queue);
        if (index >= kQueueKindCount) {
            return fail(ErrorCode::InvalidArgument, "submit(): wait names no queue");
        }
        VkSemaphoreSubmitInfo out{};
        out.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        out.semaphore = timelines_[index];
        out.value = wait.value;
        out.stageMask = to_vulkan(wait.stage);
        if (Status pushed = waits.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
        ++stats_.semaphore_waits;
    }
    if (!info.wait_binary.is_null()) {
        const VulkanSemaphore* semaphore = semaphores_.resolve(info.wait_binary);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "submit(): stale binary semaphore handle");
        }
        VkSemaphoreSubmitInfo out{};
        out.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        out.semaphore = semaphore->semaphore;
        out.stageMask = to_vulkan(info.wait_binary_stage);
        if (Status pushed = waits.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    const u64 signal_value = ++timeline_values_[queue_index];
    Array<VkSemaphoreSubmitInfo> signals(*allocator_);
    {
        VkSemaphoreSubmitInfo out{};
        out.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        out.semaphore = timelines_[queue_index];
        out.value = signal_value;
        out.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (Status pushed = signals.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (!info.signal_binary.is_null()) {
        const VulkanSemaphore* semaphore = semaphores_.resolve(info.signal_binary);
        if (semaphore == nullptr) {
            return fail(ErrorCode::NotFound, "submit(): stale binary semaphore handle");
        }
        VkSemaphoreSubmitInfo out{};
        out.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        out.semaphore = semaphore->semaphore;
        out.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (Status pushed = signals.push_back(out); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = static_cast<u32>(waits.size());
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = static_cast<u32>(commands.size());
    submit.pCommandBufferInfos = commands.data();
    submit.signalSemaphoreInfoCount = static_cast<u32>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();

    VkFence fence = VK_NULL_HANDLE;
    if (!info.signal_fence.is_null()) {
        const VulkanFence* stored = fences_.resolve(info.signal_fence);
        if (stored == nullptr) {
            return fail(ErrorCode::NotFound, "submit(): stale fence handle");
        }
        fence = stored->fence;
    }

    CY_VK_TRY(vkQueueSubmit2(queues_[queue_index], 1, &submit, fence), "vkQueueSubmit2");
    frames_[frame_slot_].timeline[queue_index] = signal_value;
    ++stats_.submissions;
    return signal_value;
}

u64 VulkanDevice::timeline_value(QueueKind queue) const noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount ? timeline_values_[index] : 0;
}

Status VulkanDevice::wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) {
    const auto index = static_cast<u32>(queue);
    if (index >= kQueueKindCount) {
        return fail(ErrorCode::InvalidArgument, "wait_timeline(): queue outside the enumeration");
    }
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &timelines_[index];
    wait.pValues = &value;
    CY_VK_TRY(vkWaitSemaphores(device_, &wait, timeout_ns == 0 ? ~0ULL : timeout_ns),
              "vkWaitSemaphores");
    return ok();
}

Status VulkanDevice::wait_idle() {
    CY_VK_TRY(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    return ok();
}

Expected<FenceHandle, Error> VulkanDevice::create_fence(bool signalled) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags =
        signalled ? VkFenceCreateFlags{VK_FENCE_CREATE_SIGNALED_BIT} : VkFenceCreateFlags{0};
    VulkanFence fence;
    CY_VK_TRY(vkCreateFence(device_, &info, nullptr, &fence.fence), "vkCreateFence");
    Expected<FenceHandle, Error> handle = fences_.create(fence);
    if (!handle) {
        vkDestroyFence(device_, fence.fence, nullptr);
    }
    return handle;
}

void VulkanDevice::destroy_fence(FenceHandle handle) noexcept {
    VulkanFence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return;
    }
    vkDestroyFence(device_, fence->fence, nullptr);
    (void)fences_.destroy(handle);
}

Status VulkanDevice::wait_fence(FenceHandle handle, u64 timeout_ns) {
    const VulkanFence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "wait_fence(): stale handle");
    }
    CY_VK_TRY(
        vkWaitForFences(device_, 1, &fence->fence, VK_TRUE, timeout_ns == 0 ? ~0ULL : timeout_ns),
        "vkWaitForFences");
    return ok();
}

Status VulkanDevice::reset_fence(FenceHandle handle) {
    const VulkanFence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "reset_fence(): stale handle");
    }
    CY_VK_TRY(vkResetFences(device_, 1, &fence->fence), "vkResetFences");
    return ok();
}

bool VulkanDevice::fence_signalled(FenceHandle handle) const noexcept {
    const VulkanFence* fence = fences_.resolve(handle);
    return fence != nullptr && vkGetFenceStatus(device_, fence->fence) == VK_SUCCESS;
}

Expected<SemaphoreHandle, Error> VulkanDevice::create_semaphore() {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VulkanSemaphore semaphore;
    CY_VK_TRY(vkCreateSemaphore(device_, &info, nullptr, &semaphore.semaphore),
              "vkCreateSemaphore");
    Expected<SemaphoreHandle, Error> handle = semaphores_.create(semaphore);
    if (!handle) {
        vkDestroySemaphore(device_, semaphore.semaphore, nullptr);
    }
    return handle;
}

void VulkanDevice::destroy_semaphore(SemaphoreHandle handle) noexcept {
    VulkanSemaphore* semaphore = semaphores_.resolve(handle);
    if (semaphore == nullptr) {
        return;
    }
    vkDestroySemaphore(device_, semaphore->semaphore, nullptr);
    (void)semaphores_.destroy(handle);
}

// --- Memory reporting
// ------------------------------------------------------------------------------------

GpuMemoryReport VulkanDevice::memory_report() const noexcept {
    GpuMemoryReport report;
    for (u32 index = 0; index < kGpuMemoryCategoryCount; ++index) {
        report.live_bytes[index] = live_bytes_[index];
        report.peak_bytes[index] = peak_bytes_[index];
    }
    report.allocation_count = allocation_count_;

    if (memory_budget_ && vma_ != nullptr) {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(vma_, budgets);
        for (u32 heap = 0; heap < properties.memoryHeapCount; ++heap) {
            if ((properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
                continue;
            }
            // The device-local heap, which is the figure the aliasing measurement is read against:
            // VK_EXT_memory_budget's heapUsage tracked M3's plan exactly, 64.00 -> 8.00 MiB.
            report.device_heap_size += properties.memoryHeaps[heap].size;
            report.device_heap_used += budgets[heap].usage;
            report.device_heap_budget += budgets[heap].budget;
        }
    }
    return report;
}

void VulkanDevice::publish_memory_pressure() noexcept {
    // GPU memory reaches the engine's own budget tree and the shared pressure monitor, not a second
    // GPU-specific report. `rhi-and-render-graph`: "GPU memory SHALL appear in the same domain and
    // budget model as CPU memory", and pressure "SHALL raise the engine's pressure level so that
    // streaming and residency systems respond through the same mechanism".
    const GpuMemoryReport report = memory_report();
    if (report.device_heap_budget != 0) {
        const f64 utilisation =
            static_cast<f64>(report.device_heap_used) / static_cast<f64>(report.device_heap_budget);
        PressureLevel platform = PressureLevel::Normal;
        if (utilisation >= 0.95) {
            platform = PressureLevel::Critical;
        } else if (utilisation >= 0.85) {
            platform = PressureLevel::Elevated;
        }
        // Folded in as a floor: the engine never reports less pressure than the device does.
        default_pressure_monitor().report_platform_level(platform);
    }
    (void)update_memory_pressure();
}

}  // namespace cy::rhi::vulkan
