// The engine's vocabulary, in Vulkan's spelling. Task 2.3.1. See vulkan_translate.h.

#include "vulkan_translate.h"

namespace cy::rhi::vulkan {

const char* result_name(VkResult result) noexcept {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        default:
            break;
    }
    return "an unnamed VkResult";
}

Error error_from(VkResult result, const char* what) noexcept {
    ErrorCode code = ErrorCode::Internal;
    switch (result) {
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            code = ErrorCode::OutOfMemory;
            break;
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_FEATURE_NOT_PRESENT:
        case VK_ERROR_LAYER_NOT_PRESENT:
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            code = ErrorCode::Unsupported;
            break;
        case VK_ERROR_INITIALIZATION_FAILED:
        case VK_ERROR_INCOMPATIBLE_DRIVER:
        case VK_ERROR_SURFACE_LOST_KHR:
        // An out-of-date swapchain belongs in this group rather than with the internal errors: it
        // is answered by resizing, not by failing the frame, and the caller is expected to recover.
        case VK_ERROR_OUT_OF_DATE_KHR:
            code = ErrorCode::Unavailable;
            break;
        case VK_TIMEOUT:
            code = ErrorCode::Timeout;
            break;
        case VK_ERROR_DEVICE_LOST:
            code = ErrorCode::Internal;
            break;
        default:
            break;
    }
    return Error{code, what, static_cast<i64>(result)};
}

VkPipelineStageFlags2 to_vulkan(Stage stage) noexcept {
    if (any(stage & Stage::AllCommands)) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_NONE;
    if (any(stage & Stage::DrawIndirect)) {
        out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if (any(stage & Stage::VertexInput)) {
        out |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    }
    if (any(stage & Stage::VertexShader)) {
        out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (any(stage & Stage::FragmentShader)) {
        out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (any(stage & Stage::EarlyFragmentTests)) {
        out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    }
    if (any(stage & Stage::LateFragmentTests)) {
        out |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if (any(stage & Stage::ColorAttachmentOutput)) {
        out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (any(stage & Stage::ComputeShader)) {
        out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if (any(stage & Stage::Copy)) {
        out |= VK_PIPELINE_STAGE_2_COPY_BIT;
    }
    if (any(stage & Stage::Resolve)) {
        out |= VK_PIPELINE_STAGE_2_RESOLVE_BIT;
    }
    if (any(stage & Stage::Blit)) {
        out |= VK_PIPELINE_STAGE_2_BLIT_BIT;
    }
    if (any(stage & Stage::Clear)) {
        out |= VK_PIPELINE_STAGE_2_CLEAR_BIT;
    }
    if (any(stage & Stage::Host)) {
        out |= VK_PIPELINE_STAGE_2_HOST_BIT;
    }
    return out;
}

VkAccessFlags2 to_vulkan(AccessFlags access) noexcept {
    VkAccessFlags2 out = VK_ACCESS_2_NONE;
    if (any(access & AccessFlags::IndirectCommandRead)) {
        out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (any(access & AccessFlags::IndexRead)) {
        out |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if (any(access & AccessFlags::VertexAttributeRead)) {
        out |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (any(access & AccessFlags::UniformRead)) {
        out |= VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    if (any(access & AccessFlags::ShaderSampledRead)) {
        out |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    if (any(access & AccessFlags::ShaderStorageRead)) {
        out |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    }
    if (any(access & AccessFlags::ShaderStorageWrite)) {
        out |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (any(access & AccessFlags::ColorAttachmentRead)) {
        out |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    }
    if (any(access & AccessFlags::ColorAttachmentWrite)) {
        out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (any(access & AccessFlags::DepthStencilAttachmentRead)) {
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if (any(access & AccessFlags::DepthStencilAttachmentWrite)) {
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (any(access & AccessFlags::TransferRead)) {
        out |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if (any(access & AccessFlags::TransferWrite)) {
        out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if (any(access & AccessFlags::HostRead)) {
        out |= VK_ACCESS_2_HOST_READ_BIT;
    }
    if (any(access & AccessFlags::HostWrite)) {
        out |= VK_ACCESS_2_HOST_WRITE_BIT;
    }
    return out;
}

VkImageLayout to_vulkan(ImageLayout layout) noexcept {
    switch (layout) {
        case ImageLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case ImageLayout::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        case ImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilReadOnly:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageLayout::TransferSource:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageLayout::TransferDestination:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ImageLayout::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

namespace {

struct FormatPair {
    Format engine;
    VkFormat vulkan;
};

// One row per engine format, in enumerator order after Undefined.
constexpr FormatPair kFormats[] = {
    {Format::R8Unorm, VK_FORMAT_R8_UNORM},
    {Format::R8Uint, VK_FORMAT_R8_UINT},
    {Format::Rg8Unorm, VK_FORMAT_R8G8_UNORM},
    {Format::Rgba8Unorm, VK_FORMAT_R8G8B8A8_UNORM},
    {Format::Rgba8Srgb, VK_FORMAT_R8G8B8A8_SRGB},
    {Format::Bgra8Unorm, VK_FORMAT_B8G8R8A8_UNORM},
    {Format::Bgra8Srgb, VK_FORMAT_B8G8R8A8_SRGB},
    {Format::R16Uint, VK_FORMAT_R16_UINT},
    {Format::R16Sfloat, VK_FORMAT_R16_SFLOAT},
    {Format::Rg16Sfloat, VK_FORMAT_R16G16_SFLOAT},
    {Format::Rgba16Sfloat, VK_FORMAT_R16G16B16A16_SFLOAT},
    {Format::R32Uint, VK_FORMAT_R32_UINT},
    {Format::R32Sint, VK_FORMAT_R32_SINT},
    {Format::R32Sfloat, VK_FORMAT_R32_SFLOAT},
    {Format::Rg32Sfloat, VK_FORMAT_R32G32_SFLOAT},
    {Format::Rgb32Sfloat, VK_FORMAT_R32G32B32_SFLOAT},
    {Format::Rgba32Sfloat, VK_FORMAT_R32G32B32A32_SFLOAT},
    {Format::Rgb10A2Unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {Format::B10G11R11Ufloat, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
    {Format::D16Unorm, VK_FORMAT_D16_UNORM},
    {Format::D32Sfloat, VK_FORMAT_D32_SFLOAT},
    {Format::D24UnormS8Uint, VK_FORMAT_D24_UNORM_S8_UINT},
    {Format::D32SfloatS8Uint, VK_FORMAT_D32_SFLOAT_S8_UINT},
    {Format::Bc1RgbaUnorm, VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
    {Format::Bc1RgbaSrgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
    {Format::Bc3Unorm, VK_FORMAT_BC3_UNORM_BLOCK},
    {Format::Bc3Srgb, VK_FORMAT_BC3_SRGB_BLOCK},
    {Format::Bc4Unorm, VK_FORMAT_BC4_UNORM_BLOCK},
    {Format::Bc5Unorm, VK_FORMAT_BC5_UNORM_BLOCK},
    {Format::Bc6HUfloat, VK_FORMAT_BC6H_UFLOAT_BLOCK},
    {Format::Bc7Unorm, VK_FORMAT_BC7_UNORM_BLOCK},
    {Format::Bc7Srgb, VK_FORMAT_BC7_SRGB_BLOCK},
};

static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == static_cast<usize>(Format::Count) - 1,
              "every engine format needs a Vulkan spelling");

}  // namespace

VkFormat to_vulkan(Format format) noexcept {
    for (const FormatPair& pair : kFormats) {
        if (pair.engine == format) {
            return pair.vulkan;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vulkan(VkFormat format) noexcept {
    for (const FormatPair& pair : kFormats) {
        if (pair.vulkan == format) {
            return pair.engine;
        }
    }
    return Format::Undefined;
}

VkImageAspectFlags to_vulkan(ImageAspect aspect) noexcept {
    switch (aspect) {
        case ImageAspect::Color:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        case ImageAspect::Depth:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case ImageAspect::DepthStencil:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageAspectFlags aspect_of(Format format) noexcept {
    const FormatInfo& info = format_info(format);
    if (info.has_depth && info.has_stencil) {
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (info.has_depth) {
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageUsageFlags to_vulkan(TextureUsage usage) noexcept {
    VkImageUsageFlags out = 0;
    if (has_usage(usage, TextureUsage::TransferSource)) {
        out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (has_usage(usage, TextureUsage::TransferDestination)) {
        out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (has_usage(usage, TextureUsage::Sampled)) {
        out |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (has_usage(usage, TextureUsage::Storage)) {
        out |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (has_usage(usage, TextureUsage::ColorAttachment)) {
        out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (has_usage(usage, TextureUsage::DepthStencilAttachment)) {
        out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (has_usage(usage, TextureUsage::InputAttachment)) {
        out |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    if (has_usage(usage, TextureUsage::TransientAttachment)) {
        out |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    return out;
}

VkBufferUsageFlags to_vulkan(BufferUsage usage) noexcept {
    VkBufferUsageFlags out = 0;
    if (has_usage(usage, BufferUsage::TransferSource)) {
        out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (has_usage(usage, BufferUsage::TransferDestination)) {
        out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (has_usage(usage, BufferUsage::Uniform)) {
        out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (has_usage(usage, BufferUsage::Storage)) {
        out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (has_usage(usage, BufferUsage::Index)) {
        out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (has_usage(usage, BufferUsage::Vertex)) {
        out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (has_usage(usage, BufferUsage::Indirect)) {
        out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (has_usage(usage, BufferUsage::ShaderDeviceAddress)) {
        out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return out;
}

VkImageType to_vulkan_image_type(TextureDimension dimension) noexcept {
    switch (dimension) {
        case TextureDimension::Texture1D:
            return VK_IMAGE_TYPE_1D;
        case TextureDimension::Texture3D:
            return VK_IMAGE_TYPE_3D;
        case TextureDimension::Texture2D:
        case TextureDimension::Cube:
            break;
    }
    return VK_IMAGE_TYPE_2D;
}

VkImageViewType to_vulkan_view_type(TextureDimension dimension, u16 array_layers) noexcept {
    switch (dimension) {
        case TextureDimension::Texture1D:
            return array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case TextureDimension::Texture3D:
            return VK_IMAGE_VIEW_TYPE_3D;
        case TextureDimension::Cube:
            return array_layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureDimension::Texture2D:
            break;
    }
    return array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

VkFilter to_vulkan(Filter filter) noexcept {
    return filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vulkan(MipmapMode mode) noexcept {
    return mode == MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                       : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vulkan(AddressMode mode) noexcept {
    switch (mode) {
        case AddressMode::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case AddressMode::Repeat:
            break;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkCompareOp to_vulkan(CompareOp op) noexcept {
    switch (op) {
        case CompareOp::Never:
            return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:
            return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:
            return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessOrEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:
            return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterOrEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:
            return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_GREATER_OR_EQUAL;
}

VkPrimitiveTopology to_vulkan(PrimitiveTopology topology) noexcept {
    switch (topology) {
        case PrimitiveTopology::PointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::TriangleList:
            break;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkPolygonMode to_vulkan(PolygonMode mode) noexcept {
    switch (mode) {
        case PolygonMode::Line:
            return VK_POLYGON_MODE_LINE;
        case PolygonMode::Point:
            return VK_POLYGON_MODE_POINT;
        case PolygonMode::Fill:
            break;
    }
    return VK_POLYGON_MODE_FILL;
}

VkCullModeFlags to_vulkan(CullMode mode) noexcept {
    switch (mode) {
        case CullMode::Front:
            return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:
            return VK_CULL_MODE_BACK_BIT;
        case CullMode::None:
            break;
    }
    return VK_CULL_MODE_NONE;
}

VkFrontFace to_vulkan(FrontFace face) noexcept {
    return face == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkBlendFactor to_vulkan(BlendFactor factor) noexcept {
    switch (factor) {
        case BlendFactor::Zero:
            return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::SourceColor:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSourceColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DestinationColor:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDestinationColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SourceAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSourceAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DestinationAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDestinationAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::One:
            break;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp to_vulkan(BlendOp op) noexcept {
    switch (op) {
        case BlendOp::Subtract:
            return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:
            return VK_BLEND_OP_MIN;
        case BlendOp::Max:
            return VK_BLEND_OP_MAX;
        case BlendOp::Add:
            break;
    }
    return VK_BLEND_OP_ADD;
}

VkColorComponentFlags to_vulkan(ColorComponent components) noexcept {
    VkColorComponentFlags out = 0;
    const auto bits = static_cast<u8>(components);
    if ((bits & static_cast<u8>(ColorComponent::R)) != 0) {
        out |= VK_COLOR_COMPONENT_R_BIT;
    }
    if ((bits & static_cast<u8>(ColorComponent::G)) != 0) {
        out |= VK_COLOR_COMPONENT_G_BIT;
    }
    if ((bits & static_cast<u8>(ColorComponent::B)) != 0) {
        out |= VK_COLOR_COMPONENT_B_BIT;
    }
    if ((bits & static_cast<u8>(ColorComponent::A)) != 0) {
        out |= VK_COLOR_COMPONENT_A_BIT;
    }
    return out;
}

VkShaderStageFlags to_vulkan(ShaderStage stages) noexcept {
    VkShaderStageFlags out = 0;
    if (has_stage(stages, ShaderStage::Vertex)) {
        out |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (has_stage(stages, ShaderStage::Fragment)) {
        out |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (has_stage(stages, ShaderStage::Compute)) {
        out |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (has_stage(stages, ShaderStage::Geometry)) {
        out |= VK_SHADER_STAGE_GEOMETRY_BIT;
    }
    if (has_stage(stages, ShaderStage::TessellationControl)) {
        out |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    }
    if (has_stage(stages, ShaderStage::TessellationEvaluation)) {
        out |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    return out;
}

VkShaderStageFlagBits to_vulkan_single(ShaderStage stage) noexcept {
    if (has_stage(stage, ShaderStage::Vertex)) {
        return VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (has_stage(stage, ShaderStage::Fragment)) {
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (has_stage(stage, ShaderStage::Geometry)) {
        return VK_SHADER_STAGE_GEOMETRY_BIT;
    }
    return VK_SHADER_STAGE_COMPUTE_BIT;
}

VkDescriptorType to_vulkan(DescriptorKind kind) noexcept {
    switch (kind) {
        case DescriptorKind::StorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorKind::SampledTexture:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorKind::StorageTexture:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorKind::Sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorKind::CombinedTextureSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorKind::InputAttachment:
            return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case DescriptorKind::UniformBuffer:
            break;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkAttachmentLoadOp to_vulkan(LoadOp op) noexcept {
    switch (op) {
        case LoadOp::Load:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare:
            break;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp to_vulkan(StoreOp op) noexcept {
    // DontCare is not a micro-optimisation: on a tiled GPU it is the difference between writing a
    // whole render target to main memory and not writing it at all. The graph decides this from the
    // declared reads, so it is always the honest answer.
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkPresentModeKHR to_vulkan(PresentMode mode) noexcept {
    switch (mode) {
        case PresentMode::Immediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PresentMode::FifoRelaxed:
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        case PresentMode::Mailbox:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Fifo:
            break;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

PresentMode from_vulkan(VkPresentModeKHR mode) noexcept {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return PresentMode::Immediate;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return PresentMode::FifoRelaxed;
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return PresentMode::Mailbox;
        default:
            break;
    }
    return PresentMode::Fifo;
}

}  // namespace cy::rhi::vulkan
