// The null device: the whole RHI without a GPU. Task 2.1.2.
//
// Every creation call runs the same validation Vulkan runs, issues a real generational handle from
// a real pool, and accounts real bytes against a synthetic heap. Nothing executes. The two things
// that make this useful rather than decorative are the command log — a comparable record of what
// the render graph asked for — and the memory accounting, which lets the aliasing measurement have
// a number on a machine with no device.

#include "null_internal.h"

#include <cy/backends/rhi/backend.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/pressure.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace cy::rhi::null {
namespace {

/// FNV-1a. The command log's hash exists to compare two runs, not to resist anything, and a
/// dependency on cy::hash for that would drag the whole hashing module into the null backend.
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

u64 hash_text(u64 seed, const char* text) noexcept {
    if (text == nullptr) {
        return seed;
    }
    return hash_bytes(seed, text, std::strlen(text));
}

/// The synthetic alignment every null allocation is rounded to. 256 bytes is a plausible device
/// alignment; the exact number does not matter, but that there IS one does — a plan computed
/// against alignment 1 would place transients a real device would refuse.
constexpr u64 kNullAlignment = 256;
constexpr u32 kNullMemoryTypeBits = 0xFU;

u64 align_to(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

/// Every mip of every layer, tightly packed. Deterministic and independent of any device, which is
/// what the aliasing gate needs it to be.
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

/// The frames-in-flight the description asked for, bounded by what a fixed-size per-frame array can
/// hold. Zero means "the default" rather than "no frames".
u32 clamp_frames_in_flight(u32 requested) noexcept {
    if (requested == 0) {
        return kDefaultFramesInFlight;
    }
    return requested > kMaxFramesInFlight ? kMaxFramesInFlight : requested;
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

}  // namespace

void StoredName::assign(const char* source) noexcept {
    if (source == nullptr) {
        text[0] = '\0';
        return;
    }
    usize index = 0;
    while (index + 1 < sizeof(text) && source[index] != '\0') {
        text[index] = source[index];
        ++index;
    }
    text[index] = '\0';
}

// --- Construction -------------------------------------------------------------------------------

NullDevice::NullDevice(Allocator& allocator, const DeviceDescription& desc) noexcept
    : allocator_(&allocator),
      buffers_(MemoryDomain::Gpu, "rhi.null.buffers"),
      textures_(MemoryDomain::Gpu, "rhi.null.textures"),
      views_(MemoryDomain::Gpu, "rhi.null.views"),
      samplers_(MemoryDomain::Gpu, "rhi.null.samplers"),
      shaders_(MemoryDomain::Gpu, "rhi.null.shaders"),
      set_layouts_(MemoryDomain::Gpu, "rhi.null.set-layouts"),
      pipeline_layouts_(MemoryDomain::Gpu, "rhi.null.pipeline-layouts"),
      descriptor_sets_(MemoryDomain::Gpu, "rhi.null.descriptor-sets"),
      graphics_pipelines_(MemoryDomain::Gpu, "rhi.null.graphics-pipelines"),
      compute_pipelines_(MemoryDomain::Gpu, "rhi.null.compute-pipelines"),
      query_pools_(MemoryDomain::Gpu, "rhi.null.queries"),
      fences_(MemoryDomain::Gpu, "rhi.null.fences"),
      semaphores_(MemoryDomain::Gpu, "rhi.null.semaphores"),
      command_buffers_(MemoryDomain::Gpu, "rhi.null.command-buffers"),
      swapchains_(MemoryDomain::Gpu, "rhi.null.swapchains"),
      live_command_buffers_(allocator),
      live_transient_textures_(allocator),
      live_transient_buffers_(allocator),
      live_storage_buffers_(allocator),
      log_(allocator),
      bindless_free_(allocator),
      barriers_(this),
      validation_enabled_(desc.enable_validation) {
    frames_in_flight_ = clamp_frames_in_flight(desc.frames_in_flight);

    capabilities_.set_backend(BackendKind::Null);
    capabilities_.set_device_name("null device");
    capabilities_.set_driver_version("0.0.0");

    // WHAT THE NULL BACKEND CLAIMS, AND WHY IT MATTERS.
    //
    // It reports the capabilities the engine's *baseline* path needs and nothing beyond them. It
    // does NOT report async compute, and that is deliberate rather than lazy: a continuous
    // integration run then exercises the single-queue fold — the same declarations producing one
    // submit, no semaphores and no ownership transfers — which is the path most machines actually
    // run and the one a regression would otherwise hide in. A test that wants the multi-queue plan
    // asks the graph for it directly (RenderGraph::compile takes the queue configuration), rather
    // than needing a device that has one.
    capabilities_.set(Capability::ComputeShaders, true);
    capabilities_.set(Capability::DynamicRendering, true);
    capabilities_.set(Capability::Bindless, true);
    capabilities_.set(Capability::BindlessPartiallyBound, true);
    capabilities_.set(Capability::DescriptorIndexingNonUniform, true);
    capabilities_.set(Capability::TimestampQueries, true);
    capabilities_.set(Capability::Multiview, true);
    capabilities_.set(Capability::MemoryBudgetReporting, true);
    capabilities_.set(Capability::AsyncCompute, false);
    capabilities_.set(Capability::DedicatedTransferQueue, false);

    DeviceLimits& limits = capabilities_.limits();
    limits.max_bound_descriptor_sets = kMaxDescriptorSets;
    limits.max_push_constant_bytes = kMaxPushConstantBytes;
    limits.max_vertex_attributes = kMaxVertexAttributes;
    limits.max_color_attachments = kMaxColorAttachments;
    limits.max_texture_dimension_2d = 16384;
    limits.max_texture_array_layers = 2048;
    limits.max_compute_workgroup_size[0] = 1024;
    limits.max_compute_workgroup_size[1] = 1024;
    limits.max_compute_workgroup_size[2] = 64;
    limits.max_compute_workgroup_invocations = 1024;
    limits.subgroup_size = 32;
    limits.max_sampled_images_per_stage = 1U << 20;
    limits.max_storage_buffers_per_stage = 1U << 20;
    limits.min_uniform_buffer_offset_alignment = 256;
    limits.min_storage_buffer_offset_alignment = 64;
    limits.optimal_buffer_copy_offset_alignment = 64;
    limits.non_coherent_atom_size = 64;
    limits.max_sampler_anisotropy = 16.0F;
    limits.timestamp_period_ns = 1;

    // Every format the engine's table names is claimed as sampleable and, where it makes sense, as
    // an attachment. A capability query on the null backend has to answer *something*, and
    // answering "no" to everything would make continuous integration exercise the degraded paths
    // exclusively — which is the opposite of what a reference backend is for.
    for (u32 index = 1; index < static_cast<u32>(Format::Count); ++index) {
        const auto format = static_cast<Format>(index);
        FormatFeature features = FormatFeature::SampledImage | FormatFeature::BlitSource |
                                 FormatFeature::BlitDestination;
        if (format_is_depth_stencil(format)) {
            features = features | FormatFeature::DepthStencilAttachment;
        } else if (!format_info(format).is_compressed) {
            features = features | FormatFeature::ColorAttachment |
                       FormatFeature::ColorAttachmentBlend | FormatFeature::StorageImage;
        }
        capabilities_.set_format_features(format, features);
    }

    // One queue family, which is what "no dedicated async compute" means, and what makes the
    // single-queue fold produce zero ownership transfers rather than transfers between two
    // synthetic families that do not exist.
    for (u32& family : queue_families_) {
        family = 0;
    }

    memory_.device_heap_size = 8ULL * 1024 * 1024 * 1024;
    memory_.device_heap_budget = memory_.device_heap_size;
}

NullDevice::~NullDevice() noexcept {
    // A device that is destroyed while it still owns resources releases them. Only mapped buffers
    // hold heap outside the pools — every other Null* record is a value inside its HandlePool, and
    // the pool frees itself — so this is the whole of it. See `live_storage_buffers_`.
    for (const BufferHandle handle : live_storage_buffers_) {
        NullBuffer* buffer = buffers_.resolve(handle);
        if (buffer != nullptr && buffer->storage != nullptr) {
            allocator_->deallocate(buffer->storage, buffer->storage_bytes, kNullAlignment);
            buffer->storage = nullptr;
            buffer->storage_bytes = 0;
        }
    }
}

u32 NullDevice::queue_family(QueueKind queue) const noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount ? queue_families_[index] : 0;
}

bool NullDevice::has_queue(QueueKind queue) const noexcept {
    switch (queue) {
        case QueueKind::Graphics:
            return true;
        case QueueKind::AsyncCompute:
            return capabilities_.has(Capability::AsyncCompute);
        case QueueKind::Transfer:
            return capabilities_.has(Capability::DedicatedTransferQueue);
        case QueueKind::Count:
            break;
    }
    return false;
}

void NullDevice::set_validation_callback(ValidationCallback callback, void* user) noexcept {
    validation_callback_ = callback;
    validation_user_ = user;
}

void NullDevice::report_validation(ValidationSeverity severity, const char* message) noexcept {
    if (severity == ValidationSeverity::Error) {
        ++stats_.validation_errors;
    } else if (severity == ValidationSeverity::Warning) {
        ++stats_.validation_warnings;
    }
    if (validation_callback_ != nullptr) {
        validation_callback_(severity, message, validation_user_);
    }
}

void NullDevice::charge(GpuMemoryCategory category, u64 bytes) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kGpuMemoryCategoryCount) {
        return;
    }
    memory_.live_bytes[index] += bytes;
    memory_.peak_bytes[index] = std::max(memory_.peak_bytes[index], memory_.live_bytes[index]);
    ++memory_.allocation_count;
    memory_.device_heap_used += bytes;
}

void NullDevice::discharge(GpuMemoryCategory category, u64 bytes) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kGpuMemoryCategoryCount) {
        return;
    }
    memory_.live_bytes[index] =
        memory_.live_bytes[index] >= bytes ? memory_.live_bytes[index] - bytes : 0;
    memory_.device_heap_used =
        memory_.device_heap_used >= bytes ? memory_.device_heap_used - bytes : 0;
}

// --- Buffers ------------------------------------------------------------------------------------

Expected<BufferHandle, Error> NullDevice::create_buffer(const BufferDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_buffer(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    NullBuffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    buffer.category = category_for(desc.memory);

    // A host-visible buffer gets real storage, so that a caller writing through
    // buffer_mapped_pointer() writes somewhere and a readback reads its own bytes back. That is the
    // difference between "the null backend runs the frame" and "the null backend survives it".
    if (desc.memory != MemoryUse::DeviceLocal) {
        buffer.storage = allocator_->allocate(desc.size, kNullAlignment);
        if (buffer.storage == nullptr) {
            return fail(ErrorCode::OutOfMemory, "the null backend could not back a mapped buffer");
        }
        std::memset(buffer.storage, 0, desc.size);
        buffer.storage_bytes = desc.size;
    }

    Expected<BufferHandle, Error> handle = buffers_.create(buffer);
    if (!handle) {
        if (buffer.storage != nullptr) {
            allocator_->deallocate(buffer.storage, buffer.storage_bytes, kNullAlignment);
        }
        return handle;
    }
    if (buffer.storage != nullptr) {
        if (Status remembered = live_storage_buffers_.push_back(*handle); !remembered) {
            allocator_->deallocate(buffer.storage, buffer.storage_bytes, kNullAlignment);
            (void)buffers_.destroy(*handle);
            return make_unexpected(remembered.error());
        }
    }
    charge(buffer.category, align_to(desc.size, kNullAlignment));
    return handle;
}

void NullDevice::destroy_buffer(BufferHandle handle) noexcept {
    NullBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "destroy_buffer() on a stale or never-issued handle");
        return;
    }
    if (buffer->storage != nullptr) {
        allocator_->deallocate(buffer->storage, buffer->storage_bytes, kNullAlignment);
        buffer->storage = nullptr;
        forget_storage_buffer(handle);
    }
    if (!buffer->transient) {
        discharge(buffer->category, align_to(buffer->desc.size, kNullAlignment));
    }
    ++stats_.resources_freed;
    (void)buffers_.destroy(handle);
}

void NullDevice::forget_storage_buffer(BufferHandle handle) noexcept {
    for (usize index = 0; index < live_storage_buffers_.size(); ++index) {
        if (live_storage_buffers_[index].bits() != handle.bits()) {
            continue;
        }
        live_storage_buffers_[index] = live_storage_buffers_[live_storage_buffers_.size() - 1];
        live_storage_buffers_.pop_back();
        return;
    }
}

bool NullDevice::is_valid(BufferHandle handle) const noexcept {
    return buffers_.resolve(handle) != nullptr;
}

void* NullDevice::buffer_mapped_pointer(BufferHandle handle) noexcept {
    NullBuffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? buffer->storage : nullptr;
}

const BufferDescription* NullDevice::buffer_description(BufferHandle handle) const noexcept {
    const NullBuffer* buffer = buffers_.resolve(handle);
    return buffer != nullptr ? &buffer->desc : nullptr;
}

// --- Textures -----------------------------------------------------------------------------------

Expected<TextureHandle, Error> NullDevice::create_texture(const TextureDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_texture(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    NullTexture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    texture.byte_size = align_to(texture_byte_size(desc), kNullAlignment);
    texture.category = category_for(desc.memory);

    Expected<TextureHandle, Error> handle = textures_.create(texture);
    if (handle) {
        charge(texture.category, texture.byte_size);
    }
    return handle;
}

void NullDevice::destroy_texture(TextureHandle handle) noexcept {
    NullTexture* texture = textures_.resolve(handle);
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
        discharge(texture->category, texture->byte_size);
    }
    ++stats_.resources_freed;
    (void)textures_.destroy(handle);
}

bool NullDevice::is_valid(TextureHandle handle) const noexcept {
    return textures_.resolve(handle) != nullptr;
}

const TextureDescription* NullDevice::texture_description(TextureHandle handle) const noexcept {
    const NullTexture* texture = textures_.resolve(handle);
    return texture != nullptr ? &texture->desc : nullptr;
}

Expected<TextureViewHandle, Error> NullDevice::create_texture_view(
    const TextureViewDescription& desc) {
    const NullTexture* texture = textures_.resolve(desc.texture);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "texture view: the texture handle is stale");
    }
    // A view needs bound memory. On a device this is a hard error; here it is reported so that the
    // ordering mistake fails in continuous integration too rather than only on hardware.
    if (!texture->bound) {
        report_validation(ValidationSeverity::Error,
                          "texture view created before its transient texture was bound — "
                          "reserve the pool and bind before creating views");
        return fail(ErrorCode::Unavailable, "the texture has no memory bound yet");
    }

    ValidationMessage message;
    if (Status valid = validate_texture_view(desc, texture->desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }

    NullTextureView view;
    view.desc = desc;
    view.name.assign(desc.name);
    view.desc.name = view.name.text;
    view.resolved = resolve_range(desc.range, texture->desc.mip_levels, texture->desc.array_layers);
    return views_.create(view);
}

void NullDevice::destroy_texture_view(TextureViewHandle handle) noexcept {
    (void)views_.destroy(handle);
}

bool NullDevice::is_valid(TextureViewHandle handle) const noexcept {
    return views_.resolve(handle) != nullptr;
}

// --- Samplers and queries
// -------------------------------------------------------------------------

Expected<SamplerHandle, Error> NullDevice::create_sampler(const SamplerDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_sampler(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    NullSampler sampler;
    sampler.desc = desc;
    sampler.name.assign(desc.name);
    sampler.desc.name = sampler.name.text;
    return samplers_.create(sampler);
}

void NullDevice::destroy_sampler(SamplerHandle handle) noexcept {
    (void)samplers_.destroy(handle);
}

Expected<QueryPoolHandle, Error> NullDevice::create_query_pool(const QueryPoolDescription& desc) {
    if (desc.count == 0) {
        return fail(ErrorCode::InvalidArgument, "a query pool of zero queries answers nothing");
    }
    NullQueryPool pool;
    pool.name.assign(desc.name);
    pool.kind = desc.kind;
    pool.count = desc.count;
    return query_pools_.create(pool);
}

void NullDevice::destroy_query_pool(QueryPoolHandle handle) noexcept {
    (void)query_pools_.destroy(handle);
}

Expected<u32, Error> NullDevice::read_query_results(QueryPoolHandle pool, u32 first, u32 count,
                                                    Span<u64> out) {
    const NullQueryPool* stored = query_pools_.resolve(pool);
    if (stored == nullptr) {
        return fail(ErrorCode::NotFound, "read_query_results(): stale query pool handle");
    }
    if (first + count > stored->count) {
        return fail(ErrorCode::OutOfRange, "read_query_results(): range outside the pool");
    }
    const u32 written = count < out.size() ? count : static_cast<u32>(out.size());
    for (u32 index = 0; index < written; ++index) {
        out[index] = 0;  // no GPU ran, so every timestamp is zero rather than a fabricated number
    }
    return written;
}

// --- Transients
// -----------------------------------------------------------------------------------

Expected<TextureHandle, Error> NullDevice::create_transient_texture(
    const TextureDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_texture(desc, capabilities_.limits(), message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    NullTexture texture;
    texture.desc = desc;
    texture.name.assign(desc.name);
    texture.desc.name = texture.name.text;
    texture.byte_size = align_to(texture_byte_size(desc), kNullAlignment);
    texture.transient = true;
    texture.bound = false;  // unbound until the graph places it — the ordering gotcha 6j names
    texture.category = GpuMemoryCategory::Transient;

    Expected<TextureHandle, Error> handle = textures_.create(texture);
    if (!handle) {
        return handle;
    }
    if (Status pushed = live_transient_textures_.push_back(*handle); !pushed) {
        (void)textures_.destroy(*handle);
        return make_unexpected(pushed.error());
    }
    return handle;
}

Expected<BufferHandle, Error> NullDevice::create_transient_buffer(const BufferDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_buffer(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    NullBuffer buffer;
    buffer.desc = desc;
    buffer.name.assign(desc.name);
    buffer.desc.name = buffer.name.text;
    buffer.transient = true;
    buffer.bound = false;
    buffer.category = GpuMemoryCategory::Transient;

    Expected<BufferHandle, Error> handle = buffers_.create(buffer);
    if (!handle) {
        return handle;
    }
    if (Status pushed = live_transient_buffers_.push_back(*handle); !pushed) {
        (void)buffers_.destroy(*handle);
        return make_unexpected(pushed.error());
    }
    return handle;
}

Expected<MemoryRequirements, Error> NullDevice::texture_memory_requirements(
    TextureHandle handle) const {
    const NullTexture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "texture_memory_requirements(): stale handle");
    }
    return MemoryRequirements{texture->byte_size, kNullAlignment, kNullMemoryTypeBits};
}

Expected<MemoryRequirements, Error> NullDevice::buffer_memory_requirements(
    BufferHandle handle) const {
    const NullBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "buffer_memory_requirements(): stale handle");
    }
    return MemoryRequirements{align_to(buffer->desc.size, kNullAlignment), kNullAlignment,
                              kNullMemoryTypeBits};
}

Status NullDevice::reserve_transient_memory(u64 bytes, u32 memory_type_bits) {
    if (bytes != 0 && (memory_type_bits & kNullMemoryTypeBits) == 0) {
        return fail(ErrorCode::Unsupported, "no memory type satisfies every transient in the plan");
    }
    if (bytes > transient_bytes_) {
        // Only the growth is charged, because the pool is kept across frames: a steady-state frame
        // reserves nothing, which is what makes the reported peak the plan's peak rather than the
        // sum of every frame's plan.
        charge(GpuMemoryCategory::Transient, bytes - transient_bytes_);
        transient_bytes_ = bytes;
        transient_high_water_ = std::max(transient_high_water_, bytes);
    }
    return ok();
}

Status NullDevice::bind_transient(TextureHandle handle, u64 offset) {
    NullTexture* texture = textures_.resolve(handle);
    if (texture == nullptr) {
        return fail(ErrorCode::NotFound, "bind_transient(): stale texture handle");
    }
    if (!texture->transient) {
        return fail(ErrorCode::InvalidArgument,
                    "bind_transient() on a resource that is not graph-owned");
    }
    if (offset + texture->byte_size > transient_bytes_) {
        return fail(ErrorCode::OutOfRange,
                    "bind_transient(): the placement reaches past the reserved pool");
    }
    texture->transient_offset = offset;
    texture->bound = true;
    return ok();
}

Status NullDevice::bind_transient(BufferHandle handle, u64 offset) {
    NullBuffer* buffer = buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "bind_transient(): stale buffer handle");
    }
    if (!buffer->transient) {
        return fail(ErrorCode::InvalidArgument,
                    "bind_transient() on a resource that is not graph-owned");
    }
    if (offset + align_to(buffer->desc.size, kNullAlignment) > transient_bytes_) {
        return fail(ErrorCode::OutOfRange,
                    "bind_transient(): the placement reaches past the reserved pool");
    }
    buffer->transient_offset = offset;
    buffer->bound = true;
    return ok();
}

void NullDevice::release_transient_resources() noexcept {
    for (TextureHandle handle : live_transient_textures_) {
        (void)textures_.destroy(handle);
    }
    for (BufferHandle handle : live_transient_buffers_) {
        NullBuffer* buffer = buffers_.resolve(handle);
        if (buffer != nullptr && buffer->storage != nullptr) {
            allocator_->deallocate(buffer->storage, buffer->storage_bytes, kNullAlignment);
        }
        (void)buffers_.destroy(handle);
    }
    live_transient_textures_.clear();
    live_transient_buffers_.clear();
}

// --- Shaders, descriptors, pipelines
// ---------------------------------------------------------------

Expected<ShaderModuleHandle, Error> NullDevice::create_shader_module(
    const ShaderModuleDescription& desc) {
    if (desc.spirv.empty()) {
        return fail(ErrorCode::InvalidArgument, "shader module: no SPIR-V");
    }
    // 0x07230203 is SPIR-V's magic number. Checking it here means a Slang output that never made it
    // through the back end is rejected in continuous integration rather than on the one machine
    // with a GPU.
    if (desc.spirv[0] != 0x07230203U) {
        return fail(ErrorCode::InvalidArgument,
                    "shader module: the first word is not SPIR-V's magic number");
    }
    NullShaderModule module;
    module.name.assign(desc.name);
    module.stage = desc.stage;
    module.word_count = desc.spirv.size();
    module.code_hash = hash_bytes(kFnvOffset, desc.spirv.data(), desc.spirv.size() * sizeof(u32));
    return shaders_.create(module);
}

void NullDevice::destroy_shader_module(ShaderModuleHandle handle) noexcept {
    (void)shaders_.destroy(handle);
}

Expected<DescriptorSetLayoutHandle, Error> NullDevice::create_descriptor_set_layout(
    const DescriptorSetLayoutDescription& desc) {
    NullDescriptorSetLayout layout;
    layout.name.assign(desc.name);
    layout.binding_count = static_cast<u32>(desc.bindings.size());
    for (const DescriptorBinding& binding : desc.bindings) {
        if (binding.count == 0) {
            layout.has_runtime_array = true;
        }
        if (binding.partially_bound && !capabilities_.has(Capability::BindlessPartiallyBound)) {
            return fail(ErrorCode::Unsupported,
                        "a partially bound binding needs Capability::BindlessPartiallyBound");
        }
    }
    return set_layouts_.create(layout);
}

void NullDevice::destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept {
    (void)set_layouts_.destroy(handle);
}

Expected<PipelineLayoutHandle, Error> NullDevice::create_pipeline_layout(
    const PipelineLayoutDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_pipeline_layout(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    NullPipelineLayout layout;
    layout.name.assign(desc.name);
    layout.set_count = static_cast<u32>(desc.set_layouts.size());
    for (const PushConstantRange& range : desc.push_constants) {
        const u32 end = range.offset + range.size;
        layout.push_constant_bytes =
            end > layout.push_constant_bytes ? end : layout.push_constant_bytes;
    }
    return pipeline_layouts_.create(layout);
}

void NullDevice::destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept {
    (void)pipeline_layouts_.destroy(handle);
}

Expected<DescriptorSetHandle, Error> NullDevice::allocate_descriptor_set(
    DescriptorSetLayoutHandle layout, bool per_frame) {
    if (set_layouts_.resolve(layout) == nullptr) {
        return fail(ErrorCode::NotFound, "allocate_descriptor_set(): stale layout handle");
    }
    NullDescriptorSet set;
    set.layout = layout;
    set.per_frame = per_frame;
    set.frame_slot = frame_slot_;
    return descriptor_sets_.create(set);
}

Status NullDevice::update_descriptor_set(DescriptorSetHandle set,
                                         Span<const DescriptorWrite> writes) {
    NullDescriptorSet* stored = descriptor_sets_.resolve(set);
    if (stored == nullptr) {
        return fail(ErrorCode::NotFound, "update_descriptor_set(): stale set handle");
    }
    for (const DescriptorWrite& write : writes) {
        // Every handle a write names is validated, because a descriptor written with a stale handle
        // is the failure mode this whole handle model exists to catch, and on a real device it is a
        // silent read of whatever took the slot.
        if (!write.buffer.is_null() && !is_valid(write.buffer)) {
            return fail(ErrorCode::NotFound, "descriptor write names a stale buffer handle");
        }
        if (!write.texture_view.is_null() && !is_valid(write.texture_view)) {
            return fail(ErrorCode::NotFound, "descriptor write names a stale texture view handle");
        }
        if (!write.sampler.is_null() && samplers_.resolve(write.sampler) == nullptr) {
            return fail(ErrorCode::NotFound, "descriptor write names a stale sampler handle");
        }
    }
    stored->write_count += static_cast<u32>(writes.size());
    return ok();
}

BindlessIndex NullDevice::bind_texture_globally(TextureViewHandle view,
                                                SamplerHandle sampler) noexcept {
    if (views_.resolve(view) == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "bind_texture_globally(): stale texture view handle");
        return kInvalidBindlessIndex;
    }
    if (!sampler.is_null() && samplers_.resolve(sampler) == nullptr) {
        report_validation(ValidationSeverity::Error,
                          "bind_texture_globally(): stale sampler handle");
        return kInvalidBindlessIndex;
    }
    if (!bindless_free_.empty()) {
        const BindlessIndex index = bindless_free_[bindless_free_.size() - 1];
        bindless_free_.pop_back();
        return index;
    }
    return bindless_next_++;
}

void NullDevice::release_bindless_index(BindlessIndex index) noexcept {
    if (index == kInvalidBindlessIndex) {
        return;
    }
    (void)bindless_free_.push_back(index);
}

Expected<GraphicsPipelineHandle, Error> NullDevice::create_graphics_pipeline(
    const GraphicsPipelineDescription& desc) {
    ValidationMessage message;
    if (Status valid = validate_graphics_pipeline(desc, message); !valid) {
        report_validation(ValidationSeverity::Error, message.text);
        return make_unexpected(valid.error());
    }
    if (pipeline_layouts_.resolve(desc.layout) == nullptr) {
        return fail(ErrorCode::NotFound, "graphics pipeline: stale pipeline layout handle");
    }
    if (shaders_.resolve(desc.vertex_shader) == nullptr) {
        return fail(ErrorCode::NotFound, "graphics pipeline: stale vertex shader handle");
    }

    NullPipeline pipeline;
    pipeline.name.assign(desc.name);
    // The cache key is a hash of the full state, which is the specification's requirement and is
    // what makes a warm start a hit. The state that matters here is the shader contents and the
    // fixed-function description; the handles themselves deliberately do not participate, because
    // two identical pipelines created from two handles must be one cache entry.
    u64 hash = kFnvOffset;
    if (const NullShaderModule* vertex = shaders_.resolve(desc.vertex_shader); vertex != nullptr) {
        hash = hash_bytes(hash, &vertex->code_hash, sizeof(vertex->code_hash));
    }
    if (const NullShaderModule* fragment = shaders_.resolve(desc.fragment_shader);
        fragment != nullptr) {
        hash = hash_bytes(hash, &fragment->code_hash, sizeof(fragment->code_hash));
    }
    hash = hash_bytes(hash, &desc.rasterisation, sizeof(desc.rasterisation));
    hash = hash_bytes(hash, &desc.depth_stencil, sizeof(desc.depth_stencil));
    hash = hash_bytes(hash, &desc.topology, sizeof(desc.topology));
    for (const ColorAttachmentState& attachment : desc.color_attachments) {
        hash = hash_bytes(hash, &attachment, sizeof(attachment));
    }
    for (const VertexAttribute& attribute : desc.vertex_attributes) {
        hash = hash_bytes(hash, &attribute, sizeof(attribute));
    }
    for (const SpecializationConstant& constant : desc.specialization) {
        hash = hash_bytes(hash, &constant, sizeof(constant));
    }
    pipeline.state_hash = hash;
    ++stats_.pipeline_cache_misses;
    return graphics_pipelines_.create(pipeline);
}

void NullDevice::destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
    (void)graphics_pipelines_.destroy(handle);
}

Expected<ComputePipelineHandle, Error> NullDevice::create_compute_pipeline(
    const ComputePipelineDescription& desc) {
    if (pipeline_layouts_.resolve(desc.layout) == nullptr) {
        return fail(ErrorCode::NotFound, "compute pipeline: stale pipeline layout handle");
    }
    const NullShaderModule* module = shaders_.resolve(desc.shader);
    if (module == nullptr) {
        return fail(ErrorCode::NotFound, "compute pipeline: stale shader module handle");
    }
    NullPipeline pipeline;
    pipeline.name.assign(desc.name);
    pipeline.compute = true;
    u64 hash = hash_bytes(kFnvOffset, &module->code_hash, sizeof(module->code_hash));
    for (const SpecializationConstant& constant : desc.specialization) {
        hash = hash_bytes(hash, &constant, sizeof(constant));
    }
    pipeline.state_hash = hash;
    ++stats_.pipeline_cache_misses;
    return compute_pipelines_.create(pipeline);
}

void NullDevice::destroy_compute_pipeline(ComputePipelineHandle handle) noexcept {
    (void)compute_pipelines_.destroy(handle);
}

Expected<u64, Error> NullDevice::save_pipeline_cache(Span<u8> out) {
    // There is nothing to persist: no pipeline was compiled. Reporting zero bytes rather than
    // failing is what lets a host write the same "save the cache on shutdown" code on both
    // backends, and a zero-byte cache loads back as an empty one.
    (void)out;
    return 0ULL;
}

Status NullDevice::load_pipeline_cache(Span<const u8> data) {
    (void)data;
    return ok();
}

// --- Frames
// ----------------------------------------------------------------------------------------

Expected<u32, Error> NullDevice::begin_frame() {
    if (frame_open_) {
        return fail(ErrorCode::InvalidArgument, "begin_frame() while a frame is already open");
    }
    frame_slot_ = static_cast<u32>(frame_index_ % frames_in_flight_);
    frame_open_ = true;
    ++stats_.frames_begun;

    // Recycle the pools of the frame this slot last held. On a device this waits on that frame's
    // fence first; here there is nothing to wait for, and the recycling itself is what the test of
    // "a resource destroyed in frame N survives until frame N completes" observes.
    // In place: only elements at or before the read position are written, and Array's iterator is
    // a pointer, so compacting while iterating is well defined.
    usize kept = 0;
    for (const CommandBufferHandle handle : live_command_buffers_) {
        NullCommandBuffer* buffer = command_buffers_.resolve(handle);
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
    return frame_slot_;
}

Status NullDevice::end_frame() {
    if (!frame_open_) {
        return fail(ErrorCode::InvalidArgument, "end_frame() with no frame open");
    }
    frame_open_ = false;
    ++frame_index_;
    ++stats_.frames_completed;
    return ok();
}

Expected<CommandBufferHandle, Error> NullDevice::acquire_command_buffer(QueueKind queue,
                                                                        bool secondary) {
    // Callable from a job worker: the render graph acquires each secondary on the thread that will
    // record it, because a backend's command allocator is externally synchronised. The null backend
    // has no allocator to protect, but it holds the same contract so that code exercised here
    // behaves the same way on a device.
    const std::lock_guard<std::mutex> lock(acquire_mutex_);
    Expected<CommandBufferHandle, Error> handle =
        command_buffers_.create(this, *allocator_, queue, secondary, frame_slot_);
    if (!handle) {
        return handle;
    }
    if (NullCommandBuffer* buffer = command_buffers_.resolve(*handle); buffer != nullptr) {
        buffer->set_handle(*handle);
    }
    if (Status pushed = live_command_buffers_.push_back(*handle); !pushed) {
        (void)command_buffers_.destroy(*handle);
        return make_unexpected(pushed.error());
    }
    ++stats_.command_buffers_recorded;
    return handle;
}

CommandBuffer* NullDevice::command_buffer(CommandBufferHandle handle) noexcept {
    return command_buffers_.resolve(handle);
}

Status NullDevice::begin_command_buffer(CommandBufferHandle handle) {
    NullCommandBuffer* buffer = command_buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "begin_command_buffer(): stale handle");
    }
    if (buffer->recording()) {
        return fail(ErrorCode::InvalidArgument, "begin_command_buffer(): already recording");
    }
    buffer->set_recording(true);
    return ok();
}

Status NullDevice::end_command_buffer(CommandBufferHandle handle) {
    NullCommandBuffer* buffer = command_buffers_.resolve(handle);
    if (buffer == nullptr) {
        return fail(ErrorCode::NotFound, "end_command_buffer(): stale handle");
    }
    if (!buffer->recording()) {
        return fail(ErrorCode::InvalidArgument, "end_command_buffer(): not recording");
    }
    buffer->set_recording(false);
    return ok();
}

Status NullDevice::execute_secondary(CommandBufferHandle primary,
                                     Span<const CommandBufferHandle> secondaries) {
    NullCommandBuffer* parent = command_buffers_.resolve(primary);
    if (parent == nullptr || parent->secondary()) {
        return fail(ErrorCode::InvalidArgument,
                    "execute_secondary(): the first argument must be a recording primary buffer");
    }
    for (CommandBufferHandle handle : secondaries) {
        NullCommandBuffer* child = command_buffers_.resolve(handle);
        if (child == nullptr || !child->secondary()) {
            return fail(ErrorCode::InvalidArgument,
                        "execute_secondary(): a named buffer is not a secondary");
        }
        // The record carries the number of commands spliced in, not the secondary's HANDLE. A
        // handle is a synthetic identity assigned in whatever order the workers asked for one, and
        // putting it in the stream would make the stream depend on thread scheduling — which is the
        // property `rhi-and-render-graph` requires it not to have.
        RecordedCommand command;
        command.kind = CommandKind::ExecuteSecondary;
        command.a = static_cast<u32>(child->log().size());
        parent->append(command);
        // And the secondary's own recording, spliced in where it executes. That is what makes the
        // stream depend on the plan rather than on which worker finished first.
        child->take_log_into(parent->mutable_log());
        // And its counts, on this thread. See RecordedCounts: the secondary was recorded on a job
        // worker, so this is where its draws and dispatches join a single-threaded total.
        parent->add_counts(child->take_counts());
    }
    return ok();
}

Expected<u64, Error> NullDevice::submit(const SubmitInfo& info) {
    const auto queue_index = static_cast<u32>(info.queue);
    if (queue_index >= kQueueKindCount) {
        return fail(ErrorCode::InvalidArgument, "submit(): queue outside the enumeration");
    }
    for (CommandBufferHandle handle : info.command_buffers) {
        const NullCommandBuffer* buffer = command_buffers_.resolve(handle);
        if (buffer == nullptr) {
            return fail(ErrorCode::NotFound, "submit(): stale command buffer handle");
        }
        if (buffer->recording()) {
            return fail(ErrorCode::InvalidArgument,
                        "submit(): a command buffer is still recording");
        }
    }
    for (const TimelineWait& wait : info.waits) {
        const auto index = static_cast<u32>(wait.queue);
        if (index >= kQueueKindCount) {
            return fail(ErrorCode::InvalidArgument, "submit(): wait names no queue");
        }
        // Nothing executes, so a wait is satisfied the moment the value has been signalled. A wait
        // on a value that was never signalled is a defect in the plan and is reported as one.
        if (wait.value > timelines_[index]) {
            report_validation(ValidationSeverity::Error,
                              "submit(): waits on a timeline value that was never signalled");
            return fail(ErrorCode::InvalidArgument,
                        "submit(): waits on a timeline value that was never signalled");
        }
        ++stats_.semaphore_waits;
    }

    if (!info.signal_fence.is_null()) {
        if (NullFence* fence = fences_.resolve(info.signal_fence); fence != nullptr) {
            fence->signalled = true;
        }
    }
    if (!info.signal_binary.is_null() && semaphores_.resolve(info.signal_binary) == nullptr) {
        return fail(ErrorCode::NotFound, "submit(): stale binary semaphore handle");
    }

    // The device's stream is assembled here, in submission order, from command buffers that were
    // each recorded by exactly one thread.
    for (const CommandBufferHandle handle : info.command_buffers) {
        if (NullCommandBuffer* buffer = command_buffers_.resolve(handle); buffer != nullptr) {
            absorb(*buffer);
        }
    }

    ++stats_.submissions;
    ++timelines_[queue_index];
    return timelines_[queue_index];
}

u64 NullDevice::timeline_value(QueueKind queue) const noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount ? timelines_[index] : 0;
}

Status NullDevice::wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) {
    (void)timeout_ns;
    const auto index = static_cast<u32>(queue);
    if (index >= kQueueKindCount) {
        return fail(ErrorCode::InvalidArgument, "wait_timeline(): queue outside the enumeration");
    }
    return value <= timelines_[index]
               ? ok()
               : fail(ErrorCode::Timeout, "wait_timeline(): value was never signalled");
}

Status NullDevice::wait_idle() {
    return ok();
}

Expected<FenceHandle, Error> NullDevice::create_fence(bool signalled) {
    return fences_.create(NullFence{signalled});
}

void NullDevice::destroy_fence(FenceHandle handle) noexcept {
    (void)fences_.destroy(handle);
}

Status NullDevice::wait_fence(FenceHandle handle, u64 timeout_ns) {
    (void)timeout_ns;
    const NullFence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "wait_fence(): stale handle");
    }
    return ok();
}

Status NullDevice::reset_fence(FenceHandle handle) {
    NullFence* fence = fences_.resolve(handle);
    if (fence == nullptr) {
        return fail(ErrorCode::NotFound, "reset_fence(): stale handle");
    }
    fence->signalled = false;
    return ok();
}

bool NullDevice::fence_signalled(FenceHandle handle) const noexcept {
    const NullFence* fence = fences_.resolve(handle);
    return fence != nullptr && fence->signalled;
}

Expected<SemaphoreHandle, Error> NullDevice::create_semaphore() {
    return semaphores_.create(NullSemaphore{});
}

void NullDevice::destroy_semaphore(SemaphoreHandle handle) noexcept {
    (void)semaphores_.destroy(handle);
}

// --- Swapchain
// -------------------------------------------------------------------------------------

Expected<SwapchainHandle, Error> NullDevice::create_swapchain(const SwapchainDescription& desc) {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        return fail(ErrorCode::InvalidArgument, "swapchain: a zero extent presents nothing");
    }
    const u32 image_count = desc.min_image_count == 0 ? 3 : desc.min_image_count;

    NullSwapchain swapchain(*allocator_);
    swapchain.name.assign(desc.name);
    swapchain.info.format =
        desc.preferred_format == Format::Undefined ? Format::Bgra8Srgb : desc.preferred_format;
    swapchain.info.present_mode = desc.present_mode;
    swapchain.info.extent = desc.extent;
    swapchain.info.image_count = image_count;

    for (u32 index = 0; index < image_count; ++index) {
        TextureDescription image;
        image.name = "swapchain image";
        image.format = swapchain.info.format;
        image.extent = Extent3D{desc.extent.width, desc.extent.height, 1};
        image.usage = TextureUsage::ColorAttachment | TextureUsage::TransferDestination;
        NullTexture texture;
        texture.desc = image;
        texture.name.assign(image.name);
        texture.desc.name = texture.name.text;
        texture.byte_size = align_to(texture_byte_size(image), kNullAlignment);
        texture.owned_by_swapchain = true;

        Expected<TextureHandle, Error> handle = textures_.create(texture);
        if (!handle) {
            return make_unexpected(handle.error());
        }
        if (Status pushed = swapchain.textures.push_back(*handle); !pushed) {
            return make_unexpected(pushed.error());
        }

        NullTextureView view;
        view.desc.name = "swapchain view";
        view.desc.texture = *handle;
        view.desc.format = swapchain.info.format;
        view.name.assign(view.desc.name);
        view.desc.name = view.name.text;
        view.resolved = SubresourceRange{0, 1, 0, 1};
        Expected<TextureViewHandle, Error> view_handle = views_.create(view);
        if (!view_handle) {
            return make_unexpected(view_handle.error());
        }
        if (Status pushed = swapchain.views.push_back(*view_handle); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return swapchains_.create(std::move(swapchain));
}

void NullDevice::destroy_swapchain(SwapchainHandle handle) noexcept {
    NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return;
    }
    for (TextureViewHandle view : swapchain->views) {
        (void)views_.destroy(view);
    }
    for (TextureHandle texture : swapchain->textures) {
        (void)textures_.destroy(texture);
    }
    (void)swapchains_.destroy(handle);
}

Status NullDevice::resize_swapchain(SwapchainHandle handle, Extent2D extent) {
    NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return fail(ErrorCode::NotFound, "resize_swapchain(): stale handle");
    }
    if (extent.width == 0 || extent.height == 0) {
        return fail(ErrorCode::InvalidArgument, "resize_swapchain(): a zero extent");
    }
    swapchain->info.extent = extent;
    for (TextureHandle texture : swapchain->textures) {
        if (NullTexture* stored = textures_.resolve(texture); stored != nullptr) {
            stored->desc.extent = Extent3D{extent.width, extent.height, 1};
            stored->byte_size = align_to(texture_byte_size(stored->desc), kNullAlignment);
        }
    }
    return ok();
}

SwapchainInfo NullDevice::swapchain_info(SwapchainHandle handle) const noexcept {
    const NullSwapchain* swapchain = swapchains_.resolve(handle);
    return swapchain != nullptr ? swapchain->info : SwapchainInfo{};
}

Expected<u32, Error> NullDevice::acquire_next_image(SwapchainHandle handle, SemaphoreHandle signal,
                                                    u64 timeout_ns) {
    (void)timeout_ns;
    NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return fail(ErrorCode::NotFound, "acquire_next_image(): stale swapchain handle");
    }
    if (!signal.is_null() && semaphores_.resolve(signal) == nullptr) {
        return fail(ErrorCode::NotFound, "acquire_next_image(): stale semaphore handle");
    }
    const u32 index = swapchain->next_image;
    swapchain->next_image = (swapchain->next_image + 1) % swapchain->info.image_count;
    return index;
}

TextureHandle NullDevice::swapchain_texture(SwapchainHandle handle, u32 index) const noexcept {
    const NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr || index >= swapchain->textures.size()) {
        return TextureHandle{};
    }
    return swapchain->textures[index];
}

TextureViewHandle NullDevice::swapchain_view(SwapchainHandle handle, u32 index) const noexcept {
    const NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr || index >= swapchain->views.size()) {
        return TextureViewHandle{};
    }
    return swapchain->views[index];
}

Status NullDevice::present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) {
    const NullSwapchain* swapchain = swapchains_.resolve(handle);
    if (swapchain == nullptr) {
        return fail(ErrorCode::NotFound, "present(): stale swapchain handle");
    }
    if (image_index >= swapchain->info.image_count) {
        return fail(ErrorCode::OutOfRange, "present(): image index outside the swapchain");
    }
    if (!wait.is_null() && semaphores_.resolve(wait) == nullptr) {
        return fail(ErrorCode::NotFound, "present(): stale semaphore handle");
    }
    return ok();
}

// --- Reporting -----------------------------------------------------------------------------------

void NullDevice::publish_memory_pressure() noexcept {
    u64 total = 0;
    for (const u64 bytes : memory_.live_bytes) {
        total += bytes;
    }
    // The engine's own domain tree, not a second GPU-specific report. `rhi-and-render-graph`:
    // "GPU memory SHALL appear in the same domain and budget model as CPU memory."
    memory_.device_heap_used = total;
    (void)update_memory_pressure();
}

BarrierRecorder& NullDevice::barrier_recorder(const GraphBarrierKey& key) noexcept {
    (void)key;
    return barriers_;
}

/// Fold one command buffer's recording into the device's stream, in submission order.
void NullDevice::absorb(NullCommandBuffer& buffer) noexcept {
    const usize first = log_.size();
    buffer.take_log_into(log_);
    const RecordedCounts counts = buffer.take_counts();
    stats_.draws += counts.draws;
    stats_.dispatches += counts.dispatches;
    stats_.validation_errors += counts.validation_errors;
    for (usize index = first; index < log_.size(); ++index) {
        const RecordedCommand& command = log_[index];
        log_hash_ = hash_bytes(log_hash_ == 0 ? kFnvOffset : log_hash_, &command.kind,
                               sizeof(command.kind));
        log_hash_ = hash_bytes(log_hash_, &command.a, sizeof(command.a));
        log_hash_ = hash_bytes(log_hash_, &command.b, sizeof(command.b));
        log_hash_ = hash_bytes(log_hash_, &command.c, sizeof(command.c));
        log_hash_ = hash_bytes(log_hash_, &command.d, sizeof(command.d));
        log_hash_ = hash_text(log_hash_, command.label);
    }
}

Span<const RecordedCommand> NullDevice::log() const noexcept {
    return {log_.data(), log_.size()};
}

void NullDevice::clear_log() noexcept {
    log_.clear();
    log_hash_ = 0;
}

// --- The module's public surface
// --------------------------------------------------------------------

namespace {

/// The concrete device, from a Device the registry or a caller handed back. Safe because this
/// module is the only thing that creates one, and `is_null_device()` is how a caller checks before
/// asking. -fno-rtti is in force, which is why it is a static_cast with a precondition rather than
/// a dynamic_cast.
NullDevice* concrete(Device* device) noexcept {
    CY_ASSERT_MSG(device == nullptr || device->capabilities().backend() == BackendKind::Null,
                  "a null-backend accessor was given a device from another backend");
    // -fno-rtti: the assertion above is the precondition dynamic_cast would have checked.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    return static_cast<NullDevice*>(device);
}

const NullDevice* concrete(const Device* device) noexcept {
    CY_ASSERT_MSG(device == nullptr || device->capabilities().backend() == BackendKind::Null,
                  "a null-backend accessor was given a device from another backend");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast) — see above.
    return static_cast<const NullDevice*>(device);
}

void destroy_null_device_entry(Allocator& allocator, Device* device) noexcept {
    destroy_null_device(allocator, device);
}

/// Registers the backend when this translation unit is part of the link.
///
/// A static library only pulls in a translation unit some symbol references, so this initialiser
/// runs when something in this file is used — which is the ordinary case, because a host that links
/// cy::rhi-null does so in order to create a device. A host that wants the registration to be a
/// statement rather than a link-order property calls register_null_backend() itself; both are
/// supported and the registration is idempotent by name.
[[maybe_unused]] const Status kNullBackendRegistered = register_null_backend();

}  // namespace

Status register_null_backend() noexcept {
    BackendRegistration registration;
    registration.name = kNullBackendName;
    registration.kind = BackendKind::Null;
    registration.create = &create_null_device;
    registration.destroy = &destroy_null_device_entry;
    registration.is_available = nullptr;  // always
    return register_backend(registration);
}

Expected<Device*, Error> create_null_device(Allocator& allocator,
                                            const DeviceDescription& desc) noexcept {
    void* storage = allocator.allocate(sizeof(NullDevice), alignof(NullDevice));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for a null device");
    }
    return static_cast<Device*>(::new (storage) NullDevice(allocator, desc));
}

void destroy_null_device(Allocator& allocator, Device* device) noexcept {
    NullDevice* null_device = concrete(device);
    if (null_device == nullptr) {
        return;
    }
    null_device->~NullDevice();
    allocator.deallocate(null_device, sizeof(NullDevice), alignof(NullDevice));
}

bool is_null_device(const Device& device) noexcept {
    return device.capabilities().backend() == BackendKind::Null;
}

Span<const RecordedCommand> command_log(const Device& device) noexcept {
    const NullDevice* null_device = concrete(&device);
    return null_device != nullptr ? null_device->log() : Span<const RecordedCommand>{};
}

u64 command_log_hash(const Device& device) noexcept {
    const NullDevice* null_device = concrete(&device);
    return null_device != nullptr ? null_device->log_hash() : 0;
}

void clear_command_log(Device& device) noexcept {
    if (NullDevice* null_device = concrete(&device); null_device != nullptr) {
        null_device->clear_log();
    }
}

}  // namespace cy::rhi::null
