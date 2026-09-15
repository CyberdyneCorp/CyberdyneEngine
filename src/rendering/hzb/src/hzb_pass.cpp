#include <cy/rendering/hzb/hzb_pass.h>

#include <cy/rendering/graph/executor.h>

#include <cstring>

#include "hzb_spirv.h"

namespace cy::rendering::hzb {
namespace {

using render::culling::hzb_level_count;

/// `hzb_reduce`'s `[numthreads(8, 8, 1)]`. One place, so a change to the shader that is not
/// reflected here covers the wrong number of texels rather than leaving a comment out of date.
constexpr u32 kReduceGroupSize = 8;

[[nodiscard]] u32 halved(u32 value) noexcept {
    return value > 1U ? (value + 1U) / 2U : 1U;
}

}  // namespace

u32 hzb_total_texels(u32 width, u32 height) noexcept {
    const u32 levels = hzb_level_count(width, height);
    u32 total = 0;
    u32 level_width = width;
    u32 level_height = height;
    for (u32 level = 0; level < levels; ++level) {
        total += level_width * level_height;
        level_width = halved(level_width);
        level_height = halved(level_height);
    }
    return total;
}

u32 hzb_level_offset(u32 width, u32 height, u32 level) noexcept {
    u32 offset = 0;
    u32 level_width = width;
    u32 level_height = height;
    for (u32 step = 0; step < level; ++step) {
        offset += level_width * level_height;
        level_width = halved(level_width);
        level_height = halved(level_height);
    }
    return offset;
}

HzbParams hzb_params(const Mat4& view_projection, u32 width, u32 height, u32 level_count,
                     bool enabled) noexcept {
    HzbParams params;
    for (u32 column = 0; column < 4; ++column) {
        params.view_projection[column][0] = view_projection.columns[column].x;
        params.view_projection[column][1] = view_projection.columns[column].y;
        params.view_projection[column][2] = view_projection.columns[column].z;
        params.view_projection[column][3] = view_projection.columns[column].w;
    }
    params.width = width;
    params.height = height;
    params.level_count = level_count;
    params.enabled = enabled ? 1U : 0U;
    return params;
}

// --- Creation -----------------------------------------------------------------------------------

HzbPass::~HzbPass() {
    destroy();
}

bool HzbPass::supported(const rhi::Device& device) noexcept {
    return device.capabilities().has(rhi::Capability::ComputeShaders);
}

Status HzbPass::create(Allocator& allocator, rhi::Device& device,
                       const HzbPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the hierarchical depth pass has already been "
                    "created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "a hierarchical depth buffer is built by a chain of compute reductions and "
                    "this device has no compute queue; the CPU model is cy::render::culling::Hzb "
                    "and computes the same answer");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "HzbPassDescription::width and ::height must both be non-zero");
    }
    const u32 levels = hzb_level_count(desc.width, desc.height);
    if (levels > kMaxLevels) {
        return fail(ErrorCode::OutOfRange,
                    "the pyramid has more levels than this pass records reduction steps for; a "
                    "resolution past 2^19 is a different problem than this one");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    level_count_ = levels;
    total_texels_ = hzb_total_texels(desc.width, desc.height);
    valid_ = false;

    if (Status created = create_pipeline(); !created) {
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
    return ok();
}

Status HzbPass::create_pipeline() noexcept {
    rhi::ShaderModuleDescription module;
    module.name = "hzb reduce";
    module.stage = rhi::ShaderStage::Compute;
    // "main", not "hzb_reduce": slangc names a single-entry SPIR-V module's entry point `main`
    // whatever `-entry` said, and the name the RHI passes is the one in the module.
    module.entry_point = "main";
    module.spirv = Span<const u32>(kHzbReduceSpirv, sizeof(kHzbReduceSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> created = device_->create_shader_module(module);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    shader_ = *created;

    rhi::DescriptorBinding binding{};
    binding.binding = 0;
    binding.kind = rhi::DescriptorKind::StorageBuffer;
    binding.count = 1;
    binding.stages = rhi::ShaderStage::Compute;
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "hzb set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(&binding, 1);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(Reduce)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "hzb layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> pipeline_layout_created =
        device_->create_pipeline_layout(pipeline_layout);
    if (!pipeline_layout_created.has_value()) {
        return make_unexpected(pipeline_layout_created.error());
    }
    pipeline_layout_ = *pipeline_layout_created;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = "hzb reduce";
    pipeline.layout = pipeline_layout_;
    pipeline.shader = shader_;
    Expected<rhi::ComputePipelineHandle, Error> pipeline_created =
        device_->create_compute_pipeline(pipeline);
    if (!pipeline_created.has_value()) {
        return make_unexpected(pipeline_created.error());
    }
    pipeline_ = *pipeline_created;
    return ok();
}

Status HzbPass::create_buffers() noexcept {
    // DeviceLocal, and a transfer destination because level 0 is SEEDED by a copy from whatever
    // buffer the depth was written into. A compute seed would let the source be a different format;
    // a copy is what the engine's depth already is — `width * height` floats, row major — and one
    // fewer entry point to keep in step with a CPU model.
    rhi::BufferDescription pyramid;
    pyramid.name = "hzb pyramid";
    pyramid.size = static_cast<u64>(total_texels_) * sizeof(f32);
    pyramid.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDestination |
                    rhi::BufferUsage::TransferSource;
    pyramid.memory = rhi::MemoryUse::DeviceLocal;
    Expected<rhi::BufferHandle, Error> created = device_->create_buffer(pyramid);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pyramid_ = *created;

    if (desc_.readback) {
        rhi::BufferDescription host;
        host.name = "hzb readback";
        host.size = pyramid.size;
        host.usage = rhi::BufferUsage::TransferDestination;
        host.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> host_created = device_->create_buffer(host);
        if (!host_created.has_value()) {
            return make_unexpected(host_created.error());
        }
        readback_ = *host_created;
    }
    return ok();
}

Status HzbPass::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    rhi::DescriptorWrite write{};
    write.binding = 0;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = pyramid_;
    write.buffer_range = 0;
    return device_->update_descriptor_set(descriptor_set_,
                                          Span<const rhi::DescriptorWrite>(&write, 1));
}

void HzbPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    // Guarded, because `readback_` is only created when the description asked for it and the
    // backend's validation reports a destroy of a never-issued handle as an ERROR — which a suite
    // that asserts zero validation errors would then attribute to the frame it just ran.
    if (!pyramid_.is_null()) {
        device_->destroy_buffer(pyramid_);
    }
    if (!readback_.is_null()) {
        device_->destroy_buffer(readback_);
    }
    device_->destroy_compute_pipeline(pipeline_);
    device_->destroy_pipeline_layout(pipeline_layout_);
    device_->destroy_descriptor_set_layout(set_layout_);
    device_->destroy_shader_module(shader_);
    pyramid_ = {};
    readback_ = {};
    pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    shader_ = {};
    descriptor_set_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
    valid_ = false;
}

// --- Recording ----------------------------------------------------------------------------------

/// The seed reads the depth through the EXECUTOR rather than through a handle captured at declare
/// time, which is the same rule `GpuTraversal::PassState` records the hard way: a declaration runs
/// now and a record callback runs later, and a handle read at the wrong end of that gap is the
/// defect that looks exactly like a shader selecting nothing.
void HzbPass::record_seed(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<HzbPass*>(user);
    const rhi::BufferHandle source = context.executor->buffer(self->seed_depth_);
    const rhi::BufferCopy copy{
        0, 0, static_cast<u64>(self->desc_.width) * self->desc_.height * sizeof(f32)};
    context.commands->copy_buffer(source, self->pyramid_, Span<const rhi::BufferCopy>(&copy, 1));
}

void HzbPass::record_reduce(const PassContext& context, void* user) noexcept {
    auto* step = static_cast<Step*>(user);
    HzbPass* self = step->self;
    context.commands->bind_compute_pipeline(self->pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    context.commands->push_constants(
        self->pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&step->push), sizeof(Reduce)));
    const u32 groups_x = (step->push.destination_width + kReduceGroupSize - 1U) / kReduceGroupSize;
    const u32 groups_y = (step->push.destination_height + kReduceGroupSize - 1U) / kReduceGroupSize;
    context.commands->dispatch(groups_x == 0 ? 1 : groups_x, groups_y == 0 ? 1 : groups_y, 1);
}

void HzbPass::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<HzbPass*>(user);
    const rhi::BufferCopy copy{0, 0, static_cast<u64>(self->total_texels_) * sizeof(f32)};
    context.commands->copy_buffer(self->pyramid_, self->readback_,
                                  Span<const rhi::BufferCopy>(&copy, 1));
}

Expected<ResourceId, Error> HzbPass::declare(RenderGraph& graph, ResourceId depth) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the hierarchical depth pass has not been created");
    }
    if (depth == kInvalidResource) {
        return fail(ErrorCode::InvalidArgument,
                    "the depth a pyramid is built from must be a graph resource, so that the "
                    "barrier between the pass that wrote it and the seed copy is the graph's");
    }
    seed_depth_ = depth;

    BufferRequest request;
    request.name = "hzb pyramid";
    request.size = static_cast<u64>(total_texels_) * sizeof(f32);
    request.extra_usage = rhi::BufferUsage::Storage;
    const ResourceId pyramid = graph.import_buffer(request, pyramid_);

    using rhi::Access;
    using rhi::QueueKind;

    graph.add_pass("hzb seed level 0", QueueKind::Graphics)
        .read(depth, Access::TransferRead)
        .write(pyramid, Access::TransferWrite)
        .record(&record_seed, this);

    // ONE PASS PER LEVEL, and the barrier between two of them is the graph's. A single dispatch
    // over the whole chain would need every group to observe the previous level's writes, which is
    // a device-wide barrier no compute shader can emit.
    u32 source_width = desc_.width;
    u32 source_height = desc_.height;
    for (u32 level = 1; level < level_count_; ++level) {
        const u32 destination_width = halved(source_width);
        const u32 destination_height = halved(source_height);
        Step& step = steps_[level];
        step.self = this;
        step.push.source_offset = hzb_level_offset(desc_.width, desc_.height, level - 1U);
        step.push.destination_offset = hzb_level_offset(desc_.width, desc_.height, level);
        step.push.source_width = source_width;
        step.push.source_height = source_height;
        step.push.destination_width = destination_width;
        step.push.destination_height = destination_height;
        graph.add_pass("hzb reduce", QueueKind::Graphics)
            .use(pyramid, Access::ComputeStorageReadWrite)
            .record(&record_reduce, &step);
        source_width = destination_width;
        source_height = destination_height;
    }

    if (desc_.readback) {
        BufferRequest host;
        host.name = "hzb readback";
        host.size = request.size;
        host.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph.import_buffer(host, readback_);
        graph.add_pass("hzb readback", QueueKind::Graphics)
            .read(pyramid, Access::TransferRead)
            .write(destination, Access::TransferWrite)
            .record(&record_readback, this);
    }
    return pyramid;
}

HzbParams HzbPass::params(const Mat4& view_projection) const noexcept {
    return hzb_params(view_projection, desc_.width, desc_.height, level_count_, valid_);
}

Status HzbPass::read_back(render::culling::Hzb& model) const noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the hierarchical depth pass has not been created");
    }
    if (readback_.is_null()) {
        return fail(ErrorCode::InvalidArgument,
                    "this pass was created without HzbPassDescription::readback, so there is no "
                    "host copy of the pyramid to read; an empty answer is the one a comparison "
                    "cannot misread as agreement");
    }
    const void* source = device_->buffer_mapped_pointer(readback_);
    if (source == nullptr) {
        return fail(ErrorCode::Internal, "the hierarchical depth readback buffer is not mapped");
    }
    if (Status resized = model.resize(desc_.width, desc_.height); !resized) {
        return resized;
    }
    const auto* texels = static_cast<const f32*>(source);
    for (u32 level = 0; level < level_count_; ++level) {
        const Span<f32> destination = model.level(level);
        const u32 offset = hzb_level_offset(desc_.width, desc_.height, level);
        if (destination.empty()) {
            return fail(ErrorCode::Internal,
                        "the CPU model has fewer levels than the device "
                        "pyramid; hzb_level_count disagrees with itself");
        }
        std::memcpy(destination.data(), texels + offset, destination.size() * sizeof(f32));
    }
    if (valid_) {
        model.mark_valid();
    }
    return ok();
}

}  // namespace cy::rendering::hzb
