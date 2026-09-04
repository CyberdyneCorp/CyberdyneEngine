#pragma once
// Resource descriptors: what a caller fills in to create a buffer, a texture, a view or a sampler.
// Task 2.1.1.
//
// `rhi-and-render-graph`: "Resources SHALL be created from descriptor structs using designated
// initializers". Every field therefore has a default that is either the common case or an obviously
// invalid value, so a `TextureDescription{.format = ..., .extent = ...}` reads as the two decisions
// the caller actually made.
//
// USAGE IS DECLARED, NOT INFERRED. A backend needs to know at creation time what a resource will be
// used for — Vulkan bakes it into VkImageUsageFlags and Metal into a storage mode — and inferring
// it from the render graph would mean creating resources after the graph is compiled, which is the
// wrong order for anything that outlives a frame. Transient graph resources are the exception and
// the graph fills their usage in from the accesses declared against them.

#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/types.h>

namespace cy::rhi {

// --- Buffers --------------------------------------------------------------------------------

enum class BufferUsage : u32 {
    None = 0,
    TransferSource = 1U << 0,
    TransferDestination = 1U << 1,
    Uniform = 1U << 2,
    Storage = 1U << 3,
    Index = 1U << 4,
    Vertex = 1U << 5,
    Indirect = 1U << 6,
    ShaderDeviceAddress = 1U << 7,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_usage(BufferUsage set, BufferUsage usage) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(usage)) != 0;
}

/// Where a resource's memory lives and who may touch it.
///
/// `HostVisibleDeviceLocal` is not a synonym for `Upload`: it is the unified-memory and
/// resizable-BAR case `rhi-and-render-graph` singles out, where per-frame instance data is written
/// straight to device-local memory and the staging copy is skipped. A device without it reports
/// Capability::HostVisibleDeviceLocalMemory false and the allocator falls back to Upload.
enum class MemoryUse : u8 {
    DeviceLocal,             // GPU only: render targets, static geometry, the GPU scene
    Upload,                  // written by the CPU, read by the GPU: staging, per-frame constants
    Readback,                // written by the GPU, read by the CPU
    HostVisibleDeviceLocal,  // both, without a copy, where the device offers it
};

struct BufferDescription {
    /// Never null and never owned. Names the resource for a debug tool and for the crash artefact;
    /// `rhi-and-render-graph` requires every resource to be named.
    const char* name = "buffer";
    u64 size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryUse memory = MemoryUse::DeviceLocal;
};

// --- Textures -------------------------------------------------------------------------------

enum class TextureDimension : u8 {
    Texture1D,
    Texture2D,
    Texture3D,
    Cube,
};

enum class TextureUsage : u32 {
    None = 0,
    TransferSource = 1U << 0,
    TransferDestination = 1U << 1,
    Sampled = 1U << 2,
    Storage = 1U << 3,
    ColorAttachment = 1U << 4,
    DepthStencilAttachment = 1U << 5,
    InputAttachment = 1U << 6,
    /// A tiled GPU can keep an attachment in tile memory and never write it to main memory. The
    /// graph sets this when a target's lifetime is one pass and nothing samples it.
    TransientAttachment = 1U << 7,
};

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr TextureUsage& operator|=(TextureUsage& a, TextureUsage b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool has_usage(TextureUsage set, TextureUsage usage) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(usage)) != 0;
}

struct TextureDescription {
    const char* name = "texture";
    TextureDimension dimension = TextureDimension::Texture2D;
    Format format = Format::Undefined;
    Extent3D extent{};
    u16 mip_levels = 1;
    u16 array_layers = 1;
    u16 sample_count = 1;
    TextureUsage usage = TextureUsage::None;
    MemoryUse memory = MemoryUse::DeviceLocal;
};

/// A view onto a texture's subresources.
///
/// SPIKE GOTCHA 6d, AND THE REASON THE GRAPH OWNS VIEW CREATION. A combined-image-sampler
/// descriptor's view must match the range the pass declared. Declaring layer 0 while binding a
/// two-layer array view produced a validation error on the layer the graph had never transitioned —
/// so the graph derives the view from the declared range rather than letting a pass author pick
/// one.
struct TextureViewDescription {
    const char* name = "view";
    TextureHandle texture;
    TextureDimension dimension = TextureDimension::Texture2D;
    /// Format::Undefined means "the texture's own format", which is what nearly every view wants.
    Format format = Format::Undefined;
    SubresourceRange range{};
};

// --- Samplers -------------------------------------------------------------------------------

enum class Filter : u8 { Nearest, Linear };
enum class MipmapMode : u8 { Nearest, Linear };

enum class AddressMode : u8 {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

/// Reversed-Z again: a shadow comparison sampler compares GreaterEqual, because the engine's depth
/// buffer is [0, 1] with near at 1. A sampler that compared Less would sample the shadow map
/// inside-out, and nothing about the image would say so.
enum class CompareOp : u8 {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

struct SamplerDescription {
    const char* name = "sampler";
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    MipmapMode mipmap_mode = MipmapMode::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    AddressMode address_w = AddressMode::Repeat;
    f32 mip_lod_bias = 0.0F;
    f32 max_anisotropy = 1.0F;  // 1 disables anisotropic filtering
    bool compare_enable = false;
    CompareOp compare_op = CompareOp::GreaterOrEqual;
    f32 min_lod = 0.0F;
    f32 max_lod = 1000.0F;
};

// --- Queries --------------------------------------------------------------------------------

enum class QueryKind : u8 {
    Timestamp,
    PipelineStatistics,
    Occlusion,
};

struct QueryPoolDescription {
    const char* name = "queries";
    QueryKind kind = QueryKind::Timestamp;
    u32 count = 0;
};

// --- Swapchain ------------------------------------------------------------------------------

/// How presentation paces. Mirrors DisplayServer's VSyncMode rather than restating it: the window
/// owns the request and the swapchain honours what the device supports, reporting back what it got.
enum class PresentMode : u8 {
    Immediate,
    Fifo,         // one presentation per refresh; always supported
    FifoRelaxed,  // tear rather than stall when a frame is late
    Mailbox,      // present the newest finished frame, never stall, never tear
};

struct SwapchainDescription {
    const char* name = "swapchain";
    /// The native surface DisplayServer::create_surface() produced for this backend's GraphicsApi.
    /// The RHI never talks to a window system; it is handed a surface and does not ask how.
    void* native_surface = nullptr;
    Extent2D extent{};
    Format preferred_format = Format::Bgra8Srgb;
    PresentMode present_mode = PresentMode::Fifo;
    u32 min_image_count = 3;
};

/// What a swapchain turned out to be, which is not always what was asked for.
struct SwapchainInfo {
    Format format = Format::Undefined;
    PresentMode present_mode = PresentMode::Fifo;
    Extent2D extent{};
    u32 image_count = 0;
};

}  // namespace cy::rhi
