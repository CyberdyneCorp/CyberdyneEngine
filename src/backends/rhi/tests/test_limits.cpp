// The hard limits, checked at creation time. Task 2.1.1.
//
// `rhi-and-render-graph`, "Descriptor exceeds a limit": creation SHALL fail "with a diagnostic
// naming the limit, at creation time rather than at draw time". Both halves are asserted here: that
// it fails, and that the message names the number — because a diagnostic that says "too many" sends
// the reader to look the limit up, and one that says "9; the limit is 8" does not.

#include <cy/test/test.h>

#include <cy/backends/rhi/validation.h>

#include <cstring>

using cy::rhi::DescriptorSetLayoutHandle;
using cy::rhi::GraphicsPipelineDescription;
using cy::rhi::PipelineLayoutDescription;
using cy::rhi::PushConstantRange;
using cy::rhi::ValidationMessage;

namespace {

bool mentions(const ValidationMessage& message, const char* text) {
    return std::strstr(message.text, text) != nullptr;
}

}  // namespace

CY_TEST_CASE("a pipeline layout over the descriptor set limit is refused, naming the limit") {
    DescriptorSetLayoutHandle sets[cy::rhi::kMaxDescriptorSets + 1] = {};
    PipelineLayoutDescription description;
    description.name = "too many sets";
    description.set_layouts =
        cy::Span<const DescriptorSetLayoutHandle>(sets, sizeof(sets) / sizeof(sets[0]));

    ValidationMessage message;
    const cy::Status status = cy::rhi::validate_pipeline_layout(description, message);
    CY_REQUIRE_FALSE(status.has_value());
    CY_CHECK_EQ(status.error().code, cy::ErrorCode::OutOfRange);
    CY_CHECK(mentions(message, "too many sets"));
    CY_CHECK(mentions(message, "9"));
    CY_CHECK(mentions(message, "8"));
}

CY_TEST_CASE("exactly the limit is accepted") {
    DescriptorSetLayoutHandle sets[cy::rhi::kMaxDescriptorSets] = {};
    PipelineLayoutDescription description;
    description.set_layouts =
        cy::Span<const DescriptorSetLayoutHandle>(sets, sizeof(sets) / sizeof(sets[0]));
    ValidationMessage message;
    CY_CHECK(cy::rhi::validate_pipeline_layout(description, message).has_value());
}

CY_TEST_CASE("push constants past 128 bytes are refused at layout creation") {
    const PushConstantRange ranges[] = {
        PushConstantRange{cy::rhi::ShaderStage::Vertex, 0, 64},
        PushConstantRange{cy::rhi::ShaderStage::Fragment, 64, 96},  // reaches 160
    };
    PipelineLayoutDescription description;
    description.name = "fat constants";
    description.push_constants = cy::Span<const PushConstantRange>(ranges, 2);

    ValidationMessage message;
    const cy::Status status = cy::rhi::validate_pipeline_layout(description, message);
    CY_REQUIRE_FALSE(status.has_value());
    CY_CHECK(mentions(message, "160"));
    CY_CHECK(mentions(message, "128"));
}

CY_TEST_CASE("too many vertex attributes or colour attachments is refused at pipeline creation") {
    cy::rhi::VertexAttribute attributes[cy::rhi::kMaxVertexAttributes + 1] = {};
    GraphicsPipelineDescription description;
    description.name = "wide vertex";
    description.layout = cy::rhi::PipelineLayoutHandle::from_slot(0, 1);
    description.vertex_shader = cy::rhi::ShaderModuleHandle::from_slot(0, 1);
    description.vertex_attributes = cy::Span<const cy::rhi::VertexAttribute>(
        attributes, sizeof(attributes) / sizeof(attributes[0]));
    ValidationMessage message;
    CY_REQUIRE_FALSE(cy::rhi::validate_graphics_pipeline(description, message).has_value());
    CY_CHECK(mentions(message, "17"));
    CY_CHECK(mentions(message, "16"));

    cy::rhi::ColorAttachmentState attachments[cy::rhi::kMaxColorAttachments + 1] = {};
    GraphicsPipelineDescription wide;
    wide.name = "wide mrt";
    wide.layout = cy::rhi::PipelineLayoutHandle::from_slot(0, 1);
    wide.vertex_shader = cy::rhi::ShaderModuleHandle::from_slot(0, 1);
    wide.color_attachments = cy::Span<const cy::rhi::ColorAttachmentState>(
        attachments, sizeof(attachments) / sizeof(attachments[0]));
    ValidationMessage second;
    CY_REQUIRE_FALSE(cy::rhi::validate_graphics_pipeline(wide, second).has_value());
    CY_CHECK(mentions(second, "9"));
    CY_CHECK(mentions(second, "8"));
}

CY_TEST_CASE("a device that cannot meet the engine's limits is refused once, at device creation") {
    // The engine's limits are a portability contract. A device that cannot meet them is rejected
    // here rather than discovered a pipeline at a time six months later.
    cy::rhi::DeviceLimits limits;
    limits.max_bound_descriptor_sets = 4;
    limits.max_push_constant_bytes = 128;
    limits.max_vertex_attributes = 16;
    limits.max_color_attachments = 8;

    ValidationMessage message;
    const cy::Status status = cy::rhi::validate_device_limits(limits, message);
    CY_REQUIRE_FALSE(status.has_value());
    CY_CHECK_EQ(status.error().code, cy::ErrorCode::Unsupported);
    CY_CHECK(mentions(message, "4"));
    CY_CHECK(mentions(message, "8"));

    limits.max_bound_descriptor_sets = 8;
    ValidationMessage second;
    CY_CHECK(cy::rhi::validate_device_limits(limits, second).has_value());
}

CY_TEST_CASE("a texture view outside its texture is refused, naming both ranges") {
    cy::rhi::TextureDescription texture;
    texture.name = "cubemap";
    texture.format = cy::rhi::Format::Rgba8Unorm;
    texture.extent = cy::rhi::Extent3D{64, 64, 1};
    texture.mip_levels = 4;
    texture.array_layers = 6;

    cy::rhi::TextureViewDescription view;
    view.name = "face 7";
    view.range = cy::rhi::SubresourceRange{0, 1, 7, 1};

    ValidationMessage message;
    const cy::Status status = cy::rhi::validate_texture_view(view, texture, message);
    CY_REQUIRE_FALSE(status.has_value());
    CY_CHECK(mentions(message, "face 7"));
    CY_CHECK(mentions(message, "cubemap"));
}

CY_TEST_CASE("format sizes are known without a device") {
    // The engine computes upload sizes and transient memory before it has a device, so these are
    // properties of the format rather than a device query.
    CY_CHECK_EQ(cy::rhi::format_byte_size(cy::rhi::Format::Rgba8Unorm, 16, 16), 16U * 16U * 4U);
    CY_CHECK_EQ(cy::rhi::format_byte_size(cy::rhi::Format::R32Uint, 1024, 1024),
                1024ULL * 1024ULL * 4ULL);
    // A 4x4 block format rounds up: a 5x5 image is 2x2 blocks.
    CY_CHECK_EQ(cy::rhi::format_byte_size(cy::rhi::Format::Bc7Unorm, 5, 5), 4U * 16U);
    CY_CHECK_EQ(cy::rhi::format_byte_size(cy::rhi::Format::Undefined, 8, 8), 0U);
    CY_CHECK(cy::rhi::format_is_depth_stencil(cy::rhi::Format::D32Sfloat));
    CY_CHECK_FALSE(cy::rhi::format_is_depth_stencil(cy::rhi::Format::Rgba8Unorm));
}
