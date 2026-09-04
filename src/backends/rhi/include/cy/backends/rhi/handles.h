#pragma once
// The RHI's object identities. Task 2.1.1.
//
// `rhi-and-render-graph`: resources are "addressed by generational handles rather than pointers",
// and "a stale texture handle SHALL fail validation rather than aliasing the new texture". That is
// M1's cy::Handle<Tag> exactly — a 32-bit slot index and a 32-bit generation, where freeing a slot
// bumps its generation so a handle held across the free compares unequal to whatever replaced it.
//
// Every handle is a distinct type. Passing a BufferHandle where a TextureHandle is expected is a
// compile error rather than a convention, which is what CY_HANDLE_TAG's phantom tag buys; the four
// confusions M1 pinned in tests/compile_fail/ apply here unchanged.
//
// A null handle is a zero generation, so a zeroed struct — a component in memset chunk storage, a
// designated initialiser that omits the field — is "no resource" rather than "slot 0".

#include <cy/core/values/handle.h>

namespace cy::rhi {

CY_HANDLE_TAG(Buffer);
CY_HANDLE_TAG(Texture);
CY_HANDLE_TAG(TextureView);
CY_HANDLE_TAG(Sampler);
CY_HANDLE_TAG(ShaderModule);
CY_HANDLE_TAG(PipelineLayout);
CY_HANDLE_TAG(DescriptorSetLayout);
CY_HANDLE_TAG(DescriptorSet);
CY_HANDLE_TAG(GraphicsPipeline);
CY_HANDLE_TAG(ComputePipeline);
CY_HANDLE_TAG(CommandBuffer);
CY_HANDLE_TAG(Fence);
CY_HANDLE_TAG(Semaphore);
CY_HANDLE_TAG(QueryPool);
CY_HANDLE_TAG(Swapchain);

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using TextureViewHandle = Handle<TextureViewTag>;
using SamplerHandle = Handle<SamplerTag>;
using ShaderModuleHandle = Handle<ShaderModuleTag>;
using PipelineLayoutHandle = Handle<PipelineLayoutTag>;
using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
using DescriptorSetHandle = Handle<DescriptorSetTag>;
using GraphicsPipelineHandle = Handle<GraphicsPipelineTag>;
using ComputePipelineHandle = Handle<ComputePipelineTag>;
using CommandBufferHandle = Handle<CommandBufferTag>;
using FenceHandle = Handle<FenceTag>;
using SemaphoreHandle = Handle<SemaphoreTag>;
using QueryPoolHandle = Handle<QueryPoolTag>;
using SwapchainHandle = Handle<SwapchainTag>;

/// A bindless slot: an index into a global descriptor array, which is what a shader receives
/// instead of a bound descriptor set. Distinct from a handle because it is what crosses to the GPU
/// — it has no generation, and validating it is the descriptor allocator's job rather than a
/// comparison the shader could make.
///
/// `rhi-and-render-graph` makes bindless the default resource model: "draw workloads generated on
/// the GPU from the GPU scene have no CPU in the loop to bind a descriptor set per draw".
using BindlessIndex = u32;
inline constexpr BindlessIndex kInvalidBindlessIndex = ~0U;

}  // namespace cy::rhi
