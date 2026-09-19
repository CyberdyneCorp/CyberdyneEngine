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

    /// PASSES RECORDED IN PARALLEL, INTO SECONDARY COMMAND BUFFERS — Metal gap 5.
    ///
    /// The engine's parallel recording records one secondary PER PASS, on job workers, before the
    /// primary loop reaches any of them, and each secondary contains a whole render pass because
    /// the pass callback itself begins and ends rendering. Vulkan's secondary command buffers do
    /// that. Metal's `MTLParallelRenderCommandEncoder` does NOT: it is parallelism WITHIN one
    /// render pass, from sub-encoders that exist only inside a live encoder, which is a different
    /// axis and not a reordering of this one. A Metal backend answers this false and the graph
    /// records sequentially — the path `ExecuteOptions::parallel_recording` already defaults to.
    ///
    /// This is a capability and NOT a precondition on `execute_secondary`, and the seed proposed
    /// the precondition: no ordering rule converts across-passes parallelism into within-a-pass
    /// parallelism, so a precondition would be a rule the engine could satisfy and Metal still
    /// could not implement.
    ParallelPassRecording,

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

/// WHAT A BACKEND OBSERVED ABOUT A DEVICE'S RAY TRACING, before anything decided anything.
///
/// THE DEFECT THIS STRUCT EXISTS TO MAKE IMPOSSIBLE. `Capability::RayTracing` was an enumerator
/// NOTHING SET from M3 until M11.c — so every device this engine could open reported no ray
/// tracing, including the RTX 5060 the GI fallback suite was written on, whose driver lists
/// `VK_KHR_ray_query`. The failure mode in the other direction is worse and is the one this shape
/// prevents: a backend that sets the capability because it was COMPILED with the extension's
/// headers, or because the extension is listed, without the device ever reporting the FEATURE.
/// Listing an extension and enabling its feature are different answers, and a driver gives both.
///
/// So the backend records what it saw, `device_reports_ray_tracing()` decides, and a test can put
/// any device in front of that decision without owning a GPU — which is what makes the claim
/// judgeable on a machine that cannot open a ray-tracing device at all.
struct RayTracingObservation {
    /// The extension is in the device's own list.
    bool acceleration_structure_extension = false;
    bool ray_query_extension = false;
    /// `VK_KHR_acceleration_structure` requires it; a device that lists one and not the other
    /// cannot create a structure, so the pair is the unit rather than either half.
    bool deferred_host_operations_extension = false;
    /// The device reported the feature bit, which is the answer that is not the same as the
    /// extension being listed.
    bool acceleration_structure_feature = false;
    bool ray_query_feature = false;
    /// The engine asked for the features when it created the device. A capability reported for a
    /// feature nobody enabled is a capability a shader cannot use.
    bool enabled_on_the_device = false;
};

/// Whether a device that reported this may be told it has ray tracing.
///
/// Every field is required and the conjunction is spelled one line per field DELIBERATELY: each
/// line is one thing the device said, so removing any one of them is a one-line mutation that
/// leaves a tree which still compiles and a capability that no longer reports the device — which is
/// exactly the defect `m11c:ray-tracing-capability-honest` is proven against.
[[nodiscard]] bool device_reports_ray_tracing(const RayTracingObservation& observed) noexcept;

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

    /// THE FORM THIS DEVICE CONSUMES A SHADER IN — Metal gap 1, as a capability rather than as a
    /// backend identity test. A cook asks this, not `backend() == BackendKind::Metal`: two Metal
    /// devices could want MSL source and a prebuilt `.metallib`, and a renderer that branched on
    /// the backend would be wrong about one of them.
    [[nodiscard]] ShaderFormat native_shader_format() const noexcept {
        return native_shader_format_;
    }
    void set_native_shader_format(ShaderFormat format) noexcept { native_shader_format_ = format; }

    /// The device's own name, for a log line and the crash artefact. Truncated rather than
    /// allocated, exactly as ScreenInfo::name is.
    [[nodiscard]] const char* device_name() const noexcept { return device_name_; }
    void set_device_name(const char* name) noexcept;

    [[nodiscard]] const char* driver_version() const noexcept { return driver_version_; }
    void set_driver_version(const char* version) noexcept;

    /// What the backend saw before it decided about ray tracing. Reported rather than inferred:
    /// `has(Capability::RayTracing)` is one bit, and "which of the five answers was missing" is the
    /// question a bug report about a device that should have traced actually asks.
    [[nodiscard]] const RayTracingObservation& ray_tracing_observation() const noexcept {
        return ray_tracing_;
    }
    void set_ray_tracing_observation(const RayTracingObservation& observed) noexcept {
        ray_tracing_ = observed;
        set(Capability::RayTracing, device_reports_ray_tracing(observed));
    }

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
    ShaderFormat native_shader_format_ = ShaderFormat::Spirv;
    char device_name_[128] = {};
    char driver_version_[64] = {};
    RayTracingObservation ray_tracing_{};
};

}  // namespace cy::rhi
