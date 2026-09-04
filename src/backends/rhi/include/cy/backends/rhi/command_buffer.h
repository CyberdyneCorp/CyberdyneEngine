#pragma once
// The recording API a pass is handed. Tasks 2.1.1 and 2.2.1.
//
// --- READ THE LIST OF METHODS, AND THEN THE LIST OF METHODS THAT ARE NOT HERE ------------------
//
// There is no barrier call. No layout transition, no queue-family ownership transfer, no
// `pipeline_barrier`, no `transition`, no escape hatch spelled `raw`. That absence is the M3
// invariant, and it is the reason this milestone exists: a pass declares what it reads and what it
// writes (see access.h) and the render graph computes the rest.
//
// It is not a convention. `rhi-and-render-graph` states it as a requirement — "The RHI's public
// recording API SHALL NOT expose barriers, image layout transitions, or queue ownership transfers"
// — and task 2.2.4 makes it structural: the barrier-emitting calls live on rhi::BarrierRecorder
// (barrier.h), which is reachable only with a passkey the render graph's executor alone can
// construct, and tools/layercheck.py fails the build if a barrier symbol appears outside
// src/backends/rhi/ and src/rendering/graph/.
//
// THE BACKEND ESCAPE HATCH THE SPECIFICATION PERMITS is `native_handle()` at the bottom of this
// file. It is documented as unsafe, excluded from the portability guarantees, and returns an opaque
// pointer — so reaching it costs a cast that says what it is, and a grep for it finds every user.
// It is not a barrier API: a caller that used it to emit one would still be caught by the gate.
//
// --- WHY A CLASS AND NOT A HANDLE WITH FREE FUNCTIONS ------------------------------------------
//
// Parallel recording (task 2.2.5) hands one CommandBuffer to each job worker. A reference is the
// natural thing for a worker to hold, and it keeps "this worker owns this command buffer" a fact
// about the object graph rather than a convention about which handle a worker was given.

#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/resources.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

/// One colour or depth attachment of a dynamic-rendering pass.
///
/// `load` and `store` are the graph's decisions, not the pass author's: the graph knows whether
/// anything reads the target afterwards, and `rhi-and-render-graph` requires an unread target's
/// store to be elided, "which matters greatly on tiled GPUs".
struct RenderAttachment {
    TextureViewHandle view;
    LoadOp load = LoadOp::DontCare;
    StoreOp store = StoreOp::Store;
    ClearValue clear{};
    /// Multisample resolve target, or a null handle for no resolve.
    TextureViewHandle resolve_view;
};

struct RenderingInfo {
    Rect2D render_area{};
    Span<const RenderAttachment> color_attachments;
    /// `view.is_null()` means no depth attachment.
    RenderAttachment depth_attachment{};
    u32 layer_count = 1;
    /// Multiview mask; zero is single-view rendering. Stereo sets two bits, which is the XR
    /// prerequisite tests/render/README.md has recorded since M0.
    u32 view_mask = 0;
};

struct BufferCopy {
    u64 source_offset = 0;
    u64 destination_offset = 0;
    u64 size = 0;
};

struct BufferTextureCopy {
    u64 buffer_offset = 0;
    u32 buffer_row_length = 0;    // zero: tightly packed to the texture's extent
    u32 buffer_image_height = 0;  // zero: tightly packed
    u16 mip_level = 0;
    u16 base_layer = 0;
    u16 layer_count = 1;
    Offset3D texture_offset{};
    Extent3D texture_extent{};
};

/// The commands a pass may record. Everything here is a draw, a dispatch, a copy, a state change or
/// a debug annotation — and nothing here is a synchronisation primitive.
class CommandBuffer {
public:
    CommandBuffer() = default;
    virtual ~CommandBuffer() = default;

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = delete;
    CommandBuffer& operator=(CommandBuffer&&) = delete;

    [[nodiscard]] virtual CommandBufferHandle handle() const noexcept = 0;

    // --- Rendering scopes ---------------------------------------------------------------------

    virtual void begin_rendering(const RenderingInfo& info) noexcept = 0;
    virtual void end_rendering() noexcept = 0;

    virtual void set_viewport(const Viewport& viewport) noexcept = 0;
    virtual void set_scissor(const Rect2D& scissor) noexcept = 0;

    // --- Binding ------------------------------------------------------------------------------

    virtual void bind_graphics_pipeline(GraphicsPipelineHandle pipeline) noexcept = 0;
    virtual void bind_compute_pipeline(ComputePipelineHandle pipeline) noexcept = 0;

    /// `first_set` plus the number of sets must not exceed kMaxDescriptorSets.
    virtual void bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                                      Span<const DescriptorSetHandle> sets) noexcept = 0;

    /// `offset + data.size()` must not exceed kMaxPushConstantBytes.
    virtual void push_constants(PipelineLayoutHandle layout, ShaderStage stages, u32 offset,
                                Span<const u8> data) noexcept = 0;

    virtual void bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> buffers,
                                     Span<const u64> offsets) noexcept = 0;
    /// `wide` selects 32-bit indices; the engine's mesh format uses 16-bit where a mesh fits.
    virtual void bind_index_buffer(BufferHandle buffer, u64 offset, bool wide) noexcept = 0;

    // --- Draws and dispatches -------------------------------------------------------------------

    virtual void draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                      u32 first_instance) noexcept = 0;
    virtual void draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                              i32 vertex_offset, u32 first_instance) noexcept = 0;
    virtual void draw_indexed_indirect(BufferHandle arguments, u64 offset, u32 draw_count,
                                       u32 stride) noexcept = 0;
    virtual void dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept = 0;
    virtual void dispatch_indirect(BufferHandle arguments, u64 offset) noexcept = 0;

    // --- Copies -------------------------------------------------------------------------------

    virtual void copy_buffer(BufferHandle source, BufferHandle destination,
                             Span<const BufferCopy> regions) noexcept = 0;
    virtual void copy_buffer_to_texture(BufferHandle source, TextureHandle destination,
                                        Span<const BufferTextureCopy> regions) noexcept = 0;
    virtual void copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                        Span<const BufferTextureCopy> regions) noexcept = 0;

    // --- Queries and debugging ------------------------------------------------------------------
    //
    // `rhi-and-render-graph` requires debug labels per render graph pass and breadcrumb markers per
    // pass "so that they survive into a crash artefact when the trace tail does not". The graph
    // emits both around every pass it records; a pass author never calls these.

    virtual void begin_debug_label(const char* name) noexcept = 0;
    virtual void end_debug_label() noexcept = 0;
    virtual void insert_debug_label(const char* name) noexcept = 0;

    virtual void write_timestamp(QueryPoolHandle pool, u32 index) noexcept = 0;
    virtual void reset_queries(QueryPoolHandle pool, u32 first, u32 count) noexcept = 0;

    /// Write `value` into the device-visible breadcrumb buffer at `slot`. The last value the GPU
    /// managed to write is what a device-loss report reads back to name the pass it died in.
    virtual void write_breadcrumb(u32 slot, u32 value) noexcept = 0;

    // --- The documented escape hatch
    // --------------------------------------------------------------

    /// UNSAFE, and excluded from every portability guarantee `rhi-and-render-graph` makes. Returns
    /// the backend's own command-buffer object (a VkCommandBuffer, an id<MTLCommandEncoder>), for
    /// backend-specific work the RHI does not model. Null on a backend with no such object, which
    /// includes the null backend — so code that reaches for it stops working in continuous
    /// integration, which is the correct amount of friction.
    ///
    /// It is not a way to emit a barrier: task 2.2.4's gate reads the source, not the type.
    [[nodiscard]] virtual void* native_handle() noexcept = 0;
};

}  // namespace cy::rhi
