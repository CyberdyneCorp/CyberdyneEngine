#pragma once
// The synchronisation vocabulary — the whole of it. Tasks 2.1.1 and 2.2.1.
//
// THIS IS THE FILE THE INVARIANT RESTS ON. `rhi-and-render-graph`: "The RHI's public recording API
// SHALL NOT expose barriers, image layout transitions, or queue ownership transfers. These SHALL be
// computed by the render graph." A pass declares an `Access` against a resource and a subresource
// range, and that is the entire surface it has. It never sees a stage mask, an access mask, an
// image layout or a barrier, because there is nowhere in the pass-facing API for one to appear.
//
// `Access` is a CLOSED ENUM OF INTENTS, and closed is the load-bearing word. If a pass could
// assemble its own {stage, access, layout} triple, the graph would be deriving barriers from
// whatever the pass author believed rather than from what the pass does, and the thirtieth pass
// would be back to hand-written synchronisation with extra steps. Adding an intent is an edit to
// this file and to the table beside it, which is a review of one table rather than of a renderer.
//
// The table below is the engine's entire synchronisation knowledge. Every barrier in every frame is
// assembled from two of its rows: the row of what the resource was last used as, and the row of
// what this pass is about to use it as. That is why the structural gate at task 2.2.4 is cheap to
// state — there is exactly one place to point at.
//
// M3's spike (13 cases on an RTX 5060, 0 failures) proved the derivation this vocabulary feeds:
// barriers, queue-family ownership transfers, cross-queue semaphores and transient aliasing all
// derive correctly from declared reads and writes under async compute.

#include <cy/backends/rhi/types.h>

namespace cy::rhi {

/// What a pass does to a resource.
///
/// Intents, not capabilities: `FragmentSampledRead` and `ComputeSampledRead` are separate because
/// they resolve to different pipeline stages and a barrier that named the wrong one would either
/// stall too much or too little. Read the name as a sentence — "this pass reads this texture as a
/// sampled texture, in the fragment stage".
enum class Access : u8 {
    // Compute.
    ComputeStorageWrite,
    ComputeStorageRead,
    ComputeStorageReadWrite,
    ComputeSampledRead,
    ComputeUniformRead,

    // Graphics.
    VertexAttributeRead,
    IndexRead,
    IndirectCommandRead,
    VertexUniformRead,
    VertexStorageRead,
    FragmentUniformRead,
    FragmentSampledRead,
    FragmentStorageRead,
    ColorAttachmentWrite,
    ColorAttachmentReadWrite,
    /// Reversed-Z writes depth in the late fragment tests and tests it in the early ones, so both
    /// stages appear: a depth attachment is read before the fragment shader and written after it.
    DepthStencilAttachmentWrite,
    DepthStencilAttachmentRead,

    // Transfer.
    TransferRead,
    TransferWrite,

    /// The host boundary is a dependency like any other. A pass that declares it makes the graph
    /// emit the transfer-to-host barrier; relying on HOST_COHERENT memory and a fence instead is
    /// how a read-back becomes intermittently wrong. (Spike gotcha 6f.)
    HostRead,

    /// What the swapchain image is in when it is handed back to the presentation engine. Declared
    /// by the pass that finishes with it, so the transition is derived like every other one.
    Present,

    Count,
};

inline constexpr u32 kAccessCount = static_cast<u32>(Access::Count);

/// One row of the table: what an intent means to a synchronisation primitive.
///
/// `layout` is ImageLayout::Undefined for the buffer-only intents, which is not a layout a barrier
/// would ever transition *to* — the graph tests `is_image` before it reads this field, and an
/// image declared with a buffer-only intent is a programmer error the declaration catches.
struct AccessInfo {
    Stage stage = Stage::None;
    AccessFlags access = AccessFlags::None;
    ImageLayout layout = ImageLayout::Undefined;
    bool is_write = false;
    /// False for the intents that make no sense against an image (HostRead), and for those that
    /// make no sense against a buffer (the attachment intents). Checked when a use is declared, so
    /// the diagnostic names the pass and the resource rather than surfacing as a wrong barrier.
    bool valid_for_image = true;
    bool valid_for_buffer = true;
    const char* name = "";
};

/// The single table. Every barrier the engine emits is assembled from two rows of it.
[[nodiscard]] const AccessInfo& access_info(Access access) noexcept;

/// The intent's own spelling, for the graph's dump and for a diagnostic. Never null.
[[nodiscard]] const char* access_name(Access access) noexcept;

/// Whether an intent writes. Spelled as a function so that a caller reads `is_write(use.access)`
/// rather than reaching through the table for one bit.
[[nodiscard]] inline bool is_write(Access access) noexcept {
    return access_info(access).is_write;
}

}  // namespace cy::rhi
