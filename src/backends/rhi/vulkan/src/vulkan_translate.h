#pragma once
// THE TRANSLATION TABLE. Task 2.3.1.
//
// The engine's synchronisation vocabulary — rhi::Stage, rhi::AccessFlags, rhi::ImageLayout — is
// Vulkan-shaped and engine-owned. This file is the only place in the engine that turns one into the
// other, which is what makes "no Vulkan type above src/backends/" a rule the compiler can enforce
// rather than a convention.
//
// M3's spike typed the access table in Vulkan and put it under this directory. The specification
// requires the graph to own synchronisation and forbids a Vulkan type above the backend layer;
// those two together move the table up into the RHI in engine types and leave THIS behind — a
// mechanical mapping with no decisions in it. The specification wins, and the cost is one function
// per backend rather than none.

#include "vulkan_common.h"

namespace cy::rhi::vulkan {

[[nodiscard]] VkPipelineStageFlags2 to_vulkan(Stage stage) noexcept;
[[nodiscard]] VkAccessFlags2 to_vulkan(AccessFlags access) noexcept;
[[nodiscard]] VkImageLayout to_vulkan(ImageLayout layout) noexcept;
[[nodiscard]] VkFormat to_vulkan(Format format) noexcept;
[[nodiscard]] Format from_vulkan(VkFormat format) noexcept;
[[nodiscard]] VkImageAspectFlags to_vulkan(ImageAspect aspect) noexcept;
[[nodiscard]] VkImageUsageFlags to_vulkan(TextureUsage usage) noexcept;
[[nodiscard]] VkBufferUsageFlags to_vulkan(BufferUsage usage) noexcept;
[[nodiscard]] VkImageType to_vulkan_image_type(TextureDimension dimension) noexcept;
[[nodiscard]] VkImageViewType to_vulkan_view_type(TextureDimension dimension,
                                                  u16 array_layers) noexcept;
[[nodiscard]] VkFilter to_vulkan(Filter filter) noexcept;
[[nodiscard]] VkSamplerMipmapMode to_vulkan(MipmapMode mode) noexcept;
[[nodiscard]] VkSamplerAddressMode to_vulkan(AddressMode mode) noexcept;
[[nodiscard]] VkCompareOp to_vulkan(CompareOp op) noexcept;
[[nodiscard]] VkPrimitiveTopology to_vulkan(PrimitiveTopology topology) noexcept;
[[nodiscard]] VkPolygonMode to_vulkan(PolygonMode mode) noexcept;
[[nodiscard]] VkCullModeFlags to_vulkan(CullMode mode) noexcept;
[[nodiscard]] VkFrontFace to_vulkan(FrontFace face) noexcept;
[[nodiscard]] VkBlendFactor to_vulkan(BlendFactor factor) noexcept;
[[nodiscard]] VkBlendOp to_vulkan(BlendOp op) noexcept;
[[nodiscard]] VkColorComponentFlags to_vulkan(ColorComponent components) noexcept;
[[nodiscard]] VkShaderStageFlags to_vulkan(ShaderStage stages) noexcept;
[[nodiscard]] VkShaderStageFlagBits to_vulkan_single(ShaderStage stage) noexcept;
[[nodiscard]] VkDescriptorType to_vulkan(DescriptorKind kind) noexcept;
[[nodiscard]] VkAttachmentLoadOp to_vulkan(LoadOp op) noexcept;
[[nodiscard]] VkAttachmentStoreOp to_vulkan(StoreOp op) noexcept;
[[nodiscard]] VkPresentModeKHR to_vulkan(PresentMode mode) noexcept;
[[nodiscard]] PresentMode from_vulkan(VkPresentModeKHR mode) noexcept;

/// The aspect a format implies, for the barriers the graph derives. A depth-stencil image
/// transitions both aspects together and a colour image has neither, so this is a property of the
/// format rather than a flag anybody sets.
[[nodiscard]] VkImageAspectFlags aspect_of(Format format) noexcept;

}  // namespace cy::rhi::vulkan
