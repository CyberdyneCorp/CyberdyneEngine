// SPDX-License-Identifier: MIT
#pragma once
// The acceleration service's structures, built on a device. Issue #35, stage 3.
//
// ================================================================================================
// THE SERVICE DECIDES; THIS BUILDS WHAT IT DECIDED
// ================================================================================================
//
// `rt::AccelerationService` keeps every decision `ray-tracing-infrastructure` assigns it — the
// build budget and its priority order, refit against rebuild by the adapter's quality floor,
// eviction, and which instances the top level traces this frame. `DeviceStructures` is a
// `rt::StructureMirror`: installed with `AccelerationService::set_mirror`, it is told what the
// service did and records the same thing on the device through the RHI's acceleration structures
// (cy/backends/rhi/acceleration.h). No consumer builds, refits or owns a structure, on either side.
//
//   bottom_built    a bottom level (re)created at its geometry's size, its triangles copied into
//                   the shared vertex and index pools, and a BUILD queued;
//   bottom_refit    the moved positions written over the same range and an UPDATE queued — the
//                   driver's in-place refit, which is what the service's refit means on a device;
//   bottom_evicted  the structure destroyed and its pool ranges returned;
//   top_level       compared with the last top level the device built: the same instances over
//                   the same bottom levels with nothing rebuilt below is an UPDATE when a transform
//                   or a refit moved something and NOTHING AT ALL when nothing did; anything else
//                   is a BUILD. `DeviceStructureReport` counts which happened.
//
// ================================================================================================
// WHAT A SHADER READS
// ================================================================================================
//
// A ray query returns a committed instance, a primitive and a distance, and nothing else. A GI hit
// needs a normal to resolve through the surface cache, so the device keeps beside the structures
// what `AccelerationService::trace_instance` reads on the host: the vertex and index pools (the
// same buffers the builds read) and one record per traced instance — its geometry's offsets into
// the pools and the instance's normal matrix — indexed by the custom index the top level gives
// each instance. `binding()` hands those four buffers and the top level to a consumer's
// descriptor set; a hit is never shaded from them.
//
// ================================================================================================
// A DEVICE WITHOUT RAY QUERIES
// ================================================================================================
//
// `create` refuses with `ErrorCode::Unsupported` — the RHI's own refusal, because the device has no
// `Capability::RayTracing` — and a consumer takes its software tier from that. The service itself
// is `Unsupported` on the same device (`rt::service_config_for`), so it never calls a mirror.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/raytracing/acceleration.h>

namespace cy::rendering::rt_gpu {

/// How large a world the device copy is sized for. The pools and the top level are allocated once;
/// a geometry or a top level that would overflow them is refused by name and counted.
struct DeviceStructuresDescription {
    /// Bottom-level slots: the service's own slot numbers, so at least as many as it ever uses.
    u32 max_bottom_levels = 256;
    u32 max_instances = 4096;
    /// Every bottom level's vertices and indices, together.
    u32 max_vertices = 1U << 20;
    u32 max_indices = 3U << 20;
};

/// What the device did with the last frame the service reported, and since creation.
struct DeviceStructureReport {
    u32 bottom_builds = 0;
    u32 bottom_updates = 0;
    u32 bottom_evictions = 0;
    u32 top_builds = 0;
    u32 top_updates = 0;
    /// Frames whose top level was the one already on the device: nothing recorded.
    u32 top_unchanged = 0;
    /// A geometry, a refit or a top level the pools or the top level could not hold.
    u32 refusals = 0;
    u32 traced_instances = 0;
    u64 structure_bytes = 0;
    u64 pool_bytes = 0;
};

/// What a consumer binds to trace the device's top level. `instances` holds four `float4` per
/// traced instance: [vertex base, index base, triangle count, instance id] as `uint`s, then the
/// instance's normal matrix as three COLUMNS, the first with the geometry's declared error in w.
struct DeviceStructureBinding {
    rhi::AccelerationStructureHandle top;
    rhi::BufferHandle positions;
    rhi::BufferHandle indices;
    rhi::BufferHandle instances;
};

class DeviceStructures final : public rt::StructureMirror {
public:
    DeviceStructures() noexcept;
    ~DeviceStructures() override;

    DeviceStructures(const DeviceStructures&) = delete;
    DeviceStructures& operator=(const DeviceStructures&) = delete;
    DeviceStructures(DeviceStructures&&) = delete;
    DeviceStructures& operator=(DeviceStructures&&) = delete;

    /// Whether `device` builds acceleration structures at all: `Capability::RayTracing`.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// `ErrorCode::Unsupported` on a device without ray tracing, before anything is allocated.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const DeviceStructuresDescription& desc) noexcept;
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept { return device_ != nullptr; }

    // --- rt::StructureMirror
    // -----------------------------------------------------------------------

    void bottom_built(u32 slot, const rt::BottomLevelView& view) noexcept override;
    void bottom_refit(u32 slot, const rt::BottomLevelView& view) noexcept override;
    void bottom_evicted(u32 slot) noexcept override;
    void top_level(Span<const rt::TopLevelInstance> instances) noexcept override;

    // --- The frame ------------------------------------------------------------------------------

    /// Declare the builds and updates queued since the last call: one pass for the bottom levels,
    /// then one for the top level, which reads them. A frame with nothing queued declares nothing.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    /// The top level's storage as a resource of `graph` — the one `declare` wrote, when it did —
    /// for a consumer's `ComputeAccelerationStructureRead`.
    [[nodiscard]] ResourceId import_top(RenderGraph& graph) noexcept;

    /// Whether a top level has been built and holds at least one instance: the predicate a
    /// consumer's hardware tier is selected on, together with the service's `active()`.
    [[nodiscard]] bool traceable() const noexcept { return top_built_ && top_count_ != 0; }

    [[nodiscard]] DeviceStructureBinding binding() const noexcept;
    /// The last `declare`'s work.
    [[nodiscard]] const DeviceStructureReport& last_frame() const noexcept { return frame_; }
    /// Everything since `create`.
    [[nodiscard]] const DeviceStructureReport& totals() const noexcept { return totals_; }

private:
    struct Bottom {
        rhi::AccelerationStructureHandle structure;
        rt::MaintenancePolicy maintenance = rt::MaintenancePolicy::Static;
        f32 declared_error_metres = 0.0F;
        u32 vertex_base = 0;
        u32 vertex_count = 0;
        u32 index_base = 0;
        u32 index_count = 0;
        bool live = false;
        /// Queued for the next `declare`: a build, or an in-place update.
        bool pending_build = false;
        bool pending_update = false;
        /// Built in this frame's pending work: the top level must be rebuilt rather than updated,
        /// because its instances now reference a structure the last top level did not.
        bool rebuilt = false;
    };
    struct Range {
        u32 base = 0;
        u32 count = 0;
    };

    [[nodiscard]] static bool allocate(Array<Range>& free, u32 capacity, u32& high, u32 count,
                                       u32& base) noexcept;
    static void release(Array<Range>& free, u32 base, u32 count) noexcept;
    void release_bottom(Bottom& bottom) noexcept;
    void write_positions(const Bottom& bottom, Span<const Vec3> positions) noexcept;
    [[nodiscard]] bool same_top(Span<const rt::TopLevelInstance> instances,
                                bool& transforms_equal) const noexcept;
    void write_instance_table(Span<const rt::TopLevelInstance> instances) noexcept;
    void account() noexcept;
    [[nodiscard]] ResourceId import_structure(
        RenderGraph& graph, rhi::AccelerationStructureHandle structure) const noexcept;

    static void record_bottoms(const PassContext& context, void* user) noexcept;
    static void record_top(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    DeviceStructuresDescription desc_{};

    rhi::BufferHandle positions_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle instance_table_;
    rhi::AccelerationStructureHandle top_;

    Array<Bottom> bottoms_;
    Array<Range> free_vertices_;
    Array<Range> free_indices_;
    u32 vertex_high_ = 0;
    u32 index_high_ = 0;

    /// The top level the device last built or updated, as the service described it.
    Array<rt::TopLevelInstance> top_instances_;
    Array<rhi::AccelerationStructureInstance> top_records_;
    u32 top_count_ = 0;
    bool top_built_ = false;
    bool top_pending_ = false;
    bool top_update_ = false;

    /// The builds `declare` records, held for the record callbacks.
    Array<rhi::AccelerationStructureBuild> bottom_builds_;
    rhi::AccelerationStructureBuild top_build_{};

    DeviceStructureReport frame_{};
    DeviceStructureReport pending_{};
    DeviceStructureReport totals_{};
};

}  // namespace cy::rendering::rt_gpu
