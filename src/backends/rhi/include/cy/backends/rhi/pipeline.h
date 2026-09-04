#pragma once
// Shader modules, pipeline layouts, descriptors and pipelines. Task 2.1.1.
//
// `rhi-and-render-graph`: shaders are consumed as SPIR-V and "reflected at load to extract
// descriptor bindings, push-constant ranges, vertex inputs, specialization constants, and compute
// workgroup size". The reflection itself belongs to `shader-system` (src/backends/shader/, task
// 3.3); what lives here is the shape the reflection produces and the pipeline it is used to build.
//
// SPIR-V IS BYTES TO THIS MODULE. A ShaderModuleDescription carries a span of words and nothing
// else — no SPIRV-Reflect type, no Slang type. A backend that does not consume SPIR-V natively
// translates offline and caches the result, which is `rhi-and-render-graph`'s Metal path and is why
// this interface names the interchange form rather than any toolchain's representation.
//
// DESCRIPTORS: BINDLESS IS THE DEFAULT AND THE CLASSIC PATH IS COMPATIBILITY. That is architectural
// rather than an optimisation — a draw workload generated on the GPU has no CPU in the loop to bind
// a set per draw. `DescriptorModel` below is what the renderer reads to find out which path it got,
// and `rhi-and-render-graph` requires the reduced capability to be reported rather than silently
// degraded.

#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/resources.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

// --- Shader modules -------------------------------------------------------------------------

enum class ShaderStage : u16 {
    None = 0,
    Vertex = 1U << 0,
    Fragment = 1U << 1,
    Compute = 1U << 2,
    Geometry = 1U << 3,
    TessellationControl = 1U << 4,
    TessellationEvaluation = 1U << 5,
    Task = 1U << 6,
    Mesh = 1U << 7,
};

[[nodiscard]] constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) noexcept {
    return static_cast<ShaderStage>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr bool has_stage(ShaderStage set, ShaderStage stage) noexcept {
    return (static_cast<u16>(set) & static_cast<u16>(stage)) != 0;
}

struct ShaderModuleDescription {
    const char* name = "shader";
    ShaderStage stage = ShaderStage::None;
    /// SPIR-V, as 32-bit words. Not owned: the caller keeps it alive across the create call only.
    Span<const u32> spirv;
    /// The function the module is entered at. Slang and HLSL name it; GLSL always says "main".
    const char* entry_point = "main";
};

/// One specialization constant, supplied at pipeline creation.
///
/// `rhi-and-render-graph`, "Specialization over permutation": a feature that can be a
/// specialization constant SHALL be one, rather than a separate preprocessor permutation. The
/// difference is one SPIR-V module in the cache instead of 2^n.
struct SpecializationConstant {
    u32 id = 0;
    u32 value = 0;
};

// --- Descriptors ----------------------------------------------------------------------------

enum class DescriptorKind : u8 {
    UniformBuffer,
    StorageBuffer,
    SampledTexture,
    StorageTexture,
    Sampler,
    CombinedTextureSampler,
    InputAttachment,
};

/// One binding in a descriptor set layout. `count` greater than one is an array; `count` of zero
/// means a runtime-sized array, which is what a bindless global table is.
struct DescriptorBinding {
    u32 binding = 0;
    DescriptorKind kind = DescriptorKind::UniformBuffer;
    u32 count = 1;
    ShaderStage stages = ShaderStage::None;
    /// Partially bound: a slot in the array may be unwritten as long as the shader does not read
    /// it. A streaming texture table needs this; a device without
    /// Capability::BindlessPartiallyBound cannot have it and the layout creation says so.
    bool partially_bound = false;
};

struct DescriptorSetLayoutDescription {
    const char* name = "set-layout";
    Span<const DescriptorBinding> bindings;
};

/// Which resource model a device gave the renderer.
enum class DescriptorModel : u8 {
    /// Global descriptor arrays indexed from the shader. The default, and what GPU-driven rendering
    /// requires.
    Bindless,
    /// Per-material descriptor sets. `rhi-and-render-graph` requires the renderer's structure to be
    /// unchanged on this path and the reduced GPU-driven capability to be reported.
    Compatibility,
};

[[nodiscard]] const char* descriptor_model_name(DescriptorModel model) noexcept;

/// One write into a descriptor set. A backend applies a batch of these in one call.
struct DescriptorWrite {
    u32 binding = 0;
    u32 array_index = 0;
    DescriptorKind kind = DescriptorKind::UniformBuffer;
    BufferHandle buffer;
    u64 buffer_offset = 0;
    u64 buffer_range = 0;  // zero means the rest of the buffer
    TextureViewHandle texture_view;
    SamplerHandle sampler;
    ImageLayout layout = ImageLayout::ShaderReadOnly;
};

// --- Pipeline layouts -----------------------------------------------------------------------

struct PushConstantRange {
    ShaderStage stages = ShaderStage::None;
    u32 offset = 0;
    u32 size = 0;
};

struct PipelineLayoutDescription {
    const char* name = "layout";
    /// At most kMaxDescriptorSets. Creation fails naming the limit when it is exceeded — at
    /// creation time rather than at draw time, which is what `rhi-and-render-graph` requires.
    Span<const DescriptorSetLayoutHandle> set_layouts;
    /// At most kMaxPushConstantBytes in total.
    Span<const PushConstantRange> push_constants;
};

// --- Graphics pipeline state ----------------------------------------------------------------

enum class PrimitiveTopology : u8 {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

enum class PolygonMode : u8 { Fill, Line, Point };

enum class CullMode : u8 { None, Front, Back };

/// The engine is right-handed, Y-up, -Z forward (`core-math`), which makes a front face
/// counter-clockwise when seen from the front. Stated as an enumerator rather than assumed, because
/// this is exactly the convention that is discovered to be wrong by looking at a rendered image.
enum class FrontFace : u8 { CounterClockwise, Clockwise };

enum class BlendFactor : u8 {
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    DestinationColor,
    OneMinusDestinationColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
};

enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };

enum class ColorComponent : u8 {
    None = 0,
    R = 1U << 0,
    G = 1U << 1,
    B = 1U << 2,
    A = 1U << 3,
    All = R | G | B | A,
};

[[nodiscard]] constexpr ColorComponent operator|(ColorComponent a, ColorComponent b) noexcept {
    return static_cast<ColorComponent>(static_cast<u8>(a) | static_cast<u8>(b));
}

struct ColorAttachmentState {
    Format format = Format::Undefined;
    bool blend_enable = false;
    BlendFactor source_color = BlendFactor::One;
    BlendFactor destination_color = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add;
    BlendFactor source_alpha = BlendFactor::One;
    BlendFactor destination_alpha = BlendFactor::Zero;
    BlendOp alpha_op = BlendOp::Add;
    ColorComponent write_mask = ColorComponent::All;
};

struct DepthStencilState {
    Format format = Format::Undefined;
    bool depth_test_enable = false;
    bool depth_write_enable = false;
    /// GreaterOrEqual is the engine's reversed-Z comparison and therefore the default. A pipeline
    /// that wants Less has to say so, which is the right way round: the unusual case is the one
    /// that carries the explanation. design.md §3.
    CompareOp depth_compare = CompareOp::GreaterOrEqual;
    bool stencil_test_enable = false;
};

struct RasterisationState {
    PolygonMode polygon_mode = PolygonMode::Fill;
    CullMode cull_mode = CullMode::Back;
    FrontFace front_face = FrontFace::CounterClockwise;
    bool depth_clamp_enable = false;
    f32 depth_bias_constant = 0.0F;
    f32 depth_bias_slope = 0.0F;
    f32 line_width = 1.0F;
};

enum class VertexInputRate : u8 { PerVertex, PerInstance };

struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;
    VertexInputRate input_rate = VertexInputRate::PerVertex;
};

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    Format format = Format::Undefined;
    u32 offset = 0;
};

struct GraphicsPipelineDescription {
    const char* name = "pipeline";
    PipelineLayoutHandle layout;
    ShaderModuleHandle vertex_shader;
    ShaderModuleHandle fragment_shader;
    Span<const SpecializationConstant> specialization;
    Span<const VertexBinding> vertex_bindings;
    /// At most kMaxVertexAttributes.
    Span<const VertexAttribute> vertex_attributes;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterisationState rasterisation{};
    DepthStencilState depth_stencil{};
    /// At most kMaxColorAttachments. Formats here are the dynamic-rendering equivalent of a render
    /// pass's attachment descriptions — Vulkan 1.3 is the baseline, so there are no VkRenderPass
    /// objects to keep in step with a framebuffer.
    Span<const ColorAttachmentState> color_attachments;
    u32 sample_count = 1;
    u32 view_mask = 0;  // multiview; zero is single-view. The XR prerequisite check reads this.
};

struct ComputePipelineDescription {
    const char* name = "pipeline";
    PipelineLayoutHandle layout;
    ShaderModuleHandle shader;
    Span<const SpecializationConstant> specialization;
};

}  // namespace cy::rhi
