#pragma once
// The RHI's value vocabulary: formats, layouts, stages, access masks, and the hard limits.
// Tasks 2.1.1 and 2.1.3.
//
// `rhi-and-render-graph` — "Explicit RHI". The interface is shaped around Vulkan because Vulkan is
// the most explicit of the three backends on the roadmap and mapping down to Metal is
// straightforward, while mapping up from a less explicit API would not be.
//
// SHAPED AROUND VULKAN IS NOT THE SAME AS SPELT IN VULKAN, and the difference is the whole reason
// this file exists. `Stage`, `AccessFlags` and `ImageLayout` below name the same concepts
// VK_PIPELINE_STAGE_2_*, VK_ACCESS_2_* and VkImageLayout name, with the same semantics, and none of
// them is a Vulkan type. That is what lets the render graph — which lives above src/backends/ and
// may not see a Vulkan header — derive every barrier in the frame. Exactly one file translates
// these into a backend's own enumerators (src/backends/rhi/vulkan/src/vulkan_translate.cpp), and
// tools/layercheck.py fails the build if a Vulkan, Slang or SPIR-V header appears above this layer.
//
// WHERE THE SPIKE AND THE SPECIFICATION DISAGREED. The M3 scheduling spike put the
// Access -> {stage, access, layout} table under src/backends/rhi/vulkan/ and typed it in Vulkan.
// The specification requires the graph to own synchronisation and forbids a Vulkan type above the
// backend layer; those two together put the table in the RHI, in engine types. The specification
// wins, and the cost is one translation function per backend rather than none.

#include <cy/core/base/types.h>

namespace cy::rhi {

// --- Hard limits ----------------------------------------------------------------------------
//
// `rhi-and-render-graph`: "Hard limits SHALL be defined and asserted". They are asserted at
// *creation* time, not at draw time — a pipeline that asks for nine descriptor sets fails when it
// is created, naming the limit, which is where the author can still do something about it.
//
// The numbers are the specification's. They are not the device's: a limit the engine states is a
// portability contract, and raising it because one device allows more is how a renderer becomes
// unportable one pipeline at a time. Device limits are reported separately, through
// DeviceCapabilities, and are checked against these at device creation.

inline constexpr u32 kMaxDescriptorSets = 8;
inline constexpr u32 kMaxPushConstantBytes = 128;
inline constexpr u32 kMaxVertexAttributes = 16;
inline constexpr u32 kMaxColorAttachments = 8;

/// Concurrent GPU frames. `rhi-and-render-graph` names 2 as the default; a device may be created
/// with fewer or more, bounded by kMaxFramesInFlight so that per-frame arrays are fixed-size.
inline constexpr u32 kDefaultFramesInFlight = 2;
inline constexpr u32 kMaxFramesInFlight = 4;

/// How many threads may record command buffers at once.
///
/// PARALLEL RECORDING IS NOT FREE OF STRUCTURE. A backend's command allocator is externally
/// synchronised — in Vulkan, a VkCommandPool may not be touched by two threads at once, and that
/// covers recording into any buffer allocated from it — so a device holds one command pool per
/// (frame, queue, recording slot) and a thread takes a slot the first time it acquires a command
/// buffer. A command buffer must then be recorded on the thread that acquired it.
///
/// The number is a ceiling on recording threads, not on workers: a job system with more workers
/// than this is fine as long as no more than this many of them record at once. Exceeding it is
/// reported as a failure from acquire_command_buffer() rather than silently sharing a pool, because
/// sharing one is a data race whose symptom is a corrupted command stream a week later.
inline constexpr u32 kMaxRecordingThreads = 16;

// --- Queues ---------------------------------------------------------------------------------

/// The queues the engine schedules onto. A device that has no dedicated async-compute or transfer
/// family reports so through DeviceCapabilities, and the render graph folds those passes onto the
/// graphics queue — which produces one submit, no semaphores and no ownership transfers from
/// exactly the same pass declarations. That fallback is the null backend's and continuous
/// integration's normal path, not a special case.
enum class QueueKind : u8 {
    Graphics = 0,
    AsyncCompute = 1,
    Transfer = 2,
    Count = 3,
};

inline constexpr u32 kQueueKindCount = static_cast<u32>(QueueKind::Count);

/// The enumerator's own spelling, for a diagnostic and for the graph's dump. Never null.
[[nodiscard]] const char* queue_kind_name(QueueKind queue) noexcept;

// --- Pipeline stages ------------------------------------------------------------------------
//
// A 64-bit mask because Vulkan's synchronisation2 stage mask is 64-bit and the engine must be able
// to express everything a backend can; the enumerators below are the subset the engine actually
// derives barriers from, and one is added when a pass needs it rather than pre-emptively.

enum class Stage : u64 {
    None = 0,
    DrawIndirect = 1ULL << 0,
    VertexInput = 1ULL << 1,
    VertexShader = 1ULL << 2,
    FragmentShader = 1ULL << 3,
    EarlyFragmentTests = 1ULL << 4,
    LateFragmentTests = 1ULL << 5,
    ColorAttachmentOutput = 1ULL << 6,
    ComputeShader = 1ULL << 7,
    Copy = 1ULL << 8,
    Resolve = 1ULL << 9,
    Blit = 1ULL << 10,
    Clear = 1ULL << 11,
    Host = 1ULL << 12,
    /// The catch-all. Used only where the specification says a mask is ignored — the halves of a
    /// queue-family ownership transfer — and never as a shortcut for "I did not work out the
    /// stage": a barrier that names it is a barrier that synchronises everything.
    AllCommands = 1ULL << 13,
};

[[nodiscard]] constexpr Stage operator|(Stage a, Stage b) noexcept {
    return static_cast<Stage>(static_cast<u64>(a) | static_cast<u64>(b));
}
[[nodiscard]] constexpr Stage operator&(Stage a, Stage b) noexcept {
    return static_cast<Stage>(static_cast<u64>(a) & static_cast<u64>(b));
}
constexpr Stage& operator|=(Stage& a, Stage b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool any(Stage value) noexcept {
    return static_cast<u64>(value) != 0;
}

// --- Access masks ---------------------------------------------------------------------------

enum class AccessFlags : u64 {
    None = 0,
    IndirectCommandRead = 1ULL << 0,
    IndexRead = 1ULL << 1,
    VertexAttributeRead = 1ULL << 2,
    UniformRead = 1ULL << 3,
    ShaderSampledRead = 1ULL << 4,
    ShaderStorageRead = 1ULL << 5,
    ShaderStorageWrite = 1ULL << 6,
    ColorAttachmentRead = 1ULL << 7,
    ColorAttachmentWrite = 1ULL << 8,
    DepthStencilAttachmentRead = 1ULL << 9,
    DepthStencilAttachmentWrite = 1ULL << 10,
    TransferRead = 1ULL << 11,
    TransferWrite = 1ULL << 12,
    HostRead = 1ULL << 13,
    HostWrite = 1ULL << 14,
};

[[nodiscard]] constexpr AccessFlags operator|(AccessFlags a, AccessFlags b) noexcept {
    return static_cast<AccessFlags>(static_cast<u64>(a) | static_cast<u64>(b));
}
[[nodiscard]] constexpr AccessFlags operator&(AccessFlags a, AccessFlags b) noexcept {
    return static_cast<AccessFlags>(static_cast<u64>(a) & static_cast<u64>(b));
}
constexpr AccessFlags& operator|=(AccessFlags& a, AccessFlags b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool any(AccessFlags value) noexcept {
    return static_cast<u64>(value) != 0;
}

// --- Image layouts --------------------------------------------------------------------------
//
// A layout is a property of a subresource, not of an image, which is why every one of them is
// tracked per (mip, layer) cell by the render graph. Nothing outside the graph ever names one:
// a pass declares an Access and the layout it implies is derived (see access.h).

enum class ImageLayout : u8 {
    Undefined = 0,  // contents are discarded; every transient's first use transitions from here
    General,
    ColorAttachment,
    DepthStencilAttachment,
    DepthStencilReadOnly,
    ShaderReadOnly,
    TransferSource,
    TransferDestination,
    Present,
};

[[nodiscard]] const char* image_layout_name(ImageLayout layout) noexcept;

/// The queue family a resource is not owned by anyone in particular on. Mirrors
/// VK_QUEUE_FAMILY_IGNORED's role: a resource in this state needs no ownership transfer.
inline constexpr u32 kQueueFamilyIgnored = ~0U;

// --- Formats --------------------------------------------------------------------------------
//
// The set M3 needs, plus the depth formats the reversed-Z work at task 5.1 asserts against. A
// format is added when something uses it; a table of two hundred enumerators nothing selects would
// be a table nobody keeps correct.

enum class Format : u16 {
    Undefined = 0,

    R8Unorm,
    R8Uint,
    Rg8Unorm,
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,

    R16Uint,
    R16Sfloat,
    Rg16Sfloat,
    Rgba16Sfloat,

    R32Uint,
    R32Sint,
    R32Sfloat,
    Rg32Sfloat,
    Rgb32Sfloat,
    Rgba32Sfloat,

    Rgb10A2Unorm,
    B10G11R11Ufloat,

    D16Unorm,
    D32Sfloat,
    D24UnormS8Uint,
    D32SfloatS8Uint,

    Bc1RgbaUnorm,
    Bc1RgbaSrgb,
    Bc3Unorm,
    Bc3Srgb,
    Bc4Unorm,
    Bc5Unorm,
    Bc6HUfloat,
    Bc7Unorm,
    Bc7Srgb,

    Count,
};

/// What the engine knows about a format without asking a device. `block_width` is 1 for an
/// uncompressed format, so `bytes_for_extent` is one expression rather than two branches.
struct FormatInfo {
    const char* name = "";
    u32 bytes_per_block = 0;
    u8 block_width = 1;
    u8 block_height = 1;
    bool has_depth = false;
    bool has_stencil = false;
    bool is_srgb = false;
    bool is_compressed = false;
};

[[nodiscard]] const FormatInfo& format_info(Format format) noexcept;
[[nodiscard]] const char* format_name(Format format) noexcept;

/// Tightly packed bytes for one mip of `width` x `height` in `format`. Zero for Format::Undefined,
/// which is the answer a caller can check rather than a number it would then multiply.
[[nodiscard]] u64 format_byte_size(Format format, u32 width, u32 height) noexcept;

/// Whether a format carries depth, stencil, or colour. The graph needs this to pick the aspect a
/// barrier names, and it is a property of the format rather than of the backend.
[[nodiscard]] bool format_is_depth_stencil(Format format) noexcept;

// --- Geometry -------------------------------------------------------------------------------

struct Extent2D {
    u32 width = 0;
    u32 height = 0;

    [[nodiscard]] friend constexpr bool operator==(Extent2D a, Extent2D b) noexcept {
        return a.width == b.width && a.height == b.height;
    }
};

struct Extent3D {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;

    [[nodiscard]] friend constexpr bool operator==(Extent3D a, Extent3D b) noexcept {
        return a.width == b.width && a.height == b.height && a.depth == b.depth;
    }
};

struct Offset3D {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
};

struct Viewport {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
    /// Reversed-Z: the engine clears depth to 0 and compares GreaterEqual, so the viewport's depth
    /// range stays [0, 1] and the *projection* is what inverts. See `core-math` and design.md §3 —
    /// inverting here instead would make every depth read in the engine ambiguous.
    f32 min_depth = 0.0F;
    f32 max_depth = 1.0F;
};

struct Rect2D {
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

// --- Subresources ---------------------------------------------------------------------------

/// A selection of an image's mips and array layers.
///
/// A count of 0 means "all remaining", and is resolved against the image the moment a pass declares
/// it — so nothing downstream ever carries a "remaining" sentinel, and every barrier the graph
/// emits names explicit counts. That resolution is what makes coalescing possible at all: two
/// ranges cannot be compared for adjacency while either of them means "whatever is left".
struct SubresourceRange {
    u16 base_mip = 0;
    u16 mip_count = 0;
    u16 base_layer = 0;
    u16 layer_count = 0;

    [[nodiscard]] static constexpr SubresourceRange whole() noexcept { return SubresourceRange{}; }
    [[nodiscard]] static constexpr SubresourceRange layer(u16 index) noexcept {
        return SubresourceRange{0, 0, index, 1};
    }
    [[nodiscard]] static constexpr SubresourceRange mip(u16 index) noexcept {
        return SubresourceRange{index, 1, 0, 0};
    }

    [[nodiscard]] friend constexpr bool operator==(const SubresourceRange& a,
                                                   const SubresourceRange& b) noexcept {
        return a.base_mip == b.base_mip && a.mip_count == b.mip_count &&
               a.base_layer == b.base_layer && a.layer_count == b.layer_count;
    }
};

// --- Attachment operations ------------------------------------------------------------------

enum class LoadOp : u8 {
    Load,
    Clear,
    DontCare,
};

/// `rhi-and-render-graph`, "Attachment store is elided": a render target nothing reads afterwards
/// gets DontCare, which matters greatly on a tiled GPU. The graph decides this from the declared
/// reads, never the pass author.
enum class StoreOp : u8 {
    Store,
    DontCare,
};

union ClearValue {
    f32 color[4];
    struct {
        f32 depth;
        u32 stencil;
    } depth_stencil;
};

/// Reversed-Z's clear value, named so that a pass never writes the literal. design.md §3: the depth
/// buffer is [0, 1], cleared to 0, compared GreaterEqual. A pass that cleared to 1.0F would look
/// correct until something intersected.
[[nodiscard]] constexpr ClearValue reversed_z_depth_clear() noexcept {
    ClearValue value{};
    value.depth_stencil.depth = 0.0F;
    value.depth_stencil.stencil = 0;
    return value;
}

}  // namespace cy::rhi
