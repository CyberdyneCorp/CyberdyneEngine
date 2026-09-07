#include <cy/rendering/virtual_texturing/frame.h>

#include <algorithm>
#include <cstring>

#include "vt_frame_spirv.h"

namespace cy::rendering::vt {
namespace {

using render::vt::kMaxFeedbackDensity;
using render::vt::kMinFeedbackDensity;
using render::vt::PageTableEntry;

/// `record_feedback`'s `[numthreads(8, 8, 1)]` and `sample_pages`'s `[numthreads(64, 1, 1)]`. One
/// place each, so that a change to the shader that is not reflected here is a dispatch covering the
/// wrong domain rather than a comment.
constexpr u32 kRecordGroupSize = 8;
constexpr u32 kSampleGroupSize = 64;

/// The three header words `resolve_feedback` writes: how many requests, how many samples, how many
/// did not fit.
constexpr u32 kResolveHeaderWords = 3;

enum Binding : u32 {
    kBindingView = 0,
    kBindingMips = 1,
    kBindingPageSamples = 2,
    kBindingResolveHeader = 3,
    kBindingRequests = 4,
    kBindingPageTable = 5,
    kBindingSampleOut = 6,
    kBindingCount = 7,
};

[[nodiscard]] u64 at_least_one(u64 count, u64 stride) noexcept {
    return (count == 0 ? 1 : count) * stride;
}

/// The density lever's declared range. `virtual-texturing` puts the bounds on the requirement, so
/// clamping is what a caller gets rather than a refusal: a density outside the range is a tuning
/// mistake, not a configuration error, and a frame that stopped for one would be worse than a frame
/// that took the nearest legal value.
[[nodiscard]] u32 clamp_density(u32 requested) noexcept {
    return std::clamp(requested, kMinFeedbackDensity, kMaxFeedbackDensity);
}

}  // namespace

VirtualTextureFrame::~VirtualTextureFrame() {
    destroy();
}

Status VirtualTextureFrame::create(Allocator& allocator, rhi::Device& device,
                                   const VirtualTextureDesc& desc,
                                   const FeedbackSettings& settings) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the virtual texturing frame already exists");
    }
    if (!device.capabilities().has(rhi::Capability::ComputeShaders)) {
        return fail(ErrorCode::Unsupported,
                    "virtual texturing's feedback and page-table passes need "
                    "Capability::ComputeShaders");
    }
    if (Status valid = render::vt::validate_description(desc); !valid) {
        return valid;
    }

    // The count array and the page-table buffer are both one entry per addressable page. That is
    // the FLAT page table's cost, and it is refused at the flat table's own ceiling rather than at
    // a number invented here.
    PageTable shape(allocator);
    if (Status configured = shape.configure(desc); !configured) {
        return configured;
    }
    const usize entries = shape.entry_count();
    if (entries == 0 || entries > PageTable::kFlatEntryLimit) {
        return fail(
            ErrorCode::Unsupported,
            "this virtual texture's page pyramid is larger than PageTable::kFlatEntryLimit; "
            "a device-side feedback buffer of one word per page is the flat table's cost "
            "and has the flat table's ceiling, and a hash on the device is what a larger "
            "address space needs");
    }

    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    settings_ = settings;
    settings_.density = clamp_density(settings.density);
    entry_count_ = static_cast<u32>(entries);

    if (Status created = create_pipelines(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_buffers(); !created) {
        destroy();
        return created;
    }
    if (Status written = write_descriptors(); !written) {
        destroy();
        return written;
    }

    // The mip table, published once: these four numbers ARE `PageTable::linear_index`'s arithmetic,
    // and the shader reads them rather than recomputing the rounding.
    auto* mips = static_cast<MipLevel*>(device_->buffer_mapped_pointer(buffers_.mips));
    if (mips == nullptr) {
        destroy();
        return fail(ErrorCode::Internal, "the virtual texturing mip table is not mapped");
    }
    u32 offset = 0;
    for (u8 mip = 0; mip < desc_.mip_count; ++mip) {
        mips[mip].tiles_x = desc_.tiles_x(mip);
        mips[mip].tiles_y = desc_.tiles_y(mip);
        mips[mip].first_entry = offset;
        mips[mip].entries_per_layer = desc_.tile_count(mip);
        offset += desc_.tile_count(mip) * desc_.layers;
    }
    write_view();
    return ok();
}

void VirtualTextureFrame::set_density(u32 pixels_per_sample) noexcept {
    settings_.density = clamp_density(pixels_per_sample);
    write_view();
}

void VirtualTextureFrame::set_mip_bias(i32 bias) noexcept {
    settings_.mip_bias = bias;
    write_view();
}

void VirtualTextureFrame::write_view() noexcept {
    if (device_ == nullptr) {
        return;
    }
    auto* target = static_cast<View*>(device_->buffer_mapped_pointer(buffers_.view));
    if (target == nullptr) {
        return;
    }
    View view;
    view.texture_id = desc_.id;
    view.mip_count = desc_.mip_count;
    view.layers = desc_.layers;
    view.entry_count = entry_count_;
    view.density = settings_.density;
    view.grid_width = settings_.grid_width;
    view.grid_height = settings_.grid_height;
    view.request_capacity = settings_.request_capacity;
    view.mip_bias = settings_.mip_bias;
    *target = view;
}

Status VirtualTextureFrame::create_pipelines() noexcept {
    const struct {
        const char* name;
        const u32* words;
        usize bytes;
        rhi::ShaderModuleHandle* out;
    } modules[3] = {
        {"vt record feedback", kVtRecordSpirv, sizeof(kVtRecordSpirv), &record_shader_},
        {"vt resolve feedback", kVtResolveSpirv, sizeof(kVtResolveSpirv), &resolve_shader_},
        {"vt sample pages", kVtSampleSpirv, sizeof(kVtSampleSpirv), &sample_shader_},
    };
    for (const auto& request : modules) {
        rhi::ShaderModuleDescription description;
        description.name = request.name;
        description.stage = rhi::ShaderStage::Compute;
        // "main", not the entry name: slangc names a single-entry SPIR-V module's entry point
        // `main` whatever `-entry` said.
        description.entry_point = "main";
        description.spirv = Span<const u32>(request.words, request.bytes / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> module =
            device_->create_shader_module(description);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        *request.out = *module;
    }

    rhi::DescriptorBinding bindings[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index == kBindingView ? rhi::DescriptorKind::UniformBuffer
                                                     : rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "vt frame set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "vt frame layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created =
        device_->create_pipeline_layout(pipeline_layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_layout_ = *created;

    const struct {
        const char* name = nullptr;
        rhi::ShaderModuleHandle shader;
        rhi::ComputePipelineHandle* out = nullptr;
    } pipelines[3] = {
        {"vt record feedback", record_shader_, &record_pipeline_},
        {"vt resolve feedback", resolve_shader_, &resolve_pipeline_},
        {"vt sample pages", sample_shader_, &sample_pipeline_},
    };
    for (const auto& request : pipelines) {
        rhi::ComputePipelineDescription description;
        description.name = request.name;
        description.layout = pipeline_layout_;
        description.shader = request.shader;
        Expected<rhi::ComputePipelineHandle, Error> pipeline =
            device_->create_compute_pipeline(description);
        if (!pipeline.has_value()) {
            return make_unexpected(pipeline.error());
        }
        *request.out = *pipeline;
    }
    return ok();
}

Status VirtualTextureFrame::create_buffers() noexcept {
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        rhi::BufferHandle* out;
    };
    const auto storage = rhi::BufferUsage::Storage;
    const auto storage_src = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const Request requests[] = {
        {"vt view", sizeof(View), rhi::BufferUsage::Uniform, rhi::MemoryUse::Upload,
         &buffers_.view},
        {"vt mips", at_least_one(desc_.mip_count, sizeof(MipLevel)), storage,
         rhi::MemoryUse::Upload, &buffers_.mips},
        // THE FEEDBACK BUFFER ITSELF, device-local. It is never mapped: the whole point is that a
        // per-pixel — here per-page — stream does not reach the CPU, and a host-visible feedback
        // buffer would make that a matter of discipline rather than of address space.
        {"vt page samples", at_least_one(entry_count_, sizeof(u32)),
         storage | rhi::BufferUsage::TransferDestination, rhi::MemoryUse::DeviceLocal,
         &buffers_.page_samples},
        {"vt page samples zero", at_least_one(entry_count_, sizeof(u32)),
         rhi::BufferUsage::TransferSource, rhi::MemoryUse::Upload, &buffers_.page_samples_zero},
        {"vt resolve header", kResolveHeaderWords * sizeof(u32), storage_src,
         rhi::MemoryUse::DeviceLocal, &buffers_.resolve_header},
        {"vt requests", at_least_one(settings_.request_capacity, sizeof(FeedbackRequest)),
         storage_src, rhi::MemoryUse::DeviceLocal, &buffers_.requests},
        {"vt page table", at_least_one(entry_count_, sizeof(u32) * 2), storage,
         rhi::MemoryUse::Upload, &buffers_.page_table},
        {"vt sample results", at_least_one(entry_count_, sizeof(GpuSampleResult)), storage_src,
         rhi::MemoryUse::DeviceLocal, &buffers_.sample_out},
        {"vt resolve header readback", kResolveHeaderWords * sizeof(u32),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.header_readback},
        {"vt requests readback", at_least_one(settings_.request_capacity, sizeof(FeedbackRequest)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.requests_readback},
        {"vt sample readback", at_least_one(entry_count_, sizeof(GpuSampleResult)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.sample_readback},
    };
    for (const Request& request : requests) {
        rhi::BufferDescription description;
        description.name = request.name;
        description.size = request.size;
        description.usage = request.usage;
        description.memory = request.memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    if (void* zero = device_->buffer_mapped_pointer(buffers_.page_samples_zero); zero != nullptr) {
        std::memset(zero, 0, static_cast<usize>(entry_count_) * sizeof(u32));
    }
    return ok();
}

Status VirtualTextureFrame::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    const rhi::BufferHandle handles[kBindingCount] = {
        buffers_.view,     buffers_.mips,       buffers_.page_samples, buffers_.resolve_header,
        buffers_.requests, buffers_.page_table, buffers_.sample_out,
    };
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = index == kBindingView ? rhi::DescriptorKind::UniformBuffer
                                                   : rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = handles[index];
    }
    return device_->update_descriptor_set(descriptor_set_,
                                          Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

void VirtualTextureFrame::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    const rhi::BufferHandle buffers[] = {
        buffers_.view,
        buffers_.mips,
        buffers_.page_samples,
        buffers_.page_samples_zero,
        buffers_.resolve_header,
        buffers_.requests,
        buffers_.page_table,
        buffers_.sample_out,
        buffers_.header_readback,
        buffers_.requests_readback,
        buffers_.sample_readback,
    };
    for (rhi::BufferHandle handle : buffers) {
        device_->destroy_buffer(handle);
    }
    buffers_ = Buffers{};
    device_->destroy_compute_pipeline(record_pipeline_);
    device_->destroy_compute_pipeline(resolve_pipeline_);
    device_->destroy_compute_pipeline(sample_pipeline_);
    device_->destroy_pipeline_layout(pipeline_layout_);
    device_->destroy_descriptor_set_layout(set_layout_);
    device_->destroy_shader_module(record_shader_);
    device_->destroy_shader_module(resolve_shader_);
    device_->destroy_shader_module(sample_shader_);
    record_pipeline_ = {};
    resolve_pipeline_ = {};
    sample_pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    record_shader_ = {};
    resolve_shader_ = {};
    sample_shader_ = {};
    descriptor_set_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
}

// --- The page table upload -------------------------------------------------------------------

Status VirtualTextureFrame::upload_page_table(const PageTable& table) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the virtual texturing frame does not exist");
    }
    if (!table.configured() || table.description().id != desc_.id) {
        return fail(ErrorCode::InvalidArgument,
                    "the page table handed to upload_page_table describes a different texture");
    }
    auto* target = static_cast<u32*>(device_->buffer_mapped_pointer(buffers_.page_table));
    if (target == nullptr) {
        return fail(ErrorCode::Internal, "the virtual texturing page table buffer is not mapped");
    }

    // Every addressable page, in `PageTable::linear_index` order — which is the order the shader
    // indexes. A page nothing has resolved reads back as `kInvalid`, exactly as `lookup()` answers
    // for it, so the buffer and the CPU table say the same thing about a page that does not exist
    // yet as well as about one that does.
    for (u8 mip = 0; mip < desc_.mip_count; ++mip) {
        for (u8 layer = 0; layer < desc_.layers; ++layer) {
            for (u32 y = 0; y < desc_.tiles_y(mip); ++y) {
                for (u32 x = 0; x < desc_.tiles_x(mip); ++x) {
                    VirtualAddress address;
                    address.texture = desc_.id;
                    address.mip = mip;
                    address.layer = layer;
                    address.tile_x = static_cast<u16>(x);
                    address.tile_y = static_cast<u16>(y);
                    const usize index = table.linear_index(address);
                    if (index >= entry_count_) {
                        continue;
                    }
                    const PageTableEntry entry = table.lookup(address);
                    target[index * 2] = entry.physical_tile;
                    target[(index * 2) + 1] = static_cast<u32>(entry.resident_mip) |
                                              (static_cast<u32>(entry.flags) << 8U) |
                                              (static_cast<u32>(entry.generation) << 16U);
                }
            }
        }
    }
    return ok();
}

// --- Declaration ------------------------------------------------------------------------------

void VirtualTextureFrame::record_clear(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VirtualTextureFrame*>(user);
    const rhi::BufferCopy copy{0, 0, static_cast<u64>(self->entry_count_) * sizeof(u32)};
    context.commands->copy_buffer(self->buffers_.page_samples_zero, self->buffers_.page_samples,
                                  Span<const rhi::BufferCopy>(&copy, 1));
}

void VirtualTextureFrame::record_feedback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VirtualTextureFrame*>(user);
    context.commands->bind_compute_pipeline(self->record_pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    const u32 groups_x = (self->settings_.grid_width + kRecordGroupSize - 1U) / kRecordGroupSize;
    const u32 groups_y = (self->settings_.grid_height + kRecordGroupSize - 1U) / kRecordGroupSize;
    context.commands->dispatch(groups_x == 0 ? 1 : groups_x, groups_y == 0 ? 1 : groups_y, 1);
}

void VirtualTextureFrame::record_resolve(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VirtualTextureFrame*>(user);
    context.commands->bind_compute_pipeline(self->resolve_pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    // ONE GROUP: the compaction's output order is ascending page index and a second group would
    // have no ordering relationship with the first.
    context.commands->dispatch(1, 1, 1);
}

void VirtualTextureFrame::record_sample(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VirtualTextureFrame*>(user);
    context.commands->bind_compute_pipeline(self->sample_pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    const u32 groups = (self->entry_count_ + kSampleGroupSize - 1U) / kSampleGroupSize;
    context.commands->dispatch(groups == 0 ? 1 : groups, 1, 1);
}

void VirtualTextureFrame::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VirtualTextureFrame*>(user);
    struct Pair {
        rhi::BufferHandle source;
        rhi::BufferHandle destination;
        u64 size = 0;
    };
    const Pair pairs[] = {
        {self->buffers_.resolve_header, self->buffers_.header_readback,
         kResolveHeaderWords * sizeof(u32)},
        {self->buffers_.requests, self->buffers_.requests_readback,
         at_least_one(self->settings_.request_capacity, sizeof(FeedbackRequest))},
        {self->buffers_.sample_out, self->buffers_.sample_readback,
         at_least_one(self->entry_count_, sizeof(GpuSampleResult))},
    };
    for (const Pair& pair : pairs) {
        const rhi::BufferCopy copy{0, 0, pair.size};
        context.commands->copy_buffer(pair.source, pair.destination,
                                      Span<const rhi::BufferCopy>(&copy, 1));
    }
}

Status VirtualTextureFrame::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the virtual texturing frame does not exist");
    }
    const auto import = [&graph, this](const char* name, rhi::BufferHandle handle,
                                       rhi::BufferUsage extra) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = extra;
        return graph.import_buffer(request, handle);
    };

    const ResourceId view = import("vt view", buffers_.view, rhi::BufferUsage::Uniform);
    const ResourceId mips = import("vt mips", buffers_.mips, rhi::BufferUsage::Storage);
    const ResourceId samples =
        import("vt page samples", buffers_.page_samples, rhi::BufferUsage::Storage);
    const ResourceId zero = import("vt page samples zero", buffers_.page_samples_zero,
                                   rhi::BufferUsage::TransferSource);
    const ResourceId header =
        import("vt resolve header", buffers_.resolve_header, rhi::BufferUsage::Storage);
    const ResourceId requests = import("vt requests", buffers_.requests, rhi::BufferUsage::Storage);
    const ResourceId page_table =
        import("vt page table", buffers_.page_table, rhi::BufferUsage::Storage);
    const ResourceId sample_out =
        import("vt sample results", buffers_.sample_out, rhi::BufferUsage::Storage);
    const ResourceId header_out = import("vt resolve header readback", buffers_.header_readback,
                                         rhi::BufferUsage::TransferDestination);
    const ResourceId requests_out = import("vt requests readback", buffers_.requests_readback,
                                           rhi::BufferUsage::TransferDestination);
    const ResourceId sample_out_host = import("vt sample readback", buffers_.sample_readback,
                                              rhi::BufferUsage::TransferDestination);

    using rhi::Access;
    using rhi::QueueKind;

    graph.add_pass("vt clear feedback", QueueKind::Graphics)
        .read(zero, Access::TransferRead)
        .write(samples, Access::TransferWrite)
        .record(&record_clear, this);

    graph.add_pass("vt record feedback", QueueKind::Graphics)
        .read(view, Access::ComputeUniformRead)
        .read(mips, Access::ComputeStorageRead)
        .use(samples, Access::ComputeStorageReadWrite)
        .record(&record_feedback, this);

    graph.add_pass("vt resolve feedback", QueueKind::Graphics)
        .read(view, Access::ComputeUniformRead)
        .read(mips, Access::ComputeStorageRead)
        .read(samples, Access::ComputeStorageRead)
        .use(header, Access::ComputeStorageReadWrite)
        .write(requests, Access::ComputeStorageWrite)
        .record(&record_resolve, this);

    graph.add_pass("vt sample pages", QueueKind::Graphics)
        .read(view, Access::ComputeUniformRead)
        .read(mips, Access::ComputeStorageRead)
        .read(page_table, Access::ComputeStorageRead)
        .write(sample_out, Access::ComputeStorageWrite)
        .record(&record_sample, this);

    graph.add_pass("vt readback", QueueKind::Graphics)
        .read(header, Access::TransferRead)
        .read(requests, Access::TransferRead)
        .read(sample_out, Access::TransferRead)
        .write(header_out, Access::TransferWrite)
        .write(requests_out, Access::TransferWrite)
        .write(sample_out_host, Access::TransferWrite)
        .record(&record_readback, this);

    graph.add_pass("vt host read", QueueKind::Graphics)
        .read(header_out, Access::HostRead)
        .read(requests_out, Access::HostRead)
        .read(sample_out_host, Access::HostRead)
        .side_effect();

    return graph.status();
}

// --- Read-back ---------------------------------------------------------------------------------

Expected<VirtualTextureFrameReadback, Error> VirtualTextureFrame::read_back() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the virtual texturing frame does not exist"});
    }
    const auto* header =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.header_readback));
    const auto* requests = static_cast<const FeedbackRequest*>(
        device_->buffer_mapped_pointer(buffers_.requests_readback));
    if (header == nullptr || requests == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "a virtual texturing read-back buffer is not mapped"});
    }

    VirtualTextureFrameReadback result;
    const u32 count =
        header[0] <= settings_.request_capacity ? header[0] : settings_.request_capacity;
    result.requests = Span<const FeedbackRequest>(requests, count);
    result.total_samples = header[1];
    result.dropped = header[2];
    // WHAT THE CPU ACTUALLY MAPPED. Three header words and `count` compacted entries — never the
    // page array and never a pixel. This number is the requirement "a per-pixel request stream
    // SHALL NOT reach the CPU" expressed as something a test can assert against the pixel count.
    result.bytes_read =
        (kResolveHeaderWords * sizeof(u32)) + (static_cast<u64>(count) * sizeof(FeedbackRequest));
    bytes_read_ = result.bytes_read;
    return result;
}

Expected<Span<const GpuSampleResult>, Error> VirtualTextureFrame::sample_results() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the virtual texturing frame does not exist"});
    }
    const auto* results = static_cast<const GpuSampleResult*>(
        device_->buffer_mapped_pointer(buffers_.sample_readback));
    if (results == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "the virtual texturing sample buffer is not mapped"});
    }
    return Span<const GpuSampleResult>(results, entry_count_);
}

// --- The CPU mirror of the recording pass -------------------------------------------------------

bool VirtualTextureFrame::samples_pixel(u32 x, u32 y) const noexcept {
    const u32 pixel = (y * settings_.grid_width) + x;
    return (pixel % settings_.density) == 0;
}

VirtualAddress VirtualTextureFrame::address_of_pixel(u32 x, u32 y) const noexcept {
    // EVERY LINE OF THIS IS `record_feedback`'S, IN THE SAME ORDER AND THE SAME INTEGER TYPES.
    // Integer division truncates the same way on both sides, and writing the CPU form as floating
    // point "because it is clearer" is how the two stop agreeing at a tile boundary.
    const i32 half_width = static_cast<i32>(settings_.grid_width) / 2;
    const i32 half_height = static_cast<i32>(settings_.grid_height) / 2;
    const i32 dx = static_cast<i32>(x) - half_width;
    const i32 dy = static_cast<i32>(y) - half_height;
    const i32 abs_x = dx < 0 ? -dx : dx;
    const i32 abs_y = dy < 0 ? -dy : dy;
    const i32 away = abs_x > abs_y ? abs_x : abs_y;
    i32 mip = (away * static_cast<i32>(desc_.mip_count)) / (half_width > 0 ? half_width : 1);
    mip += settings_.mip_bias;
    // The same clamp `record_feedback` applies, and in the same order: bias first, then the range.
    // A clamp written the other way round answers differently for a bias that pushes past both
    // ends.
    mip = std::clamp(mip, 0, static_cast<i32>(desc_.mip_count) - 1);

    const auto level = static_cast<u8>(mip);
    VirtualAddress address;
    address.texture = desc_.id;
    address.mip = level;
    address.layer = 0;
    address.tile_x = static_cast<u16>((x * desc_.tiles_x(level)) / settings_.grid_width);
    address.tile_y = static_cast<u16>((y * desc_.tiles_y(level)) / settings_.grid_height);
    return address;
}

}  // namespace cy::rendering::vt
