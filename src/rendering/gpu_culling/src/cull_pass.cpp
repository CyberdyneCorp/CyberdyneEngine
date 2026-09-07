#include <cy/rendering/gpu_culling/cull_pass.h>

#include <cstring>

#include "gpu_cull_spirv.h"

namespace cy::rendering::gpu_culling {
namespace {

using render::culling::GpuCullCounters;
using render::culling::GpuCullView;
using render::culling::GpuDrawIndexedIndirect;
using render::culling::GpuDrawPayload;
using render::culling::GpuLodChain;
using render::culling::GpuMeshLod;
using render::culling::GpuVisibilityRange;
using render::culling::kGpuCullOcclusion;

/// `cull_instances`'s `[numthreads(64, 1, 1)]`. One place, so that a change to the shader that is
/// not reflected here is a dispatch that covers the wrong number of slots rather than a comment.
constexpr u32 kCullGroupSize = 64;

/// How many words `GpuCullCounters` is. Asserted against the struct rather than assumed, because
/// the shader indexes it as a flat array of words and the two descriptions must agree.
constexpr u32 kCounterWords = static_cast<u32>(sizeof(GpuCullCounters) / sizeof(u32));
static_assert(sizeof(GpuCullCounters) == kCounterWords * sizeof(u32));

/// The thirteen bindings the shader declares, in the order it declares them.
enum Binding : u32 {
    kBindingView = 0,
    kBindingInstances = 1,
    kBindingChains = 2,
    kBindingMeshLods = 3,
    kBindingRanges = 4,
    kBindingPreviousLevels = 5,
    kBindingSlotEmit = 6,
    kBindingSlotCommand = 7,
    kBindingSlotPayload = 8,
    kBindingCounters = 9,
    kBindingCommands = 10,
    kBindingPayloads = 11,
    kBindingVirtualGeometry = 12,
    kBindingCount = 13,
};

/// A buffer is never zero-sized: Vulkan has no zero-length buffer, and a descriptor must name
/// something even when the span behind it is empty. The push-constant counts are what tell the
/// shader that an empty span is empty.
[[nodiscard]] u64 at_least_one(u64 count, u64 stride) noexcept {
    return (count == 0 ? 1 : count) * stride;
}

template <typename T>
void copy_span(void* destination, Span<const T> source) noexcept {
    if (!source.empty() && destination != nullptr) {
        std::memcpy(destination, source.data(), source.size() * sizeof(T));
    }
}

}  // namespace

// --- Creation
// -------------------------------------------------------------------------------------

GpuCullPass::~GpuCullPass() {
    destroy();
}

bool GpuCullPass::supported(const rhi::Device& device) noexcept {
    return device.capabilities().has(rhi::Capability::ComputeShaders);
}

Status GpuCullPass::create(Allocator& allocator, rhi::Device& device,
                           const GpuCullPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the culling pass has already been created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "GPU-driven culling needs Capability::ComputeShaders; the CPU path for a "
                    "device without it is cpu_reference_cull, which computes the same answer");
    }
    if (desc.max_instances == 0 || desc.max_draws == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuCullPassDescription::max_instances and ::max_draws must both be non-zero; "
                    "the buffers cannot grow on the device and are sized once");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;

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
    return ok();
}

Status GpuCullPass::create_pipelines() noexcept {
    const struct {
        const char* name;
        const u32* words;
        usize bytes;
        rhi::ShaderModuleHandle* out;
        const char* entry;
    } modules[2] = {
        // "main", not "cull_instances": slangc names a single-entry SPIR-V module's entry point
        // `main` whatever `-entry` said, and the name the RHI passes is the one in the module.
        {"gpu cull instances", kGpuCullInstancesSpirv, sizeof(kGpuCullInstancesSpirv),
         &cull_shader_, "main"},
        {"gpu cull compact", kGpuCullCompactSpirv, sizeof(kGpuCullCompactSpirv), &compact_shader_,
         "main"},
    };
    for (const auto& request : modules) {
        rhi::ShaderModuleDescription description;
        description.name = request.name;
        description.stage = rhi::ShaderStage::Compute;
        description.entry_point = request.entry;
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
    set_layout.name = "gpu cull set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(Sizes)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "gpu cull layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created =
        device_->create_pipeline_layout(pipeline_layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_layout_ = *created;

    rhi::ComputePipelineDescription cull;
    cull.name = "gpu cull instances";
    cull.layout = pipeline_layout_;
    cull.shader = cull_shader_;
    Expected<rhi::ComputePipelineHandle, Error> cull_pipeline =
        device_->create_compute_pipeline(cull);
    if (!cull_pipeline.has_value()) {
        return make_unexpected(cull_pipeline.error());
    }
    cull_pipeline_ = *cull_pipeline;

    rhi::ComputePipelineDescription compact;
    compact.name = "gpu cull compact";
    compact.layout = pipeline_layout_;
    compact.shader = compact_shader_;
    Expected<rhi::ComputePipelineHandle, Error> compact_pipeline =
        device_->create_compute_pipeline(compact);
    if (!compact_pipeline.has_value()) {
        return make_unexpected(compact_pipeline.error());
    }
    compact_pipeline_ = *compact_pipeline;
    return ok();
}

Status GpuCullPass::create_buffers() noexcept {
    // WHICH MEMORY EACH BUFFER LIVES IN, AND WHY.
    //
    // The inputs are Upload: a cull's inputs change every frame by definition, so a device-local
    // copy would be a staging write followed by a transfer of the same bytes.
    //
    // `previous_levels` is Upload as well and is the one buffer the shader both reads and writes
    // through host memory. It is the LOD hysteresis state: the CPU seeds it, the dispatch updates
    // it, and the next frame reads what the dispatch wrote. A device-local round trip would be two
    // extra copies a frame for one word per instance.
    //
    // The outputs are DeviceLocal and copied into Readback buffers by a transfer pass, because
    // `commands` is what an indirect draw reads and indirect arguments belong in device memory. The
    // read-back is what makes the comparison against the reference possible; a shipped frame that
    // only draws never touches it.
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
        {"gpu cull view", sizeof(GpuCullView), rhi::BufferUsage::Uniform, rhi::MemoryUse::Upload,
         &buffers_.view},
        {"gpu cull instances", at_least_one(desc_.max_instances, sizeof(render::GpuInstance)),
         storage, rhi::MemoryUse::Upload, &buffers_.instances},
        {"gpu cull chains", at_least_one(desc_.max_lod_chains, sizeof(GpuLodChain)), storage,
         rhi::MemoryUse::Upload, &buffers_.chains},
        {"gpu cull mesh lods", at_least_one(desc_.max_mesh_lods, sizeof(GpuMeshLod)), storage,
         rhi::MemoryUse::Upload, &buffers_.mesh_lods},
        {"gpu cull ranges", at_least_one(desc_.max_visibility_ranges, sizeof(GpuVisibilityRange)),
         storage, rhi::MemoryUse::Upload, &buffers_.ranges},
        {"gpu cull previous levels", at_least_one(desc_.max_previous_levels, sizeof(u32)), storage,
         rhi::MemoryUse::Upload, &buffers_.previous_levels},
        {"gpu cull slot emit", at_least_one(desc_.max_instances, sizeof(u32)), storage,
         rhi::MemoryUse::DeviceLocal, &buffers_.slot_emit},
        {"gpu cull slot command", at_least_one(desc_.max_instances, sizeof(GpuDrawIndexedIndirect)),
         storage, rhi::MemoryUse::DeviceLocal, &buffers_.slot_command},
        {"gpu cull slot payload", at_least_one(desc_.max_instances, sizeof(GpuDrawPayload)),
         storage, rhi::MemoryUse::DeviceLocal, &buffers_.slot_payload},
        {"gpu cull counters", sizeof(GpuCullCounters),
         storage_src | rhi::BufferUsage::TransferDestination, rhi::MemoryUse::DeviceLocal,
         &buffers_.counters},
        {"gpu cull commands", at_least_one(desc_.max_draws, sizeof(GpuDrawIndexedIndirect)),
         storage_src | rhi::BufferUsage::Indirect, rhi::MemoryUse::DeviceLocal, &buffers_.commands},
        {"gpu cull payloads", at_least_one(desc_.max_draws, sizeof(GpuDrawPayload)), storage_src,
         rhi::MemoryUse::DeviceLocal, &buffers_.payloads},
        {"gpu cull virtual geometry", at_least_one(desc_.max_draws, sizeof(u32)), storage_src,
         rhi::MemoryUse::DeviceLocal, &buffers_.virtual_geometry},
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

    // The staging half of the four read-back buffers, and the sixteen zero words the counters are
    // cleared from. A `vkCmdFillBuffer` would do the clear in one command and is not on the RHI's
    // recording surface — a copy from an upload buffer is, and it costs sixty-four bytes.
    const Request host_requests[] = {
        {"gpu cull counters zero", sizeof(GpuCullCounters), rhi::BufferUsage::TransferSource,
         rhi::MemoryUse::Upload, &readback_.counters_zero},
        {"gpu cull counters readback", sizeof(GpuCullCounters),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback, &readback_.counters},
        {"gpu cull commands readback",
         at_least_one(desc_.max_draws, sizeof(GpuDrawIndexedIndirect)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback, &readback_.commands},
        {"gpu cull payloads readback", at_least_one(desc_.max_draws, sizeof(GpuDrawPayload)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback, &readback_.payloads},
        {"gpu cull cluster readback", at_least_one(desc_.max_draws, sizeof(u32)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &readback_.virtual_geometry},
    };
    for (const Request& request : host_requests) {
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
    if (void* zero = device_->buffer_mapped_pointer(readback_.counters_zero); zero != nullptr) {
        std::memset(zero, 0, sizeof(GpuCullCounters));
    }
    return ok();
}

Status GpuCullPass::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    const rhi::BufferHandle handles[kBindingCount] = {
        buffers_.view,
        buffers_.instances,
        buffers_.chains,
        buffers_.mesh_lods,
        buffers_.ranges,
        buffers_.previous_levels,
        buffers_.slot_emit,
        buffers_.slot_command,
        buffers_.slot_payload,
        buffers_.counters,
        buffers_.commands,
        buffers_.payloads,
        buffers_.virtual_geometry,
    };
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = index == kBindingView ? rhi::DescriptorKind::UniformBuffer
                                                   : rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = handles[index];
        writes[index].buffer_range = 0;  // the rest of the buffer
    }
    return device_->update_descriptor_set(descriptor_set_,
                                          Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

void GpuCullPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    const rhi::BufferHandle buffers[] = {
        buffers_.view,
        buffers_.instances,
        buffers_.chains,
        buffers_.mesh_lods,
        buffers_.ranges,
        buffers_.previous_levels,
        buffers_.slot_emit,
        buffers_.slot_command,
        buffers_.slot_payload,
        buffers_.counters,
        buffers_.commands,
        buffers_.payloads,
        buffers_.virtual_geometry,
        readback_.counters_zero,
        readback_.counters,
        readback_.commands,
        readback_.payloads,
        readback_.virtual_geometry,
    };
    for (rhi::BufferHandle handle : buffers) {
        device_->destroy_buffer(handle);
    }
    buffers_ = Buffers{};
    readback_ = Readback{};
    device_->destroy_compute_pipeline(cull_pipeline_);
    device_->destroy_compute_pipeline(compact_pipeline_);
    device_->destroy_pipeline_layout(pipeline_layout_);
    device_->destroy_descriptor_set_layout(set_layout_);
    device_->destroy_shader_module(cull_shader_);
    device_->destroy_shader_module(compact_shader_);
    cull_pipeline_ = {};
    compact_pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    cull_shader_ = {};
    compact_shader_ = {};
    descriptor_set_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
}

// --- Upload
// ---------------------------------------------------------------------------------------

Status GpuCullPass::upload(const render::culling::GpuCullScene& scene,
                           const GpuCullView& view) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the culling pass has not been created");
    }
    if ((view.flags & kGpuCullOcclusion) != 0U) {
        return fail(ErrorCode::NotImplemented,
                    "kGpuCullOcclusion is set and no hierarchical depth buffer exists on the "
                    "device yet; a dispatch that ignored the flag would report 'nothing was "
                    "occluded' and be indistinguishable from a working occlusion cull");
    }
    const u32 instances = view.instance_count < scene.instances.size()
                              ? view.instance_count
                              : static_cast<u32>(scene.instances.size());
    if (instances > desc_.max_instances) {
        return fail(ErrorCode::OutOfRange,
                    "the scene has more instances than the culling pass was sized for");
    }
    if (scene.chains.size() > desc_.max_lod_chains ||
        scene.mesh_lods.size() > desc_.max_mesh_lods ||
        scene.ranges.size() > desc_.max_visibility_ranges ||
        scene.previous_levels.size() > desc_.max_previous_levels) {
        return fail(ErrorCode::OutOfRange,
                    "one of the culling pass's side tables is larger than it was sized for");
    }
    instance_count_ = instances;

    // The view goes down with its own instance count clamped the way the reference clamps it, so
    // the dispatch and `cpu_reference_cull` cover the same slots without the caller having to know
    // that the clamp exists.
    GpuCullView uploaded = view;
    uploaded.instance_count = instances;
    if (void* target = device_->buffer_mapped_pointer(buffers_.view); target != nullptr) {
        std::memcpy(target, &uploaded, sizeof(GpuCullView));
    } else {
        return fail(ErrorCode::Internal, "the culling view buffer is not mapped");
    }

    copy_span(device_->buffer_mapped_pointer(buffers_.instances),
              Span<const render::GpuInstance>(scene.instances.data(), instances));
    copy_span(device_->buffer_mapped_pointer(buffers_.chains), scene.chains);
    copy_span(device_->buffer_mapped_pointer(buffers_.mesh_lods), scene.mesh_lods);
    copy_span(device_->buffer_mapped_pointer(buffers_.ranges), scene.ranges);
    copy_span(device_->buffer_mapped_pointer(buffers_.previous_levels),
              Span<const u32>(scene.previous_levels.data(), scene.previous_levels.size()));

    sizes_.chain_count = static_cast<u32>(scene.chains.size());
    sizes_.mesh_lod_count = static_cast<u32>(scene.mesh_lods.size());
    sizes_.range_count = static_cast<u32>(scene.ranges.size());
    sizes_.previous_level_count = static_cast<u32>(scene.previous_levels.size());
    sizes_.draw_capacity = desc_.max_draws;
    return ok();
}

// --- Declaration
// ------------------------------------------------------------------------------------

void GpuCullPass::record_cull(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<GpuCullPass*>(user);
    context.commands->bind_compute_pipeline(self->cull_pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    context.commands->push_constants(
        self->pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&self->sizes_), sizeof(Sizes)));
    const u32 groups = (self->instance_count_ + kCullGroupSize - 1U) / kCullGroupSize;
    context.commands->dispatch(groups == 0 ? 1 : groups, 1, 1);
}

void GpuCullPass::record_compact(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<GpuCullPass*>(user);
    context.commands->bind_compute_pipeline(self->compact_pipeline_);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    context.commands->push_constants(
        self->pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&self->sizes_), sizeof(Sizes)));
    // ONE GROUP. The compaction's whole purpose is that the survivors come out in ascending slot
    // order, and a second group would have no ordering relationship with the first.
    context.commands->dispatch(1, 1, 1);
}

void GpuCullPass::record_clear(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<GpuCullPass*>(user);
    const rhi::BufferCopy copy{0, 0, sizeof(GpuCullCounters)};
    context.commands->copy_buffer(self->readback_.counters_zero, self->buffers_.counters,
                                  Span<const rhi::BufferCopy>(&copy, 1));
}

void GpuCullPass::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<GpuCullPass*>(user);
    struct Pair {
        rhi::BufferHandle source;
        rhi::BufferHandle destination;
        u64 size = 0;
    };
    const Pair pairs[] = {
        {self->buffers_.counters, self->readback_.counters, sizeof(GpuCullCounters)},
        {self->buffers_.commands, self->readback_.commands,
         at_least_one(self->desc_.max_draws, sizeof(GpuDrawIndexedIndirect))},
        {self->buffers_.payloads, self->readback_.payloads,
         at_least_one(self->desc_.max_draws, sizeof(GpuDrawPayload))},
        {self->buffers_.virtual_geometry, self->readback_.virtual_geometry,
         at_least_one(self->desc_.max_draws, sizeof(u32))},
    };
    for (const Pair& pair : pairs) {
        const rhi::BufferCopy copy{0, 0, pair.size};
        context.commands->copy_buffer(pair.source, pair.destination,
                                      Span<const rhi::BufferCopy>(&copy, 1));
    }
}

Status GpuCullPass::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the culling pass has not been created");
    }

    // Every resource is imported rather than graph-owned: the buffers outlive the frame, the
    // descriptor set names them once, and a transient's handle changes every frame. `import_buffer`
    // wants the size the caller promises, which is what the descriptions carry.
    const auto import = [&graph, this](const char* name, rhi::BufferHandle handle,
                                       rhi::BufferUsage extra) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = extra;
        return graph.import_buffer(request, handle);
    };

    const ResourceId view = import("gpu cull view", buffers_.view, rhi::BufferUsage::Uniform);
    const ResourceId instances =
        import("gpu cull instances", buffers_.instances, rhi::BufferUsage::Storage);
    const ResourceId chains = import("gpu cull chains", buffers_.chains, rhi::BufferUsage::Storage);
    const ResourceId mesh_lods =
        import("gpu cull mesh lods", buffers_.mesh_lods, rhi::BufferUsage::Storage);
    const ResourceId ranges = import("gpu cull ranges", buffers_.ranges, rhi::BufferUsage::Storage);
    const ResourceId previous =
        import("gpu cull previous levels", buffers_.previous_levels, rhi::BufferUsage::Storage);
    const ResourceId slot_emit =
        import("gpu cull slot emit", buffers_.slot_emit, rhi::BufferUsage::Storage);
    const ResourceId slot_command =
        import("gpu cull slot command", buffers_.slot_command, rhi::BufferUsage::Storage);
    const ResourceId slot_payload =
        import("gpu cull slot payload", buffers_.slot_payload, rhi::BufferUsage::Storage);
    const ResourceId zero =
        import("gpu cull counters zero", readback_.counters_zero, rhi::BufferUsage::TransferSource);
    const ResourceId counters =
        import("gpu cull counters", buffers_.counters, rhi::BufferUsage::Storage);
    const ResourceId commands =
        import("gpu cull commands", buffers_.commands, rhi::BufferUsage::Storage);
    const ResourceId payloads =
        import("gpu cull payloads", buffers_.payloads, rhi::BufferUsage::Storage);
    const ResourceId clusters =
        import("gpu cull virtual geometry", buffers_.virtual_geometry, rhi::BufferUsage::Storage);
    const ResourceId counters_out = import("gpu cull counters readback", readback_.counters,
                                           rhi::BufferUsage::TransferDestination);
    const ResourceId commands_out = import("gpu cull commands readback", readback_.commands,
                                           rhi::BufferUsage::TransferDestination);
    const ResourceId payloads_out = import("gpu cull payloads readback", readback_.payloads,
                                           rhi::BufferUsage::TransferDestination);
    const ResourceId clusters_out = import("gpu cull cluster readback", readback_.virtual_geometry,
                                           rhi::BufferUsage::TransferDestination);

    using rhi::Access;
    using rhi::QueueKind;

    graph.add_pass("gpu cull clear counters", QueueKind::Graphics)
        .read(zero, Access::TransferRead)
        .write(counters, Access::TransferWrite)
        .record(&record_clear, this);

    graph.add_pass("gpu cull instances", QueueKind::Graphics)
        .read(view, Access::ComputeUniformRead)
        .read(instances, Access::ComputeStorageRead)
        .read(chains, Access::ComputeStorageRead)
        .read(mesh_lods, Access::ComputeStorageRead)
        .read(ranges, Access::ComputeStorageRead)
        .use(previous, Access::ComputeStorageReadWrite)
        .write(slot_emit, Access::ComputeStorageWrite)
        .write(slot_command, Access::ComputeStorageWrite)
        .write(slot_payload, Access::ComputeStorageWrite)
        .use(counters, Access::ComputeStorageReadWrite)
        .record(&record_cull, this);

    graph.add_pass("gpu cull compact", QueueKind::Graphics)
        .read(view, Access::ComputeUniformRead)
        .read(slot_emit, Access::ComputeStorageRead)
        .read(slot_command, Access::ComputeStorageRead)
        .read(slot_payload, Access::ComputeStorageRead)
        .write(commands, Access::ComputeStorageWrite)
        .write(payloads, Access::ComputeStorageWrite)
        .write(clusters, Access::ComputeStorageWrite)
        .record(&record_compact, this);

    graph.add_pass("gpu cull readback", QueueKind::Graphics)
        .read(counters, Access::TransferRead)
        .read(commands, Access::TransferRead)
        .read(payloads, Access::TransferRead)
        .read(clusters, Access::TransferRead)
        .write(counters_out, Access::TransferWrite)
        .write(commands_out, Access::TransferWrite)
        .write(payloads_out, Access::TransferWrite)
        .write(clusters_out, Access::TransferWrite)
        .record(&record_readback, this);

    // The host boundary is a dependency like any other: declaring it is what makes the graph emit
    // the transfer-to-host barrier, rather than this module relying on coherent memory and a fence.
    graph.add_pass("gpu cull host read", QueueKind::Graphics)
        .read(counters_out, Access::HostRead)
        .read(commands_out, Access::HostRead)
        .read(payloads_out, Access::HostRead)
        .read(clusters_out, Access::HostRead)
        .read(previous, Access::HostRead)
        .side_effect();

    return graph.status();
}

// --- Read-back
// ---------------------------------------------------------------------------------------

Expected<GpuCullReadback, Error> GpuCullPass::read_back() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the culling pass has not been created"});
    }
    const auto* counters =
        static_cast<const GpuCullCounters*>(device_->buffer_mapped_pointer(readback_.counters));
    const auto* commands = static_cast<const GpuDrawIndexedIndirect*>(
        device_->buffer_mapped_pointer(readback_.commands));
    const auto* payloads =
        static_cast<const GpuDrawPayload*>(device_->buffer_mapped_pointer(readback_.payloads));
    const auto* clusters =
        static_cast<const u32*>(device_->buffer_mapped_pointer(readback_.virtual_geometry));
    const auto* previous =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.previous_levels));
    if (counters == nullptr || commands == nullptr || payloads == nullptr || clusters == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "a culling read-back buffer is not mapped"});
    }

    GpuCullReadback result;
    result.counters = *counters;
    // The buffers are sized for the high water mark and the dispatch filled a prefix of them; the
    // spans stop at what the counters say was written, so nothing downstream reads uninitialised
    // device memory and calls it a draw.
    const u32 draws =
        result.counters.draws <= desc_.max_draws ? result.counters.draws : desc_.max_draws;
    const u32 clustered = result.counters.virtual_geometry <= desc_.max_draws
                              ? result.counters.virtual_geometry
                              : desc_.max_draws;
    result.commands = Span<const GpuDrawIndexedIndirect>(commands, draws);
    result.payloads = Span<const GpuDrawPayload>(payloads, draws);
    result.virtual_geometry = Span<const u32>(clusters, clustered);
    result.previous_levels = previous == nullptr
                                 ? Span<const u32>()
                                 : Span<const u32>(previous, sizes_.previous_level_count);
    return result;
}

}  // namespace cy::rendering::gpu_culling
