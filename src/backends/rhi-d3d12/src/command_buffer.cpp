// SPDX-License-Identifier: MIT

#include "d3d12_internal.h"

#include <cstring>

namespace cy::rhi::d3d12 {
namespace {

[[nodiscard]] D3D12_RESOURCE_STATES image_state(ImageUse use) noexcept {
    switch (use) {
        case ImageUse::Undefined:
            return D3D12_RESOURCE_STATE_COMMON;
        case ImageUse::Storage:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ImageUse::ColorAttachment:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ImageUse::DepthStencilAttachment:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ImageUse::DepthStencilReadOnly:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ImageUse::SampledRead:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ImageUse::TransferSource:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ImageUse::TransferDestination:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case ImageUse::Presentable:
            return D3D12_RESOURCE_STATE_PRESENT;
        case ImageUse::Count:
            break;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

[[nodiscard]] D3D12_RESOURCE_STATES buffer_state(AccessFlags access) noexcept {
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (any(access & AccessFlags::IndirectCommandRead)) {
        state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    if (any(access & AccessFlags::IndexRead)) {
        state |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if (any(access & AccessFlags::VertexAttributeRead)) {
        state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (any(access & AccessFlags::UniformRead)) {
        state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (any(access & (AccessFlags::ShaderSampledRead | AccessFlags::ShaderStorageRead))) {
        state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (any(access & AccessFlags::ShaderStorageWrite)) {
        state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (any(access & AccessFlags::TransferRead)) {
        state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (any(access & AccessFlags::TransferWrite)) {
        state |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    return state;
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE offset(D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 index,
                                                 u32 stride) noexcept {
    handle.ptr += static_cast<SIZE_T>(index) * stride;
    return handle;
}

}  // namespace

D3D12CommandBuffer::D3D12CommandBuffer(D3D12Device* device, QueueKind queue, bool secondary,
                                       u32 frame_slot) noexcept
    : device_(device), queue_(queue), secondary_(secondary), frame_slot_(frame_slot) {}

void D3D12CommandBuffer::begin_rendering(const RenderingInfo& info) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxColorAttachments]{};
    u32 count = 0;
    for (const RenderAttachment& attachment : info.color_attachments) {
        D3D12TextureView* view = device_->view(attachment.view);
        if (view == nullptr || !view->has_rtv) {
            continue;
        }
        rtvs[count++] = view->rtv;
        if (attachment.load == LoadOp::Clear) {
            list->ClearRenderTargetView(view->rtv, attachment.clear.color, 0, nullptr);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE* depth = nullptr;
    if (!info.depth_attachment.view.is_null()) {
        D3D12TextureView* view = device_->view(info.depth_attachment.view);
        if (view != nullptr && view->has_dsv) {
            dsv = view->dsv;
            depth = &dsv;
            if (info.depth_attachment.load == LoadOp::Clear) {
                list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                            info.depth_attachment.clear.depth_stencil.depth,
                                            static_cast<UINT8>(
                                                info.depth_attachment.clear.depth_stencil.stencil),
                                            0,
                                            nullptr);
            }
        }
    }
    list->OMSetRenderTargets(count, rtvs, FALSE, depth);
}

void D3D12CommandBuffer::end_rendering() noexcept {}

void D3D12CommandBuffer::set_viewport(const Viewport& viewport) noexcept {
    const D3D12_VIEWPORT native{viewport.x,      viewport.y,         viewport.width,
                                viewport.height, viewport.min_depth, viewport.max_depth};
    list->RSSetViewports(1, &native);
}

void D3D12CommandBuffer::set_scissor(const Rect2D& scissor) noexcept {
    const D3D12_RECT native{scissor.x, scissor.y, scissor.x + static_cast<LONG>(scissor.width),
                            scissor.y + static_cast<LONG>(scissor.height)};
    list->RSSetScissorRects(1, &native);
}

void D3D12CommandBuffer::bind_graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
    D3D12GraphicsPipeline* pipeline = device_->graphics_pipeline(handle);
    if (pipeline == nullptr) {
        return;
    }
    D3D12PipelineLayout* layout = device_->pipeline_layout(pipeline->layout);
    list->SetPipelineState(pipeline->pipeline.Get());
    list->SetGraphicsRootSignature(layout->root_signature.Get());
    list->IASetPrimitiveTopology(pipeline->topology);
    graphics_pipeline_ = handle;
    compute_bound_ = false;
}

void D3D12CommandBuffer::bind_compute_pipeline(ComputePipelineHandle handle) noexcept {
    D3D12ComputePipeline* pipeline = device_->compute_pipeline(handle);
    if (pipeline == nullptr) {
        return;
    }
    D3D12PipelineLayout* layout = device_->pipeline_layout(pipeline->layout);
    list->SetPipelineState(pipeline->pipeline.Get());
    list->SetComputeRootSignature(layout->root_signature.Get());
    compute_bound_ = true;
}

void D3D12CommandBuffer::bind_descriptor_sets(PipelineLayoutHandle layout_handle, u32 first_set,
                                              Span<const DescriptorSetHandle> sets) noexcept {
    D3D12PipelineLayout* layout = device_->pipeline_layout(layout_handle);
    if (layout == nullptr) {
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {device_->resource_heap(), device_->sampler_heap()};
    list->SetDescriptorHeaps(2, heaps);
    for (u32 index = 0; index < sets.size(); ++index) {
        const u32 set_index = first_set + index;
        if (set_index >= layout->set_count) {
            return;
        }
        D3D12DescriptorSet* set = device_->descriptor_set(sets[index]);
        if (set == nullptr) {
            return;
        }
        if (layout->resource_root[set_index] != ~0U) {
            if (compute_bound_) {
                list->SetComputeRootDescriptorTable(layout->resource_root[set_index],
                                                    device_->resource_gpu(set->resource_base));
            } else {
                list->SetGraphicsRootDescriptorTable(layout->resource_root[set_index],
                                                     device_->resource_gpu(set->resource_base));
            }
        }
        if (layout->sampler_root[set_index] != ~0U) {
            if (compute_bound_) {
                list->SetComputeRootDescriptorTable(layout->sampler_root[set_index],
                                                    device_->sampler_gpu(set->sampler_base));
            } else {
                list->SetGraphicsRootDescriptorTable(layout->sampler_root[set_index],
                                                     device_->sampler_gpu(set->sampler_base));
            }
        }
    }
}

void D3D12CommandBuffer::push_constants(PipelineLayoutHandle layout_handle, ShaderStage,
                                        u32 byte_offset, Span<const u8> data) noexcept {
    D3D12PipelineLayout* layout = device_->pipeline_layout(layout_handle);
    if (layout == nullptr || layout->push_root == ~0U || data.empty()) {
        return;
    }
    const u32 count = static_cast<u32>((data.size() + 3U) / 4U);
    if (compute_bound_) {
        list->SetComputeRoot32BitConstants(layout->push_root, count, data.data(), byte_offset / 4U);
    } else {
        list->SetGraphicsRoot32BitConstants(layout->push_root, count, data.data(),
                                            byte_offset / 4U);
    }
}

void D3D12CommandBuffer::bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> buffers,
                                             Span<const u64> offsets) noexcept {
    D3D12GraphicsPipeline* pipeline = device_->graphics_pipeline(graphics_pipeline_);
    if (pipeline == nullptr) {
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW views[kMaxVertexAttributes]{};
    for (u32 index = 0; index < buffers.size(); ++index) {
        D3D12Buffer* buffer = device_->buffer(buffers[index]);
        if (buffer == nullptr || !buffer->resource) {
            return;
        }
        const u64 at = index < offsets.size() ? offsets[index] : 0;
        views[index].BufferLocation = buffer->resource->GetGPUVirtualAddress() + at;
        views[index].SizeInBytes = static_cast<UINT>(buffer->desc.size - at);
        const u32 binding = first_binding + index;
        views[index].StrideInBytes =
            binding < pipeline->vertex_binding_count ? pipeline->vertex_strides[binding] : 0;
    }
    list->IASetVertexBuffers(first_binding, static_cast<UINT>(buffers.size()), views);
}

void D3D12CommandBuffer::bind_index_buffer(BufferHandle handle, u64 offset_bytes,
                                           bool wide) noexcept {
    D3D12Buffer* buffer = device_->buffer(handle);
    if (buffer == nullptr || !buffer->resource) {
        return;
    }
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buffer->resource->GetGPUVirtualAddress() + offset_bytes;
    view.SizeInBytes = static_cast<UINT>(buffer->desc.size - offset_bytes);
    view.Format = wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    list->IASetIndexBuffer(&view);
}

void D3D12CommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                              u32 first_instance) noexcept {
    list->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
    device_->add_draw();
}

void D3D12CommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                                      i32 vertex_offset, u32 first_instance) noexcept {
    list->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset,
                               first_instance);
    device_->add_draw();
}

void D3D12CommandBuffer::draw_indexed_indirect(BufferHandle handle, u64 offset_bytes,
                                                u32 draw_count, u32 stride) noexcept {
    D3D12Buffer* buffer = device_->buffer(handle);
    if (buffer == nullptr || !buffer->resource || stride != sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)) {
        device_->report_validation(ValidationSeverity::Error,
                                   "D3D12 indexed indirect arguments are invalid");
        return;
    }
    list->ExecuteIndirect(device_->draw_indexed_signature(), draw_count, buffer->resource.Get(),
                          offset_bytes, nullptr, 0);
    device_->add_draw();
}

void D3D12CommandBuffer::dispatch(u32 x, u32 y, u32 z) noexcept {
    list->Dispatch(x, y, z);
    device_->add_dispatch();
}

void D3D12CommandBuffer::dispatch_indirect(BufferHandle handle, u64 offset_bytes) noexcept {
    D3D12Buffer* buffer = device_->buffer(handle);
    if (buffer == nullptr || !buffer->resource) {
        device_->report_validation(ValidationSeverity::Error,
                                   "D3D12 dispatch indirect arguments are invalid");
        return;
    }
    list->ExecuteIndirect(device_->dispatch_signature(), 1, buffer->resource.Get(), offset_bytes,
                          nullptr, 0);
    device_->add_dispatch();
}

void D3D12CommandBuffer::copy_buffer(BufferHandle source_handle, BufferHandle destination_handle,
                                     Span<const BufferCopy> regions) noexcept {
    D3D12Buffer* source = device_->buffer(source_handle);
    D3D12Buffer* destination = device_->buffer(destination_handle);
    if (source == nullptr || destination == nullptr) {
        return;
    }
    for (const BufferCopy& region : regions) {
        list->CopyBufferRegion(destination->resource.Get(), region.destination_offset,
                               source->resource.Get(), region.source_offset, region.size);
    }
}

void D3D12CommandBuffer::copy_buffer_to_texture(BufferHandle source_handle,
                                                TextureHandle destination_handle,
                                                Span<const BufferTextureCopy> regions) noexcept {
    D3D12Buffer* source = device_->buffer(source_handle);
    D3D12Texture* destination = device_->texture(destination_handle);
    if (source == nullptr || destination == nullptr) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = destination->resource->GetDesc();
    for (const BufferTextureCopy& region : regions) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total = 0;
        const UINT subresource =
            region.mip_level + region.base_layer * destination->desc.mip_levels;
        device_->raw_device()->GetCopyableFootprints(&desc, subresource, 1, region.buffer_offset,
                                                     &footprint, &rows, &row_bytes, &total);
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = source->resource.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = destination->resource.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination_location.SubresourceIndex = subresource;
        D3D12_BOX box{0,
                      0,
                      0,
                      region.texture_extent.width,
                      region.texture_extent.height,
                      region.texture_extent.depth};
        list->CopyTextureRegion(&destination_location, region.texture_offset.x,
                                region.texture_offset.y, region.texture_offset.z, &source_location,
                                &box);
    }
}

void D3D12CommandBuffer::copy_texture_to_buffer(TextureHandle source_handle,
                                                BufferHandle destination_handle,
                                                Span<const BufferTextureCopy> regions) noexcept {
    D3D12Texture* source = device_->texture(source_handle);
    D3D12Buffer* destination = device_->buffer(destination_handle);
    if (source == nullptr || destination == nullptr) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = source->resource->GetDesc();
    for (const BufferTextureCopy& region : regions) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total = 0;
        const UINT subresource = region.mip_level + region.base_layer * source->desc.mip_levels;
        device_->raw_device()->GetCopyableFootprints(&desc, subresource, 1, region.buffer_offset,
                                                     &footprint, &rows, &row_bytes, &total);
        D3D12_TEXTURE_COPY_LOCATION source_location{};
        source_location.pResource = source->resource.Get();
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source_location.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION destination_location{};
        destination_location.pResource = destination->resource.Get();
        destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination_location.PlacedFootprint = footprint;
        D3D12_BOX box{static_cast<UINT>(region.texture_offset.x),
                      static_cast<UINT>(region.texture_offset.y),
                      static_cast<UINT>(region.texture_offset.z),
                      static_cast<UINT>(region.texture_offset.x) + region.texture_extent.width,
                      static_cast<UINT>(region.texture_offset.y) + region.texture_extent.height,
                      static_cast<UINT>(region.texture_offset.z) + region.texture_extent.depth};
        list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, &box);
    }
}

void D3D12CommandBuffer::begin_debug_label(const char* name) noexcept {
    if (name != nullptr) {
        list->BeginEvent(0, name, static_cast<UINT>(std::strlen(name)));
    }
}
void D3D12CommandBuffer::end_debug_label() noexcept {
    list->EndEvent();
}
void D3D12CommandBuffer::insert_debug_label(const char* name) noexcept {
    if (name != nullptr) {
        list->SetMarker(0, name, static_cast<UINT>(std::strlen(name)));
    }
}

void D3D12CommandBuffer::write_timestamp(QueryPoolHandle handle, u32 index) noexcept {
    D3D12QueryPool* pool = device_->query_pool(handle);
    if (pool == nullptr || index >= pool->count) {
        return;
    }
    list->EndQuery(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    list->ResolveQueryData(pool->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1,
                           pool->readback.Get(), index * sizeof(u64));
}

void D3D12CommandBuffer::reset_queries(QueryPoolHandle, u32, u32) noexcept {
    // D3D12 query heaps need no explicit reset; resolving a new value replaces the old result.
}

void D3D12CommandBuffer::write_breadcrumb(u32, u32) noexcept {}

void D3D12BarrierRecorder::record_barriers(CommandBufferHandle command_handle,
                                           const BarrierBatch& batch) noexcept {
    D3D12CommandBuffer* command = device_->commands(command_handle);
    if (command == nullptr || command->raw() == nullptr || batch.empty()) {
        return;
    }
    Array<D3D12_RESOURCE_BARRIER> native;
    if (!native.reserve(batch.count())) {
        return;
    }
    for (const ImageBarrier& source : batch.images) {
        D3D12Texture* texture = device_->texture(source.texture);
        if (texture == nullptr || !texture->resource) {
            continue;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture->resource.Get();
        barrier.Transition.StateBefore = image_state(source.old_use);
        barrier.Transition.StateAfter = image_state(source.new_use);
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        (void)native.push_back(barrier);
    }
    for (const BufferBarrier& source : batch.buffers) {
        D3D12Buffer* buffer = device_->buffer(source.buffer);
        if (buffer == nullptr || !buffer->resource) {
            continue;
        }
        const D3D12_RESOURCE_STATES before = buffer_state(source.src_access);
        const D3D12_RESOURCE_STATES after = buffer_state(source.dst_access);
        D3D12_RESOURCE_BARRIER barrier{};
        if (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && after == before) {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = buffer->resource.Get();
        } else {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffer->resource.Get();
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        (void)native.push_back(barrier);
    }
    for (const MemoryBarrier& ignored : batch.memory) {
        (void)ignored;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        (void)native.push_back(barrier);
    }
    if (!native.empty()) {
        command->raw()->ResourceBarrier(static_cast<UINT>(native.size()), native.data());
    }
    ++batches_;
    barriers_ += batch.count();
}

}  // namespace cy::rhi::d3d12
