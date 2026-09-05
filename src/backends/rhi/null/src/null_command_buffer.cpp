// The null backend's command recording and barrier recorder. Task 2.1.2.
//
// Every call appends one RecordedCommand to the device's log and returns. The log is the point: it
// is what makes "the null backend records the same graph" (task 6.4) a comparison of two numbers
// rather than an assertion nobody can check, and it is what a graph test asserts a pass order
// against without needing a device.
//
// WHAT EACH RECORD'S FIELDS MEAN is documented per command below, because a flat four-word record
// is only readable if the mapping is written down next to the code that fills it.

#include "null_internal.h"

namespace cy::rhi::null {
namespace {

/// The low 32 bits of a handle, for the record's identity fields. The whole handle goes in
/// `handle_bits` where a command has one primary subject; this is for the secondary ones, where the
/// slot index is enough to tell two apart in a comparison.
u32 short_id(u64 bits) noexcept {
    return static_cast<u32>(bits);
}

}  // namespace

void NullCommandBuffer::append(const RecordedCommand& command) noexcept {
    // The command buffer's own array, so two workers recording into two buffers touch nothing in
    // common. A failure to grow drops the record rather than reporting: the log is a diagnostic,
    // and an out-of-memory here would already have failed the recording it describes.
    (void)log_.push_back(command);
}

void NullCommandBuffer::take_log_into(Array<RecordedCommand>& target) noexcept {
    for (const RecordedCommand& command : log_) {
        (void)target.push_back(command);
    }
    log_.clear();
}

const char* command_kind_name(CommandKind kind) noexcept {
    switch (kind) {
        case CommandKind::BeginRendering:
            return "begin-rendering";
        case CommandKind::EndRendering:
            return "end-rendering";
        case CommandKind::SetViewport:
            return "set-viewport";
        case CommandKind::SetScissor:
            return "set-scissor";
        case CommandKind::BindGraphicsPipeline:
            return "bind-graphics-pipeline";
        case CommandKind::BindComputePipeline:
            return "bind-compute-pipeline";
        case CommandKind::BindDescriptorSets:
            return "bind-descriptor-sets";
        case CommandKind::PushConstants:
            return "push-constants";
        case CommandKind::BindVertexBuffers:
            return "bind-vertex-buffers";
        case CommandKind::BindIndexBuffer:
            return "bind-index-buffer";
        case CommandKind::Draw:
            return "draw";
        case CommandKind::DrawIndexed:
            return "draw-indexed";
        case CommandKind::DrawIndexedIndirect:
            return "draw-indexed-indirect";
        case CommandKind::Dispatch:
            return "dispatch";
        case CommandKind::DispatchIndirect:
            return "dispatch-indirect";
        case CommandKind::CopyBuffer:
            return "copy-buffer";
        case CommandKind::CopyBufferToTexture:
            return "copy-buffer-to-texture";
        case CommandKind::CopyTextureToBuffer:
            return "copy-texture-to-buffer";
        case CommandKind::BeginDebugLabel:
            return "begin-debug-label";
        case CommandKind::EndDebugLabel:
            return "end-debug-label";
        case CommandKind::InsertDebugLabel:
            return "insert-debug-label";
        case CommandKind::WriteTimestamp:
            return "write-timestamp";
        case CommandKind::ResetQueries:
            return "reset-queries";
        case CommandKind::WriteBreadcrumb:
            return "write-breadcrumb";
        case CommandKind::Barriers:
            return "barriers";
        case CommandKind::ExecuteSecondary:
            return "execute-secondary";
    }
    return "<invalid>";
}

// --- Rendering scopes ---------------------------------------------------------------------------

/// a: colour attachment count, b: 1 when a depth attachment is present, c: layer count,
/// d: multiview mask.
void NullCommandBuffer::begin_rendering(const RenderingInfo& info) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BeginRendering;
    command.a = static_cast<u32>(info.color_attachments.size());
    command.b = info.depth_attachment.view.is_null() ? 0U : 1U;
    command.c = info.layer_count;
    command.d = info.view_mask;
    command.handle_bits =
        info.color_attachments.empty() ? 0ULL : info.color_attachments[0].view.bits();
    append(command);
}

void NullCommandBuffer::end_rendering() noexcept {
    RecordedCommand command;
    command.kind = CommandKind::EndRendering;
    append(command);
}

/// a-d: the viewport rectangle, rounded to integers. The depth range is deliberately not recorded:
/// it is [0, 1] for every pipeline the engine builds (reversed-Z inverts the projection, not the
/// viewport), so recording it would only add a constant to every comparison.
void NullCommandBuffer::set_viewport(const Viewport& viewport) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::SetViewport;
    command.a = static_cast<u32>(viewport.x);
    command.b = static_cast<u32>(viewport.y);
    command.c = static_cast<u32>(viewport.width);
    command.d = static_cast<u32>(viewport.height);
    append(command);
}

void NullCommandBuffer::set_scissor(const Rect2D& scissor) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::SetScissor;
    command.a = static_cast<u32>(scissor.x);
    command.b = static_cast<u32>(scissor.y);
    command.c = scissor.width;
    command.d = scissor.height;
    append(command);
}

// --- Binding ------------------------------------------------------------------------------------

void NullCommandBuffer::bind_graphics_pipeline(GraphicsPipelineHandle pipeline) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BindGraphicsPipeline;
    command.handle_bits = pipeline.bits();
    append(command);
}

void NullCommandBuffer::bind_compute_pipeline(ComputePipelineHandle pipeline) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BindComputePipeline;
    command.handle_bits = pipeline.bits();
    append(command);
}

/// a: first set, b: set count, c: the first set's slot index.
void NullCommandBuffer::bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                                             Span<const DescriptorSetHandle> sets) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BindDescriptorSets;
    command.a = first_set;
    command.b = static_cast<u32>(sets.size());
    command.c = sets.empty() ? 0U : short_id(sets[0].bits());
    command.handle_bits = layout.bits();
    append(command);

    // The limit is the engine's, not the device's, and it is checked where it is exceeded rather
    // than at the draw that would have used the ninth set. See types.h.
    if (first_set + sets.size() > kMaxDescriptorSets) {
        ++counts_.validation_errors;
    }
}

/// a: offset, b: byte count, c: the stage mask.
void NullCommandBuffer::push_constants(PipelineLayoutHandle layout, ShaderStage stages, u32 offset,
                                       Span<const u8> data) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::PushConstants;
    command.a = offset;
    command.b = static_cast<u32>(data.size());
    command.c = static_cast<u32>(stages);
    command.handle_bits = layout.bits();
    append(command);

    if (offset + data.size() > kMaxPushConstantBytes) {
        ++counts_.validation_errors;
    }
}

/// a: first binding, b: buffer count, c: the first buffer's slot index.
void NullCommandBuffer::bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> buffers,
                                            Span<const u64> offsets) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BindVertexBuffers;
    command.a = first_binding;
    command.b = static_cast<u32>(buffers.size());
    command.c = buffers.empty() ? 0U : short_id(buffers[0].bits());
    command.d = static_cast<u32>(offsets.size());
    append(command);
}

/// a: the byte offset, b: 1 for 32-bit indices.
void NullCommandBuffer::bind_index_buffer(BufferHandle buffer, u64 offset, bool wide) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BindIndexBuffer;
    command.a = static_cast<u32>(offset);
    command.b = wide ? 1U : 0U;
    command.handle_bits = buffer.bits();
    append(command);
}

// --- Draws and dispatches
// -------------------------------------------------------------------------

void NullCommandBuffer::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                             u32 first_instance) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::Draw;
    command.a = vertex_count;
    command.b = instance_count;
    command.c = first_vertex;
    command.d = first_instance;
    append(command);
    ++counts_.draws;
}

void NullCommandBuffer::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                                     i32 vertex_offset, u32 first_instance) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::DrawIndexed;
    command.a = index_count;
    command.b = instance_count;
    command.c = first_index;
    command.d = first_instance;
    command.handle_bits = static_cast<u64>(static_cast<u32>(vertex_offset));
    append(command);
    ++counts_.draws;
}

void NullCommandBuffer::draw_indexed_indirect(BufferHandle arguments, u64 offset, u32 draw_count,
                                              u32 stride) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::DrawIndexedIndirect;
    command.a = static_cast<u32>(offset);
    command.b = draw_count;
    command.c = stride;
    command.handle_bits = arguments.bits();
    append(command);
    ++counts_.draws;
}

void NullCommandBuffer::dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::Dispatch;
    command.a = groups_x;
    command.b = groups_y;
    command.c = groups_z;
    append(command);
    ++counts_.dispatches;
}

void NullCommandBuffer::dispatch_indirect(BufferHandle arguments, u64 offset) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::DispatchIndirect;
    command.a = static_cast<u32>(offset);
    command.handle_bits = arguments.bits();
    append(command);
    ++counts_.dispatches;
}

// --- Copies
// ---------------------------------------------------------------------------------------

/// a: region count, b: the destination's slot index, c: the total bytes the regions move.
void NullCommandBuffer::copy_buffer(BufferHandle source, BufferHandle destination,
                                    Span<const BufferCopy> regions) noexcept {
    u64 bytes = 0;
    for (const BufferCopy& region : regions) {
        bytes += region.size;
    }
    RecordedCommand command;
    command.kind = CommandKind::CopyBuffer;
    command.a = static_cast<u32>(regions.size());
    command.b = short_id(destination.bits());
    command.c = static_cast<u32>(bytes);
    command.handle_bits = source.bits();
    append(command);
}

void NullCommandBuffer::copy_buffer_to_texture(BufferHandle source, TextureHandle destination,
                                               Span<const BufferTextureCopy> regions) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::CopyBufferToTexture;
    command.a = static_cast<u32>(regions.size());
    command.b = short_id(destination.bits());
    command.handle_bits = source.bits();
    append(command);
}

void NullCommandBuffer::copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                               Span<const BufferTextureCopy> regions) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::CopyTextureToBuffer;
    command.a = static_cast<u32>(regions.size());
    command.b = short_id(destination.bits());
    command.handle_bits = source.bits();
    append(command);
}

// --- Debugging
// ------------------------------------------------------------------------------------

void NullCommandBuffer::begin_debug_label(const char* name) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::BeginDebugLabel;
    command.label = name != nullptr ? name : "";
    append(command);
}

void NullCommandBuffer::end_debug_label() noexcept {
    RecordedCommand command;
    command.kind = CommandKind::EndDebugLabel;
    append(command);
}

void NullCommandBuffer::insert_debug_label(const char* name) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::InsertDebugLabel;
    command.label = name != nullptr ? name : "";
    append(command);
}

void NullCommandBuffer::write_timestamp(QueryPoolHandle pool, u32 index) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::WriteTimestamp;
    command.a = index;
    command.handle_bits = pool.bits();
    append(command);
}

void NullCommandBuffer::reset_queries(QueryPoolHandle pool, u32 first, u32 count) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::ResetQueries;
    command.a = first;
    command.b = count;
    command.handle_bits = pool.bits();
    append(command);
}

void NullCommandBuffer::write_breadcrumb(u32 slot, u32 value) noexcept {
    RecordedCommand command;
    command.kind = CommandKind::WriteBreadcrumb;
    command.a = slot;
    command.b = value;
    append(command);
}

// --- The barrier recorder
// ---------------------------------------------------------------------------

/// a: image barriers, b: buffer barriers, c: memory barriers, d: queue-family transfers among them.
///
/// Recording the counts rather than the barriers themselves is deliberate: what a comparison
/// between two runs, or between the null backend and Vulkan, has to agree on is that the same
/// barriers were emitted at the same points — and the barriers' contents are already asserted
/// directly by the graph's own tests, against the derived plan, without a device in the way.
void NullBarrierRecorder::record_barriers(CommandBufferHandle command_buffer,
                                          const BarrierBatch& batch) noexcept {
    NullCommandBuffer* target = device_->buffer_for(command_buffer);
    if (target == nullptr) {
        return;
    }
    u32 transfers = 0;
    for (const ImageBarrier& barrier : batch.images) {
        if (barrier.src_queue_family != barrier.dst_queue_family) {
            ++transfers;
        }
    }
    for (const BufferBarrier& barrier : batch.buffers) {
        if (barrier.src_queue_family != barrier.dst_queue_family) {
            ++transfers;
        }
    }

    RecordedCommand command;
    command.kind = CommandKind::Barriers;
    command.a = static_cast<u32>(batch.images.size());
    command.b = static_cast<u32>(batch.buffers.size());
    command.c = static_cast<u32>(batch.memory.size());
    command.d = transfers;
    command.handle_bits = command_buffer.bits();
    target->append(command);

    ++batches_;
    barriers_ += batch.count();

    DeviceStatistics& stats = device_->mutable_statistics();
    ++stats.barrier_batches;
    stats.barriers += batch.count();
    stats.queue_ownership_transfers += transfers;
}

}  // namespace cy::rhi::null
