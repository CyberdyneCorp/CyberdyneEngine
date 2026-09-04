// Recording, and the one place in this backend that emits a barrier. Tasks 2.2.4 and 2.3.1.
//
// The recording half is mechanical: each method is one or two Vulkan calls. The interesting file is
// the bottom half — VulkanBarrierRecorder — which turns a derived BarrierBatch into a
// VkDependencyInfo. It is reachable only through VulkanDevice::barrier_recorder(), whose argument
// cannot be constructed outside cy::rendering::GraphExecutor, and the source-level gate in
// tools/layercheck/layercheck.py covers what a passkey cannot.

#include "vulkan_device.h"

namespace cy::rhi::vulkan {

void VulkanCommandBuffer::begin_rendering(const RenderingInfo& info) noexcept {
    // Dynamic rendering: no VkRenderPass, no VkFramebuffer, nothing to keep in step with a
    // pipeline. Vulkan 1.3 is the baseline precisely so that this is the only path.
    VkRenderingAttachmentInfo colour[kMaxColorAttachments] = {};
    const u32 colour_count = info.color_attachments.size() < kMaxColorAttachments
                                 ? static_cast<u32>(info.color_attachments.size())
                                 : kMaxColorAttachments;
    for (u32 index = 0; index < colour_count; ++index) {
        const RenderAttachment& attachment = info.color_attachments[index];
        VulkanTextureView* view = device_->view(attachment.view);
        colour[index].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colour[index].imageView = view != nullptr ? view->view : VK_NULL_HANDLE;
        colour[index].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colour[index].loadOp = to_vulkan(attachment.load);
        // DontCare is the graph's decision, from the declared reads. On a tiled GPU it is the
        // difference between writing a whole render target to main memory and not writing it.
        colour[index].storeOp = to_vulkan(attachment.store);
        for (u32 channel = 0; channel < 4; ++channel) {
            colour[index].clearValue.color.float32[channel] = attachment.clear.color[channel];
        }
        if (VulkanTextureView* resolve = device_->view(attachment.resolve_view);
            resolve != nullptr) {
            colour[index].resolveImageView = resolve->view;
            colour[index].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colour[index].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        }
    }

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    const bool has_depth = !info.depth_attachment.view.is_null();
    if (has_depth) {
        VulkanTextureView* view = device_->view(info.depth_attachment.view);
        depth.imageView = view != nullptr ? view->view : VK_NULL_HANDLE;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = to_vulkan(info.depth_attachment.load);
        depth.storeOp = to_vulkan(info.depth_attachment.store);
        // Reversed-Z: cleared to 0, compared GreaterEqual. The value comes from the attachment the
        // graph filled in, and rhi::reversed_z_depth_clear() is what a pass uses to name it.
        depth.clearValue.depthStencil.depth = info.depth_attachment.clear.depth_stencil.depth;
        depth.clearValue.depthStencil.stencil = info.depth_attachment.clear.depth_stencil.stencil;
    }

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.offset = VkOffset2D{info.render_area.x, info.render_area.y};
    rendering.renderArea.extent = VkExtent2D{info.render_area.width, info.render_area.height};
    rendering.layerCount = info.layer_count;
    // Multiview: a non-zero mask renders every view in one pass, which is the XR prerequisite
    // tests/render/README.md has recorded since M0.
    rendering.viewMask = info.view_mask;
    rendering.colorAttachmentCount = colour_count;
    rendering.pColorAttachments = colour;
    rendering.pDepthAttachment = has_depth ? &depth : nullptr;
    vkCmdBeginRendering(commands_, &rendering);
}

void VulkanCommandBuffer::end_rendering() noexcept {
    vkCmdEndRendering(commands_);
}

void VulkanCommandBuffer::set_viewport(const Viewport& viewport) noexcept {
    // The Y flip: Vulkan's clip space has +Y down and the engine is Y-up, so the viewport is given
    // a negative height with its origin at the bottom. `core-math` fixes the convention; this is
    // the one line that honours it, and getting it wrong renders everything upside down — which is
    // exactly the class of error task 5.4 verifies against rendered output.
    VkViewport out{};
    out.x = viewport.x;
    out.y = viewport.y + viewport.height;
    out.width = viewport.width;
    out.height = -viewport.height;
    out.minDepth = viewport.min_depth;
    out.maxDepth = viewport.max_depth;
    vkCmdSetViewport(commands_, 0, 1, &out);
}

void VulkanCommandBuffer::set_scissor(const Rect2D& scissor) noexcept {
    VkRect2D out{};
    out.offset = VkOffset2D{scissor.x, scissor.y};
    out.extent = VkExtent2D{scissor.width, scissor.height};
    vkCmdSetScissor(commands_, 0, 1, &out);
}

void VulkanCommandBuffer::bind_graphics_pipeline(GraphicsPipelineHandle pipeline) noexcept {
    if (VulkanPipeline* stored = device_->graphics_pipeline(pipeline); stored != nullptr) {
        vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_GRAPHICS, stored->pipeline);
    }
}

void VulkanCommandBuffer::bind_compute_pipeline(ComputePipelineHandle pipeline) noexcept {
    if (VulkanPipeline* stored = device_->compute_pipeline(pipeline); stored != nullptr) {
        vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_COMPUTE, stored->pipeline);
    }
}

void VulkanCommandBuffer::bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                                               Span<const DescriptorSetHandle> sets) noexcept {
    VulkanPipelineLayout* stored = device_->pipeline_layout(layout);
    if (stored == nullptr || sets.empty()) {
        return;
    }
    VkDescriptorSet raw[kMaxDescriptorSets] = {};
    const u32 count =
        sets.size() < kMaxDescriptorSets ? static_cast<u32>(sets.size()) : kMaxDescriptorSets;
    for (u32 index = 0; index < count; ++index) {
        VulkanDescriptorSet* set = device_->descriptor_set(sets[index]);
        raw[index] = set != nullptr ? set->set : VK_NULL_HANDLE;
    }
    const VkPipelineBindPoint bind_point = queue_ == QueueKind::AsyncCompute
                                               ? VK_PIPELINE_BIND_POINT_COMPUTE
                                               : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(commands_, bind_point, stored->layout, first_set, count, raw, 0,
                            nullptr);
}

void VulkanCommandBuffer::push_constants(PipelineLayoutHandle layout, ShaderStage stages,
                                         u32 offset, Span<const u8> data) noexcept {
    VulkanPipelineLayout* stored = device_->pipeline_layout(layout);
    if (stored == nullptr || data.empty()) {
        return;
    }
    if (offset + data.size() > kMaxPushConstantBytes) {
        device_->report_validation(ValidationSeverity::Error,
                                   "push_constants() past the engine's 128-byte limit");
        return;
    }
    vkCmdPushConstants(commands_, stored->layout, to_vulkan(stages), offset,
                       static_cast<u32>(data.size()), data.data());
}

void VulkanCommandBuffer::bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> buffers,
                                              Span<const u64> offsets) noexcept {
    constexpr u32 kMaxBindings = 8;
    VkBuffer raw[kMaxBindings] = {};
    VkDeviceSize raw_offsets[kMaxBindings] = {};
    const u32 count =
        buffers.size() < kMaxBindings ? static_cast<u32>(buffers.size()) : kMaxBindings;
    for (u32 index = 0; index < count; ++index) {
        VulkanBuffer* buffer = device_->buffer(buffers[index]);
        raw[index] = buffer != nullptr ? buffer->buffer : VK_NULL_HANDLE;
        raw_offsets[index] = index < offsets.size() ? offsets[index] : 0;
    }
    if (count != 0) {
        vkCmdBindVertexBuffers(commands_, first_binding, count, raw, raw_offsets);
    }
}

void VulkanCommandBuffer::bind_index_buffer(BufferHandle buffer, u64 offset, bool wide) noexcept {
    if (VulkanBuffer* stored = device_->buffer(buffer); stored != nullptr) {
        vkCmdBindIndexBuffer(commands_, stored->buffer, offset,
                             wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    }
}

void VulkanCommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                               u32 first_instance) noexcept {
    vkCmdDraw(commands_, vertex_count, instance_count, first_vertex, first_instance);
    device_->mutable_statistics().draws++;
}

void VulkanCommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                                       i32 vertex_offset, u32 first_instance) noexcept {
    vkCmdDrawIndexed(commands_, index_count, instance_count, first_index, vertex_offset,
                     first_instance);
    device_->mutable_statistics().draws++;
}

void VulkanCommandBuffer::draw_indexed_indirect(BufferHandle arguments, u64 offset, u32 draw_count,
                                                u32 stride) noexcept {
    if (VulkanBuffer* buffer = device_->buffer(arguments); buffer != nullptr) {
        vkCmdDrawIndexedIndirect(commands_, buffer->buffer, offset, draw_count, stride);
        device_->mutable_statistics().draws++;
    }
}

void VulkanCommandBuffer::dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept {
    vkCmdDispatch(commands_, groups_x, groups_y, groups_z);
    device_->mutable_statistics().dispatches++;
}

void VulkanCommandBuffer::dispatch_indirect(BufferHandle arguments, u64 offset) noexcept {
    if (VulkanBuffer* buffer = device_->buffer(arguments); buffer != nullptr) {
        vkCmdDispatchIndirect(commands_, buffer->buffer, offset);
        device_->mutable_statistics().dispatches++;
    }
}

void VulkanCommandBuffer::copy_buffer(BufferHandle source, BufferHandle destination,
                                      Span<const BufferCopy> regions) noexcept {
    VulkanBuffer* from = device_->buffer(source);
    VulkanBuffer* to = device_->buffer(destination);
    if (from == nullptr || to == nullptr || regions.empty()) {
        return;
    }
    constexpr u32 kMaxRegions = 16;
    VkBufferCopy raw[kMaxRegions] = {};
    const u32 count = regions.size() < kMaxRegions ? static_cast<u32>(regions.size()) : kMaxRegions;
    for (u32 index = 0; index < count; ++index) {
        raw[index].srcOffset = regions[index].source_offset;
        raw[index].dstOffset = regions[index].destination_offset;
        raw[index].size = regions[index].size;
    }
    vkCmdCopyBuffer(commands_, from->buffer, to->buffer, count, raw);
}

void VulkanCommandBuffer::copy_buffer_to_texture(BufferHandle source, TextureHandle destination,
                                                 Span<const BufferTextureCopy> regions) noexcept {
    VulkanBuffer* from = device_->buffer(source);
    VulkanTexture* to = device_->texture(destination);
    if (from == nullptr || to == nullptr || regions.empty()) {
        return;
    }
    constexpr u32 kMaxRegions = 16;
    VkBufferImageCopy raw[kMaxRegions] = {};
    const u32 count = regions.size() < kMaxRegions ? static_cast<u32>(regions.size()) : kMaxRegions;
    for (u32 index = 0; index < count; ++index) {
        const BufferTextureCopy& region = regions[index];
        raw[index].bufferOffset = region.buffer_offset;
        raw[index].bufferRowLength = region.buffer_row_length;
        raw[index].bufferImageHeight = region.buffer_image_height;
        raw[index].imageSubresource.aspectMask = aspect_of(to->desc.format);
        raw[index].imageSubresource.mipLevel = region.mip_level;
        raw[index].imageSubresource.baseArrayLayer = region.base_layer;
        raw[index].imageSubresource.layerCount = region.layer_count;
        raw[index].imageOffset =
            VkOffset3D{region.texture_offset.x, region.texture_offset.y, region.texture_offset.z};
        raw[index].imageExtent = VkExtent3D{
            region.texture_extent.width, region.texture_extent.height, region.texture_extent.depth};
    }
    // The layout is the one the graph transitioned to: a copy destination is TRANSFER_DST_OPTIMAL
    // because the pass declared Access::TransferWrite, and there is no other way for it to be here.
    vkCmdCopyBufferToImage(commands_, from->buffer, to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           count, raw);
}

void VulkanCommandBuffer::copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                                 Span<const BufferTextureCopy> regions) noexcept {
    VulkanTexture* from = device_->texture(source);
    VulkanBuffer* to = device_->buffer(destination);
    if (from == nullptr || to == nullptr || regions.empty()) {
        return;
    }
    constexpr u32 kMaxRegions = 16;
    VkBufferImageCopy raw[kMaxRegions] = {};
    const u32 count = regions.size() < kMaxRegions ? static_cast<u32>(regions.size()) : kMaxRegions;
    for (u32 index = 0; index < count; ++index) {
        const BufferTextureCopy& region = regions[index];
        raw[index].bufferOffset = region.buffer_offset;
        raw[index].bufferRowLength = region.buffer_row_length;
        raw[index].bufferImageHeight = region.buffer_image_height;
        raw[index].imageSubresource.aspectMask = aspect_of(from->desc.format);
        raw[index].imageSubresource.mipLevel = region.mip_level;
        raw[index].imageSubresource.baseArrayLayer = region.base_layer;
        raw[index].imageSubresource.layerCount = region.layer_count;
        raw[index].imageOffset =
            VkOffset3D{region.texture_offset.x, region.texture_offset.y, region.texture_offset.z};
        raw[index].imageExtent = VkExtent3D{
            region.texture_extent.width, region.texture_extent.height, region.texture_extent.depth};
    }
    vkCmdCopyImageToBuffer(commands_, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->buffer,
                           count, raw);
}

void VulkanCommandBuffer::begin_debug_label(const char* name) noexcept {
    if (!device_->debug_markers() || vkCmdBeginDebugUtilsLabelEXT == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name != nullptr ? name : "";
    vkCmdBeginDebugUtilsLabelEXT(commands_, &label);
}

void VulkanCommandBuffer::end_debug_label() noexcept {
    if (device_->debug_markers() && vkCmdEndDebugUtilsLabelEXT != nullptr) {
        vkCmdEndDebugUtilsLabelEXT(commands_);
    }
}

void VulkanCommandBuffer::insert_debug_label(const char* name) noexcept {
    if (!device_->debug_markers() || vkCmdInsertDebugUtilsLabelEXT == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name != nullptr ? name : "";
    vkCmdInsertDebugUtilsLabelEXT(commands_, &label);
}

void VulkanCommandBuffer::write_timestamp(QueryPoolHandle pool, u32 index) noexcept {
    if (VulkanQueryPool* stored = device_->query_pool(pool); stored != nullptr) {
        vkCmdWriteTimestamp2(commands_, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, stored->pool,
                             index);
    }
}

void VulkanCommandBuffer::reset_queries(QueryPoolHandle pool, u32 first, u32 count) noexcept {
    if (VulkanQueryPool* stored = device_->query_pool(pool); stored != nullptr) {
        vkCmdResetQueryPool(commands_, stored->pool, first, count);
    }
}

void VulkanCommandBuffer::write_breadcrumb(u32 slot, u32 value) noexcept {
    // vkCmdFillBuffer rather than a copy: it needs no staging and it executes on the transfer
    // stage, so the last value the GPU managed to write is the last pass it reached. That is what a
    // device loss reads back when the trace tail did not survive.
    if (slot >= device_->breadcrumb_slots() || device_->breadcrumb_buffer() == VK_NULL_HANDLE) {
        return;
    }
    vkCmdFillBuffer(commands_, device_->breadcrumb_buffer(),
                    static_cast<VkDeviceSize>(slot) * sizeof(u32), sizeof(u32), value);
}

// --- The barrier recorder
// -------------------------------------------------------------------------
//
// A derived BarrierBatch becomes one VkDependencyInfo and one vkCmdPipelineBarrier2. One call
// rather than three: splitting a batch into three consecutive barriers is correctness-preserving
// and measurably worse, and doing it by accident is easy when a batch has three members.

void VulkanBarrierRecorder::record_barriers(CommandBufferHandle command_buffer,
                                            const BarrierBatch& batch) noexcept {
    VulkanCommandBuffer* commands = device_->vulkan_command_buffer(command_buffer);
    if (commands == nullptr || batch.empty()) {
        return;
    }

    constexpr u32 kMaxPerBatch = 64;
    VkImageMemoryBarrier2 images[kMaxPerBatch] = {};
    VkBufferMemoryBarrier2 buffers[kMaxPerBatch] = {};
    VkMemoryBarrier2 memory[kMaxPerBatch] = {};

    u32 image_count = 0;
    for (const ImageBarrier& barrier : batch.images) {
        if (image_count == kMaxPerBatch) {
            break;
        }
        VulkanTexture* texture = device_->texture(barrier.texture);
        if (texture == nullptr) {
            device_->report_validation(ValidationSeverity::Error,
                                       "a derived barrier names a texture handle that is stale — "
                                       "the executor did not patch it");
            continue;
        }
        VkImageMemoryBarrier2& out = images[image_count];
        out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        out.srcStageMask = to_vulkan(barrier.src_stage);
        out.srcAccessMask = to_vulkan(barrier.src_access);
        out.dstStageMask = to_vulkan(barrier.dst_stage);
        out.dstAccessMask = to_vulkan(barrier.dst_access);
        out.oldLayout = to_vulkan(barrier.old_layout);
        out.newLayout = to_vulkan(barrier.new_layout);
        // VK_QUEUE_FAMILY_IGNORED on both halves unless this is an ownership transfer, and the
        // engine's kQueueFamilyIgnored is the same sentinel.
        out.srcQueueFamilyIndex = barrier.src_queue_family == kQueueFamilyIgnored
                                      ? VK_QUEUE_FAMILY_IGNORED
                                      : barrier.src_queue_family;
        out.dstQueueFamilyIndex = barrier.dst_queue_family == kQueueFamilyIgnored
                                      ? VK_QUEUE_FAMILY_IGNORED
                                      : barrier.dst_queue_family;
        out.image = texture->image;
        out.subresourceRange.aspectMask = to_vulkan(barrier.aspect);
        out.subresourceRange.baseMipLevel = barrier.range.base_mip;
        out.subresourceRange.levelCount = barrier.range.mip_count;
        out.subresourceRange.baseArrayLayer = barrier.range.base_layer;
        out.subresourceRange.layerCount = barrier.range.layer_count;
        ++image_count;
    }

    u32 buffer_count = 0;
    for (const BufferBarrier& barrier : batch.buffers) {
        if (buffer_count == kMaxPerBatch) {
            break;
        }
        VulkanBuffer* buffer = device_->buffer(barrier.buffer);
        if (buffer == nullptr) {
            continue;
        }
        VkBufferMemoryBarrier2& out = buffers[buffer_count];
        out.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        out.srcStageMask = to_vulkan(barrier.src_stage);
        out.srcAccessMask = to_vulkan(barrier.src_access);
        out.dstStageMask = to_vulkan(barrier.dst_stage);
        out.dstAccessMask = to_vulkan(barrier.dst_access);
        out.srcQueueFamilyIndex = barrier.src_queue_family == kQueueFamilyIgnored
                                      ? VK_QUEUE_FAMILY_IGNORED
                                      : barrier.src_queue_family;
        out.dstQueueFamilyIndex = barrier.dst_queue_family == kQueueFamilyIgnored
                                      ? VK_QUEUE_FAMILY_IGNORED
                                      : barrier.dst_queue_family;
        out.buffer = buffer->buffer;
        out.offset = barrier.offset;
        out.size = barrier.size == 0 ? VK_WHOLE_SIZE : barrier.size;
        ++buffer_count;
    }

    u32 memory_count = 0;
    for (const MemoryBarrier& barrier : batch.memory) {
        if (memory_count == kMaxPerBatch) {
            break;
        }
        VkMemoryBarrier2& out = memory[memory_count];
        out.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        out.srcStageMask = to_vulkan(barrier.src_stage);
        out.srcAccessMask = to_vulkan(barrier.src_access);
        out.dstStageMask = to_vulkan(barrier.dst_stage);
        out.dstAccessMask = to_vulkan(barrier.dst_access);
        ++memory_count;
    }

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = memory_count;
    dependency.pMemoryBarriers = memory;
    dependency.bufferMemoryBarrierCount = buffer_count;
    dependency.pBufferMemoryBarriers = buffers;
    dependency.imageMemoryBarrierCount = image_count;
    dependency.pImageMemoryBarriers = images;
    vkCmdPipelineBarrier2(commands->raw(), &dependency);

    ++batches_;
    barriers_ += image_count + buffer_count + memory_count;

    DeviceStatistics& stats = device_->mutable_statistics();
    ++stats.barrier_batches;
    stats.barriers += image_count + buffer_count + memory_count;
    for (u32 index = 0; index < image_count; ++index) {
        if (images[index].srcQueueFamilyIndex != images[index].dstQueueFamilyIndex) {
            ++stats.queue_ownership_transfers;
        }
    }
}

}  // namespace cy::rhi::vulkan
