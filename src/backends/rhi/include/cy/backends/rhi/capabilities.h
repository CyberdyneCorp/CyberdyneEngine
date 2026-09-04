#pragma once
// The backend capability model. Task 2.1.3.
//
// `rhi-and-render-graph`: "Every optional capability SHALL be queryable... The renderer SHALL
// branch on capabilities, never on backend identity."
//
// That rule is why this is an enumerated set with a query rather than a struct of bools the
// renderer reads directly. A query has one shape at every call site, a missing capability has one
// answer, and a capability nobody has implemented yet has an enumerator rather than an assumption.
// A renderer that asked `if (backend == Vulkan)` would be right on the day it was written and wrong
// on the day a Vulkan device turned out not to have mesh shaders.
//
// The set is deliberately larger than what M3 implements, for the same reason DisplayServer's
// Feature set is: a capability with no enumerator is a capability that gets assumed instead.

#include <cy/backends/rhi/types.h>
#include <cy/core/base/types.h>

namespace cy::rhi {

enum class Capability : u16 {
    // Shader stages beyond vertex and fragment.
    ComputeShaders,
    GeometryShaders,
    TessellationShaders,
    MeshShaders,

    // Queues. AsyncCompute is the one the render graph's scheduling model branches on: without it
    // every pass folds onto the graphics queue and the same declarations produce one submit.
    AsyncCompute,
    DedicatedTransferQueue,

    // Resource models.
    Bindless,
    BindlessPartiallyBound,  // a descriptor array with holes, which a streaming table needs
    DescriptorIndexingNonUniform,
    BufferDeviceAddress,
    SparseResources,

    // Rendering features.
    DynamicRendering,  // no VkRenderPass objects; the M3 baseline requires it
    Multiview,         // the XR prerequisite (tests/render/README.md)
    VariableRateShading,
    RayTracing,
    ConservativeRasterisation,

    // Compute and subgroup features.
    SubgroupBallot,
    SubgroupArithmetic,
    ShaderInt64Atomics,
    ShaderFloat16,

    // Memory and timing.
    TimestampQueries,
    PipelineStatisticsQueries,
    HostVisibleDeviceLocalMemory,  // unified memory or resizable BAR: uploads skip the staging copy
    MemoryBudgetReporting,         // the device tells the engine how much of the heap is in use
    MemoryPriority,

    // Debugging. Reported rather than assumed: a shipping driver has neither.
    DebugMarkers,
    DeviceFaultReporting,  // a driver-reported reason on device loss

    Count,
};

inline constexpr u32 kCapabilityCount = static_cast<u32>(Capability::Count);

/// The enumerator's own spelling, for a diagnostic and for the capability report. Never null.
[[nodiscard]] const char* capability_name(Capability capability) noexcept;

/// What a device can do with one format. Queried per format because support is per format: a device
/// may sample R32Sfloat and refuse to blend it.
enum class FormatFeature : u8 {
    /// Supports nothing. Named rather than cast from zero: a device answering "no" for a format is
    /// an ordinary answer, and a zero-valued enumerator is what makes a default-initialised table
    /// of them mean that.
    None = 0,
    SampledImage = 1U << 0,
    StorageImage = 1U << 1,
    StorageImageAtomic = 1U << 2,
    ColorAttachment = 1U << 3,
    ColorAttachmentBlend = 1U << 4,
    DepthStencilAttachment = 1U << 5,
    BlitSource = 1U << 6,
    BlitDestination = 1U << 7,
};

[[nodiscard]] constexpr FormatFeature operator|(FormatFeature a, FormatFeature b) noexcept {
    return static_cast<FormatFeature>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has_feature(FormatFeature set, FormatFeature feature) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(feature)) != 0;
}

/// The numbers a device reports about itself, as opposed to the boolean capabilities above.
///
/// These are checked against the engine's own hard limits (types.h) when a device is created: a
/// device that cannot bind kMaxDescriptorSets sets is a device the engine refuses rather than one
/// it discovers a pipeline at a time.
struct DeviceLimits {
    u32 max_bound_descriptor_sets = 0;
    u32 max_push_constant_bytes = 0;
    u32 max_vertex_attributes = 0;
    u32 max_color_attachments = 0;
    u32 max_texture_dimension_2d = 0;
    u32 max_texture_array_layers = 0;
    u32 max_compute_workgroup_size[3] = {0, 0, 0};
    u32 max_compute_workgroup_invocations = 0;
    u32 subgroup_size = 0;
    u32 max_sampled_images_per_stage = 0;
    u32 max_storage_buffers_per_stage = 0;
    /// The alignment a uniform or storage buffer binding's offset must satisfy. Needed by the
    /// per-frame ring allocator, and a number every backend reports differently.
    u64 min_uniform_buffer_offset_alignment = 1;
    u64 min_storage_buffer_offset_alignment = 1;
    u64 optimal_buffer_copy_offset_alignment = 1;
    u64 non_coherent_atom_size = 1;
    f32 max_sampler_anisotropy = 1.0F;
    u64 timestamp_period_ns = 0;
};

/// Which backend answered. Reported for a log line and a crash artefact — never branched on. The
/// renderer branches on Capability; this exists so a bug report says which backend produced it.
enum class BackendKind : u8 {
    Null = 0,
    Vulkan,
    Metal,
    D3D12,
};

[[nodiscard]] const char* backend_kind_name(BackendKind kind) noexcept;

/// Everything a device says about itself. Filled by the backend at creation and immutable after.
class DeviceCapabilities {
public:
    DeviceCapabilities() = default;

    [[nodiscard]] bool has(Capability capability) const noexcept {
        const auto index = static_cast<u32>(capability);
        return index < kCapabilityCount && supported_[index];
    }

    void set(Capability capability, bool supported) noexcept {
        const auto index = static_cast<u32>(capability);
        if (index < kCapabilityCount) {
            supported_[index] = supported;
        }
    }

    [[nodiscard]] FormatFeature format_features(Format format) const noexcept {
        const auto index = static_cast<u32>(format);
        return index < static_cast<u32>(Format::Count) ? format_features_[index]
                                                       : FormatFeature::None;
    }

    void set_format_features(Format format, FormatFeature features) noexcept {
        const auto index = static_cast<u32>(format);
        if (index < static_cast<u32>(Format::Count)) {
            format_features_[index] = features;
        }
    }

    [[nodiscard]] const DeviceLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] DeviceLimits& limits() noexcept { return limits_; }

    [[nodiscard]] BackendKind backend() const noexcept { return backend_; }
    void set_backend(BackendKind kind) noexcept { backend_ = kind; }

    /// The device's own name, for a log line and the crash artefact. Truncated rather than
    /// allocated, exactly as ScreenInfo::name is.
    [[nodiscard]] const char* device_name() const noexcept { return device_name_; }
    void set_device_name(const char* name) noexcept;

    [[nodiscard]] const char* driver_version() const noexcept { return driver_version_; }
    void set_driver_version(const char* version) noexcept;

    /// GPU-driven rendering needs bindless. `rhi-and-render-graph` requires the compatibility
    /// path's limitations to be reported rather than to degrade silently, and this is the one
    /// question the renderer asks to find out which path it is on.
    [[nodiscard]] bool supports_gpu_driven() const noexcept {
        return has(Capability::Bindless) && has(Capability::BindlessPartiallyBound) &&
               has(Capability::DescriptorIndexingNonUniform);
    }

private:
    bool supported_[kCapabilityCount] = {};
    FormatFeature format_features_[static_cast<u32>(Format::Count)] = {};
    DeviceLimits limits_{};
    BackendKind backend_ = BackendKind::Null;
    char device_name_[128] = {};
    char driver_version_[64] = {};
};

}  // namespace cy::rhi
