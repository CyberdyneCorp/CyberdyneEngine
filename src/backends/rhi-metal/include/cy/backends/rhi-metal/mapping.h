#pragma once
// THE METAL SEED'S REASON FOR EXISTING: every place `cy::rhi` says something a Metal device cannot
// be asked. M7 task 10.5.
//
// `delivery-roadmap` seeds Metal at M7 and delivers it at M11, and the M7 task says why: "to expose
// Vulkan-specific assumptions in `rhi-and-render-graph` WHILE THEY ARE STILL CHEAP". So the
// deliverable of this module is not a backend. It is this file, `README.md`, and the fact that both
// COMPILE AND RUN ON A MACHINE WITH NO METAL — because a finding that can only be read on hardware
// nobody in this project has is a finding nobody reads.
//
// ================================================================================================
// HOW THE NUMBERS IN THIS FILE ARE KEPT HONEST
// ================================================================================================
//
// The mappings below return `u32`s whose values are transcribed from Metal's own headers. That is a
// transcription and transcriptions are wrong, so it is CHECKED rather than trusted: `src/device.mm`
// carries one `static_assert` per row against the real `MTLPixelFormat` enumerators, inside
// `#if __has_include(<Metal/Metal.h>)`. On this machine those assertions do not run and the table
// is UNVERIFIED — stated plainly here rather than implied by the file compiling. On the day it is
// built on a Mac a wrong number is a compile error naming the row, not a wrong picture.
//
// ================================================================================================
// THE FINDINGS, IN THE ORDER OF WHAT EACH COSTS TO FIX LATER
// ================================================================================================
//
// Each has a `MetalGap` enumerator below, and `metal_gaps()` returns them all with their text — so
// a diagnostic, a test and this comment cannot drift apart. `README.md` carries the argument for
// each at length.
//
//   1. SHADERS ARE SPIR-V IN THE INTERFACE. `ShaderModuleDescription::spirv` is `Span<const u32>`
//      and there is no second field. Metal consumes MSL source or a compiled `.metallib`, never
//      SPIR-V. `pipeline.h` anticipates this — "a backend that does not consume SPIR-V natively
//      translates offline and caches the result" — but leaves the backend nowhere to PUT the
//      translated form, so the translation lands inside `create_shader_module`, which the engine
//      treats as cheap. CHEAP NOW: one optional `Span<const u8> native` beside the SPIR-V, and a
//      `native_shader_format()` capability. EXPENSIVE LATER: every cook, every cache key and every
//      hot reload path already assumes one interchange form.
//
//   2. TRANSIENT MEMORY IS SPELLED IN VULKAN. `reserve_transient_memory(bytes, memory_type_bits)`
//      and `bind_transient(handle, offset)` are `VkMemoryRequirements` and `vkBindImageMemory`.
//      `memory_type_bits` HAS NO METAL ANALOGUE AT ALL: a `MTLHeap` picks one `MTLStorageMode` at
//      creation and there is no bitmask of types to intersect. A Metal backend must answer `~0u`
//      and hope nobody intersects it with anything meaningful. CHEAP NOW: an opaque
//      `MemoryPoolClass` the backend defines and the graph only compares for equality.
//
//   3. `ImageLayout` IS A VULKAN OBJECT IN A NEUTRAL INTERFACE. Metal has no image layouts; a
//      resource on a hazard-tracked heap needs no transition, and one on an untracked heap needs a
//      `MTLFence` between encoders rather than a layout. Every `ImageBarrier`'s `old_layout` and
//      `new_layout` are dropped on the floor by a Metal backend. **AND THE ENGINE ALREADY HAS THE
//      INFORMATION IT NEEDS WITHOUT THEM**: `access.h` carries the access masks the layouts were
//      derived FROM. CHEAP NOW: make the layout a backend-internal derivation from the access
//      masks, which is what the Vulkan backend does anyway.
//
//   4. QUEUE FAMILIES ARE A VULKAN CONCEPT AND THE INTERFACE RETURNS ONE. `queue_family(QueueKind)`
//      returns a `u32` and `kQueueFamilyIgnored` exists so a barrier can say "no transfer". Metal
//      has `MTLCommandQueue` objects with no family index and no ownership-transfer concept at all.
//      CHEAP NOW: `bool needs_queue_ownership_transfer()` on the capabilities, and the family index
//      never leaves the Vulkan backend.
//
//   5. SECONDARY COMMAND BUFFERS ARE RECORDED BEFORE THEIR PASS EXISTS. `execute_secondary(primary,
//      ...)` mirrors Vulkan's inheritance model, where a secondary buffer is recorded against a
//      render pass *description* and executed into an instance later. Metal's equivalent is
//      `MTLParallelRenderCommandEncoder`, whose sub-encoders can only be created FROM a live
//      encoder — so a Metal backend cannot record a secondary buffer that does not yet have a pass.
//      CHEAP NOW: make "the pass is begun before its secondaries are recorded" an interface
//      precondition. The render graph probably already satisfies it; nothing says so.
//
//   6. THE PIPELINE CACHE IS A MEMORY BLOB. `save_pipeline_cache(Span<u8> out)` is
//      `vkGetPipelineCacheData`. Metal's `MTLBinaryArchive` is serialised to a URL and there is no
//      way to hand it over as bytes without writing a file and reading it back. CHEAP NOW: a path,
//      or an opaque backend-defined token the engine only stores.
//
//   7. `D24UnormS8Uint` HAS NO APPLE-SILICON EQUIVALENT. `MTLPixelFormatDepth24Unorm_Stencil8` is
//      unavailable on Apple GPUs. A backend must substitute `Depth32Float_Stencil8`, which is a
//      different memory footprint and a different precision. CHEAP NOW: a per-format support query
//      on `DeviceCapabilities` so the engine picks rather than the backend substituting quietly.
//
//   8. PUSH-CONSTANT RANGES CARRY AN OFFSET AND A STAGE MASK. Vulkan shares one block across stages
//      at declared offsets; Metal's `setBytes:length:atIndex:` binds a whole buffer to one stage's
//      argument table. A range shared between vertex and fragment becomes two uploads.
//      CHEAP NOW: nothing — this one is genuinely fine, because the sizes involved are small. It is
//      listed so the next reader does not have to re-derive that it is fine.
//
// AND THREE THINGS THAT MAP CLEANLY, which is worth as much as the list above:
//
//   * TIMELINE SEMAPHORES. `timeline_value` / `wait_timeline` is `MTLSharedEvent` almost exactly.
//   * SPECIALIZATION CONSTANTS. `SpecializationConstant` is `MTLFunctionConstantValues`.
//   * REVERSED-Z. `Viewport::min_depth`/`max_depth` stay [0, 1] and the projection inverts, so
//     Metal's [0, 1] clip depth needs no adjustment. A design that had inverted the viewport would
//     have needed one here.

#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/types.h>

namespace cy::rhi::metal {

/// A Metal enumerator value, as a plain integer. Deliberately not `MTLPixelFormat`: this header is
/// compiled on machines with no Metal, and that is the whole point of it.
using MetalEnum = u32;

/// `MTLPixelFormatInvalid`.
inline constexpr MetalEnum kMetalPixelFormatInvalid = 0;

/// The `MTLPixelFormat` for an engine format, or `kMetalPixelFormatInvalid`.
///
/// Two engine formats have no exact Metal equivalent and this function answers INVALID for both
/// rather than substituting — see `metal_format_substitution` for what a backend does next. A
/// mapping that silently substituted would give a depth buffer of a different precision than the
/// one the engine asked for, and nothing would say so.
[[nodiscard]] MetalEnum metal_pixel_format(Format format) noexcept;

/// What a backend must use instead, for the formats that have no equivalent, and
/// `Format::Undefined` for the ones that do. Separating this from the mapping is what makes the
/// substitution a decision somebody made rather than a line in a table.
[[nodiscard]] Format metal_format_substitution(Format format) noexcept;

/// `MTLStorageMode` for a buffer's declared usage. Metal's storage modes are the closest thing it
/// has to Vulkan's memory types, and they are NOT a bitmask — see gap 2.
enum class MetalStorageMode : u32 {
    Shared = 0,   ///< MTLStorageModeShared: CPU and GPU, coherent. Unified memory's default.
    Managed = 1,  ///< MTLStorageModeManaged: two copies, explicit synchronisation. Intel Macs only.
    Private = 2,  ///< MTLStorageModePrivate: GPU only. What a device-local buffer becomes.
    Memoryless = 3,  ///< MTLStorageModeMemoryless: tile memory only. A transient depth buffer.
};

[[nodiscard]] MetalStorageMode metal_storage_mode(bool host_visible, bool transient_only) noexcept;

/// `MTLBarrierScope`, the closest Metal has to an access mask. It has THREE bits — buffers,
/// textures and render targets — against `AccessFlags`' twenty. That collapse is not a loss: Metal
/// tracks hazards per resource rather than per access, so the engine's finer masks are information
/// a Metal backend has no use for. It IS a reason the two backends cannot share a barrier
/// derivation, which is why the graph derives barriers and the backend applies them.
enum class MetalBarrierScope : u32 {
    None = 0,
    Buffers = 1U << 0,
    Textures = 1U << 1,
    RenderTargets = 1U << 2,
};

[[nodiscard]] MetalBarrierScope metal_barrier_scope(AccessFlags access) noexcept;

/// `MTLRenderStages`, for the fence a barrier becomes. Two bits — vertex and fragment — against
/// `Stage`'s sixteen.
enum class MetalRenderStage : u32 {
    None = 0,
    Vertex = 1U << 0,
    Fragment = 1U << 1,
};

[[nodiscard]] MetalRenderStage metal_render_stage(Stage stage) noexcept;

/// `MTLLoadAction` and `MTLStoreAction`. These map exactly, which is worth recording: attachment
/// load and store operations are a tiler's own vocabulary and Vulkan borrowed them.
[[nodiscard]] MetalEnum metal_load_action(LoadOp op) noexcept;
[[nodiscard]] MetalEnum metal_store_action(StoreOp op) noexcept;

/// Every place the abstraction says something Metal cannot be asked. The enumerator order is the
/// order of the header comment above and of `README.md`.
enum class MetalGap : u8 {
    ShaderInterchangeIsSpirv = 0,
    TransientMemoryTypeBits,
    ImageLayoutHasNoEquivalent,
    QueueFamilyIndex,
    SecondaryCommandBufferInheritance,
    PipelineCacheIsABlob,
    Depth24Stencil8Unavailable,
    PushConstantRangeOffsets,
    Count,
};

inline constexpr u32 kMetalGapCount = static_cast<u32>(MetalGap::Count);

/// One gap: what the interface says, what Metal does instead, and what changing it costs now
/// against what it costs at M11.
struct MetalGapRecord {
    MetalGap gap = MetalGap::ShaderInterchangeIsSpirv;
    /// The `cy::rhi` declaration the gap is about, spelled the way it appears in the header.
    const char* interface_element = "";
    /// What Metal has instead. Empty means "nothing at all", which is a different finding from
    /// "something with different semantics" and is the more expensive one.
    const char* metal_equivalent = "";
    /// The change that would close it, stated as something a reviewer could apply.
    const char* remedy = "";
    /// True when a Metal backend can proceed by substituting something, false when the entry point
    /// simply cannot be implemented as declared. Four of the eight are the second kind.
    bool workaroundable = true;
};

[[nodiscard]] const MetalGapRecord& metal_gap(MetalGap gap) noexcept;
[[nodiscard]] Span<const MetalGapRecord> metal_gaps() noexcept;

/// How many of the gaps have no workaround. The number M11 pays for if nothing changes before then,
/// and the number this seed exists to make visible at M7.
[[nodiscard]] u32 metal_blocking_gap_count() noexcept;

}  // namespace cy::rhi::metal
