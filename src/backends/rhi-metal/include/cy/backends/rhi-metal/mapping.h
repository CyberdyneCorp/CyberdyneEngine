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
// THE FINDINGS, AND WHAT M11.d's INTERFACE WORK DID WITH EACH
// ================================================================================================
//
// Each has a `MetalGap` enumerator below, and `metal_gaps()` returns them all with their text and
// their STATUS — so a diagnostic, a test and this comment cannot drift apart, and "closed" is a
// value in a table rather than a sentence somebody wrote. `README.md` carries the argument at
// length. M11.d section 1 settled the interface on Vulkan and null BEFORE either new backend
// exists, which is the whole reason this seed was written four milestones early.
//
// THREE OF THE SEED'S OWN PROPOSED REMEDIES TURNED OUT WRONG WHEN MEASURED, and that is the most
// valuable thing on this list: a remedy nobody applied is a guess. Gaps 2, 3 and 5 below say what
// was wrong and what replaced it.
//
//   1. SHADERS ARE SPIR-V IN THE INTERFACE — CLOSED, ADDITIVELY. `ShaderModuleDescription` gained
//      `Span<const u8> native` and a `ShaderFormat native_format` beside the SPIR-V, and
//      `DeviceCapabilities::native_shader_format()` says which form a device consumes. SPIR-V stays
//      the interchange form and every existing caller passes `spirv` untouched — the measured cost
//      was ZERO forced call sites. `validate_shader_module()` in the interface module enforces
//      "exactly one of the two" for every backend at once.
//
//   2. TRANSIENT MEMORY IS SPELLED IN VULKAN — CLOSED, AND THE SEED'S REMEDY WAS WRONG IN ONE WORD.
//      `MemoryRequirements::memory_type_bits` (a `u32` Vulkan bitmask) became `MemoryPoolClass`, an
//      opaque token the graph MEETS over every transient and tests for empty. The seed proposed a
//      token the graph "only compares for EQUALITY"; measured on this project's own devices, an
//      NVIDIA RTX 5060 answers 0x03 for transient images and 0x1F for transient buffers — they
//      DIFFER — so an equality would have refused to put images and buffers in one pool and split
//      the transient heap in two, losing exactly the aliasing the plan exists to report. A meet
//      keeps the proof and loses the Vulkan spelling.
//
//   3. `ImageLayout` IS A VULKAN OBJECT IN A NEUTRAL INTERFACE — CLOSED, AND THE SEED'S REMEDY IS
//      NOT IMPLEMENTABLE. The seed said the engine "already has the information without them"
//      because `access.h` carries the masks the layouts were derived from, so a backend could
//      derive the layout itself. IT CANNOT: `compile.cpp` deliberately puts only the WRITE access
//      in a barrier's `src_access` — a write-after-read needs an execution dependency and not a
//      memory one — so a barrier's source mask is not the resource's current state, and a backend
//      deriving "what it was" from it would transition from the wrong one. What was done instead:
//      `ImageLayout` became `ImageUse`, the engine's own vocabulary for what an image is being used
//      as, and the mapping to `VkImageLayout` moved entirely inside `vulkan_translate.cpp`. A Metal
//      backend maps `ImageUse` to nothing, which is a mapping rather than a field it drops.
//
//   4. QUEUE FAMILIES ARE A VULKAN CONCEPT AND THE INTERFACE RETURNS ONE — CLOSED AS PROPOSED.
//      `Device::queue_family()` is gone, `kQueueFamilyIgnored` is gone, and the barrier carries
//      `QueueKind`s with an `ownership_transfer` flag. `DeviceCapabilities` answers
//      `needs_queue_ownership_transfer()` and `queue_ownership_domain(QueueKind)` — an opaque
//      domain the graph only compares — and the family index never leaves the Vulkan backend.
//
//   5. SECONDARY COMMAND BUFFERS ARE RECORDED BEFORE THEIR PASS EXISTS — CLOSED, AND THE SEED'S
//      REMEDY DOES NOT ADDRESS THE MISMATCH. The seed proposed stating "the pass is begun before
//      its secondaries are recorded" as a precondition. Read against the tree, this engine records
//      one secondary PER PASS on job workers, each containing a whole render pass, before the
//      primary loop reaches any of them — parallelism ACROSS passes.
//      `MTLParallelRenderCommandEncoder` is parallelism WITHIN one pass. They are different axes
//      and no ordering rule converts one into the other, so the precondition would be a rule the
//      engine could satisfy and Metal still could not implement. What was done instead:
//      `Capability::ParallelPassRecording`, one term in `executor.cpp`. A device that answers false
//      records sequentially and produces the identical command stream.
//
//   6. THE PIPELINE CACHE IS A MEMORY BLOB — CLOSED AS PROPOSED, and the finding beside it is worth
//      more than the signature. `save_pipeline_cache`/`load_pipeline_cache` take a PATH, because
//      `MTLBinaryArchive` serialises to a URL. AND NOTHING IN THE TREE CALLS EITHER OF THEM: the
//      requirement they serve — "the cache is persisted across runs, so a warm start compiles
//      nothing" — is unimplemented ABOVE the RHI, which changing a signature does not fix.
//
//   7. `D24UnormS8Uint` HAS NO APPLE-SILICON EQUIVALENT — CLOSED, and it was never an interface
//      change. `DeviceCapabilities::format_features()` has answered per format since M3 and both
//      backends populate it for every format; what it had was NO CONSUMER above
//      `src/backends/rhi/`. M11.d added the consumer: `select_depth_stencil_format()` is the engine
//      picking, `FrameAssembly` calls it, and `validate_texture` refuses a depth target the device
//      does not support instead of letting a backend substitute quietly.
//
//   8. PUSH-CONSTANT RANGES CARRY AN OFFSET AND A STAGE MASK — NO CHANGE NEEDED, and that is
//      RECORDED rather than silently skipped. Vulkan shares one block across stages at declared
//      offsets; Metal's `setBytes:length:atIndex:` binds a whole buffer to one stage's argument
//      table, so a range shared between vertex and fragment becomes two small uploads. The sizes
//      involved make that free. Listed so the next reader does not re-derive it.
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

/// WHERE A GAP STANDS. M11.d task 1.5: `metal_gaps()` shrinks AS DATA, because a gap closed in
/// prose and not in this table is a gap that will be re-found at the first Metal compile.
///
/// The rows are NEVER DELETED when they close, and that is the point of having a status rather
/// than a shorter table: the finding, the remedy that was argued for, and what was actually done
/// are one row a reviewer reads together. What shrinks is `metal_open_gap_count()`.
enum class MetalGapStatus : u8 {
    /// Still true of `cy::rhi` as it stands.
    Open = 0,
    /// The interface changed. `closed_by` says what it changed to.
    Closed,
    /// Measured and found to need no change at all. Recorded rather than silently skipped, so the
    /// next reader does not spend an afternoon re-deriving that it is fine.
    NoChangeNeeded,
};

/// One gap: what the interface says, what Metal does instead, what changing it costs now against
/// what it costs at M11, and where it stands after M11.d settled the interface.
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
    /// Where the gap stands. Every row was `Open` when the seed was written at M7.
    MetalGapStatus status = MetalGapStatus::Open;
    /// What closed it, in the interface's own spelling, or why it needed nothing. Empty only while
    /// the gap is still open — a closed row with no account of how is a claim nobody can check.
    const char* closed_by = "";
};

[[nodiscard]] const MetalGapRecord& metal_gap(MetalGap gap) noexcept;
[[nodiscard]] Span<const MetalGapRecord> metal_gaps() noexcept;

/// HOW MANY ARE STILL OPEN. The number that shrinks, and the only honest measure of what M11.d's
/// interface work did: every row stays in the table, and this counts the ones a Metal backend would
/// still hit. Eight at M7; what it is now is what `unit.rhi_metal_seed` prints.
[[nodiscard]] u32 metal_open_gap_count() noexcept;

/// How many of the gaps have no workaround AND are still open. The number M11 pays for if nothing
/// changes before then, and the number this seed exists to make visible at M7.
[[nodiscard]] u32 metal_blocking_gap_count() noexcept;

}  // namespace cy::rhi::metal
