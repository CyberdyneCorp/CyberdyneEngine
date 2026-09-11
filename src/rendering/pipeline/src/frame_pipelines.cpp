// The pipeline state objects the frame's passes bind. M8.c task 1b.1.

#include <cy/rendering/pipeline/frame_pipelines.h>

#include "frame_spirv.h"

#include <cy/core/math/vec.h>

#include <cmath>
#include <cstring>

namespace cy::rendering::pipeline {
namespace {

template <typename T, usize N>
[[nodiscard]] Span<const u32> words(const T (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

/// One binding of the view set. All seven are visible to both stages: the depth pipeline reads the
/// frame block and the instance rows in its vertex stage, and the forward one reads the material
/// table in its fragment stage, so a per-stage split would be two layouts describing one buffer.
[[nodiscard]] rhi::DescriptorBinding view_binding(u32 binding, rhi::DescriptorKind kind) noexcept {
    rhi::DescriptorBinding entry;
    entry.binding = binding;
    entry.kind = kind;
    entry.count = 1;
    entry.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    return entry;
}

[[nodiscard]] bool sample_count_is_legal(u32 samples) noexcept {
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

/// IEEE 754 binary32 to binary16, round-to-nearest-even, with no intrinsic and no library.
///
/// Written out because the engine has no half type: `core-math` carries `f32` and `f64` and
/// nothing between them, and the one place a half is needed is a vertex attribute format. Denormals
/// flush to zero, which is what every consumer of a normal vector wants and what the hardware does
/// on the read side anyway.
[[nodiscard]] u16 to_half(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16U) & 0x8000U;
    i32 exponent = static_cast<i32>((bits >> 23U) & 0xFFU) - 127 + 15;
    u32 mantissa = bits & 0x007FFFFFU;
    if (exponent >= 0x1F) {
        return static_cast<u16>(sign | 0x7C00U | (mantissa != 0 ? 0x0200U : 0U));
    }
    if (exponent <= 0) {
        return static_cast<u16>(sign);
    }
    // Round to nearest even on the ten bits kept.
    const u32 rounded = mantissa + 0x00000FFFU + ((mantissa >> 13U) & 1U);
    if ((rounded & 0x00800000U) != 0) {
        ++exponent;
        mantissa = 0;
    } else {
        mantissa = rounded;
    }
    if (exponent >= 0x1F) {
        return static_cast<u16>(sign | 0x7C00U);
    }
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10U) | (mantissa >> 13U));
}

/// The octahedral encoding `cy/packing.slang`'s `decodeOctahedral` inverts.
///
/// The [0, 1] range is the standard library's, not a choice made here: `encodeOctahedral` ends with
/// `* 0.5 + 0.5` and `decodeOctahedral` begins with `* 2.0 - 1.0`, so this function has to end the
/// same way or every normal in the frame is folded through the wrong octant.
[[nodiscard]] Vec2 encode_octahedral(Vec3 vector) noexcept {
    const f32 length = std::fabs(vector.x) + std::fabs(vector.y) + std::fabs(vector.z);
    const f32 scale = length > 0.0F ? 1.0F / length : 0.0F;
    f32 x = vector.x * scale;
    f32 y = vector.y * scale;
    if (vector.z < 0.0F) {
        const f32 folded_x = (1.0F - std::fabs(y)) * (x >= 0.0F ? 1.0F : -1.0F);
        const f32 folded_y = (1.0F - std::fabs(x)) * (y >= 0.0F ? 1.0F : -1.0F);
        x = folded_x;
        y = folded_y;
    }
    return Vec2{(x * 0.5F) + 0.5F, (y * 0.5F) + 0.5F};
}

}  // namespace

void pack_normal_stream(Vec3 normal, Vec3 tangent, u16 out[4]) noexcept {
    const Vec2 encoded_normal = encode_octahedral(normal);
    const Vec2 encoded_tangent = encode_octahedral(tangent);
    out[0] = to_half(encoded_normal.x);
    out[1] = to_half(encoded_normal.y);
    out[2] = to_half(encoded_tangent.x);
    out[3] = to_half(encoded_tangent.y);
}

const char* frame_pipeline_kind_name(FramePipelineKind kind) noexcept {
    switch (kind) {
        case FramePipelineKind::Depth:
            return "depth";
        case FramePipelineKind::Opaque:
            return "opaque";
        case FramePipelineKind::Transparent:
            return "transparent";
        case FramePipelineKind::Resolve:
            return "resolve";
        case FramePipelineKind::Count:
            break;
    }
    return "unknown";
}

FramePipelines::~FramePipelines() {
    shutdown();
}

rhi::DescriptorSetLayoutHandle FramePipelines::set_layout(u32 set) const noexcept {
    if (set >= kSetCount) {
        return rhi::DescriptorSetLayoutHandle{};
    }
    return sets_[set];
}

rhi::GraphicsPipelineHandle FramePipelines::pipeline(FramePipelineKind kind) const noexcept {
    const auto index = static_cast<u32>(kind);
    if (index >= kFramePipelineKindCount) {
        return rhi::GraphicsPipelineHandle{};
    }
    return pipelines_[index];
}

Status FramePipelines::create_modules(rhi::Device& device) noexcept {
    struct Request {
        const char* name;
        rhi::ShaderStage stage;
        const char* entry;
        Span<const u32> spirv;
        rhi::ShaderModuleHandle* out;
    };
    const Request requests[] = {
        {"cy frame depth vertex", rhi::ShaderStage::Vertex, "main", words(kFrameDepthVertexSpirv),
         &depth_vertex_},
        {"cy frame depth fragment", rhi::ShaderStage::Fragment, "main",
         words(kFrameDepthFragmentSpirv), &depth_fragment_},
        {"cy frame forward vertex", rhi::ShaderStage::Vertex, "main",
         words(kFrameForwardVertexSpirv), &forward_vertex_},
        {"cy frame forward fragment", rhi::ShaderStage::Fragment, "main",
         words(kFrameForwardFragmentSpirv), &forward_fragment_},
        {"cy frame resolve vertex", rhi::ShaderStage::Vertex, "main",
         words(kFrameResolveVertexSpirv), &resolve_vertex_},
        {"cy frame resolve fragment", rhi::ShaderStage::Fragment, "main",
         words(kFrameResolveFragmentSpirv), &resolve_fragment_},
    };
    for (const Request& request : requests) {
        rhi::ShaderModuleDescription description;
        description.name = request.name;
        description.stage = request.stage;
        description.entry_point = request.entry;
        description.spirv = request.spirv;
        Expected<rhi::ShaderModuleHandle, Error> module = device.create_shader_module(description);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        *request.out = *module;
    }
    return ok();
}

Status FramePipelines::create_layouts(rhi::Device& device) noexcept {
    const rhi::DescriptorBinding globals[] = {
        view_binding(0, rhi::DescriptorKind::UniformBuffer),
    };
    const rhi::DescriptorBinding view[] = {
        view_binding(kViewBindingFrame, rhi::DescriptorKind::UniformBuffer),
        view_binding(kViewBindingLights, rhi::DescriptorKind::StorageBuffer),
        view_binding(kViewBindingClusterHeaders, rhi::DescriptorKind::StorageBuffer),
        view_binding(kViewBindingClusterIndices, rhi::DescriptorKind::StorageBuffer),
        view_binding(kViewBindingDrawInstances, rhi::DescriptorKind::StorageBuffer),
        view_binding(kViewBindingInstances, rhi::DescriptorKind::StorageBuffer),
        view_binding(kViewBindingMaterials, rhi::DescriptorKind::StorageBuffer),
    };
    const rhi::DescriptorBinding pass[] = {
        view_binding(kPassBindingSceneColor, rhi::DescriptorKind::SampledTexture),
        view_binding(kPassBindingSampler, rhi::DescriptorKind::Sampler),
    };

    struct SetRequest {
        const char* name;
        Span<const rhi::DescriptorBinding> bindings;
    };
    const SetRequest sets[kSetCount] = {
        {"cy frame globals", Span<const rhi::DescriptorBinding>(globals, 1)},
        {"cy frame view", Span<const rhi::DescriptorBinding>(view, kViewBindingCount)},
        {"cy frame pass", Span<const rhi::DescriptorBinding>(pass, 2)},
    };
    for (u32 index = 0; index < kSetCount; ++index) {
        rhi::DescriptorSetLayoutDescription description;
        description.name = sets[index].name;
        description.bindings = sets[index].bindings;
        Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
            device.create_descriptor_set_layout(description);
        if (!layout.has_value()) {
            return make_unexpected(layout.error());
        }
        sets_[index] = *layout;
    }

    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(DrawPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "cy frame layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets_, kSetCount);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created = device.create_pipeline_layout(layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    layout_ = *created;

    rhi::SamplerDescription sampler;
    sampler.name = "cy frame linear clamp";
    sampler.address_u = rhi::AddressMode::ClampToEdge;
    sampler.address_v = rhi::AddressMode::ClampToEdge;
    sampler.address_w = rhi::AddressMode::ClampToEdge;
    Expected<rhi::SamplerHandle, Error> made = device.create_sampler(sampler);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    sampler_ = *made;
    return ok();
}

Status FramePipelines::create_geometry_pipeline(rhi::Device& device, const PipelineSetup& setup,
                                                FramePipelineKind kind) noexcept {
    const bool depth_only = kind == FramePipelineKind::Depth;
    const bool blended = kind == FramePipelineKind::Transparent;

    const rhi::VertexBinding bindings[] = {
        {kPositionStream, kPositionStreamStride, rhi::VertexInputRate::PerVertex},
        {kNormalStream, kNormalStreamStride, rhi::VertexInputRate::PerVertex},
        {kUvStream, kUvStreamStride, rhi::VertexInputRate::PerVertex},
    };
    const rhi::VertexAttribute attributes[] = {
        {0, kPositionStream, rhi::Format::Rgb32Sfloat, 0},
        {1, kNormalStream, rhi::Format::Rgba16Sfloat, 0},
        {2, kUvStream, rhi::Format::Rg32Sfloat, 0},
    };
    // THE DEPTH PIPELINE BINDS ONE STREAM. `render::kDepthPassStreams` is `stream_bit(Position)`
    // and this is what makes that constant structural: a depth pass that declared three bindings
    // would need three buffers bound to draw, which is exactly the bandwidth the stream split
    // exists to avoid.
    const usize stream_count = depth_only ? 1U : 3U;

    rhi::ColorAttachmentState color;
    color.format = setup.color_format;
    if (blended) {
        // `rendering-forward-clustered`: transparent draws are sorted back to front and composited
        // with source-alpha blending. The pipeline says so rather than the pass.
        color.blend_enable = true;
        color.source_color = rhi::BlendFactor::SourceAlpha;
        color.destination_color = rhi::BlendFactor::OneMinusSourceAlpha;
        color.source_alpha = rhi::BlendFactor::One;
        color.destination_alpha = rhi::BlendFactor::OneMinusSourceAlpha;
    }

    rhi::GraphicsPipelineDescription description;
    description.name = frame_pipeline_kind_name(kind);
    description.layout = layout_;
    description.vertex_shader = depth_only ? depth_vertex_ : forward_vertex_;
    description.fragment_shader = depth_only ? depth_fragment_ : forward_fragment_;
    description.vertex_bindings = Span<const rhi::VertexBinding>(bindings, stream_count);
    description.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, stream_count);
    description.color_attachments = depth_only ? Span<const rhi::ColorAttachmentState>()
                                               : Span<const rhi::ColorAttachmentState>(&color, 1);
    description.sample_count = setup.sample_count;
    description.depth_stencil.format = setup.depth_format;
    description.depth_stencil.depth_test_enable = true;
    // REVERSED-Z, AND THE THREE CASES ARE NOT THE SAME. The prepass writes depth and compares
    // GreaterOrEqual. The opaque pass tests EQUAL against what the prepass wrote and does not write
    // — `rendering-forward-clustered` states that outright, and `ForwardFrame` declares
    // `DepthStencilAttachmentRead` for it so the graph derives a read barrier rather than a write
    // one. A transparent draw tests GreaterOrEqual against the opaque depth and does not write it.
    description.depth_stencil.depth_write_enable = depth_only;
    description.depth_stencil.depth_compare =
        kind == FramePipelineKind::Opaque ? rhi::CompareOp::Equal : rhi::CompareOp::GreaterOrEqual;
    description.rasterisation.cull_mode = blended ? rhi::CullMode::None : rhi::CullMode::Back;
    description.rasterisation.front_face = rhi::FrontFace::CounterClockwise;

    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device.create_graphics_pipeline(description);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipelines_[static_cast<u32>(kind)] = *created;
    ++created_;
    return ok();
}

Status FramePipelines::create_resolve_pipeline(rhi::Device& device,
                                               const PipelineSetup& setup) noexcept {
    // `cy/fullscreen.slang`'s own two entry points, unmodified: one oversized triangle derived from
    // `SV_VertexID` with no vertex buffer, and the exposure-plus-tonemap resolve over it. The
    // standard library supplies the pass; this module supplies the state it runs in.
    rhi::ColorAttachmentState color;
    color.format = setup.output_format;

    rhi::GraphicsPipelineDescription description;
    description.name = "resolve";
    description.layout = layout_;
    description.vertex_shader = resolve_vertex_;
    description.fragment_shader = resolve_fragment_;
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    // No depth attachment at all: the post chain writes the output and reads the scene colour, and
    // `ForwardFrame`'s post-process pass declares neither a depth read nor a depth write.
    description.depth_stencil.format = rhi::Format::Undefined;
    description.depth_stencil.depth_test_enable = false;
    description.depth_stencil.depth_write_enable = false;

    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device.create_graphics_pipeline(description);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipelines_[static_cast<u32>(FramePipelineKind::Resolve)] = *created;
    ++created_;
    return ok();
}

Status FramePipelines::create_pipelines(rhi::Device& device, const PipelineSetup& setup) noexcept {
    if (Status made = create_geometry_pipeline(device, setup, FramePipelineKind::Depth); !made) {
        return made;
    }
    if (Status made = create_geometry_pipeline(device, setup, FramePipelineKind::Opaque); !made) {
        return made;
    }
    if (setup.transparency) {
        if (Status made = create_geometry_pipeline(device, setup, FramePipelineKind::Transparent);
            !made) {
            return made;
        }
    }
    if (setup.tonemap) {
        if (Status made = create_resolve_pipeline(device, setup); !made) {
            return made;
        }
    }
    return ok();
}

Status FramePipelines::initialize(rhi::Device& device, const PipelineSetup& setup) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "frame pipelines: already initialized");
    }
    if (!sample_count_is_legal(setup.sample_count)) {
        return fail(ErrorCode::InvalidArgument, "frame pipelines: MSAA must be 1, 2, 4 or 8");
    }
    if (setup.color_format == rhi::Format::Undefined ||
        setup.depth_format == rhi::Format::Undefined) {
        return fail(ErrorCode::InvalidArgument,
                    "frame pipelines: the colour and depth formats must be the frame's");
    }
    device_ = &device;
    setup_ = setup;
    created_ = 0;
    if (Status made = create_modules(device); !made) {
        shutdown();
        return made;
    }
    if (Status made = create_layouts(device); !made) {
        shutdown();
        return made;
    }
    if (Status made = create_pipelines(device, setup); !made) {
        shutdown();
        return made;
    }
    ready_ = true;
    return ok();
}

void FramePipelines::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (rhi::GraphicsPipelineHandle& handle : pipelines_) {
        if (!handle.is_null()) {
            device.destroy_graphics_pipeline(handle);
            handle = rhi::GraphicsPipelineHandle{};
        }
    }
    if (!sampler_.is_null()) {
        device.destroy_sampler(sampler_);
        sampler_ = rhi::SamplerHandle{};
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
        layout_ = rhi::PipelineLayoutHandle{};
    }
    for (rhi::DescriptorSetLayoutHandle& handle : sets_) {
        if (!handle.is_null()) {
            device.destroy_descriptor_set_layout(handle);
            handle = rhi::DescriptorSetLayoutHandle{};
        }
    }
    rhi::ShaderModuleHandle* modules[] = {&depth_vertex_,     &depth_fragment_, &forward_vertex_,
                                          &forward_fragment_, &resolve_vertex_, &resolve_fragment_};
    for (rhi::ShaderModuleHandle* handle : modules) {
        if (!handle->is_null()) {
            device.destroy_shader_module(*handle);
            *handle = rhi::ShaderModuleHandle{};
        }
    }
    device_ = nullptr;
    created_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::pipeline
