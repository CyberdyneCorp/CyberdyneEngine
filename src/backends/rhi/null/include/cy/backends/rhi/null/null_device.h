#pragma once
// The null backend. Task 2.1.2, design.md §1.
//
// --- WHY THIS WAS WRITTEN BEFORE THE VULKAN BACKEND ---------------------------------------------
//
// Written first, it forces the RHI to be an interface rather than a thin wrapper over whichever
// Vulkan calls were convenient, because there is no Vulkan to lean on. Written afterwards, it
// becomes a set of empty functions shaped by decisions Vulkan already made, and stops being a
// reference for what the RHI actually requires. Every ambiguity in device.h was resolved by asking
// what this file would have to do — which is how `texture_memory_requirements()` came to be a query
// that answers before anything is bound, and how the transient pool came to be reserved rather than
// allocated per resource.
//
// --- IT IS NOT A SET OF EMPTY FUNCTIONS ---------------------------------------------------------
//
// `rhi-and-render-graph`, "Null backend": it "SHALL implement the full interface without a GPU,
// satisfying resource creation and command recording as no-ops while preserving handle semantics
// and validation". So:
//
//   * every handle is a real generational handle from a real pool, and a stale one fails validation
//     rather than aliasing whatever took its slot;
//   * every creation call runs the same limit checks Vulkan runs, so a pipeline that would be
//     refused on a device is refused in continuous integration;
//   * every recorded command is APPENDED TO A LOG, and the log has a hash. That is what makes
//     "the null backend records the same graph" (task 6.4) a comparison rather than an assertion:
//     two runs, two backends, one number.
//   * memory is accounted, so the aliasing measurement (task 7.3) has a figure on a machine with no
//     GPU — a synthetic one, but one that tracks the plan exactly, which is what a regression gate
//     needs.
//
// What it does not do is execute anything. A dispatch changes no memory, so a test that needs
// pixels needs a device; a test that needs a plan, a barrier count, a submit count or an ordering
// does not, and that is most of them.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

namespace cy::rhi::null {

/// What the null backend recorded. One enumerator per call on cy::rhi::CommandBuffer, plus the
/// barrier batch — which is the one thing a pass cannot record and the graph always can.
enum class CommandKind : u8 {
    BeginRendering,
    EndRendering,
    SetViewport,
    SetScissor,
    BindGraphicsPipeline,
    BindComputePipeline,
    BindDescriptorSets,
    PushConstants,
    BindVertexBuffers,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    DrawIndexedIndirect,
    Dispatch,
    DispatchIndirect,
    CopyBuffer,
    CopyBufferToTexture,
    CopyTextureToBuffer,
    BeginDebugLabel,
    EndDebugLabel,
    InsertDebugLabel,
    WriteTimestamp,
    ResetQueries,
    WriteBreadcrumb,
    Barriers,
    ExecuteSecondary,
};

[[nodiscard]] const char* command_kind_name(CommandKind kind) noexcept;

/// One recorded command.
///
/// Deliberately flat and fixed-size: the log is compared between runs and between backends, and a
/// record that owned a heap allocation would make the comparison depend on where the allocation
/// landed. `a` through `d` carry whatever the call's arguments were, documented per kind in
/// null_command_buffer.cpp, and `label` carries the one string a command can have.
struct RecordedCommand {
    CommandKind kind = CommandKind::EndRendering;
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
    u32 d = 0;
    u64 handle_bits = 0;
    /// Not owned. Debug labels and pass names are string literals or graph-owned storage that
    /// outlives the frame, exactly as cy::Error::message is.
    const char* label = "";
};

/// Register the null backend under `cy::rhi::kNullBackendName`.
///
/// Called by a static initialiser in this module, so linking cy::rhi-null is all that is required.
/// Exposed as well so that a test can register it explicitly and not depend on initialiser
/// ordering, which is exactly the kind of thing that works everywhere except the one platform
/// nobody tested. Idempotent by name.
Status register_null_backend() noexcept;

/// Create a null device directly, bypassing the registry. The caller owns it and destroys it with
/// `destroy_null_device`.
///
/// The concrete type stays private: everything a caller needs is on cy::rhi::Device, and the two
/// things that are not — the command log and its hash — are the free functions below. Keeping the
/// class out of this header is what stops the null backend from acquiring an interface of its own
/// that the Vulkan backend then does not have.
Expected<Device*, Error> create_null_device(Allocator& allocator,
                                            const DeviceDescription& desc) noexcept;
void destroy_null_device(Allocator& allocator, Device* device) noexcept;

/// Whether `device` came from this backend. Asked rather than assumed: the engine builds with
/// -fno-rtti, so the accessors below cannot check for themselves and this is what a caller uses
/// before calling them.
[[nodiscard]] bool is_null_device(const Device& device) noexcept;

/// The recorded command stream, for a test that wants to compare two runs or two backends.
/// The device must be one this backend created; CY_ASSERT catches the case where it is not.
[[nodiscard]] Span<const RecordedCommand> command_log(const Device& device) noexcept;
/// A 64-bit hash of the recorded stream. Two runs that recorded the same commands in the same order
/// produce the same number; deterministic submission (design.md §6) is what makes that a useful
/// thing to assert rather than a coincidence.
[[nodiscard]] u64 command_log_hash(const Device& device) noexcept;
void clear_command_log(Device& device) noexcept;

}  // namespace cy::rhi::null
