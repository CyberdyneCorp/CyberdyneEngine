// The limit checks, one per creation call. Task 2.1.1.

#include <cy/backends/rhi/validation.h>

#include <cstdarg>
#include <cstdio>

namespace cy::rhi {

// NOLINTNEXTLINE(cert-dcl50-cpp)
const char* ValidationMessage::format(const char* pattern, ...) noexcept {
    va_list arguments;
    va_start(arguments, pattern);
    const int written = std::vsnprintf(text, sizeof(text), pattern, arguments);
    va_end(arguments);
    if (written < 0) {
        text[0] = '\0';
    }
    return text;
}

Status validate_buffer(const BufferDescription& desc, ValidationMessage& message) noexcept {
    if (desc.size == 0) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("buffer '%s': a size of zero has no address a caller may read",
                                   desc.name));
    }
    if (desc.usage == BufferUsage::None) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("buffer '%s': no usage declared. A backend bakes usage into the "
                                   "allocation, so it cannot be inferred later",
                                   desc.name));
    }
    if (desc.memory == MemoryUse::Readback &&
        !has_usage(desc.usage, BufferUsage::TransferDestination)) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("buffer '%s': a readback buffer must declare "
                                   "BufferUsage::TransferDestination — nothing else can fill it",
                                   desc.name));
    }
    return ok();
}

Status validate_texture(const TextureDescription& desc, const DeviceLimits& limits,
                        ValidationMessage& message) noexcept {
    if (desc.format == Format::Undefined) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("texture '%s': Format::Undefined", desc.name));
    }
    if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("texture '%s': extent %ux%ux%u has a zero dimension", desc.name,
                                   desc.extent.width, desc.extent.height, desc.extent.depth));
    }
    if (desc.mip_levels == 0 || desc.array_layers == 0) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("texture '%s': %u mips and %u layers; both are at least 1",
                                   desc.name, desc.mip_levels, desc.array_layers));
    }
    if (desc.usage == TextureUsage::None) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("texture '%s': no usage declared", desc.name));
    }
    if (limits.max_texture_dimension_2d != 0 &&
        (desc.extent.width > limits.max_texture_dimension_2d ||
         desc.extent.height > limits.max_texture_dimension_2d)) {
        return fail(ErrorCode::OutOfRange,
                    message.format("texture '%s': %ux%u exceeds the device's maximum 2D dimension "
                                   "of %u",
                                   desc.name, desc.extent.width, desc.extent.height,
                                   limits.max_texture_dimension_2d));
    }
    if (limits.max_texture_array_layers != 0 &&
        desc.array_layers > limits.max_texture_array_layers) {
        return fail(ErrorCode::OutOfRange,
                    message.format("texture '%s': %u array layers exceeds the device's maximum of "
                                   "%u",
                                   desc.name, desc.array_layers, limits.max_texture_array_layers));
    }
    if (format_is_depth_stencil(desc.format) &&
        has_usage(desc.usage, TextureUsage::ColorAttachment)) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("texture '%s': %s is a depth format and cannot be a colour "
                                   "attachment",
                                   desc.name, format_name(desc.format)));
    }
    return ok();
}

Status validate_texture_view(const TextureViewDescription& desc, const TextureDescription& texture,
                             ValidationMessage& message) noexcept {
    const SubresourceRange range =
        resolve_range(desc.range, texture.mip_levels, texture.array_layers);
    if (range.mip_count == 0 || range.layer_count == 0) {
        return fail(
            ErrorCode::InvalidArgument,
            message.format("view '%s': selects no subresources of '%s'", desc.name, texture.name));
    }
    if (static_cast<u32>(range.base_mip) + range.mip_count > texture.mip_levels) {
        return fail(ErrorCode::OutOfRange,
                    message.format("view '%s': mips [%u, %u) of '%s', which has %u", desc.name,
                                   range.base_mip, range.base_mip + range.mip_count, texture.name,
                                   texture.mip_levels));
    }
    if (static_cast<u32>(range.base_layer) + range.layer_count > texture.array_layers) {
        return fail(ErrorCode::OutOfRange,
                    message.format("view '%s': layers [%u, %u) of '%s', which has %u", desc.name,
                                   range.base_layer, range.base_layer + range.layer_count,
                                   texture.name, texture.array_layers));
    }
    return ok();
}

Status validate_sampler(const SamplerDescription& desc, const DeviceLimits& limits,
                        ValidationMessage& message) noexcept {
    if (desc.max_anisotropy > limits.max_sampler_anisotropy) {
        return fail(ErrorCode::OutOfRange,
                    message.format("sampler '%s': %.1f anisotropy exceeds the device's maximum of "
                                   "%.1f",
                                   desc.name, static_cast<double>(desc.max_anisotropy),
                                   static_cast<double>(limits.max_sampler_anisotropy)));
    }
    if (desc.min_lod > desc.max_lod) {
        return fail(
            ErrorCode::InvalidArgument,
            message.format("sampler '%s': min_lod %.1f is above max_lod %.1f", desc.name,
                           static_cast<double>(desc.min_lod), static_cast<double>(desc.max_lod)));
    }
    return ok();
}

Status validate_pipeline_layout(const PipelineLayoutDescription& desc,
                                ValidationMessage& message) noexcept {
    if (desc.set_layouts.size() > kMaxDescriptorSets) {
        return fail(ErrorCode::OutOfRange,
                    message.format("pipeline layout '%s': %zu descriptor sets; the engine's limit "
                                   "is %u simultaneously bound sets",
                                   desc.name, desc.set_layouts.size(), kMaxDescriptorSets));
    }
    u32 push_constant_end = 0;
    for (const PushConstantRange& range : desc.push_constants) {
        push_constant_end = push_constant_end > range.offset + range.size
                                ? push_constant_end
                                : range.offset + range.size;
    }
    if (push_constant_end > kMaxPushConstantBytes) {
        return fail(ErrorCode::OutOfRange,
                    message.format("pipeline layout '%s': push constants reach %u bytes; the "
                                   "engine's limit is %u",
                                   desc.name, push_constant_end, kMaxPushConstantBytes));
    }
    return ok();
}

Status validate_graphics_pipeline(const GraphicsPipelineDescription& desc,
                                  ValidationMessage& message) noexcept {
    if (desc.vertex_attributes.size() > kMaxVertexAttributes) {
        return fail(ErrorCode::OutOfRange,
                    message.format("pipeline '%s': %zu vertex attributes; the engine's limit is %u",
                                   desc.name, desc.vertex_attributes.size(), kMaxVertexAttributes));
    }
    if (desc.color_attachments.size() > kMaxColorAttachments) {
        return fail(ErrorCode::OutOfRange,
                    message.format("pipeline '%s': %zu colour attachments; the engine's limit is "
                                   "%u",
                                   desc.name, desc.color_attachments.size(), kMaxColorAttachments));
    }
    if (desc.layout.is_null()) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("pipeline '%s': no pipeline layout", desc.name));
    }
    if (desc.vertex_shader.is_null()) {
        return fail(ErrorCode::InvalidArgument,
                    message.format("pipeline '%s': no vertex shader", desc.name));
    }
    return ok();
}

Status validate_device_limits(const DeviceLimits& limits, ValidationMessage& message) noexcept {
    // The engine's hard limits are a portability contract. A device that cannot meet them is
    // refused here, once, rather than discovered a pipeline at a time six months later.
    if (limits.max_bound_descriptor_sets < kMaxDescriptorSets) {
        return fail(ErrorCode::Unsupported,
                    message.format("the device binds at most %u descriptor sets; the engine "
                                   "requires %u",
                                   limits.max_bound_descriptor_sets, kMaxDescriptorSets));
    }
    if (limits.max_push_constant_bytes < kMaxPushConstantBytes) {
        return fail(ErrorCode::Unsupported,
                    message.format("the device offers %u push-constant bytes; the engine requires "
                                   "%u",
                                   limits.max_push_constant_bytes, kMaxPushConstantBytes));
    }
    if (limits.max_vertex_attributes < kMaxVertexAttributes) {
        return fail(ErrorCode::Unsupported,
                    message.format("the device offers %u vertex attributes; the engine requires %u",
                                   limits.max_vertex_attributes, kMaxVertexAttributes));
    }
    if (limits.max_color_attachments < kMaxColorAttachments) {
        return fail(ErrorCode::Unsupported,
                    message.format("the device offers %u colour attachments; the engine requires "
                                   "%u",
                                   limits.max_color_attachments, kMaxColorAttachments));
    }
    return ok();
}

SubresourceRange resolve_range(const SubresourceRange& range, u16 mip_levels,
                               u16 array_layers) noexcept {
    SubresourceRange out = range;
    if (out.mip_count == 0) {
        out.mip_count = out.base_mip < mip_levels ? static_cast<u16>(mip_levels - out.base_mip)
                                                  : static_cast<u16>(0);
    }
    if (out.layer_count == 0) {
        out.layer_count = out.base_layer < array_layers
                              ? static_cast<u16>(array_layers - out.base_layer)
                              : static_cast<u16>(0);
    }
    return out;
}

bool access_valid_for(Access access, bool is_image) noexcept {
    const AccessInfo& info = access_info(access);
    return is_image ? info.valid_for_image : info.valid_for_buffer;
}

}  // namespace cy::rhi
