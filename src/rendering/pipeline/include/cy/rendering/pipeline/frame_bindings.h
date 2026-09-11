#pragma once
// The per-frame uploads and the descriptor sets that name them. M8.c task 1b.1.
//
// ================================================================================================
// WHY A RING, AND WHY THE DESCRIPTOR SETS ARE PER-FRAME
// ================================================================================================
//
// M3's artefact shipped with this defect and M4's `render.frames` suite exists because of it:
//
// > `allocate_descriptor_set(layout, per_frame)` took the flag, recorded it, and then allocated
// > from the CURRENT frame's pool whichever value it had — and a frame pool is reset the moment its
// > slot comes round. A set the caller asked to be persistent was recycled after `frames_in_flight`
// > frames, and from that frame on every draw bound a `VkDescriptorSet` the driver had already
// > destroyed: 24 validation errors a frame from frame 3, on a run that still printed "exit 0".
//
// So: the buffers are a RING of `frames_in_flight` slots, written into the slot `begin_frame()`
// returned, and the descriptor sets are allocated `per_frame` and rewritten every frame. A single
// buffer written every frame would be overwritten while the previous frame was still reading it,
// which is the same class of defect one indirection further down and is not visible in a one-frame
// test.
//
// ================================================================================================
// EVERY SPAN IS SOMEBODY ELSE'S OUTPUT
// ================================================================================================
//
// `FrameUpload` below is `FrameAssembly`'s report, copied. The lights are `build_gpu_light`'s, the
// headers and indices are `assign_clusters`', the draws are `sort_draw_list`'s, and the material
// bytes are `MaterialTable::bytes()`. Nothing here recomputes any of them: a number that moves
// names the module that moved it, which is the same rule `AssemblyReport` states for itself.
//
// The one span that is NOT the assembly's is `instances`. The GPU scene is the render server's and
// this module holds no second copy of it, exactly as `FrameAssembly` holds no copy of the mesh
// table; a caller that has one publishes its rows.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/forward/draw_list.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/material/material.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::pipeline {

/// How much of each the ring is sized for. A frame that exceeds one of these is REFUSED naming the
/// number rather than silently truncated: a light list quietly cut short is a frame that renders
/// and is wrong, which is the failure mode this whole layer exists to stop being invisible.
struct BindingCapacity {
    u32 lights = 256;
    u32 cluster_headers = 4096;
    u32 cluster_indices = 16384;
    u32 draws = 4096;
    u32 instances = 4096;
    u32 material_bytes = 16384;

    /// The capacity one `AssemblyDescription` implies. The grid's own dimensions decide the header
    /// and index counts, so the two cannot disagree with the assignment that fills them.
    [[nodiscard]] static BindingCapacity for_grid(const ClusterGrid& grid, u32 max_draws,
                                                  u32 max_instances, u32 material_capacity,
                                                  u32 max_lights) noexcept;
};

/// What one frame writes. Every span is a module's own output — see the header comment.
struct FrameUpload {
    GlobalsData globals;
    FrameViewData view;
    Span<const GpuLight> lights;
    Span<const ClusterHeader> cluster_headers;
    Span<const u32> cluster_indices;
    Span<const GpuDrawInstance> draws;
    Span<const InstanceTransform> instances;
    /// `MaterialTable::bytes()`. Uploaded whole rather than by its dirty interval, because the
    /// interval is a transfer optimisation over a device-local table and this ring is host-visible;
    /// the interval belongs with the transfer that uses it, which is not this one.
    Span<const u8> materials;
};

/// The ring of per-frame buffers, and the descriptor sets that name them.
class FrameBindings {
public:
    FrameBindings() noexcept = default;
    ~FrameBindings();

    FrameBindings(const FrameBindings&) = delete;
    FrameBindings& operator=(const FrameBindings&) = delete;
    FrameBindings(FrameBindings&&) = delete;
    FrameBindings& operator=(FrameBindings&&) = delete;

    /// Create the ring. `pipelines` must be initialized: its set layouts are what the sets are
    /// allocated from.
    [[nodiscard]] Status initialize(rhi::Device& device, const FramePipelines& pipelines,
                                    const BindingCapacity& capacity) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const BindingCapacity& capacity() const noexcept { return capacity_; }

    /// Write one frame's data into the ring slot `begin_frame()` returned, allocate this frame's
    /// descriptor sets and point them at it.
    ///
    /// CALL IT INSIDE THE HOST'S DEVICE FRAME, after `Device::begin_frame()` — the sets are
    /// allocated from that frame's pool.
    [[nodiscard]] Status upload(u32 frame_slot, const FrameUpload& upload) noexcept;

    /// Point the pass set's texture at the frame's scene colour, for the tonemapping resolve.
    /// Separate from `upload` because the view only exists once the graph has realised its
    /// transients, which is after `upload` and inside the record callback's own frame.
    [[nodiscard]] Status bind_scene_color(rhi::TextureViewHandle view) noexcept;

    /// The three sets, in the order a `bind_descriptor_sets(layout, 0, sets)` wants them.
    [[nodiscard]] Span<const rhi::DescriptorSetHandle> sets() const noexcept;

    /// The ring slot the last `upload` wrote, as transfer SOURCES.
    ///
    /// `ForwardFrame`'s Prepare stage is "the only pass that writes" the frame's own `lights` and
    /// `draw_instances` buffers, and declares `Access::TransferWrite` on both. These are what it
    /// copies FROM, so that pass records the transfer it was declared for instead of leaving a
    /// derived barrier around a transfer that never happened.
    [[nodiscard]] rhi::BufferHandle staged_lights() const noexcept;
    [[nodiscard]] rhi::BufferHandle staged_draws() const noexcept;
    /// The bytes the last upload actually wrote into each, which is what a copy region is sized by.
    [[nodiscard]] u64 staged_light_bytes() const noexcept { return staged_light_bytes_; }
    [[nodiscard]] u64 staged_draw_bytes() const noexcept { return staged_draw_bytes_; }
    /// How many frames have been uploaded. What a many-frame case reads to know the ring turned.
    [[nodiscard]] u64 uploads() const noexcept { return uploads_; }

private:
    struct Slot {
        rhi::BufferHandle globals;
        rhi::BufferHandle view;
        rhi::BufferHandle lights;
        rhi::BufferHandle cluster_headers;
        rhi::BufferHandle cluster_indices;
        rhi::BufferHandle draws;
        rhi::BufferHandle instances;
        rhi::BufferHandle materials;
    };

    [[nodiscard]] Status create_slot(rhi::Device& device, u32 index) noexcept;
    [[nodiscard]] Status write_sets(u32 frame_slot) noexcept;

    rhi::Device* device_ = nullptr;
    const FramePipelines* pipelines_ = nullptr;
    BindingCapacity capacity_;
    Slot slots_[rhi::kMaxFramesInFlight];
    u32 slot_count_ = 0;
    rhi::DescriptorSetHandle sets_[kSetCount];
    u32 current_slot_ = 0;
    u64 staged_light_bytes_ = 0;
    u64 staged_draw_bytes_ = 0;
    u64 uploads_ = 0;
    bool ready_ = false;
};

}  // namespace cy::rendering::pipeline
