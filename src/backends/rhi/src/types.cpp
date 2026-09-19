// The format table and the enumerator spellings. Task 2.1.1.
//
// Everything here is data the engine knows without a device. What a *particular* device can do with
// a format is a capability query (capabilities.h) and is answered by the backend; the size of a
// block and whether it carries depth are properties of the format itself, and asking a device for
// them would mean an engine that cannot compute an upload size before it has one.

#include <cy/backends/rhi/types.h>

#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/pipeline.h>

namespace cy::rhi {
namespace {

constexpr u32 kFormatCount = static_cast<u32>(Format::Count);

// name, bytes per block, block w, block h, depth, stencil, srgb, compressed
constexpr FormatInfo kFormatTable[kFormatCount] = {
    {"Undefined", 0, 1, 1, false, false, false, false},

    {"R8Unorm", 1, 1, 1, false, false, false, false},
    {"R8Uint", 1, 1, 1, false, false, false, false},
    {"Rg8Unorm", 2, 1, 1, false, false, false, false},
    {"Rgba8Unorm", 4, 1, 1, false, false, false, false},
    {"Rgba8Srgb", 4, 1, 1, false, false, true, false},
    {"Bgra8Unorm", 4, 1, 1, false, false, false, false},
    {"Bgra8Srgb", 4, 1, 1, false, false, true, false},

    {"R16Uint", 2, 1, 1, false, false, false, false},
    {"R16Sfloat", 2, 1, 1, false, false, false, false},
    {"Rg16Sfloat", 4, 1, 1, false, false, false, false},
    {"Rgba16Sfloat", 8, 1, 1, false, false, false, false},

    {"R32Uint", 4, 1, 1, false, false, false, false},
    {"R32Sint", 4, 1, 1, false, false, false, false},
    {"R32Sfloat", 4, 1, 1, false, false, false, false},
    {"Rg32Sfloat", 8, 1, 1, false, false, false, false},
    {"Rgb32Sfloat", 12, 1, 1, false, false, false, false},
    {"Rgba32Sfloat", 16, 1, 1, false, false, false, false},

    {"Rgb10A2Unorm", 4, 1, 1, false, false, false, false},
    {"B10G11R11Ufloat", 4, 1, 1, false, false, false, false},

    {"D16Unorm", 2, 1, 1, true, false, false, false},
    {"D32Sfloat", 4, 1, 1, true, false, false, false},
    {"D24UnormS8Uint", 4, 1, 1, true, true, false, false},
    {"D32SfloatS8Uint", 8, 1, 1, true, true, false, false},

    // Block-compressed: 4x4 texels per block, 8 or 16 bytes depending on the family.
    {"Bc1RgbaUnorm", 8, 4, 4, false, false, false, true},
    {"Bc1RgbaSrgb", 8, 4, 4, false, false, true, true},
    {"Bc3Unorm", 16, 4, 4, false, false, false, true},
    {"Bc3Srgb", 16, 4, 4, false, false, true, true},
    {"Bc4Unorm", 8, 4, 4, false, false, false, true},
    {"Bc5Unorm", 16, 4, 4, false, false, false, true},
    {"Bc6HUfloat", 16, 4, 4, false, false, false, true},
    {"Bc7Unorm", 16, 4, 4, false, false, false, true},
    {"Bc7Srgb", 16, 4, 4, false, false, true, true},
};

static_assert(sizeof(kFormatTable) / sizeof(kFormatTable[0]) == kFormatCount,
              "every Format enumerator needs a row, in enumerator order");

constexpr const char* kQueueKindNames[kQueueKindCount] = {"graphics", "async-compute", "transfer"};

constexpr const char* kImageUseNames[static_cast<u32>(ImageUse::Count)] = {"Undefined",
                                                                           "Storage",
                                                                           "ColorAttachment",
                                                                           "DepthStencilAttachment",
                                                                           "DepthStencilReadOnly",
                                                                           "SampledRead",
                                                                           "TransferSource",
                                                                           "TransferDestination",
                                                                           "Presentable"};

constexpr const char* kCapabilityNames[kCapabilityCount] = {
    "ComputeShaders",
    "GeometryShaders",
    "TessellationShaders",
    "MeshShaders",
    "AsyncCompute",
    "DedicatedTransferQueue",
    "Bindless",
    "BindlessPartiallyBound",
    "DescriptorIndexingNonUniform",
    "BufferDeviceAddress",
    "SparseResources",
    "ParallelPassRecording",
    "DynamicRendering",
    "Multiview",
    "VariableRateShading",
    "RayTracing",
    "ConservativeRasterisation",
    "SubgroupBallot",
    "SubgroupArithmetic",
    "ShaderInt64Atomics",
    "ShaderFloat16",
    "TimestampQueries",
    "PipelineStatisticsQueries",
    "HostVisibleDeviceLocalMemory",
    "MemoryBudgetReporting",
    "MemoryPriority",
    "DebugMarkers",
    "DeviceFaultReporting",
};

constexpr const char* kGpuMemoryCategoryNames[] = {"persistent", "streaming", "upload", "readback",
                                                   "transient"};

/// Copy at most `capacity - 1` characters and always terminate. Used for the fixed-size name
/// buffers in DeviceCapabilities, which are truncated rather than allocated for the same reason
/// ScreenInfo::name is: a device name is a diagnostic, not a string the engine owns.
void copy_truncated(char* destination, usize capacity, const char* source) noexcept {
    if (capacity == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    usize index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

}  // namespace

const char* queue_kind_name(QueueKind queue) noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount ? kQueueKindNames[index] : "<invalid>";
}

const char* image_use_name(ImageUse use) noexcept {
    const auto index = static_cast<u32>(use);
    constexpr u32 count = static_cast<u32>(ImageUse::Count);
    return index < count ? kImageUseNames[index] : "<invalid>";
}

const char* shader_format_name(ShaderFormat format) noexcept {
    switch (format) {
        case ShaderFormat::Spirv:
            return "spirv";
        case ShaderFormat::Msl:
            return "msl";
        case ShaderFormat::MetalLibrary:
            return "metallib";
        case ShaderFormat::Dxil:
            return "dxil";
        case ShaderFormat::Count:
            break;
    }
    return "<invalid>";
}

const FormatInfo& format_info(Format format) noexcept {
    const auto index = static_cast<u32>(format);
    return kFormatTable[index < kFormatCount ? index : 0];
}

const char* format_name(Format format) noexcept {
    return format_info(format).name;
}

u64 format_byte_size(Format format, u32 width, u32 height) noexcept {
    const FormatInfo& info = format_info(format);
    if (info.bytes_per_block == 0) {
        return 0;
    }
    const u64 blocks_x = (static_cast<u64>(width) + info.block_width - 1) / info.block_width;
    const u64 blocks_y = (static_cast<u64>(height) + info.block_height - 1) / info.block_height;
    return blocks_x * blocks_y * info.bytes_per_block;
}

bool format_is_depth_stencil(Format format) noexcept {
    const FormatInfo& info = format_info(format);
    return info.has_depth || info.has_stencil;
}

const char* capability_name(Capability capability) noexcept {
    const auto index = static_cast<u32>(capability);
    return index < kCapabilityCount ? kCapabilityNames[index] : "<invalid>";
}

Format select_supported_format(const DeviceCapabilities& caps, Span<const Format> preferences,
                               FormatFeature required) noexcept {
    for (const Format candidate : preferences) {
        if (candidate == Format::Undefined) {
            continue;
        }
        if (has_feature(caps.format_features(candidate), required)) {
            return candidate;
        }
    }
    return Format::Undefined;
}

Format select_depth_stencil_format(const DeviceCapabilities& caps, Format preferred) noexcept {
    // The preference order is the engine's, written out rather than derived, so that reading it
    // answers "what will this become on a device that refuses it" without running anything.
    static constexpr Format kWithStencil[] = {Format::D32SfloatS8Uint, Format::D24UnormS8Uint};
    static constexpr Format kDepthOnly[] = {Format::D32Sfloat, Format::D16Unorm};

    const FormatInfo& wanted = format_info(preferred);
    if (!wanted.has_depth) {
        return Format::Undefined;
    }
    if (has_feature(caps.format_features(preferred), FormatFeature::DepthStencilAttachment)) {
        return preferred;
    }
    // STENCIL IS NEVER DROPPED. A caller that asked for a stencil aspect and got a depth-only
    // format back would lose every stencil test silently, which is worse than being told no.
    const Span<const Format> fallbacks =
        wanted.has_stencil ? Span<const Format>(kWithStencil) : Span<const Format>(kDepthOnly);
    return select_supported_format(caps, fallbacks, FormatFeature::DepthStencilAttachment);
}

bool device_reports_ray_tracing(const RayTracingObservation& observed) noexcept {
    // ONE LINE PER ANSWER THE DEVICE GAVE. A single `&&` chain would read the same and could not be
    // broken one answer at a time, and being able to break it one answer at a time is the whole
    // point: each line below is a distinct way for a backend to report a capability the device did
    // not claim, and `m11c:ray-tracing-capability-honest` is proven by deleting one of them.
    bool reported = true;
    reported = reported && observed.acceleration_structure_extension;
    reported = reported && observed.ray_query_extension;
    reported = reported && observed.deferred_host_operations_extension;
    reported = reported && observed.acceleration_structure_feature;
    reported = reported && observed.ray_query_feature;
    reported = reported && observed.enabled_on_the_device;
    return reported;
}

const char* backend_kind_name(BackendKind kind) noexcept {
    switch (kind) {
        case BackendKind::Null:
            return "null";
        case BackendKind::Vulkan:
            return "vulkan";
        case BackendKind::Metal:
            return "metal";
        case BackendKind::D3D12:
            return "d3d12";
    }
    return "<invalid>";
}

const char* descriptor_kind_name(DescriptorKind kind) noexcept {
    switch (kind) {
        case DescriptorKind::UniformBuffer:
            return "uniform-buffer";
        case DescriptorKind::StorageBuffer:
            return "storage-buffer";
        case DescriptorKind::SampledTexture:
            return "sampled-texture";
        case DescriptorKind::StorageTexture:
            return "storage-texture";
        case DescriptorKind::Sampler:
            return "sampler";
        case DescriptorKind::CombinedTextureSampler:
            return "combined-texture-sampler";
        case DescriptorKind::InputAttachment:
            return "input-attachment";
    }
    return "unknown";
}

const char* descriptor_model_name(DescriptorModel model) noexcept {
    return model == DescriptorModel::Bindless ? "bindless" : "compatibility";
}

const char* gpu_memory_category_name(GpuMemoryCategory category) noexcept {
    const auto index = static_cast<u32>(category);
    return index < kGpuMemoryCategoryCount ? kGpuMemoryCategoryNames[index] : "<invalid>";
}

void DeviceCapabilities::set_device_name(const char* name) noexcept {
    copy_truncated(device_name_, sizeof(device_name_), name);
}

void DeviceCapabilities::set_driver_version(const char* version) noexcept {
    copy_truncated(driver_version_, sizeof(driver_version_), version);
}

}  // namespace cy::rhi
