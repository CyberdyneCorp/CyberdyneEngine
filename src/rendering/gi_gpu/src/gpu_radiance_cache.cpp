// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>

#include <cy/backends/rhi/validation.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "gi_gpu_msl.h"
#include "gi_gpu_spirv.h"

namespace cy::rendering::gi_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

/// `[numthreads(64, 1, 1)]` on every entry point.
constexpr u32 kGroupSize = 64;

/// Words of the probe set's constants, as gi_probe_common.slang numbers them (uint4 index * 4).
///
/// `kWord` and not `k`: `kGather`, `kLayout` and `kScreen` are ALSO enumerators of the class's own
/// `Pipeline` and `Binding`, and inside a member function the class scope is searched first — so a
/// bare `kGather` there was the pipeline index 0, not word 4, and every constant it addressed
/// landed a word or a uint4 away from where the shader reads it.
constexpr u32 kWordCounts = 0 * 4;
constexpr u32 kWordGather = 1 * 4;
constexpr u32 kWordSkyZenith = 2 * 4;
constexpr u32 kWordSkyHorizon = 3 * 4;
constexpr u32 kWordSkyGround = 4 * 4;
constexpr u32 kWordLayout = 5 * 4;
constexpr u32 kWordCentres = 6 * 4;
constexpr u32 kWordScreen = 14 * 4;
constexpr u32 kWordScreenHzb = 15 * 4;
constexpr u32 kWordWorldToView = 16 * 4;
constexpr u32 kWordWorldToClip = 20 * 4;
constexpr u32 kWordBudget = 24 * 4;
constexpr u32 kWordTier = 25 * 4;
constexpr u32 kWordHzbLevels = 26 * 4;

constexpr u64 kVec4 = 16;
constexpr u64 kProbeVec4s = 4;
constexpr u64 kResultVec4s = 14;
constexpr u64 kQueryVec4s = 2;
constexpr u64 kTierRayVec4s = 2;
constexpr u64 kTierResultVec4s = 2;
constexpr u32 kCounterCount = 8;
constexpr u32 kNoProbe = 0xFFFFFFFFU;

/// What `SoftwareTracer` and `HardwareTracer` trust a hit, before the proxy and the cache.
constexpr f32 kSoftwareBaseConfidence = 0.85F;
constexpr f32 kHardwareBaseConfidence = 0.95F;

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

[[nodiscard]] f32 float_of(u32 word) noexcept {
    f32 value = 0.0F;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void put(u32* words, u32 at, Vec3 value, u32 w) noexcept {
    words[at + 0] = bits(value.x);
    words[at + 1] = bits(value.y);
    words[at + 2] = bits(value.z);
    words[at + 3] = w;
}

void put_matrix(u32* words, u32 at, const Mat4& m) noexcept {
    for (u32 column = 0; column < 4; ++column) {
        words[at + (column * 4U) + 0] = bits(m.columns[column].x);
        words[at + (column * 4U) + 1] = bits(m.columns[column].y);
        words[at + (column * 4U) + 2] = bits(m.columns[column].z);
        words[at + (column * 4U) + 3] = bits(m.columns[column].w);
    }
}

[[nodiscard]] u32 groups(u32 count) noexcept {
    const u32 needed = (count + kGroupSize - 1U) / kGroupSize;
    return needed == 0 ? 1 : needed;
}

/// `floor_div` in radiance_cache.cpp: the clipmap cell a coordinate falls in.
[[nodiscard]] i32 floor_div(f32 value, f32 divisor) noexcept {
    return static_cast<i32>(std::floor(value / divisor));
}

[[nodiscard]] u32 wrap(i32 value, i32 modulus) noexcept {
    return static_cast<u32>(((value % modulus) + modulus) % modulus);
}

}  // namespace

GpuRadianceCache::~GpuRadianceCache() {
    destroy();
}

// --- Creation ------------------------------------------------------------------------------------

Status GpuRadianceCache::create(Allocator& allocator, rhi::Device& device, GpuGiScene& scene,
                                const GpuRadianceCacheDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is already created");
    }
    if (scene.device_ != &device) {
        return fail(ErrorCode::InvalidArgument,
                    "the device radiance cache gathers through a GI scene created on the same "
                    "device; create the scene first");
    }
    if (desc.max_probes == 0 || desc.max_selection == 0 || desc.max_rays_per_probe == 0 ||
        desc.max_rays_per_probe > kMaxProbeRays || desc.max_queries == 0 ||
        desc.max_tier_rays == 0 || desc.max_screen_pixels == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuRadianceCacheDescription: every capacity must be non-zero, and at most "
                    "kMaxProbeRays rays a probe; the buffers are sized once and cannot grow");
    }
    allocator_ = &allocator;
    device_ = &device;
    scene_ = &scene;
    desc_ = desc;
    hardware_pipelines_ = device.capabilities().has(rhi::Capability::RayTracing);
    Status made = create_layouts();
    if (made) {
        made = create_pipelines();
    }
    if (made) {
        made = create_buffers();
    }
    if (made) {
        made = write_descriptors();
    }
    if (made) {
        made = revisions_.resize(desc.max_probes);
    }
    if (made) {
        made = seen_.resize(desc.max_probes);
    }
    if (made && device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "gi stage 3 timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 2;
        Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer);
        if (pool) {
            timer_ = *pool;
        }
    }
    if (!made) {
        destroy();
        return made;
    }
    for (u32 index = 0; index < desc.max_probes; ++index) {
        revisions_[index] = 0;
        seen_[index] = 0;
    }
    std::memset(constants_, 0, sizeof(constants_));
    constants_[kWordTier + 0] = bits(1.0F);
    constants_[kWordTier + 1] = bits(kSoftwareBaseConfidence);
    constants_[kWordTier + 2] = bits(kHardwareBaseConfidence);
    write_constants();
    return ok();
}

Status GpuRadianceCache::create_layouts() noexcept {
    rhi::DescriptorBinding bindings[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription probe;
    probe.name = "gi probe set";
    probe.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(probe);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    probe_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(GpuGiScene::Dispatch)};
    const rhi::DescriptorSetLayoutHandle software_sets[2] = {scene_->set_layout_, probe_layout_};
    rhi::PipelineLayoutDescription software;
    software.name = "gi probe layout";
    software.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(software_sets, 2);
    software.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> pipeline_layout =
        device_->create_pipeline_layout(software);
    if (!pipeline_layout) {
        return make_unexpected(pipeline_layout.error());
    }
    software_pipeline_layout_ = *pipeline_layout;

    if (!hardware_pipelines_) {
        return ok();
    }
    // `GiHardwareSet`: the top level, then the instance table and the two pools.
    rhi::DescriptorBinding hardware_bindings[4] = {};
    for (u32 index = 0; index < 4; ++index) {
        hardware_bindings[index].binding = index;
        hardware_bindings[index].kind = index == 0 ? rhi::DescriptorKind::AccelerationStructure
                                                   : rhi::DescriptorKind::StorageBuffer;
        hardware_bindings[index].count = 1;
        hardware_bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription hardware;
    hardware.name = "gi hardware set";
    hardware.bindings = Span<const rhi::DescriptorBinding>(hardware_bindings, 4);
    layout = device_->create_descriptor_set_layout(hardware);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    hardware_layout_ = *layout;
    const rhi::DescriptorSetLayoutHandle hardware_sets[3] = {scene_->set_layout_, probe_layout_,
                                                             hardware_layout_};
    rhi::PipelineLayoutDescription with_hardware;
    with_hardware.name = "gi hardware layout";
    with_hardware.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(hardware_sets, 3);
    with_hardware.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    pipeline_layout = device_->create_pipeline_layout(with_hardware);
    if (!pipeline_layout) {
        return make_unexpected(pipeline_layout.error());
    }
    hardware_pipeline_layout_ = *pipeline_layout;
    return ok();
}

Status GpuRadianceCache::create_pipelines() noexcept {
    struct Source {
        const char* name;
        Span<const u32> spirv;
        const char* msl;
        usize msl_size;
        const char* entry;
    };
#define CY_GI_SOURCE(label, stem, entry)                                             \
    Source {                                                                         \
        label, {k##stem##Spirv, sizeof(k##stem##Spirv) / sizeof(u32)}, k##stem##Msl, \
            sizeof(k##stem##Msl) - 1, entry                                          \
    }
    const Source sources[kPipelineCount] = {
        CY_GI_SOURCE("gi gather probes", GiGatherProbes, "cyGiGatherProbes"),
        CY_GI_SOURCE("gi sample probes", GiSampleProbes, "cyGiSampleProbes"),
        CY_GI_SOURCE("gi screen hzb", GiBuildScreenHzb, "cyGiBuildScreenHzb"),
        CY_GI_SOURCE("gi trace screen", GiTraceScreen, "cyGiTraceScreen"),
        CY_GI_SOURCE("gi trace software", GiTraceSoftware, "cyGiTraceSoftware"),
        CY_GI_SOURCE("gi trace tiered", GiTraceTiered, "cyGiTraceTiered"),
        CY_GI_SOURCE("gi gather probes hardware", GiGatherProbesHardware,
                     "cyGiGatherProbesHardware"),
        CY_GI_SOURCE("gi trace hardware", GiTraceHardware, "cyGiTraceHardware"),
        CY_GI_SOURCE("gi trace tiered hardware", GiTraceTieredHardware, "cyGiTraceTieredHardware"),
    };
#undef CY_GI_SOURCE
    const u32 count =
        hardware_pipelines_ ? static_cast<u32>(kPipelineCount) : static_cast<u32>(kFirstHardware);
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
        pipeline.layout =
            which >= kFirstHardware ? hardware_pipeline_layout_ : software_pipeline_layout_;
        pipeline.shader = shaders_[which];
        pipeline.workgroup_size[0] = kGroupSize;
        Expected<rhi::ComputePipelineHandle, Error> created =
            device_->create_compute_pipeline(pipeline);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipelines_[which] = *created;
    }
    return ok();
}

Status GpuRadianceCache::create_buffers() noexcept {
    // Everything the host writes is Upload and mapped, written in place; the four outputs the host
    // reads back are device-local and copied into Readback buffers by declared transfers. The probe
    // records and payloads are written by both: the host for a probe whose revision moved, the
    // gather for the probes it gathers.
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
    };
    const auto storage = rhi::BufferUsage::Storage;
    const auto output = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const auto host = rhi::BufferUsage::TransferDestination;
    const u64 probes = desc_.max_probes;
    const u64 levels_cells = static_cast<u64>(kMaxProbeLevels) * 4096U;
    u64 hzb_texels = 0;
    for (u64 texels = desc_.max_screen_pixels; texels > 1; texels = (texels + 3) / 4) {
        hzb_texels += texels + 64;
    }
    hzb_texels += 64;
    const Request requests[kBufferCount] = {
        {"gi probe constants", sizeof(constants_), storage, rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probes", probes * kProbeVec4s * kVec4, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe payloads", probes * gi::kMaxProbeFloats * 4U, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe selection", static_cast<u64>(desc_.max_selection) * 4U, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe directions", static_cast<u64>(desc_.max_rays_per_probe) * 2U * kVec4, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe results", desc_.max_selection * kResultVec4s * kVec4, output,
         rhi::MemoryUse::DeviceLocal},
        {"gi probe layout", levels_cells * 4U, storage, rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe queries", desc_.max_queries * kQueryVec4s * kVec4, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe samples", desc_.max_queries * kVec4, output, rhi::MemoryUse::DeviceLocal},
        {"gi tier rays", desc_.max_tier_rays * kTierRayVec4s * kVec4, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi tier results", desc_.max_tier_rays * kTierResultVec4s * kVec4, output,
         rhi::MemoryUse::DeviceLocal},
        {"gi screen depth", static_cast<u64>(desc_.max_screen_pixels) * 4U, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi screen colour", desc_.max_screen_pixels * kVec4, storage,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi screen hzb", hzb_texels * 4U, storage, rhi::MemoryUse::DeviceLocal},
        // Host-visible, so a batch zeroes it in place before it is recorded.
        {"gi tier counters", static_cast<u64>(kCounterCount) * 4U, output,
         rhi::MemoryUse::HostVisibleDeviceLocal},
        {"gi probe results host", desc_.max_selection * kResultVec4s * kVec4, host,
         rhi::MemoryUse::Readback},
        {"gi probe samples host", desc_.max_queries * kVec4, host, rhi::MemoryUse::Readback},
        {"gi tier results host", desc_.max_tier_rays * kTierResultVec4s * kVec4, host,
         rhi::MemoryUse::Readback},
        {"gi tier counters host", static_cast<u64>(kCounterCount) * 4U, host,
         rhi::MemoryUse::Readback},
    };
    for (u32 index = 0; index < kBufferCount; ++index) {
        rhi::BufferDescription description;
        description.name = requests[index].name;
        description.size = requests[index].size;
        description.usage = requests[index].usage;
        description.memory = requests[index].memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        buffers_[index] = *buffer;
        sizes_[index] = requests[index].size;
        if (requests[index].memory == rhi::MemoryUse::HostVisibleDeviceLocal) {
            if (void* pointer = device_->buffer_mapped_pointer(*buffer); pointer != nullptr) {
                // The layout reads "no probe" until it is written; everything else reads zero.
                std::memset(pointer, index == kLayout ? 0xFF : 0,
                            static_cast<usize>(requests[index].size));
            }
        }
    }
    return ok();
}

Status GpuRadianceCache::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(probe_layout_, false);
    if (!set) {
        return make_unexpected(set.error());
    }
    probe_set_ = *set;
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = buffers_[index];
    }
    return device_->update_descriptor_set(probe_set_,
                                          Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

Status GpuRadianceCache::write_hardware_descriptors() noexcept {
    if (!hardware_pipelines_ || structures_ == nullptr || !structures_->created()) {
        return ok();
    }
    if (!hardware_set_) {
        Expected<rhi::DescriptorSetHandle, Error> set =
            device_->allocate_descriptor_set(hardware_layout_, false);
        if (!set) {
            return make_unexpected(set.error());
        }
        hardware_set_ = *set;
    }
    const rt_gpu::DeviceStructureBinding binding = structures_->binding();
    rhi::DescriptorWrite writes[4] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::AccelerationStructure;
    writes[0].acceleration_structure = binding.top;
    const rhi::BufferHandle buffers[3] = {binding.instances, binding.positions, binding.indices};
    for (u32 index = 0; index < 3; ++index) {
        writes[index + 1].binding = index + 1;
        writes[index + 1].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index + 1].buffer = buffers[index];
    }
    return device_->update_descriptor_set(hardware_set_,
                                          Span<const rhi::DescriptorWrite>(writes, 4));
}

void GpuRadianceCache::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (rhi::BufferHandle& buffer : buffers_) {
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
         {&software_pipeline_layout_, &hardware_pipeline_layout_}) {
        if (*layout) {
            device_->destroy_pipeline_layout(*layout);
        }
        *layout = {};
    }
    for (rhi::DescriptorSetLayoutHandle* layout : {&probe_layout_, &hardware_layout_}) {
        if (*layout) {
            device_->destroy_descriptor_set_layout(*layout);
        }
        *layout = {};
    }
    if (timer_) {
        device_->destroy_query_pool(timer_);
    }
    timer_ = {};
    probe_set_ = {};
    hardware_set_ = {};
    revisions_.clear();
    seen_.clear();
    handles_.clear();
    submitted_revisions_.clear();
    layout_valid_ = false;
    hardware_pipelines_ = false;
    device_ = nullptr;
    scene_ = nullptr;
    allocator_ = nullptr;
}

Status GpuRadianceCache::bind_hardware(const rt::AccelerationService* service,
                                       rt_gpu::DeviceStructures* structures) noexcept {
    service_ = service;
    structures_ = structures;
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is not created");
    }
    return write_hardware_descriptors();
}

gi::RadianceSource GpuRadianceCache::world_tier() const noexcept {
    // `HardwareTracer::available()`, with a device behind it: one predicate, so a device without
    // ray queries and a profile that switched them off are the same software path.
    const bool hardware = hardware_pipelines_ && hardware_set_ && service_ != nullptr &&
                          service_->active() && structures_ != nullptr && structures_->traceable();
    return hardware ? gi::RadianceSource::HardwareTrace : gi::RadianceSource::SoftwareTrace;
}

void* GpuRadianceCache::mapped(Binding binding) const noexcept {
    return device_ == nullptr ? nullptr : device_->buffer_mapped_pointer(buffers_[binding]);
}

void GpuRadianceCache::write_constants() noexcept {
    if (void* target = mapped(kConstants); target != nullptr) {
        std::memcpy(target, constants_, sizeof(constants_));
    }
}

void GpuRadianceCache::write_sky(const gi::SkyTerm& sky) noexcept {
    put(constants_, kWordSkyZenith, sky.zenith, bits(sky.intensity));
    put(constants_, kWordSkyHorizon, sky.horizon, 0U);
    put(constants_, kWordSkyGround, sky.ground, 0U);
}

// --- The mirror ----------------------------------------------------------------------------------

void GpuRadianceCache::write_layout(const gi::RadianceCache& cache) noexcept {
    const gi::ProbeCacheSettings& settings = cache.settings();
    const auto half = static_cast<i32>(settings.half_extent_probes);
    const i32 edge = (half * 2) + 1;
    const u32 cells = static_cast<u32>(edge * edge * edge);
    auto* layout = static_cast<u32*>(mapped(kLayout));
    std::memset(layout, 0xFF, static_cast<usize>(cells) * settings.levels * sizeof(u32));
    const Vec3 camera = cache.camera();
    for (u32 level = 0; level < settings.levels; ++level) {
        const f32 spacing = cache.level_spacing(level);
        centres_[level][0] = floor_div(camera.x, spacing);
        centres_[level][1] = floor_div(camera.y, spacing);
        centres_[level][2] = floor_div(camera.z, spacing);
        constants_[kWordCentres + (level * 4U) + 0] = static_cast<u32>(centres_[level][0]);
        constants_[kWordCentres + (level * 4U) + 1] = static_cast<u32>(centres_[level][1]);
        constants_[kWordCentres + (level * 4U) + 2] = static_cast<u32>(centres_[level][2]);
        constants_[kWordCentres + (level * 4U) + 3] = 0;
    }
    const Span<const gi::Probe> probes = cache.probes();
    for (u32 handle = 0; handle < probes.size(); ++handle) {
        const gi::Probe& probe = probes[handle];
        if (!probe.live || probe.level >= settings.levels) {
            continue;
        }
        // The same arithmetic `retire_outside_window` keys a probe's cell by, so a probe is found
        // exactly where the host's occupancy map holds it.
        const f32 spacing = cache.level_spacing(probe.level);
        const i32 cell[3] = {floor_div(probe.position.x, spacing),
                             floor_div(probe.position.y, spacing),
                             floor_div(probe.position.z, spacing)};
        const u32 index = (((wrap(cell[2], edge) * static_cast<u32>(edge)) + wrap(cell[1], edge)) *
                           static_cast<u32>(edge)) +
                          wrap(cell[0], edge);
        layout[(probe.level * cells) + index] = handle;
    }
    constants_[kWordGather + 2] = settings.levels;
    constants_[kWordGather + 3] = settings.half_extent_probes;
    constants_[kWordLayout + 0] = bits(settings.base_spacing_metres);
    constants_[kWordLayout + 2] = static_cast<u32>(edge);
    layout_valid_ = true;
}

Status GpuRadianceCache::mirror(const gi::RadianceCache& cache) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is not created");
    }
    const gi::ProbeCacheSettings& settings = cache.settings();
    const Span<const gi::Probe> probes = cache.probes();
    if (probes.size() > desc_.max_probes) {
        return fail(ErrorCode::OutOfRange,
                    "the radiance cache holds more probe slots than the device copy was created "
                    "for; raise max_probes");
    }
    const i32 edge = (static_cast<i32>(settings.half_extent_probes) * 2) + 1;
    if (settings.levels > kMaxProbeLevels || static_cast<u32>(edge * edge * edge) > 4096U) {
        return fail(ErrorCode::OutOfRange,
                    "the radiance cache's clipmap is larger than the device layout addresses: at "
                    "most eight levels of a 15-cell window");
    }
    const gi::EncodingTraits traits = gi::encoding_traits(settings.encoding);
    const u32 stride = traits.floats_per_probe;
    auto* records = static_cast<f32*>(mapped(kProbes));
    auto* payloads = static_cast<f32*>(mapped(kPayloads));
    u32 uploaded = 0;
    bool placement_moved =
        !layout_valid_ || constants_[kWordCounts + 0] != static_cast<u32>(settings.encoding);
    for (u32 handle = 0; handle < probes.size(); ++handle) {
        const gi::Probe& probe = probes[handle];
        if (seen_[handle] != 0 && revisions_[handle] == probe.revision) {
            continue;
        }
        // A revision moves at creation, retirement, invalidation and a seed. The first two are a
        // change of placement, which is what the layout is rebuilt on.
        placement_moved = true;
        f32* record = records + (static_cast<usize>(handle) * kProbeVec4s * 4U);
        record[0] = probe.position.x;
        record[1] = probe.position.y;
        record[2] = probe.position.z;
        record[3] = static_cast<f32>(probe.level);
        record[4] = float_of(static_cast<u32>(probe.last_update_frame & 0xFFFFFFFFULL));
        record[5] = probe.live && probe.valid ? 1.0F : 0.0F;
        record[6] = probe.live ? 1.0F : 0.0F;
        record[7] = 0.0F;
        for (u32 axis = 0; axis < 6; ++axis) {
            record[8 + axis] = probe.axis_distance[axis];
        }
        record[14] = 0.0F;
        record[15] = 0.0F;
        const Span<const f32> payload = cache.probe_payload(handle);
        std::memcpy(payloads + (static_cast<usize>(handle) * stride), payload.data(),
                    static_cast<usize>(stride) * sizeof(f32));
        revisions_[handle] = probe.revision;
        seen_[handle] = 1;
        uploaded += 1;
    }
    // THE WINDOW MOVES WITHOUT A REVISION when the camera crosses a cell with no probe created or
    // retired, so the centres are compared as well.
    const Vec3 camera = cache.camera();
    for (u32 level = 0; level < settings.levels && !placement_moved; ++level) {
        const f32 spacing = cache.level_spacing(level);
        placement_moved = centres_[level][0] != floor_div(camera.x, spacing) ||
                          centres_[level][1] != floor_div(camera.y, spacing) ||
                          centres_[level][2] != floor_div(camera.z, spacing);
    }
    submission_.probes_uploaded = uploaded;
    submission_.layout_rebuilt = placement_moved;
    if (placement_moved) {
        write_layout(cache);
    }
    constants_[kWordCounts + 0] = static_cast<u32>(settings.encoding);
    constants_[kWordCounts + 1] = stride;
    constants_[kWordGather + 1] = bits(settings.visibility_bias_metres);
    write_constants();
    return ok();
}

Status GpuRadianceCache::upload(const gi::RadianceCache& cache) noexcept {
    return mirror(cache);
}

void GpuRadianceCache::reset_mirror() noexcept {
    for (u8& seen : seen_) {
        seen = 0;
    }
    layout_valid_ = false;
}

// --- The gather ----------------------------------------------------------------------------------

u32 GpuRadianceCache::submit(const gi::RadianceCache& cache, Span<const u32> selected,
                             const gi::ProbeUpdateContext& context) noexcept {
    submission_ = GpuProbeSubmission{};
    submission_.frame = context.frame;
    handles_.clear();
    submitted_revisions_.clear();
    declared_ = false;
    error_ = Error{};
    if (device_ == nullptr || scene_ == nullptr || field_ == nullptr) {
        error_ = Error{ErrorCode::InvalidArgument,
                       "the device radiance cache is not created or has no field bound"};
        return 0;
    }
    // The scene the rays meet first: the field, and — for a host-shaded surface cache — the cards
    // and their radiance. A frame in which nothing moved writes nothing for the field.
    Expected<FieldUploadReport, Error> field = scene_->upload_field(*field_);
    if (!field) {
        error_ = field.error();
        return 0;
    }
    if (surfaces_ != nullptr) {
        Expected<CardUploadReport, Error> cards =
            scene_->upload_cards(surfaces_->pages(), surfaces_->lookup_radius());
        if (!cards) {
            error_ = cards.error();
            return 0;
        }
        if (Status radiance = scene_->upload_card_radiance(surfaces_->pages()); !radiance) {
            error_ = radiance.error();
            return 0;
        }
    }
    if (Status mirrored = mirror(cache); !mirrored) {
        error_ = mirrored.error();
        return 0;
    }

    const u32 rays = std::max(1U, context.rays_per_probe);
    if (rays > desc_.max_rays_per_probe) {
        error_ = Error{ErrorCode::OutOfRange,
                       "more rays per probe than the device radiance cache was created for"};
        return 0;
    }
    // The frame's ray table, computed once: the host's own sequence, so both sides trace the same
    // directions to the last bit.
    auto* directions = static_cast<f32*>(mapped(kDirections));
    const u32 rotation = gi::probe_ray_rotation(context.frame);
    for (u32 index = 0; index < rays; ++index) {
        const Vec3 direction = gi::probe_ray_direction(index, rays, rotation);
        const Vec3 unit = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
        f32* entry = directions + (static_cast<usize>(index) * 8U);
        entry[0] = direction.x;
        entry[1] = direction.y;
        entry[2] = direction.z;
        entry[3] = 0.0F;
        entry[4] = unit.x;
        entry[5] = unit.y;
        entry[6] = unit.z;
        entry[7] = 0.0F;
    }

    // A PREFIX when the device was sized for fewer probes than the budget: the selection is in
    // priority order — starved probes first — so what is dropped is the least urgent work.
    const auto accepted = static_cast<u32>(std::min<usize>(selected.size(), desc_.max_selection));
    auto* list = static_cast<u32*>(mapped(kSelection));
    const Span<const gi::Probe> probes = cache.probes();
    for (u32 index = 0; index < accepted; ++index) {
        const u32 handle = selected[index];
        list[index] = handle;
        if (!handles_.push_back(handle) ||
            !submitted_revisions_.push_back(probes[handle].revision)) {
            error_ = Error{ErrorCode::OutOfMemory, "the submission list could not grow"};
            handles_.clear();
            submitted_revisions_.clear();
            return 0;
        }
    }
    constants_[kWordCounts + 2] = rays;
    constants_[kWordCounts + 3] = accepted;
    constants_[kWordGather + 0] = bits(context.max_ray_distance_metres);
    write_sky(context.sky);
    write_constants();

    submission_.probes = accepted;
    submission_.rays_per_probe = rays;
    submission_.tier = world_tier();
    return accepted;
}

ResourceId GpuRadianceCache::import(RenderGraph& graph, Binding binding) noexcept {
    static constexpr const char* kNames[kBufferCount] = {
        "gi probe constants",    "gi probes",           "gi probe payloads",
        "gi probe selection",    "gi probe directions", "gi probe results",
        "gi probe layout",       "gi probe queries",    "gi probe samples",
        "gi tier rays",          "gi tier results",     "gi screen depth",
        "gi screen colour",      "gi screen hzb",       "gi tier counters",
        "gi probe results host", "gi samples host",     "gi tier results host",
        "gi tier counters host",
    };
    BufferRequest request;
    request.name = kNames[binding];
    request.size = sizes_[binding];
    return graph.import_buffer_once(request, buffers_[binding]);
}

void GpuRadianceCache::read_scene(RenderGraph& graph, PassBuilder& pass, bool hardware) noexcept {
    using B = GpuGiScene;
    for (const B::Binding binding : {B::kConstants, B::kPageTable, B::kBricks, B::kCards,
                                     B::kCardState, B::kGridRanges, B::kGridItems}) {
        pass.read(scene_->import(graph, binding), Access::ComputeStorageRead);
    }
    pass.read(import(graph, kConstants), Access::ComputeStorageRead);
    if (hardware && structures_ != nullptr) {
        pass.read(structures_->import_top(graph), Access::ComputeAccelerationStructureRead);
        const rt_gpu::DeviceStructureBinding binding = structures_->binding();
        const rhi::BufferHandle buffers[3] = {binding.instances, binding.positions,
                                              binding.indices};
        const char* names[3] = {"rt instance table", "rt vertex pool", "rt index pool"};
        for (u32 index = 0; index < 3; ++index) {
            BufferRequest request;
            request.name = names[index];
            request.size = device_->buffer_description(buffers[index]) != nullptr
                               ? device_->buffer_description(buffers[index])->size
                               : 0;
            pass.read(graph.import_buffer_once(request, buffers[index]),
                      Access::ComputeStorageRead);
        }
    }
}

void GpuRadianceCache::declare_copy(RenderGraph& graph, Copy& copy, const char* name) noexcept {
    const ResourceId source = import(graph, copy.source);
    const ResourceId destination = import(graph, copy.destination);
    graph.add_pass(name, QueueKind::Graphics)
        .read(source, Access::TransferRead)
        .write(destination, Access::TransferWrite)
        .record(&record_copy, &copy);
    graph.add_pass(name, QueueKind::Graphics).read(destination, Access::HostRead).side_effect();
}

Status GpuRadianceCache::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr || submission_.probes == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "nothing was submitted; a gather over an empty selection would report success "
                    "and write nothing");
    }
    const bool hardware = submission_.tier == gi::RadianceSource::HardwareTrace;
    const auto frame = static_cast<u32>(submission_.frame & 0xFFFFFFFFULL);
    // One workgroup per probe: its lanes are the probe's rays.
    gather_ = Recording{this, hardware ? kGatherHardware : kGather,
                        GpuGiScene::Dispatch{submission_.probes, frame, {0, 0}}, timing_,
                        submission_.probes};
    PassBuilder pass = graph.add_pass("gi gather probes", QueueKind::Graphics);
    read_scene(graph, pass, hardware);
    pass.read(import(graph, kSelection), Access::ComputeStorageRead)
        .read(import(graph, kDirections), Access::ComputeStorageRead)
        .write(import(graph, kProbes), Access::ComputeStorageReadWrite)
        .write(import(graph, kPayloads), Access::ComputeStorageReadWrite)
        .write(import(graph, kResults), Access::ComputeStorageWrite)
        .record(&record_dispatch, &gather_);
    results_copy_ = Copy{this, kResults, kResultsHost,
                         static_cast<u64>(submission_.probes) * kResultVec4s * kVec4};
    declare_copy(graph, results_copy_, "gi probe results readback");
    if (Status status = graph.status(); !status) {
        return status;
    }
    declared_ = true;
    return ok();
}

u32 GpuRadianceCache::retire(gi::RadianceCache& cache) noexcept {
    if (device_ == nullptr || handles_.empty()) {
        return 0;
    }
    if (!declared_) {
        // Submitted and never declared: nothing ran, so nothing comes back.
        handles_.clear();
        submitted_revisions_.clear();
        return 0;
    }
    const auto* results = static_cast<const f32*>(mapped(kResultsHost));
    if (results == nullptr) {
        return 0;
    }
    const u32 stride = gi::encoding_traits(cache.settings().encoding).floats_per_probe;
    u32 written = 0;
    for (usize index = 0; index < handles_.size(); ++index) {
        const f32* record = results + (index * kResultVec4s * 4U);
        gi::ProbeGatherResult result;
        result.probe = handles_[index];
        result.revision = submitted_revisions_[index];
        result.payload = Span<const f32>(record, stride);
        for (u32 axis = 0; axis < 6; ++axis) {
            result.axis_distance[axis] = record[48 + axis];
        }
        result.error = record[54];
        result.frame = submission_.frame;
        if (cache.apply(result)) {
            written += 1;
        }
    }
    handles_.clear();
    submitted_revisions_.clear();
    declared_ = false;
    return written;
}

// --- The query -----------------------------------------------------------------------------------

Status GpuRadianceCache::set_queries(Span<const GpuProbeQuery> queries) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is not created");
    }
    if (queries.empty() || queries.size() > desc_.max_queries) {
        return fail(ErrorCode::OutOfRange,
                    "a query batch holds between one and max_queries points");
    }
    auto* target = static_cast<f32*>(mapped(kQueries));
    for (usize index = 0; index < queries.size(); ++index) {
        f32* entry = target + (index * kQueryVec4s * 4U);
        entry[0] = queries[index].position.x;
        entry[1] = queries[index].position.y;
        entry[2] = queries[index].position.z;
        entry[3] = 0.0F;
        entry[4] = queries[index].normal.x;
        entry[5] = queries[index].normal.y;
        entry[6] = queries[index].normal.z;
        entry[7] = 0.0F;
    }
    query_count_ = static_cast<u32>(queries.size());
    return ok();
}

Status GpuRadianceCache::declare_sample(RenderGraph& graph, u64 frame) noexcept {
    if (device_ == nullptr || query_count_ == 0 || !layout_valid_) {
        return fail(ErrorCode::InvalidArgument,
                    "a query needs a created cache, an uploaded probe set and a batch from "
                    "set_queries()");
    }
    sample_ = Recording{
        this, kSample,
        GpuGiScene::Dispatch{query_count_, static_cast<u32>(frame & 0xFFFFFFFFULL), {0, 0}},
        timing_};
    graph.add_pass("gi sample probes", QueueKind::Graphics)
        .read(import(graph, kConstants), Access::ComputeStorageRead)
        .read(import(graph, kProbes), Access::ComputeStorageRead)
        .read(import(graph, kPayloads), Access::ComputeStorageRead)
        .read(import(graph, kLayout), Access::ComputeStorageRead)
        .read(import(graph, kQueries), Access::ComputeStorageRead)
        .write(import(graph, kSamples), Access::ComputeStorageWrite)
        .record(&record_dispatch, &sample_);
    samples_copy_ = Copy{this, kSamples, kSamplesHost, static_cast<u64>(query_count_) * kVec4};
    declare_copy(graph, samples_copy_, "gi probe samples readback");
    return graph.status();
}

Status GpuRadianceCache::read_back_samples(Span<gi::RadianceSample> out) const noexcept {
    if (device_ == nullptr || out.size() > query_count_) {
        return fail(ErrorCode::InvalidArgument, "the sample span is larger than the last batch");
    }
    const auto* samples = static_cast<const f32*>(mapped(kSamplesHost));
    if (samples == nullptr) {
        return fail(ErrorCode::Internal, "the probe samples readback buffer is not mapped");
    }
    for (usize index = 0; index < out.size(); ++index) {
        const f32* entry = samples + (index * 4U);
        out[index].radiance = Vec3{entry[0], entry[1], entry[2]};
        out[index].confidence = entry[3];
        out[index].source = gi::RadianceSource::RadianceCache;
        out[index].hit_distance = -1.0F;
    }
    return ok();
}

// --- Recording -----------------------------------------------------------------------------------

void GpuRadianceCache::record_dispatch(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const Recording*>(user);
    GpuRadianceCache& self = *recording->self;
    const bool hardware = recording->pipeline >= kFirstHardware;
    const rhi::PipelineLayoutHandle layout =
        hardware ? self.hardware_pipeline_layout_ : self.software_pipeline_layout_;
    rhi::CommandBuffer& commands = *context.commands;
    const bool timed = recording->timed && self.timer_;
    if (timed) {
        commands.reset_queries(self.timer_, 0, 2);
        commands.write_timestamp(self.timer_, 0);
    }
    commands.bind_compute_pipeline(self.pipelines_[recording->pipeline]);
    const rhi::DescriptorSetHandle sets[3] = {self.scene_->set_, self.probe_set_,
                                              self.hardware_set_};
    commands.bind_descriptor_sets(layout, 0,
                                  Span<const rhi::DescriptorSetHandle>(sets, hardware ? 3U : 2U));
    commands.push_constants(layout, rhi::ShaderStage::Compute, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(&recording->dispatch),
                                           sizeof(GpuGiScene::Dispatch)));
    commands.dispatch(
        recording->groups != 0 ? recording->groups : groups(recording->dispatch.count), 1, 1);
    if (timed) {
        commands.write_timestamp(self.timer_, 1);
        self.timed_ = true;
    }
}

void GpuRadianceCache::record_copy(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Copy*>(user);
    const rhi::BufferCopy region{0, 0, copy->bytes};
    context.commands->copy_buffer(copy->self->buffers_[copy->source],
                                  copy->self->buffers_[copy->destination],
                                  Span<const rhi::BufferCopy>(&region, 1));
}

GpuDispatchTime GpuRadianceCache::last_time() const noexcept {
    GpuDispatchTime time;
    if (device_ == nullptr || !timer_ || !timed_) {
        return time;
    }
    u64 stamps[2] = {};
    Expected<u32, Error> read = device_->read_query_results(timer_, 0, 2, Span<u64>(stamps, 2));
    if (!read || *read != 2 || stamps[1] < stamps[0]) {
        return time;
    }
    time.milliseconds = static_cast<f64>(stamps[1] - stamps[0]) / 1.0e6;
    time.valid = true;
    return time;
}

// --- The tiers -----------------------------------------------------------------------------------

Status GpuTierTracer::set_view(const gi::ScreenView* view) noexcept {
    GpuRadianceCache& cache = *cache_;
    if (cache.device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is not created");
    }
    u32* constants = cache.constants_;
    if (view == nullptr || view->width == 0 || view->height == 0 || view->depth.empty() ||
        view->colour.empty()) {
        // No screen: `ScreenTracer::trace` answers nothing, which is what a probe gather's view is.
        constants[kWordScreen + 0] = 0;
        constants[kWordScreen + 1] = 0;
        screen_ = false;
        cache.write_constants();
        return ok();
    }
    const u64 pixels = static_cast<u64>(view->width) * view->height;
    if (pixels > cache.desc_.max_screen_pixels || view->depth.size() < pixels ||
        view->colour.size() < pixels) {
        return fail(ErrorCode::OutOfRange,
                    "the screen is larger than the device tiers were created for, or its buffers "
                    "are shorter than its extent");
    }
    std::memcpy(cache.mapped(GpuRadianceCache::kScreenDepth), view->depth.data(),
                static_cast<usize>(pixels) * sizeof(f32));
    auto* colour = static_cast<f32*>(cache.mapped(GpuRadianceCache::kScreenColour));
    for (u64 pixel = 0; pixel < pixels; ++pixel) {
        colour[(pixel * 4U) + 0] = view->colour[pixel].x;
        colour[(pixel * 4U) + 1] = view->colour[pixel].y;
        colour[(pixel * 4U) + 2] = view->colour[pixel].z;
        colour[(pixel * 4U) + 3] = 0.0F;
    }
    constants[kWordScreen + 0] = view->width;
    constants[kWordScreen + 1] = view->height;
    constants[kWordScreen + 2] = view->max_steps;
    constants[kWordScreen + 3] = bits(view->thickness_metres);
    put_matrix(constants, kWordWorldToView, view->world_to_view);
    put_matrix(constants, kWordWorldToClip, view->world_to_clip);

    // The pyramid: level 0 is the screen, each level above halves it (rounding up) until a level is
    // a single texel or the table is full.
    u32 width = view->width;
    u32 height = view->height;
    u32 offset = 0;
    hzb_levels_ = 0;
    while (hzb_levels_ < kMaxScreenLevels) {
        constants[kWordHzbLevels + (hzb_levels_ * 4U) + 0] = offset;
        constants[kWordHzbLevels + (hzb_levels_ * 4U) + 1] = width;
        constants[kWordHzbLevels + (hzb_levels_ * 4U) + 2] = height;
        constants[kWordHzbLevels + (hzb_levels_ * 4U) + 3] = 0;
        offset += width * height;
        hzb_levels_ += 1;
        if (width == 1 && height == 1) {
            break;
        }
        width = (width + 1U) / 2U;
        height = (height + 1U) / 2U;
    }
    constants[kWordScreenHzb + 0] = hzb_levels_;
    screen_ = true;
    hzb_dirty_ = true;
    cache.write_constants();
    return ok();
}

void GpuTierTracer::set_config(const gi::TieredTracer::Config& config) noexcept {
    u32* constants = cache_->constants_;
    constants[kWordScreenHzb + 2] = bits(config.max_distance_metres);
    constants[kWordScreenHzb + 3] = bits(config.escalation_threshold);
    cache_->write_sky(config.sky);
    cache_->write_constants();
}

void GpuTierTracer::set_budget(const gi::TraceBudget& budget) noexcept {
    cache_->constants_[kWordBudget + 0] = budget.world_rays;
    cache_->constants_[kWordBudget + 1] = budget.escalations;
    cache_->write_constants();
}

void GpuTierTracer::set_cache_frame(u64 frame) noexcept {
    cache_->constants_[kWordBudget + 3] = static_cast<u32>(frame & 0xFFFFFFFFULL);
    cache_->write_constants();
}

Status GpuTierTracer::set_rays(Span<const GpuTierRay> rays) noexcept {
    GpuRadianceCache& cache = *cache_;
    if (cache.device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device radiance cache is not created");
    }
    if (rays.empty() || rays.size() > cache.desc_.max_tier_rays) {
        return fail(ErrorCode::OutOfRange, "a tier batch holds between one and max_tier_rays rays");
    }
    auto* target = static_cast<f32*>(cache.mapped(GpuRadianceCache::kTierRays));
    for (usize index = 0; index < rays.size(); ++index) {
        f32* entry = target + (index * kTierRayVec4s * 4U);
        entry[0] = rays[index].origin.x;
        entry[1] = rays[index].origin.y;
        entry[2] = rays[index].origin.z;
        entry[3] = rays[index].max_distance;
        entry[4] = rays[index].direction.x;
        entry[5] = rays[index].direction.y;
        entry[6] = rays[index].direction.z;
        entry[7] = 0.0F;
    }
    ray_count_ = static_cast<u32>(rays.size());
    cache.constants_[kWordBudget + 2] = ray_count_;
    cache.write_constants();
    return ok();
}

Status GpuTierTracer::declare(RenderGraph& graph, GpuTierMode mode) noexcept {
    using C = GpuRadianceCache;
    C& cache = *cache_;
    if (cache.device_ == nullptr || ray_count_ == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a tier batch needs a created cache and rays from set_rays()");
    }
    const bool screen_mode = mode == GpuTierMode::Screen || mode == GpuTierMode::ScreenAccelerated;
    if (screen_mode && !screen_) {
        return fail(ErrorCode::InvalidArgument, "a screen batch needs a view from set_view()");
    }
    const bool accelerated = mode == GpuTierMode::ScreenAccelerated || mode == GpuTierMode::Tiered;
    cache.constants_[kWordScreenHzb + 1] = accelerated ? 1U : 0U;
    cache.write_constants();

    // The counters start each batch at zero, written in place like every other host input.
    std::memset(cache.mapped(C::kCounters), 0, static_cast<usize>(kCounterCount) * sizeof(u32));
    const ResourceId counters = cache.import(graph, C::kCounters);
    const ResourceId hzb = cache.import(graph, C::kScreenHzb);
    if (screen_ && accelerated && hzb_dirty_) {
        for (u32 level = 0; level < hzb_levels_; ++level) {
            const u32 width = cache.constants_[kWordHzbLevels + (level * 4U) + 1];
            const u32 height = cache.constants_[kWordHzbLevels + (level * 4U) + 2];
            hzb_[level] = C::Recording{&cache, C::kBuildHzb,
                                       GpuGiScene::Dispatch{width * height, level, {0, 0}}, false};
            PassBuilder pass = graph.add_pass("gi screen hzb", rhi::QueueKind::Graphics);
            pass.read(cache.import(graph, C::kConstants), Access::ComputeStorageRead)
                .read(cache.import(graph, C::kScreenDepth), Access::ComputeStorageRead);
            if (level == 0) {
                pass.write(hzb, Access::ComputeStorageWrite);
            } else {
                pass.write(hzb, Access::ComputeStorageReadWrite);
            }
            pass.record(&C::record_dispatch, &hzb_[level]);
        }
        hzb_dirty_ = false;
    }

    C::Pipeline pipeline = C::kTraceScreen;
    const bool hardware = cache.world_tier() == gi::RadianceSource::HardwareTrace;
    if (mode == GpuTierMode::World) {
        pipeline = hardware ? C::kTraceHardware : C::kTraceSoftware;
    } else if (mode == GpuTierMode::Tiered) {
        pipeline = hardware ? C::kTraceTieredHardware : C::kTraceTiered;
    }
    trace_ =
        C::Recording{&cache, pipeline, GpuGiScene::Dispatch{ray_count_, 0, {0, 0}}, cache.timing_};
    PassBuilder pass = graph.add_pass("gi trace tiers", rhi::QueueKind::Graphics);
    cache.read_scene(graph, pass, pipeline >= C::kFirstHardware);
    pass.read(cache.import(graph, C::kTierRays), Access::ComputeStorageRead)
        .read(cache.import(graph, C::kScreenDepth), Access::ComputeStorageRead)
        .read(cache.import(graph, C::kScreenColour), Access::ComputeStorageRead)
        .read(hzb, Access::ComputeStorageRead)
        .write(counters, Access::ComputeStorageReadWrite)
        .write(cache.import(graph, C::kTierResults), Access::ComputeStorageWrite)
        .record(&C::record_dispatch, &trace_);
    results_copy_ = C::Copy{&cache, C::kTierResults, C::kTierResultsHost,
                            static_cast<u64>(ray_count_) * kTierResultVec4s * kVec4};
    cache.declare_copy(graph, results_copy_, "gi tier results readback");
    counters_copy_ =
        C::Copy{&cache, C::kCounters, C::kCountersHost, static_cast<u64>(kCounterCount) * 4U};
    cache.declare_copy(graph, counters_copy_, "gi tier counters readback");
    return graph.status();
}

Status GpuTierTracer::read_back(Span<gi::RadianceSample> out,
                                GpuTierCounters& counters) const noexcept {
    const GpuRadianceCache& cache = *cache_;
    if (cache.device_ == nullptr || out.size() > ray_count_) {
        return fail(ErrorCode::InvalidArgument, "the sample span is larger than the last batch");
    }
    const auto* results = static_cast<const f32*>(cache.mapped(GpuRadianceCache::kTierResultsHost));
    const auto* words = static_cast<const u32*>(cache.mapped(GpuRadianceCache::kCountersHost));
    if (results == nullptr || words == nullptr) {
        return fail(ErrorCode::Internal, "the tier readback buffers are not mapped");
    }
    for (usize index = 0; index < out.size(); ++index) {
        const f32* entry = results + (index * kTierResultVec4s * 4U);
        out[index].radiance = Vec3{entry[0], entry[1], entry[2]};
        out[index].confidence = entry[3];
        out[index].hit_distance = entry[4];
        u32 source = 0;
        std::memcpy(&source, &entry[5], sizeof(source));
        out[index].source = static_cast<gi::RadianceSource>(source);
    }
    counters = GpuTierCounters{};
    counters.escalations = words[1];
    counters.budget_refusals = words[2];
    counters.world_rays = words[0] - std::min(words[0], words[2]);
    counters.depth_reads = words[3];
    counters.screen_rays = words[4];
    counters.software_rays = words[5];
    counters.hardware_rays = words[6];
    counters.sky_answers = words[7];
    return ok();
}

}  // namespace cy::rendering::gi_gpu
