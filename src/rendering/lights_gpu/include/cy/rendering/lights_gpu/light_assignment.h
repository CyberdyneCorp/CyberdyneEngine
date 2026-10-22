// SPDX-License-Identifier: MIT
#pragma once
// The frame's clustered light lists, assigned on the device. `rendering-forward-clustered` —
// "Assignment SHALL run as a compute pass" — and the half of "make hundreds to thousands of
// dynamic lights cheap" that keeps the clustered path's cost off the host.
//
// ================================================================================================
// WHAT MOVES TO THE DEVICE, AND WHAT STAYS
// ================================================================================================
//
// `assign_clusters` (forward/cluster.h) tests every element against every cluster on the host:
// 65 280 clusters at 1080p times a thousand lights is sixty-five million box tests a frame. Here
// the host builds each light's view-space element — the bounding sphere, the 60-degree cone rule,
// the channels, in `FrameAssembly::build_lights` as before — and the device does the rest: one
// workgroup a screen tile culls the lights against the tile's frustum, and one thread a depth slice
// keeps its cluster's nearest lights (light_assignment.slang).
//
// DECALS STAY ON THE HOST. A decal is an oriented box `assign_clusters` already tests, and there
// are tens of them rather than thousands; with device assignment the host's pass runs over the
// decals alone and this module copies its lists through, so the forward pass reads one header array
// for every element type exactly as before.
//
// ================================================================================================
// THE SAME LISTS AS THE HOST'S, EXCEPT WHERE THE HOST'S WERE LOOSER
// ================================================================================================
//
// A cluster's box contains its slice of the tile's frustum and is larger than it, so the host's
// sphere-against-box test admits a light whose sphere touches a corner of the box outside the
// frustum. No pixel of the tile lies there, so such a light contributes nothing to the tile; the
// device rejects it at the tile's planes. With no cluster overflowing, a device list is the host's
// list less exactly those lights — `render.many_lights` checks that — and a cluster that overflows
// on the host can keep a light on the device that the host dropped for one that reached no pixel.
//
//     GpuLightAssignment assignment;
//     assignment.create(allocator, device, {grid, max_lights});
//     // per frame, before `FrameAssembly::assemble` (with `AssemblyDescription::device_lights`):
//     const LightListResources lists = assignment.import_lists(graph);
//     view.cluster_headers = lists.headers;  view.cluster_indices = lists.indices;
//     sinks.cluster_assignment = assignment.stage();
//     // after it, before the upload:
//     assignment.set_frame({assembly.light_elements(), &assembly.clusters(), layer, tan, aspect});
//     upload.cluster_headers_buffer = assignment.headers();
//     upload.cluster_indices_buffer = assignment.indices();

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lights_gpu/detail/compute.h>

#include <cstddef>
#include <type_traits>

namespace cy::rendering::lights_gpu {

/// The longest light list a cluster may keep on the device. `ClusterGridConfig::
/// max_elements_per_cluster` above it is refused by `create`.
inline constexpr u32 kMaxDeviceListLength = 64;
/// Slices a grid may have: one thread a slice in a 64-thread workgroup.
inline constexpr u32 kMaxDeviceSlices = 64;

/// One light as the assignment reads it: `ClusterElement`'s light half, 48 bytes.
struct alignas(16) GpuClusterElement {
    f32 view_position[3] = {0.0F, 0.0F, 0.0F};
    f32 radius = 0.0F;
    f32 view_direction[3] = {0.0F, 0.0F, -1.0F};
    /// 1 for a sphere: `ClusterElement::cone_cos`.
    f32 cone_cos = 1.0F;
    u32 payload = 0;
    u32 layer_mask = 0xFFFFFFFFU;
    u32 reserved[2] = {0, 0};
};

static_assert(sizeof(GpuClusterElement) == 48, "light_assignment.slang's CyClusterElement");
static_assert(offsetof(GpuClusterElement, cone_cos) == 28);
static_assert(offsetof(GpuClusterElement, payload) == 32);
static_assert(std::is_trivially_copyable_v<GpuClusterElement>);

[[nodiscard]] GpuClusterElement gpu_cluster_element(const ClusterElement& element) noexcept;

/// What the dispatch reads, word for word light_assignment.slang's `CyAssignConstants`.
struct AssignmentConstants {
    u32 dimensions[4] = {0, 0, 0, 0};
    f32 planes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 view[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 counts[4] = {0, 0, 0, 0};
    u32 copies[4] = {0, 0, 0, 0};
};

static_assert(sizeof(AssignmentConstants) == 80);

struct LightAssignmentDescription {
    /// The grid the frame assigns over: `FrameAssembly`'s, from the same description.
    ClusterGrid grid;
    /// The most lights a frame may carry.
    u32 max_lights = 4096;
    /// The most indices the host's own lists (the decals') may hold.
    u32 max_host_indices = 65536;
    /// Host copies of the headers and indices after each frame, for a test.
    bool readback = false;
};

/// The three buffers the forward pass reads, imported into a frame's graph: the light lists, and
/// the light records in device memory the frame's prepare stage fills.
struct LightListResources {
    ResourceId headers = kInvalidResource;
    ResourceId indices = kInvalidResource;
    ResourceId lights = kInvalidResource;
};

/// One frame's lights.
struct LightAssignmentFrame {
    /// The light elements, view space: `FrameAssembly::light_elements()`.
    Span<const ClusterElement> lights;
    /// The host's lists for every other element type, or null with none.
    const ClusterAssignment* host = nullptr;
    u32 view_layer_mask = 0xFFFFFFFFU;
    f32 tan_half_fov_y = 0.0F;
    f32 aspect = 1.0F;
};

/// What the last executed frame's dispatch counted. `rendering-lighting-and-shadows`: "the
/// shortfall SHALL be reported" — `overflow` is it.
struct AssignmentStatistics {
    u32 overflow = 0;
    u32 assignments = 0;
    u32 max_per_cluster = 0;
};

class GpuLightAssignment {
public:
    GpuLightAssignment() noexcept = default;
    ~GpuLightAssignment();

    GpuLightAssignment(const GpuLightAssignment&) = delete;
    GpuLightAssignment& operator=(const GpuLightAssignment&) = delete;
    GpuLightAssignment(GpuLightAssignment&&) = delete;
    GpuLightAssignment& operator=(GpuLightAssignment&&) = delete;

    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const LightAssignmentDescription& desc) noexcept;
    void destroy() noexcept;
    [[nodiscard]] const LightAssignmentDescription& description() const noexcept { return desc_; }

    /// Import the two list buffers into this frame's graph, for `AssemblyView`.
    [[nodiscard]] LightListResources import_lists(RenderGraph& graph) noexcept;
    /// This frame's lights. Copied, so the spans may change afterwards.
    [[nodiscard]] Status set_frame(const LightAssignmentFrame& frame) noexcept;

    /// The frame's hook: `FramePassKind::ClusterAssignment` declared as this module's passes.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    /// The buffers the frame's set 1 binds in place of the host's lists and the host's light ring.
    /// THE LIGHTS IN DEVICE MEMORY ARE HALF OF WHAT MAKES A THOUSAND LIGHTS CHEAP: read from the
    /// host's upload ring, every pixel's light loop fetched its records across the bus, and a
    /// thousand lights at 1080p cost nine milliseconds of shading; from device memory, a tenth of
    /// that (`render.many_lights` (c)).
    [[nodiscard]] rhi::BufferHandle headers() const noexcept { return headers_; }
    [[nodiscard]] rhi::BufferHandle indices() const noexcept { return indices_; }
    [[nodiscard]] rhi::BufferHandle lights() const noexcept { return lights_; }
    /// Where the device lists begin in `indices()`: after the host's.
    [[nodiscard]] u32 light_base() const noexcept { return desc_.max_host_indices; }

    [[nodiscard]] AssignmentStatistics statistics() const noexcept;
    /// The dispatch's device time in the last executed frame, in milliseconds; negative without
    /// timestamp queries. Call after its fence.
    [[nodiscard]] f64 last_device_ms() const noexcept;
    /// The headers and the indices the last executed frame wrote. Only with `readback`.
    [[nodiscard]] Status read_back(Span<ClusterHeader> headers, Span<u32> indices) const noexcept;

    // Public because `RecordFn` is a plain function pointer.
    void record_clear(const PassContext& context) noexcept;
    void record_dispatch(const PassContext& context) noexcept;
    void record_readback(const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] u64 header_bytes() const noexcept;
    [[nodiscard]] u64 index_bytes() const noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    LightAssignmentDescription desc_{};
    Array<GpuClusterElement> elements_{system_allocator(MemoryDomain::Renderer)};
    Array<ClusterHeader> host_headers_{system_allocator(MemoryDomain::Renderer)};
    Array<u32> host_indices_{system_allocator(MemoryDomain::Renderer)};
    AssignmentConstants constants_{};
    bool framed_ = false;

    detail::ComputePipeline pipeline_;
    rhi::BufferHandle elements_ring_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle constants_ring_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle host_headers_ring_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle host_indices_ring_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle headers_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle lights_;
    rhi::BufferHandle statistics_;
    rhi::BufferHandle statistics_zero_;
    rhi::BufferHandle statistics_readback_;
    rhi::BufferHandle headers_readback_;
    rhi::BufferHandle indices_readback_;
    rhi::QueryPoolHandle timer_;
    bool timed_ = false;
    bool counted_ = false;

    ResourceId lights_resource_ = kInvalidResource;
    ResourceId headers_resource_ = kInvalidResource;
    ResourceId indices_resource_ = kInvalidResource;
    ResourceId statistics_resource_ = kInvalidResource;
    ResourceId statistics_readback_resource_ = kInvalidResource;
    ResourceId headers_readback_resource_ = kInvalidResource;
    ResourceId indices_readback_resource_ = kInvalidResource;
};

}  // namespace cy::rendering::lights_gpu
