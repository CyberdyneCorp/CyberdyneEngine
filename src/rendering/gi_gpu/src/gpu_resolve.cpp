// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_resolve.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/graph/executor.h>

#include <algorithm>
#include <cstring>

#include "gi_gpu_msl.h"
#include "gi_gpu_spirv.h"

namespace cy::rendering::gi_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

/// The resolve's constant words, as gi_resolve_common.slang numbers them (uint4 index * 4).
constexpr u32 kWordCounts = 0 * 4;
constexpr u32 kWordCamera = 1 * 4;
constexpr u32 kWordRays = 2 * 4;
constexpr u32 kWordFar = 3 * 4;
constexpr u32 kWordReflect = 4 * 4;
constexpr u32 kWordIntensity = 5 * 4;
constexpr u32 kWordWorldToClip = 6 * 4;
constexpr u32 kWordPreviousToClip = 10 * 4;
constexpr u32 kWordUnpack = 14 * 4;
constexpr u32 kWordViewToRelative = 15 * 4;
constexpr u32 kWordFrame = 18 * 4;
constexpr u32 kWordJitter = 19 * 4;
constexpr u32 kWordSignals = 20 * 4;
constexpr u32 kWordInvalid = 26 * 4;
constexpr u32 kWordFull = 36 * 4;

constexpr u64 kVec4 = 16;
constexpr u32 kSignals = 2;
/// `[numthreads(64, 1, 1)]` on the one-dimensional entry points, 8 x 8 on the two image ones.
constexpr u32 kLinearGroup = 64;
constexpr u32 kTileGroup = 8;

/// The descriptor bindings of each set, in its `ParameterBlock`'s order.
constexpr u32 kSurfaceBindings = 5;
constexpr u32 kResolveBindings = 3;
constexpr u32 kDenoiseBindings = 8;
constexpr u32 kPublishBindings = 7;

/// The push block of the denoiser's entry points: `GiDenoisePush`.
struct DenoisePush {
    u32 count = 0;
    u32 pass = 0;
    u32 direction = 0;
    u32 reserved = 0;
};

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

void put_matrix(u32* words, u32 at, const Mat4& m) noexcept {
    for (u32 column = 0; column < 4; ++column) {
        words[at + (column * 4U) + 0] = bits(m.columns[column].x);
        words[at + (column * 4U) + 1] = bits(m.columns[column].y);
        words[at + (column * 4U) + 2] = bits(m.columns[column].z);
        words[at + (column * 4U) + 3] = bits(m.columns[column].w);
    }
}

[[nodiscard]] u32 linear_groups(u32 count) noexcept {
    const u32 needed = (count + kLinearGroup - 1U) / kLinearGroup;
    return needed == 0 ? 1 : needed;
}

[[nodiscard]] u32 tile_groups(u32 extent) noexcept {
    return (extent + kTileGroup - 1U) / kTileGroup;
}

[[nodiscard]] rhi::DescriptorWrite buffer_write(u32 binding, rhi::BufferHandle buffer) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = buffer;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite sampled_write(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite storage_write(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::Storage;
    return write;
}

[[nodiscard]] f64 stamp_ms(const u64* stamps, u32 from, u32 to) noexcept {
    return stamps[to] >= stamps[from] ? static_cast<f64>(stamps[to] - stamps[from]) / 1.0e6 : 0.0;
}

}  // namespace

u32 gi_admission(gi::GiMode mode) noexcept {
    // "Admitted" is "the mode's exclusion mask for that surface leaves the dynamic diffuse sources
    // in". The radiance cache stands for all of them: `exclusion_for` excludes the five together.
    const u32 dynamic = gi::source_bit(gi::RadianceSource::RadianceCache);
    u32 admitted = 0;
    if ((gi::exclusion_for(mode, false, false) & dynamic) == 0) {
        admitted |= kGiAdmitPlain;
    }
    if ((gi::exclusion_for(mode, true, false) & dynamic) == 0) {
        admitted |= kGiAdmitLightmapped;
    }
    if ((gi::exclusion_for(mode, false, true) & dynamic) == 0) {
        admitted |= kGiAdmitWithVolume;
    }
    return admitted;
}

void write_gi_control(u32 diffuse_slot, u32 specular_slot, gi::GiMode mode, u32 out[4]) noexcept {
    out[0] = diffuse_slot;
    out[1] = specular_slot;
    out[2] = gi_admission(mode);
    out[3] = 0;
}

GpuScreenResolve::~GpuScreenResolve() {
    destroy();
}

bool GpuScreenResolve::supported(const rhi::Device& device) noexcept {
    return GpuGiScene::supported(device);
}

// --- Creation ------------------------------------------------------------------------------------

Status GpuScreenResolve::create(Allocator& allocator, rhi::Device& device, GpuRadianceCache& cache,
                                const GpuScreenResolveDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device resolve is already created");
    }
    if (cache.device_ != &device || cache.scene_ == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the device resolve reads a radiance cache created on the same device; create "
                    "the cache first");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument, "GpuScreenResolveDescription: a view has pixels");
    }
    allocator_ = &allocator;
    device_ = &device;
    cache_ = &cache;
    desc_ = desc;
    hardware_pipeline_ = cache.hardware_pipelines_;
    Status made = create_layouts();
    if (made) {
        made = create_pipelines();
    }
    if (made) {
        made = create_resources();
    }
    if (made && device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "gi resolve timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = kStamps;
        Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer);
        if (pool) {
            timer_ = *pool;
        }
    }
    if (!made) {
        destroy();
        return made;
    }
    std::memset(words_, 0, sizeof(words_));
    divisor_ = 0;
    (void)set_divisor(1);
    set_settings(gi::ScreenResolveSettings{});
    set_denoiser(denoise::Denoiser{});
    return ok();
}

Status GpuScreenResolve::create_layouts() noexcept {
    struct LayoutShape {
        const char* name;
        u32 count;
        /// The bindings that are textures, as a mask, and which of those are storage images.
        u32 textures;
        u32 storage_textures;
    };
    const LayoutShape shapes[kLayoutCount] = {
        {"gi surface set", kSurfaceBindings, 0b0110U, 0U},
        {"gi resolve set", kResolveBindings, 0U, 0U},
        {"gi denoise set", kDenoiseBindings, 0U, 0U},
        {"gi publish set", kPublishBindings, 0b1100000U, 0b1100000U},
    };
    for (u32 which = 0; which < kLayoutCount; ++which) {
        const LayoutShape& shape = shapes[which];
        rhi::DescriptorBinding bindings[kDenoiseBindings] = {};
        for (u32 index = 0; index < shape.count; ++index) {
            const u32 bit = 1U << index;
            bindings[index].binding = index;
            bindings[index].kind = (shape.textures & bit) == 0U ? rhi::DescriptorKind::StorageBuffer
                                   : (shape.storage_textures & bit) != 0U
                                       ? rhi::DescriptorKind::StorageTexture
                                       : rhi::DescriptorKind::SampledTexture;
            bindings[index].count = 1;
            bindings[index].stages = rhi::ShaderStage::Compute;
        }
        rhi::DescriptorSetLayoutDescription description;
        description.name = shape.name;
        description.bindings = Span<const rhi::DescriptorBinding>(bindings, shape.count);
        Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
            device_->create_descriptor_set_layout(description);
        if (!layout) {
            return make_unexpected(layout.error());
        }
        layouts_[which] = *layout;
    }

    const auto make = [this](const char* name, Span<const rhi::DescriptorSetLayoutHandle> sets,
                             u32 push_bytes,
                             rhi::PipelineLayoutHandle& out) noexcept -> Status {
        const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, push_bytes};
        rhi::PipelineLayoutDescription description;
        description.name = name;
        description.set_layouts = sets;
        if (push_bytes != 0) {
            description.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        }
        Expected<rhi::PipelineLayoutHandle, Error> layout =
            device_->create_pipeline_layout(description);
        if (!layout) {
            return make_unexpected(layout.error());
        }
        out = *layout;
        return ok();
    };
    const u32 dispatch_bytes = static_cast<u32>(sizeof(GpuGiScene::Dispatch));
    const u32 denoise_bytes = static_cast<u32>(sizeof(DenoisePush));
    if (Status made = make("gi surface layout", {&layouts_[kSurfaceLayout], 1}, 0,
                           surface_pipeline_layout_);
        !made) {
        return made;
    }
    if (Status made = make("gi denoise layout", {&layouts_[kDenoiseLayout], 1}, denoise_bytes,
                           denoise_pipeline_layout_);
        !made) {
        return made;
    }
    if (Status made = make("gi publish layout", {&layouts_[kPublishLayout], 1}, 0,
                           publish_pipeline_layout_);
        !made) {
        return made;
    }
    const rhi::DescriptorSetLayoutHandle software[3] = {cache_->scene_->set_layout_,
                                                        cache_->probe_layout_,
                                                        layouts_[kResolveLayout]};
    if (Status made =
            make("gi resolve layout", {software, 3}, dispatch_bytes, resolve_pipeline_layout_);
        !made) {
        return made;
    }
    if (!hardware_pipeline_) {
        return ok();
    }
    const rhi::DescriptorSetLayoutHandle hardware[4] = {
        cache_->scene_->set_layout_, cache_->probe_layout_, layouts_[kResolveLayout],
        cache_->hardware_layout_};
    return make("gi resolve hardware layout", {hardware, 4}, dispatch_bytes,
                resolve_hardware_pipeline_layout_);
}

Status GpuScreenResolve::create_pipelines() noexcept {
    struct Source {
        const char* name;
        Span<const u32> spirv;
        const char* msl;
        usize msl_size;
        const char* entry;
        u32 group_x;
        u32 group_y;
    };
#define CY_GI_RESOLVE_SOURCE(label, stem, entry, x, y)                               \
    Source {                                                                         \
        label, {k##stem##Spirv, sizeof(k##stem##Spirv) / sizeof(u32)}, k##stem##Msl, \
            sizeof(k##stem##Msl) - 1, entry, x, y                                    \
    }
    const Source sources[kPipelineCount] = {
        CY_GI_RESOLVE_SOURCE("gi unpack surfaces", GiUnpackSurfaces, "cyGiUnpackSurfaces",
                             kTileGroup, kTileGroup),
        CY_GI_RESOLVE_SOURCE("gi reproject", GiReproject, "cyGiReproject", kLinearGroup, 1),
        CY_GI_RESOLVE_SOURCE("gi denoise accumulate", GiDenoiseAccumulate,
                             "cyGiDenoiseAccumulate", kLinearGroup, 1),
        CY_GI_RESOLVE_SOURCE("gi denoise variance", GiDenoiseVariance, "cyGiDenoiseVariance",
                             kLinearGroup, 1),
        CY_GI_RESOLVE_SOURCE("gi denoise filter", GiDenoiseFilter, "cyGiDenoiseFilter",
                             kLinearGroup, 1),
        CY_GI_RESOLVE_SOURCE("gi publish", GiPublish, "cyGiPublish", kTileGroup, kTileGroup),
        CY_GI_RESOLVE_SOURCE("gi resolve", GiResolve, "cyGiResolve", kLinearGroup, 1),
        CY_GI_RESOLVE_SOURCE("gi resolve hardware", GiResolveHardware, "cyGiResolveHardware",
                             kLinearGroup, 1),
    };
#undef CY_GI_RESOLVE_SOURCE
    const rhi::PipelineLayoutHandle layouts[kPipelineCount] = {
        surface_pipeline_layout_, denoise_pipeline_layout_,  denoise_pipeline_layout_,
        denoise_pipeline_layout_, denoise_pipeline_layout_,  publish_pipeline_layout_,
        resolve_pipeline_layout_, resolve_hardware_pipeline_layout_,
    };
    // The ray query pipeline exists only where the device has ray queries, as stage 3's do.
    const u32 count = hardware_pipeline_ ? static_cast<u32>(kPipelineCount)
                                         : static_cast<u32>(kResolveHardware);
    for (u32 which = 0; which < count; ++which) {
        const Source& source = sources[which];
        rhi::ShaderModuleBundle bundle;
        bundle.spirv = source.spirv;
        bundle.msl = {reinterpret_cast<const u8*>(source.msl), source.msl_size};
        bundle.spirv_entry_point = "main";
        bundle.msl_entry_point = source.entry;
        rhi::ValidationMessage message;
        auto module =
            rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                      source.name, rhi::ShaderStage::Compute, message);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
        if (!shader.has_value()) {
            return make_unexpected(shader.error());
        }
        shaders_[which] = *shader;
        rhi::ComputePipelineDescription pipeline;
        pipeline.name = source.name;
        pipeline.layout = layouts[which];
        pipeline.shader = shaders_[which];
        pipeline.workgroup_size[0] = source.group_x;
        pipeline.workgroup_size[1] = source.group_y;
        Expected<rhi::ComputePipelineHandle, Error> created =
            device_->create_compute_pipeline(pipeline);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipelines_[which] = *created;
    }
    return ok();
}

Status GpuScreenResolve::create_resources() noexcept {
    const u64 pixels = static_cast<u64>(desc_.width) * desc_.height;
    for (u32 index = 0; index < rhi::kMaxFramesInFlight; ++index) {
        rhi::BufferDescription description;
        description.name = "gi resolve constants";
        description.size = sizeof(words_);
        description.usage = rhi::BufferUsage::Storage;
        description.memory = rhi::MemoryUse::HostVisibleDeviceLocal;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        ring_[index] = *buffer;
    }
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        bool host_copy;
    };
    const auto device_only = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const auto host = rhi::BufferUsage::TransferDestination;
    // Every buffer the passes read is the device's own: a test writes its surfaces through an
    // upload buffer and a declared copy, and reads results back through declared copies. (A
    // host-visible surface buffer was the first form: at 1080p it is 132 MB, more than a resizable
    // BAR holds beside a second, and the allocation that fell back to system memory made the
    // cascade, which reads it at every tap, forty times slower.)
    const Request requests[kBufferCount] = {
        {"gi resolve surfaces", 2U * pixels * 2U * kVec4,
         device_only | rhi::BufferUsage::TransferDestination, rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve noisy", pixels * kSignals * kVec4, device_only, rhi::MemoryUse::DeviceLocal,
         false},
        {"gi resolve sources", pixels * 8U, device_only, rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve history", 2U * kSignals * pixels * 2U * kVec4, device_only,
         rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve variance", kSignals * pixels * 4U, device_only, rhi::MemoryUse::DeviceLocal,
         false},
        {"gi resolve filter a", kSignals * pixels * kVec4, device_only,
         rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve filter b", kSignals * pixels * kVec4, device_only,
         rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve noisy host", pixels * kSignals * kVec4, host, rhi::MemoryUse::Readback, true},
        {"gi resolve sources host", pixels * 8U, host, rhi::MemoryUse::Readback, true},
        {"gi resolve final host", kSignals * pixels * kVec4, host, rhi::MemoryUse::Readback, true},
        {"gi resolve surfaces host", pixels * 2U * kVec4, host, rhi::MemoryUse::Readback, true},
        {"gi resolve surfaces upload", pixels * 2U * kVec4, rhi::BufferUsage::TransferSource,
         rhi::MemoryUse::Upload, false},
        {"gi resolve guidance", pixels * kVec4, device_only | rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::DeviceLocal, false},
        {"gi resolve guidance upload", pixels * kVec4, rhi::BufferUsage::TransferSource,
         rhi::MemoryUse::Upload, false},
        // Readback-only: a frame never keeps a copy of what it sampled.
        {"gi resolve published", pixels * kSignals * kVec4, device_only, rhi::MemoryUse::DeviceLocal,
         true},
        {"gi resolve published host", pixels * kSignals * kVec4, host, rhi::MemoryUse::Readback,
         true},
    };
    for (u32 index = 0; index < kBufferCount; ++index) {
        // The upload buffers wait for the first `set_surfaces`, and the readbacks for `readback`.
        if ((requests[index].host_copy && !desc_.readback) || index == kSurfacesUpload ||
            index == kGuidanceUpload) {
            sizes_[index] = requests[index].size;
            continue;
        }
        rhi::BufferDescription description;
        description.name = requests[index].name;
        description.size = requests[index].size;
        description.usage = requests[index].usage;
        description.memory = requests[index].memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        buffers_[index] = *buffer;
        sizes_[index] = requests[index].size;
        if (void* mapped = device_->buffer_mapped_pointer(*buffer); mapped != nullptr) {
            std::memset(mapped, 0, static_cast<usize>(requests[index].size));
        }
    }

    const auto make_target = [this](const char* name, rhi::TextureHandle& texture,
                                    rhi::TextureViewHandle& view) noexcept -> Status {
        rhi::TextureDescription target;
        target.name = name;
        target.format = target_format();
        target.extent = rhi::Extent3D{desc_.width, desc_.height, 1};
        target.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage |
                       rhi::TextureUsage::TransferSource;
        Expected<rhi::TextureHandle, Error> made = device_->create_texture(target);
        if (!made) {
            return make_unexpected(made.error());
        }
        texture = *made;
        rhi::TextureViewDescription description;
        description.name = name;
        description.texture = texture;
        Expected<rhi::TextureViewHandle, Error> made_view = device_->create_texture_view(description);
        if (!made_view) {
            return make_unexpected(made_view.error());
        }
        view = *made_view;
        return ok();
    };
    if (Status made = make_target("gi indirect diffuse", diffuse_, diffuse_view_); !made) {
        return made;
    }
    return make_target("gi indirect specular", specular_, specular_view_);
}

void GpuScreenResolve::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (rhi::BufferHandle& buffer : buffers_) {
        if (buffer) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (rhi::BufferHandle& buffer : ring_) {
        if (buffer) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (u32 which = 0; which < kPipelineCount; ++which) {
        if (pipelines_[which]) {
            device_->destroy_compute_pipeline(pipelines_[which]);
        }
        if (shaders_[which]) {
            device_->destroy_shader_module(shaders_[which]);
        }
        pipelines_[which] = {};
        shaders_[which] = {};
    }
    for (rhi::PipelineLayoutHandle* layout :
         {&surface_pipeline_layout_, &resolve_pipeline_layout_, &resolve_hardware_pipeline_layout_,
          &denoise_pipeline_layout_, &publish_pipeline_layout_}) {
        if (*layout) {
            device_->destroy_pipeline_layout(*layout);
        }
        *layout = {};
    }
    for (rhi::DescriptorSetLayoutHandle& layout : layouts_) {
        if (layout) {
            device_->destroy_descriptor_set_layout(layout);
        }
        layout = {};
    }
    for (rhi::TextureViewHandle* view : {&diffuse_view_, &specular_view_}) {
        if (*view) {
            device_->destroy_texture_view(*view);
        }
        *view = {};
    }
    for (rhi::TextureHandle* texture : {&diffuse_, &specular_}) {
        if (*texture) {
            device_->destroy_texture(*texture);
        }
        *texture = {};
    }
    if (timer_) {
        device_->destroy_query_pool(timer_);
    }
    timer_ = {};
    hardware_pipeline_ = false;
    history_valid_ = false;
    view_valid_ = false;
    began_ = false;
    cache_ = nullptr;
    device_ = nullptr;
    allocator_ = nullptr;
}

// --- Settings ------------------------------------------------------------------------------------

void GpuScreenResolve::set_settings(const gi::ScreenResolveSettings& settings) noexcept {
    settings_ = settings;
    words_[kWordCamera + 3] = bits(settings.ray_density);
    words_[kWordRays + 0] = bits(settings.max_ray_distance_metres);
    words_[kWordRays + 1] = bits(settings.origin_lift_metres);
    words_[kWordRays + 2] = settings.sky_visibility_rays;
    words_[kWordRays + 3] = gi::exclusion_for(settings.mode, false, false);
    words_[kWordFar + 0] = bits(settings.error_targets.far_field_begins_metres);
    words_[kWordFar + 1] = bits(settings.error_targets.near_field_metres);
    words_[kWordFar + 2] = bits(settings.error_targets.far_field_metres);
    words_[kWordReflect + 0] = bits(settings.reflection_thresholds.mirror_max_roughness);
    words_[kWordReflect + 1] = bits(settings.reflection_thresholds.cache_min_roughness);
    words_[kWordReflect + 2] = settings.reflection_thresholds.max_rays;
    words_[kWordIntensity + 0] = bits(settings.diffuse_intensity);
    words_[kWordIntensity + 1] = bits(settings.specular_intensity);
    // `classify_history`'s tolerance: `ScreenHistoryInputs`' default, which the frame uses.
    words_[kWordIntensity + 2] = bits(gi::ScreenHistoryInputs{}.depth_tolerance);
}

void GpuScreenResolve::set_denoiser(const denoise::Denoiser& denoiser) noexcept {
    const denoise::QualityPosition& quality =
        denoise::quality_ladder()[std::min(denoiser.quality_position(),
                                           denoise::kQualityPositionCount - 1)];
    const denoise::SignalKind kinds[kSignals] = {denoise::SignalKind::IndirectDiffuse,
                                                 denoise::SignalKind::IndirectSpecular};
    u32 passes = 0;
    for (u32 signal = 0; signal < kSignals; ++signal) {
        const denoise::SignalConfig& config = denoiser.config(kinds[signal]);
        const u32 at = kWordSignals + (signal * 12U);
        words_[at + 0] = bits(config.sigma_depth);
        words_[at + 1] = bits(config.sigma_normal);
        words_[at + 2] = bits(config.sigma_value);
        words_[at + 3] = bits(config.sigma_roughness);
        words_[at + 4] = bits(config.roughness_widening);
        words_[at + 5] =
            bits(static_cast<f32>(std::min(config.history_length, quality.history_length)));
        words_[at + 6] = std::min(config.max_passes, quality.max_passes);
        words_[at + 7] = quality.kernel_extent;
        // No visibility buffer in a forward frame: identity is never available, as
        // `Denoiser::filter` decides from an empty `instance_id`.
        words_[at + 8] = 0;
        words_[at + 9] = config.domain == denoise::SignalDomain::Visibility ? 1U : 0U;
        words_[at + 10] = 0;
        words_[at + 11] = 0;
        passes = std::max(passes, words_[at + 6]);
    }
    words_[kWordFrame + 1] = passes;
}

void GpuScreenResolve::set_field(const gi::DistanceField& field) noexcept {
    const gi::ClipmapSettings& settings = field.settings();
    const u32 levels = std::max(settings.levels, 1U);
    words_[kWordFar + 3] =
        bits(settings.base_extent_metres * static_cast<f32>(1U << (levels - 1U)));
}

void GpuScreenResolve::set_sky(const gi::SkyTerm& sky) noexcept {
    if (cache_ != nullptr) {
        cache_->write_sky(sky);
        cache_->write_constants();
    }
}

void GpuScreenResolve::set_cache_frame(u64 frame) noexcept {
    words_[kWordReflect + 3] = static_cast<u32>(frame & 0xFFFFFFFFULL);
    if (cache_ != nullptr) {
        // The card staleness `gpWorldRadiance` reads is the probe set's word: `GpuTierTracer`'s.
        cache_->constants_[(24 * 4) + 3] = static_cast<u32>(frame & 0xFFFFFFFFULL);
        cache_->write_constants();
    }
}

Status GpuScreenResolve::set_divisor(u32 divisor) noexcept {
    if (divisor == 0 || divisor > 4) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuScreenResolve::set_divisor: one resolves every pixel, up to four every "
                    "fourth each way");
    }
    if (divisor == divisor_) {
        return ok();
    }
    // The history and the surface slots are laid out by the grid's cell count: a new grid is a
    // new history, as a resize is.
    divisor_ = divisor;
    grid_width_ = gi::screen_grid_extent(desc_.width, divisor);
    grid_height_ = gi::screen_grid_extent(desc_.height, divisor);
    words_[kWordCounts + 0] = grid_width_;
    words_[kWordCounts + 1] = grid_height_;
    words_[kWordCounts + 2] = grid_width_ * grid_height_;
    words_[kWordFull + 0] = desc_.width;
    words_[kWordFull + 1] = desc_.height;
    words_[kWordFull + 2] = divisor;
    words_[kWordFull + 3] = desc_.readback ? 1U : 0U;
    history_valid_ = false;
    view_valid_ = false;
    return ok();
}

Status GpuScreenResolve::sync(const gi::RadianceCache& cache) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device resolve is not created");
    }
    GpuGiScene& scene = *cache_->scene_;
    if (cache_->field_ != nullptr) {
        Expected<FieldUploadReport, Error> field = scene.upload_field(*cache_->field_);
        if (!field) {
            return make_unexpected(field.error());
        }
    }
    if (const gi::SurfaceCache* surfaces = cache_->surfaces_; surfaces != nullptr) {
        Expected<CardUploadReport, Error> cards =
            scene.upload_cards(surfaces->pages(), surfaces->lookup_radius());
        if (!cards) {
            return make_unexpected(cards.error());
        }
        if (Status radiance = scene.upload_card_radiance(surfaces->pages()); !radiance) {
            return radiance;
        }
    }
    return cache_->upload(cache);
}

// --- One frame -----------------------------------------------------------------------------------

rhi::BufferHandle GpuScreenResolve::constants() const noexcept {
    return ring_[ring_slot_];
}

void GpuScreenResolve::write_constants() noexcept {
    if (void* target = device_->buffer_mapped_pointer(constants()); target != nullptr) {
        std::memcpy(target, words_, sizeof(words_));
    }
}

Status GpuScreenResolve::begin_frame(const GpuResolveView& view, u64 frame) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device resolve is not created");
    }
    // The slots turn over: last frame's surfaces and history are the other slot now.
    if (began_) {
        slot_ = 1U - slot_;
        ring_slot_ = (ring_slot_ + 1U) % rhi::kMaxFramesInFlight;
    }
    words_[kWordCounts + 3] = slot_;
    words_[kWordCamera + 0] = bits(view.camera.x);
    words_[kWordCamera + 1] = bits(view.camera.y);
    words_[kWordCamera + 2] = bits(view.camera.z);
    put_matrix(words_, kWordWorldToClip, view.world_to_clip);
    put_matrix(words_, kWordPreviousToClip,
               view_valid_ ? previous_world_to_clip_ : view.world_to_clip);
    for (u32 index = 0; index < 4; ++index) {
        words_[kWordUnpack + index] = bits(view.projection[index]);
    }
    // The jittered image shows the unjittered point at (x - jx, y + jy): +y is up in the temporal
    // framework's convention and down in a pixel row, as GTAO's reconstruction has it.
    words_[kWordJitter + 0] = bits(-view.jitter.x);
    words_[kWordJitter + 1] = bits(view.jitter.y);
    // View to camera-relative is the rotation's transpose: row i is column i.
    for (u32 row = 0; row < 3; ++row) {
        words_[kWordViewToRelative + (row * 4U) + 0] = bits(view.relative_to_view.columns[row].x);
        words_[kWordViewToRelative + (row * 4U) + 1] = bits(view.relative_to_view.columns[row].y);
        words_[kWordViewToRelative + (row * 4U) + 2] = bits(view.relative_to_view.columns[row].z);
        words_[kWordViewToRelative + (row * 4U) + 3] = 0;
    }
    words_[kWordIntensity + 3] = history_valid_ ? 1U : 0U;
    words_[kWordInvalid] = 0;
    words_[kWordFrame + 0] = static_cast<u32>(frame & 0xFFFFFFFFULL);
    const u32 passes = words_[kWordFrame + 1];
    final_in_b_ = (passes & 1U) == 1U;
    words_[kWordFrame + 2] = final_in_b_ ? 1U : 0U;
    previous_world_to_clip_ = view.world_to_clip;
    view_valid_ = true;
    began_ = true;
    write_constants();
    return ok();
}

void GpuScreenResolve::set_jitter(Vec2 jitter) noexcept {
    words_[kWordJitter + 0] = bits(-jitter.x);
    words_[kWordJitter + 1] = bits(jitter.y);
    if (device_ != nullptr && began_) {
        write_constants();
    }
}

void GpuScreenResolve::set_invalidated(Span<const Aabb> regions) noexcept {
    const auto count =
        static_cast<u32>(std::min<usize>(regions.size(), gi::kScreenHistoryRegions));
    words_[kWordInvalid] = count;
    for (u32 index = 0; index < count; ++index) {
        const u32 at = kWordInvalid + 4U + (index * 8U);
        words_[at + 0] = bits(regions[index].min.x);
        words_[at + 1] = bits(regions[index].min.y);
        words_[at + 2] = bits(regions[index].min.z);
        words_[at + 3] = 0;
        words_[at + 4] = bits(regions[index].max.x);
        words_[at + 5] = bits(regions[index].max.y);
        words_[at + 6] = bits(regions[index].max.z);
        words_[at + 7] = 0;
    }
    if (device_ != nullptr && began_) {
        write_constants();
    }
}

Status GpuScreenResolve::set_surfaces(const gi::ScreenSurfaces& surfaces) noexcept {
    if (device_ == nullptr || !began_) {
        return fail(ErrorCode::InvalidArgument, "set_surfaces needs a created resolve and a frame");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    if (surfaces.width != desc_.width || surfaces.height != desc_.height ||
        surfaces.depth.size() != pixels || surfaces.position.size() != pixels ||
        surfaces.normal.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "the surfaces do not cover the view the resolve was created for");
    }
    if (Status built = grid_.build(surfaces, divisor_); !built) {
        return built;
    }
    for (const Buffer upload : {kSurfacesUpload, kGuidanceUpload}) {
        if (buffers_[upload]) {
            continue;
        }
        rhi::BufferDescription description;
        description.name = upload == kSurfacesUpload ? "gi resolve surfaces upload"
                                                     : "gi resolve guidance upload";
        description.size = sizes_[upload];
        description.usage = rhi::BufferUsage::TransferSource;
        description.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        buffers_[upload] = *buffer;
    }
    auto* cells = static_cast<f32*>(device_->buffer_mapped_pointer(buffers_[kSurfacesUpload]));
    auto* guidance = static_cast<f32*>(device_->buffer_mapped_pointer(buffers_[kGuidanceUpload]));
    if (cells == nullptr || guidance == nullptr) {
        return fail(ErrorCode::Internal, "the resolve's upload buffers are not mapped");
    }
    const gi::ScreenSurfaces grid = grid_.surfaces();
    const usize count = static_cast<usize>(grid.width) * grid.height;
    for (usize index = 0; index < count; ++index) {
        f32* record = cells + (index * 8U);
        record[0] = grid.position[index].x;
        record[1] = grid.position[index].y;
        record[2] = grid.position[index].z;
        record[3] = grid.depth[index];
        record[4] = grid.normal[index].x;
        record[5] = grid.normal[index].y;
        record[6] = grid.normal[index].z;
        record[7] = grid.roughness[index];
    }
    for (usize index = 0; index < pixels; ++index) {
        f32* entry = guidance + (index * 4U);
        const bool surface = surfaces.depth[index] > 0.0F;
        entry[0] = surface ? surfaces.normal[index].x : 0.0F;
        entry[1] = surface ? surfaces.normal[index].y : 1.0F;
        entry[2] = surface ? surfaces.normal[index].z : 0.0F;
        entry[3] = surface ? surfaces.depth[index] : 0.0F;
    }
    surfaces_uploaded_ = true;
    return ok();
}

ResourceId GpuScreenResolve::import_diffuse(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "gi indirect diffuse";
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    // UNDEFINED EVERY FRAME, as GTAO's target is: the publish rewrites every texel.
    imported_diffuse_ = graph.import_texture(request, diffuse_, rhi::ImageUse::Undefined);
    return imported_diffuse_;
}

ResourceId GpuScreenResolve::import_specular(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "gi indirect specular";
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    imported_specular_ = graph.import_texture(request, specular_, rhi::ImageUse::Undefined);
    return imported_specular_;
}

ResourceId GpuScreenResolve::import(RenderGraph& graph, Buffer buffer) noexcept {
    static constexpr const char* kNames[kBufferCount] = {
        "gi resolve surfaces",      "gi resolve noisy",        "gi resolve sources",
        "gi resolve history",       "gi resolve variance",     "gi resolve filter a",
        "gi resolve filter b",      "gi resolve noisy host",   "gi resolve sources host",
        "gi resolve final host",    "gi resolve surfaces host", "gi resolve surfaces upload",
        "gi resolve guidance",      "gi resolve guidance upload", "gi resolve published",
        "gi resolve published host",
    };
    BufferRequest request;
    request.name = kNames[buffer];
    request.size = sizes_[buffer];
    return graph.import_buffer_once(request, buffers_[buffer]);
}

ResourceId GpuScreenResolve::import_constants(RenderGraph& graph) noexcept {
    BufferRequest request;
    request.name = "gi resolve constants";
    request.size = sizeof(words_);
    return graph.import_buffer_once(request, constants());
}

u32 GpuScreenResolve::filter_passes() const noexcept {
    return words_[kWordFrame + 1];
}

FrameStageDeclaration GpuScreenResolve::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId GpuScreenResolve::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                       void* user) noexcept {
    return static_cast<GpuScreenResolve*>(user)->declare(graph, inputs);
}

PassId GpuScreenResolve::declare_resolve(RenderGraph& graph, bool first) noexcept {
    const bool hardware =
        hardware_pipeline_ && cache_->world_tier() == gi::RadianceSource::HardwareTrace;
    last_tier_ = hardware ? gi::RadianceSource::HardwareTrace : gi::RadianceSource::SoftwareTrace;
    const u32 pixels = grid_width_ * grid_height_;

    Step& reproject = steps_[step_count_++];
    reproject = Step{this, kReproject, 2, first, pixels};
    PassBuilder pass = graph.add_pass("gi reproject", QueueKind::Graphics);
    const PassId id = pass.read(import_constants(graph), Access::ComputeStorageRead)
                          .read(import(graph, kSurfaces), Access::ComputeStorageRead)
                          .write(import(graph, kSources), Access::ComputeStorageWrite)
                          .record(&record_step, &reproject)
                          .id();

    Step& resolve = steps_[step_count_++];
    resolve = Step{this, hardware ? kResolveHardware : kResolve, 3, false, pixels};
    PassBuilder trace = graph.add_pass("gi resolve", QueueKind::Graphics);
    cache_->read_scene(graph, trace, hardware);
    using C = GpuRadianceCache;
    for (const C::Binding binding : {C::kProbes, C::kPayloads, C::kLayout}) {
        trace.read(cache_->import(graph, binding), Access::ComputeStorageRead);
    }
    trace.read(import_constants(graph), Access::ComputeStorageRead)
        .read(import(graph, kSurfaces), Access::ComputeStorageRead)
        .write(import(graph, kNoisy), Access::ComputeStorageWrite)
        .record(&record_step, &resolve);
    return id;
}

void GpuScreenResolve::declare_denoiser(RenderGraph& graph, ResourceId diffuse,
                                        ResourceId specular) noexcept {
    const u32 items = grid_width_ * grid_height_ * kSignals;
    const ResourceId constants = import_constants(graph);
    const ResourceId surfaces = import(graph, kSurfaces);
    const ResourceId history = import(graph, kHistory);
    const ResourceId filter_a = import(graph, kFilterA);
    const ResourceId filter_b = import(graph, kFilterB);

    Step& accumulate = steps_[step_count_++];
    accumulate = Step{this, kAccumulate, 4, false, items};
    graph.add_pass("gi denoise accumulate", QueueKind::Graphics)
        .read(constants, Access::ComputeStorageRead)
        .read(import(graph, kNoisy), Access::ComputeStorageRead)
        .read(import(graph, kSources), Access::ComputeStorageRead)
        .write(history, Access::ComputeStorageReadWrite)
        .record(&record_step, &accumulate);

    Step& variance = steps_[step_count_++];
    variance = Step{this, kVariance, 5, false, items};
    graph.add_pass("gi denoise variance", QueueKind::Graphics)
        .read(constants, Access::ComputeStorageRead)
        .read(history, Access::ComputeStorageRead)
        .write(import(graph, kVariances), Access::ComputeStorageWrite)
        .write(filter_a, Access::ComputeStorageWrite)
        .record(&record_step, &variance);

    const u32 passes = filter_passes();
    for (u32 index = 0; index < passes; ++index) {
        const bool a_to_b = (index & 1U) == 0U;
        Step& filter = steps_[step_count_++];
        filter = Step{this, kFilter, index + 1U == passes ? 6U : ~0U, false, items};
        filter.pass = index;
        filter.direction = a_to_b ? 0U : 1U;
        graph.add_pass("gi denoise filter", QueueKind::Graphics)
            .read(constants, Access::ComputeStorageRead)
            .read(surfaces, Access::ComputeStorageRead)
            .read(history, Access::ComputeStorageRead)
            .read(import(graph, kVariances), Access::ComputeStorageRead)
            .read(a_to_b ? filter_a : filter_b, Access::ComputeStorageRead)
            .write(a_to_b ? filter_b : filter_a, Access::ComputeStorageWrite)
            .record(&record_step, &filter);
    }

    Step& publish = steps_[step_count_++];
    publish = Step{this, kPublish, 7, false, desc_.width * desc_.height};
    publish.diffuse = diffuse;
    publish.specular = specular;
    PassBuilder pass = graph.add_pass("gi publish", QueueKind::Graphics);
    pass.read(constants, Access::ComputeStorageRead)
        .read(final_in_b_ ? filter_b : filter_a, Access::ComputeStorageRead)
        .read(surfaces, Access::ComputeStorageRead)
        .read(import(graph, kGuidance), Access::ComputeStorageRead)
        .write(diffuse, Access::ComputeStorageWrite)
        .write(specular, Access::ComputeStorageWrite);
    if (desc_.readback) {
        pass.write(import(graph, kPublished), Access::ComputeStorageWrite);
    }
    pass.record(&record_step, &publish);
}

void GpuScreenResolve::declare_copy(RenderGraph& graph, Copy& copy, const char* name) noexcept {
    const ResourceId source = import(graph, copy.source);
    const ResourceId destination = import(graph, copy.destination);
    graph.add_pass(name, QueueKind::Graphics)
        .read(source, Access::TransferRead)
        .write(destination, Access::TransferWrite)
        .record(&record_copy, &copy);
    graph.add_pass(name, QueueKind::Graphics).read(destination, Access::HostRead).side_effect();
}

void GpuScreenResolve::declare_readbacks(RenderGraph& graph) noexcept {
    if (!desc_.readback) {
        return;
    }
    const u64 pixels = static_cast<u64>(grid_width_) * grid_height_;
    const u64 frame_pixels = static_cast<u64>(desc_.width) * desc_.height;
    copies_[0] = Copy{this, kNoisy, kNoisyHost, 0, pixels * kSignals * kVec4};
    copies_[1] = Copy{this, kSources, kSourcesHost, 0, pixels * 8U};
    copies_[2] =
        Copy{this, final_in_b_ ? kFilterB : kFilterA, kFinalHost, 0, pixels * kSignals * kVec4};
    copies_[3] =
        Copy{this, kSurfaces, kSurfacesHost, static_cast<u64>(slot_) * pixels * 2U * kVec4,
             pixels * 2U * kVec4};
    copies_[4] = Copy{this, kPublished, kPublishedHost, 0, frame_pixels * kSignals * kVec4};
    declare_copy(graph, copies_[0], "gi resolve noisy readback");
    declare_copy(graph, copies_[1], "gi resolve sources readback");
    declare_copy(graph, copies_[2], "gi resolve final readback");
    declare_copy(graph, copies_[3], "gi resolve surfaces readback");
    declare_copy(graph, copies_[4], "gi resolve published readback");
}

PassId GpuScreenResolve::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would sample a term nothing wrote.
    if (device_ == nullptr || !began_ || inputs.target == kInvalidResource ||
        inputs.target != imported_diffuse_ || imported_specular_ == kInvalidResource ||
        inputs.depth == kInvalidResource || inputs.normal_roughness == kInvalidResource ||
        inputs.width != desc_.width || inputs.height != desc_.height) {
        return kInvalidPass;
    }
    step_count_ = 0;
    timed_ = false;
    Step& unpack = steps_[step_count_++];
    unpack = Step{this, kUnpack, 1, true, desc_.width * desc_.height};
    unpack.depth = inputs.depth;
    unpack.normals = inputs.normal_roughness;
    const PassId first = graph.add_pass("gi unpack surfaces", QueueKind::Graphics)
                             .read(import_constants(graph), Access::ComputeStorageRead)
                             .read(inputs.depth, Access::ComputeSampledRead)
                             .read(inputs.normal_roughness, Access::ComputeSampledRead)
                             .write(import(graph, kSurfaces), Access::ComputeStorageReadWrite)
                             .write(import(graph, kGuidance), Access::ComputeStorageWrite)
                             .record(&record_step, &unpack)
                             .id();
    (void)declare_resolve(graph, false);
    declare_denoiser(graph, imported_diffuse_, imported_specular_);
    declare_readbacks(graph);
    history_valid_ = true;
    return graph.status() ? first : kInvalidPass;
}

Status GpuScreenResolve::declare_from_surfaces(RenderGraph& graph) noexcept {
    if (device_ == nullptr || !began_) {
        return fail(ErrorCode::InvalidArgument, "declare needs a created resolve and a frame");
    }
    if (!surfaces_uploaded_) {
        return fail(ErrorCode::InvalidArgument,
                    "declare_from_surfaces needs this frame's surfaces from set_surfaces");
    }
    step_count_ = 0;
    timed_ = false;
    const ResourceId diffuse = import_diffuse(graph);
    const ResourceId specular = import_specular(graph);
    const u64 bytes = static_cast<u64>(grid_width_) * grid_height_ * 2U * kVec4;
    upload_ = Copy{this, kSurfacesUpload, kSurfaces, 0, bytes};
    upload_.target_offset = static_cast<u64>(slot_) * bytes;
    graph.add_pass("gi resolve surfaces upload", QueueKind::Graphics)
        .read(import(graph, kSurfacesUpload), Access::TransferRead)
        .write(import(graph, kSurfaces), Access::TransferWrite)
        .record(&record_copy, &upload_);
    guidance_upload_ = Copy{this, kGuidanceUpload, kGuidance, 0,
                            static_cast<u64>(desc_.width) * desc_.height * kVec4};
    graph.add_pass("gi resolve guidance upload", QueueKind::Graphics)
        .read(import(graph, kGuidanceUpload), Access::TransferRead)
        .write(import(graph, kGuidance), Access::TransferWrite)
        .record(&record_copy, &guidance_upload_);
    surfaces_uploaded_ = false;
    (void)declare_resolve(graph, true);
    declare_denoiser(graph, diffuse, specular);
    declare_readbacks(graph);
    history_valid_ = true;
    return graph.status();
}

// --- Recording -----------------------------------------------------------------------------------

Expected<rhi::DescriptorSetHandle, Error> GpuScreenResolve::transient_set(
    const PassContext& context, const Step& step) noexcept {
    rhi::DescriptorWrite writes[kDenoiseBindings] = {};
    u32 count = 0;
    Layout layout = kDenoiseLayout;
    switch (step.pipeline) {
        case kUnpack:
            layout = kSurfaceLayout;
            writes[0] = buffer_write(0, constants());
            writes[1] = sampled_write(1, context.executor->view(step.depth));
            writes[2] = sampled_write(2, context.executor->view(step.normals));
            writes[3] = buffer_write(3, buffers_[kSurfaces]);
            writes[4] = buffer_write(4, buffers_[kGuidance]);
            count = kSurfaceBindings;
            break;
        case kResolve:
        case kResolveHardware:
            layout = kResolveLayout;
            writes[0] = buffer_write(0, constants());
            writes[1] = buffer_write(1, buffers_[kSurfaces]);
            writes[2] = buffer_write(2, buffers_[kNoisy]);
            count = kResolveBindings;
            break;
        case kPublish:
            layout = kPublishLayout;
            writes[0] = buffer_write(0, constants());
            writes[1] = buffer_write(1, buffers_[final_in_b_ ? kFilterB : kFilterA]);
            writes[2] = buffer_write(2, buffers_[kSurfaces]);
            writes[3] = buffer_write(3, buffers_[kGuidance]);
            // Bound whether or not the flag asks for a copy — a set's every binding is written —
            // and with no copy kept, to a buffer the shader then never writes.
            writes[4] = buffer_write(4, buffers_[kPublished] ? buffers_[kPublished]
                                                             : buffers_[kGuidance]);
            writes[5] = storage_write(5, context.executor->view(step.diffuse));
            writes[6] = storage_write(6, context.executor->view(step.specular));
            count = kPublishBindings;
            break;
        case kReproject:
        case kAccumulate:
        case kVariance:
        case kFilter:
        case kPipelineCount: {
            const Buffer order[kDenoiseBindings - 1] = {kSurfaces,  kNoisy,   kSources, kHistory,
                                                        kVariances, kFilterA, kFilterB};
            writes[0] = buffer_write(0, constants());
            for (u32 index = 0; index < kDenoiseBindings - 1; ++index) {
                writes[index + 1] = buffer_write(index + 1, buffers_[order[index]]);
            }
            count = kDenoiseBindings;
            break;
        }
    }
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(layouts_[layout], true);
    if (!set) {
        return set;
    }
    if (Status written =
            device_->update_descriptor_set(*set, Span<const rhi::DescriptorWrite>(writes, count));
        !written) {
        return make_unexpected(written.error());
    }
    return set;
}

void GpuScreenResolve::record_step(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    GpuScreenResolve& self = *step->self;
    rhi::CommandBuffer& commands = *context.commands;
    const bool timed = self.timing_ && self.timer_;
    if (timed && step->first) {
        commands.reset_queries(self.timer_, 0, kStamps);
        commands.write_timestamp(self.timer_, 0);
        if (step->pipeline != kUnpack) {
            // No unpack this frame: its interval is empty.
            commands.write_timestamp(self.timer_, 1);
        }
    }
    Expected<rhi::DescriptorSetHandle, Error> set = self.transient_set(context, *step);
    if (!set) {
        return;
    }
    commands.bind_compute_pipeline(self.pipelines_[step->pipeline]);
    switch (step->pipeline) {
        case kUnpack:
            commands.bind_descriptor_sets(self.surface_pipeline_layout_, 0,
                                          Span<const rhi::DescriptorSetHandle>(&*set, 1));
            commands.dispatch(tile_groups(self.desc_.width), tile_groups(self.desc_.height), 1);
            break;
        case kPublish:
            commands.bind_descriptor_sets(self.publish_pipeline_layout_, 0,
                                          Span<const rhi::DescriptorSetHandle>(&*set, 1));
            commands.dispatch(tile_groups(self.desc_.width), tile_groups(self.desc_.height), 1);
            break;
        case kResolve:
        case kResolveHardware: {
            const bool hardware = step->pipeline == kResolveHardware;
            const rhi::PipelineLayoutHandle layout = hardware
                                                         ? self.resolve_hardware_pipeline_layout_
                                                         : self.resolve_pipeline_layout_;
            const rhi::DescriptorSetHandle sets[4] = {self.cache_->scene_->set_,
                                                      self.cache_->probe_set_, *set,
                                                      self.cache_->hardware_set_};
            commands.bind_descriptor_sets(
                layout, 0, Span<const rhi::DescriptorSetHandle>(sets, hardware ? 4U : 3U));
            const GpuGiScene::Dispatch dispatch{step->count, self.words_[kWordFrame], {0, 0}};
            commands.push_constants(layout, rhi::ShaderStage::Compute, 0,
                                    Span<const u8>(reinterpret_cast<const u8*>(&dispatch),
                                                   sizeof(dispatch)));
            commands.dispatch(linear_groups(step->count), 1, 1);
            break;
        }
        case kReproject:
        case kAccumulate:
        case kVariance:
        case kFilter:
        case kPipelineCount: {
            commands.bind_descriptor_sets(self.denoise_pipeline_layout_, 0,
                                          Span<const rhi::DescriptorSetHandle>(&*set, 1));
            const DenoisePush push{step->count, step->pass, step->direction, 0};
            commands.push_constants(self.denoise_pipeline_layout_, rhi::ShaderStage::Compute, 0,
                                    Span<const u8>(reinterpret_cast<const u8*>(&push),
                                                   sizeof(push)));
            commands.dispatch(linear_groups(step->count), 1, 1);
            break;
        }
    }
    if (timed && step->stamp < kStamps) {
        commands.write_timestamp(self.timer_, step->stamp);
        self.timed_ = step->stamp + 1U == kStamps ? true : self.timed_;
    }
}

void GpuScreenResolve::record_copy(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Copy*>(user);
    const rhi::BufferCopy region{copy->offset, copy->target_offset, copy->bytes};
    context.commands->copy_buffer(copy->self->buffers_[copy->source],
                                  copy->self->buffers_[copy->destination],
                                  Span<const rhi::BufferCopy>(&region, 1));
}

GpuResolveTimes GpuScreenResolve::last_times() const noexcept {
    GpuResolveTimes times;
    if (device_ == nullptr || !timer_ || !timed_) {
        return times;
    }
    u64 stamps[kStamps] = {};
    Expected<u32, Error> read =
        device_->read_query_results(timer_, 0, kStamps, Span<u64>(stamps, kStamps));
    if (!read || *read != kStamps) {
        return times;
    }
    times.unpack_ms = stamp_ms(stamps, 0, 1);
    times.reproject_ms = stamp_ms(stamps, 1, 2);
    times.resolve_ms = stamp_ms(stamps, 2, 3);
    times.accumulate_ms = stamp_ms(stamps, 3, 4);
    times.variance_ms = stamp_ms(stamps, 4, 5);
    times.filter_ms = stamp_ms(stamps, 5, 6);
    times.publish_ms = stamp_ms(stamps, 6, 7);
    times.total_ms = stamp_ms(stamps, 0, 7);
    times.valid = true;
    return times;
}

// --- Readback ------------------------------------------------------------------------------------

Status GpuScreenResolve::read_back_noisy(Span<Vec3> diffuse, Span<Vec3> specular) const noexcept {
    const usize pixels = static_cast<usize>(grid_width_) * grid_height_;
    const auto* data =
        desc_.readback ? static_cast<const f32*>(device_->buffer_mapped_pointer(buffers_[kNoisyHost]))
                       : nullptr;
    if (data == nullptr || diffuse.size() != pixels || specular.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back_noisy needs `readback` and one entry a pixel for each signal");
    }
    for (usize index = 0; index < pixels; ++index) {
        const f32* d = data + (index * 8U);
        diffuse[index] = Vec3{d[0], d[1], d[2]};
        specular[index] = Vec3{d[4], d[5], d[6]};
    }
    return ok();
}

Status GpuScreenResolve::read_back_denoised(Span<Vec3> diffuse,
                                            Span<Vec3> specular) const noexcept {
    const usize pixels = static_cast<usize>(grid_width_) * grid_height_;
    const auto* data =
        desc_.readback ? static_cast<const f32*>(device_->buffer_mapped_pointer(buffers_[kFinalHost]))
                       : nullptr;
    if (data == nullptr || diffuse.size() != pixels || specular.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back_denoised needs `readback` and one entry a pixel for each signal");
    }
    for (usize index = 0; index < pixels; ++index) {
        const f32* d = data + (index * 4U);
        const f32* s = data + ((pixels + index) * 4U);
        diffuse[index] = Vec3{d[0], d[1], d[2]};
        specular[index] = Vec3{s[0], s[1], s[2]};
    }
    return ok();
}

Status GpuScreenResolve::read_back_history(Span<i32> source, Span<f32> confidence) const noexcept {
    const usize pixels = static_cast<usize>(grid_width_) * grid_height_;
    const auto* data =
        desc_.readback
            ? static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_[kSourcesHost]))
            : nullptr;
    if (data == nullptr || source.size() != pixels || confidence.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back_history needs `readback` and one entry a pixel");
    }
    for (usize index = 0; index < pixels; ++index) {
        i32 landing = 0;
        f32 trust = 0.0F;
        std::memcpy(&landing, &data[index * 2U], sizeof(landing));
        std::memcpy(&trust, &data[(index * 2U) + 1U], sizeof(trust));
        source[index] = landing;
        confidence[index] = trust;
    }
    return ok();
}

Status GpuScreenResolve::read_back_surfaces(Span<Vec3> position, Span<f32> depth,
                                            Span<Vec3> normal,
                                            Span<f32> roughness) const noexcept {
    const usize pixels = static_cast<usize>(grid_width_) * grid_height_;
    const auto* data =
        desc_.readback
            ? static_cast<const f32*>(device_->buffer_mapped_pointer(buffers_[kSurfacesHost]))
            : nullptr;
    if (data == nullptr || position.size() != pixels || depth.size() != pixels ||
        normal.size() != pixels || roughness.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back_surfaces needs `readback` and one entry a pixel");
    }
    for (usize index = 0; index < pixels; ++index) {
        const f32* record = data + (index * 8U);
        position[index] = Vec3{record[0], record[1], record[2]};
        depth[index] = record[3];
        normal[index] = Vec3{record[4], record[5], record[6]};
        roughness[index] = record[7];
    }
    return ok();
}

Status GpuScreenResolve::read_back_published(Span<Vec3> diffuse,
                                             Span<Vec3> specular) const noexcept {
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    const auto* data =
        desc_.readback
            ? static_cast<const f32*>(device_->buffer_mapped_pointer(buffers_[kPublishedHost]))
            : nullptr;
    if (data == nullptr || diffuse.size() != pixels || specular.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back_published needs `readback` and one entry a frame pixel");
    }
    for (usize index = 0; index < pixels; ++index) {
        const f32* d = data + (index * 4U);
        const f32* s = data + ((pixels + index) * 4U);
        diffuse[index] = Vec3{d[0], d[1], d[2]};
        specular[index] = Vec3{s[0], s[1], s[2]};
    }
    return ok();
}

}  // namespace cy::rendering::gi_gpu
