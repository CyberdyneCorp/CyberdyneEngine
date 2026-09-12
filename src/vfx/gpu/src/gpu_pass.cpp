#include <cy/vfx/gpu/gpu_pass.h>

#include "vfx_support_spirv.h"

#include <cy/core/memory/scope.h>

#include <algorithm>
#include <cstring>

#if CY_SHADER_SLANG
#    include <cy/backends/shader/compiler.h>
#    include <cy/backends/shader/slang/slang_compiler.h>
#    include <cy/backends/shader/source.h>
#endif

namespace cy::vfx::gpu {
namespace {

/// Vulkan has no zero-length buffer, and a descriptor must name something even when the array
/// behind it is empty — an effect with no parameters still binds binding 5.
[[nodiscard]] u64 at_least_one(u64 count, u64 stride) noexcept {
    return (count == 0 ? 1 : count) * stride;
}

/// The particle-count cap this sub-step runs under. The same rule `world.cpp`'s `capped` applies,
/// and it is written twice because the CPU path's copy may not name a device and this one may not
/// name the world — the two are compared by `test_vfx_gpu_pass.cpp` over the same inputs rather
/// than trusted to agree.
[[nodiscard]] u32 capped(u32 block_capacity, f32 count_cap_scale, u32 reserved) noexcept {
    const f32 scaled = static_cast<f32>(block_capacity) * count_cap_scale;
    const u32 cap = scaled > 0.0F ? static_cast<u32>(scaled) : 0U;
    return std::min(std::max(cap, reserved), block_capacity);
}

}  // namespace

// --- Creation
// -------------------------------------------------------------------------------------

VfxGpuPass::~VfxGpuPass() {
    destroy();
}

bool VfxGpuPass::supported(const rhi::Device& device) noexcept {
    // COMPUTE ONLY, and the absence of a second test is deliberate: indirect dispatch is core in
    // every API this engine targets and `rhi::Capability` has no enumerator for it. A host that
    // knows its device cannot do indirect dispatch declares
    // `DeviceCapability::indirect_dispatch` false and `decide_path` reports
    // `FallbackReason::DeviceLacksIndirectDispatch`; this class cannot discover it.
    return device.capabilities().has(rhi::Capability::ComputeShaders);
}

Status VfxGpuPass::create(Allocator& allocator, rhi::Device& device, const CompiledSystem& system,
                          const GpuPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "this VFX GPU pass has already been created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "a VFX GPU simulation needs Capability::ComputeShaders; `runtime.h`'s "
                    "FallbackReason::DeviceLacksCompute is what a caller reports instead");
    }
    if (desc.emitter >= system.emitters().size()) {
        return fail(ErrorCode::OutOfRange,
                    "GpuPassDescription::emitter is past the end of the compiled system");
    }

    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;

    const CompiledEmitter& emitter = system.emitters()[desc.emitter];
    block_capacity_ = emitter.capacity();
    if (block_capacity_ == 0) {
        return fail(
            ErrorCode::InvalidArgument,
            "the emitter's cooked capacity is zero; the GPU block is sized from it once and "
            "cannot grow on the device");
    }
    block_words_ = gpu_block_words(emitter.layout(), block_capacity_);
    parameter_words_ = static_cast<u32>(system.parameters().size()) * 4U;
    channel_count_ = std::min(static_cast<u32>(system.channels().size()), kGpuMaxEventChannels);
    has_position_ = false;
    if (const AttributeSlot* position = emitter.layout().find(Name::intern("position"));
        position != nullptr) {
        has_position_ = !position->elided && position->components >= 3;
    }

    // ASYNC IS CAPABILITY-GATED AND DISABLEABLE, which is the requirement in one line. The graph
    // does the rest: a pass on another queue becomes a separate submit with a timeline wait, and
    // this module emits no synchronisation of its own.
    queue_ = (desc.async_compute && device.has_queue(rhi::QueueKind::AsyncCompute))
                 ? rhi::QueueKind::AsyncCompute
                 : rhi::QueueKind::Graphics;

    if (Status compiled = compile_kernel(system, desc); !compiled) {
        destroy();
        return compiled;
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

Status VfxGpuPass::compile_kernel(const CompiledSystem& system,
                                  const GpuPassDescription& desc) noexcept {
    if (!desc.kernel_spirv.empty()) {
        // THE SHIPPING PATH. A cooked bundle supplies the module and no front end is involved,
        // which is what `shader-system` requires of a shipping build: it contains no Slang
        // compiler. Nothing in this tree cooks one yet — see this header's closing note.
        return create_pipelines(desc.kernel_spirv);
    }

    const CompiledEmitter& emitter = system.emitters()[desc.emitter];
    if (Status assembled =
            assemble_dispatch_unit(emitter, system.parameters(), system.channels(), source_);
        !assembled) {
        return assembled;
    }

#if CY_SHADER_SLANG
    // REGISTERED EXPLICITLY. The front end registers itself from a static initialiser, and a static
    // initialiser in an archive's object file is dropped by the linker when nothing else in that
    // file is referenced — which is exactly the case here, because everything this module calls is
    // in `cy::shader` rather than in `cy::shader-slang`. Without this line `create_compiler` falls
    // back to the SPIR-V passthrough, `compiles_source()` answers false and the GPU path reports a
    // missing front end on a build that has one.
    (void)shader::slang::register_slang_backend();
    shader::CompilerSelection selection;
    auto front_end = shader::create_compiler(*allocator_, shader::kSlangBackendName, selection);
    if (!front_end.has_value()) {
        return make_unexpected(front_end.error());
    }
    struct Handle {
        Allocator& allocator;
        shader::ShaderCompiler* compiler;
        ~Handle() { shader::destroy_compiler(allocator, compiler); }
    } handle{*allocator_, front_end.value()};

    if (!handle.compiler->compiles_source()) {
        return fail(ErrorCode::Unsupported,
                    "the selected shader front end cannot compile source, so the generated VFX "
                    "kernel cannot become a module; supply GpuPassDescription::kernel_spirv");
    }

    // `SourceOrigin::Generated` and a named generator, because `shader-system`'s "The boundary is
    // explicit" scenario asks a diagnostic to distinguish "the shader is broken" from "the
    // generator emitted broken Slang" — and this file is the second of those two when it goes
    // wrong.
    shader::SourceUnit unit;
    unit.module_name = Name::intern("vfx.kernel");
    unit.origin = shader::SourceOrigin::Generated;
    unit.generator = Name::intern("vfx-graph");
    unit.text = source_.span();

    shader::CompileRequest request;
    request.source = unit;
    request.entry_point = Name::intern(kVfxKernelEntryPoint);
    request.stage = rhi::ShaderStage::Compute;

    shader::DiagnosticLog diagnostics(*allocator_);
    auto compiled = handle.compiler->compile(request, diagnostics);
    if (!compiled.has_value()) {
        // The generated source stays in `source_` on failure, deliberately: `generated_source()` is
        // what an author or an editor is shown beside the diagnostic, and a compiler that threw the
        // text away would report a line number for a file nobody has.
        return make_unexpected(compiled.error());
    }
    return create_pipelines(compiled.value().spirv());
#else
    return fail(ErrorCode::Unsupported,
                "this build has no Slang front end (CY_SHADER_SLANG is off), so the generated VFX "
                "kernel cannot be compiled here; a Profile or Shipping build must be given the "
                "cooked module through GpuPassDescription::kernel_spirv");
#endif
}

Status VfxGpuPass::create_pipelines(Span<const u32> kernel_spirv) noexcept {
    // One set layout and one pipeline layout for all four pipelines, which is the whole point of
    // the fixed binding contract in <cy/vfx/gpu_layout.h>: a support dispatch reads what a kernel
    // wrote through the same descriptors, and there is no second set to keep in step.
    rhi::DescriptorBinding bindings[kGpuBindingCount] = {};
    for (u32 index = 0; index < kGpuBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "vfx set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kGpuBindingCount);
    auto layout = device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(GpuPushConstants)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "vfx layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    auto pipeline_layout_handle = device_->create_pipeline_layout(pipeline_layout);
    if (!pipeline_layout_handle.has_value()) {
        return make_unexpected(pipeline_layout_handle.error());
    }
    pipeline_layout_ = *pipeline_layout_handle;

    struct Program {
        const char* name;
        Span<const u32> spirv;
        rhi::ShaderModuleHandle* module;
        rhi::ComputePipelineHandle* pipeline;
    };
    const Program programs[] = {
        {"vfx kernel", kernel_spirv, &kernel_module_, &kernel_pipeline_},
        {"vfx reset", Span<const u32>(kVfxResetSpirv, sizeof(kVfxResetSpirv) / sizeof(u32)),
         &reset_module_, &reset_pipeline_},
        {"vfx compact", Span<const u32>(kVfxCompactSpirv, sizeof(kVfxCompactSpirv) / sizeof(u32)),
         &compact_module_, &compact_pipeline_},
        {"vfx sort", Span<const u32>(kVfxSortSpirv, sizeof(kVfxSortSpirv) / sizeof(u32)),
         &sort_module_, &sort_pipeline_},
    };
    for (const Program& program : programs) {
        rhi::ShaderModuleDescription module;
        module.name = program.name;
        module.stage = rhi::ShaderStage::Compute;
        // "main", not the entry name: slangc names a single-entry SPIR-V module's entry point
        // `main` whatever `-entry` said, and the name the RHI passes is the one in the module.
        module.entry_point = "main";
        module.spirv = program.spirv;
        auto created = device_->create_shader_module(module);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        *program.module = *created;

        rhi::ComputePipelineDescription pipeline;
        pipeline.name = program.name;
        pipeline.layout = pipeline_layout_;
        pipeline.shader = *created;
        auto pipeline_handle = device_->create_compute_pipeline(pipeline);
        if (!pipeline_handle.has_value()) {
            return make_unexpected(pipeline_handle.error());
        }
        *program.pipeline = *pipeline_handle;
    }
    return ok();
}

Status VfxGpuPass::create_buffers() noexcept {
    // WHICH MEMORY EACH BUFFER LIVES IN, AND WHY.
    //
    // The particle block, the liveness array, the two lists, the counters, the keys, the event ring
    // and the indirect arguments are ALL DeviceLocal, and that is the requirement rather than a
    // tuning choice: "particle state SHALL live in GPU buffers and REMAIN THERE". There is no
    // mapped pointer to any of them and `buffer_mapped_pointer` on one returns null.
    //
    // The PARAMETERS are Upload, and they are not per-particle data: four words an effect
    // parameter, rewritten when gameplay sets one. `vfx-system` bars the CPU from per-particle
    // traffic, not from an effect's own Intensity.
    //
    // The read-back copies exist only when the description asks for them, and the particle one is
    // a full copy of every attribute of every slot — the suite's, never a frame's.
    const auto storage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const u64 sort_words = gpu_round_up_pow2(block_capacity_);

    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        rhi::BufferHandle* out;
    };
    const Request requests[] = {
        {"vfx particles", at_least_one(block_words_, sizeof(u32)), storage,
         rhi::MemoryUse::DeviceLocal, &buffers_.particles},
        {"vfx alive", at_least_one(block_capacity_, sizeof(u32)), storage,
         rhi::MemoryUse::DeviceLocal, &buffers_.alive},
        // The index and key arrays are sized to the sort's power of two rather than to the block,
        // because the sort pads up to it and a pad that wrote past the end would be a corruption
        // the sort's own bound could not catch.
        {"vfx indices", at_least_one(sort_words, sizeof(u32)), storage, rhi::MemoryUse::DeviceLocal,
         &buffers_.indices},
        {"vfx counts", at_least_one(kGpuCountWordCount, sizeof(u32)), storage,
         rhi::MemoryUse::DeviceLocal, &buffers_.counts},
        {"vfx free", at_least_one(block_capacity_, sizeof(u32)), storage,
         rhi::MemoryUse::DeviceLocal, &buffers_.free},
        {"vfx parameters", at_least_one(parameter_words_, sizeof(u32)), rhi::BufferUsage::Storage,
         rhi::MemoryUse::Upload, &buffers_.parameters},
        {"vfx keys", at_least_one(sort_words, sizeof(u32)), storage, rhi::MemoryUse::DeviceLocal,
         &buffers_.keys},
        {"vfx events",
         at_least_one(static_cast<u64>(kGpuMaxEventChannels) * kGpuEventStride * kGpuEventWords,
                      sizeof(u32)),
         storage, rhi::MemoryUse::DeviceLocal, &buffers_.events},
        // INDIRECT as well as Storage: the compaction writes it as a storage buffer and the device
        // reads it as dispatch arguments, and a buffer created without the second usage would be
        // refused by validation at the `dispatch_indirect` rather than at creation.
        {"vfx args", kGpuArgBytes, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
         rhi::MemoryUse::DeviceLocal, &buffers_.args},
    };
    for (const Request& request : requests) {
        rhi::BufferDescription description;
        description.name = request.name;
        description.size = request.size;
        description.usage = request.usage;
        description.memory = request.memory;
        auto buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    if (!desc_.read_back) {
        return ok();
    }
    const Request host_requests[] = {
        {"vfx particles readback", at_least_one(block_words_, sizeof(u32)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.particles_readback},
        {"vfx alive readback", at_least_one(block_capacity_, sizeof(u32)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback, &buffers_.alive_readback},
        {"vfx indices readback", at_least_one(sort_words, sizeof(u32)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.indices_readback},
        {"vfx counts readback", at_least_one(kGpuCountWordCount, sizeof(u32)),
         rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback,
         &buffers_.counts_readback},
    };
    for (const Request& request : host_requests) {
        rhi::BufferDescription description;
        description.name = request.name;
        description.size = request.size;
        description.usage = request.usage;
        description.memory = request.memory;
        auto buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        *request.out = *buffer;
    }
    return ok();
}

Status VfxGpuPass::write_descriptors() noexcept {
    auto set = device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    // IN BINDING ORDER, and the order is `GpuBinding`'s. A row out of place here binds the free
    // list where the kernel expects the particle block, which compiles, runs and is wrong.
    const rhi::BufferHandle handles[kGpuBindingCount] = {
        buffers_.particles,  buffers_.alive, buffers_.indices, buffers_.counts, buffers_.free,
        buffers_.parameters, buffers_.keys,  buffers_.events,  buffers_.args,
    };
    rhi::DescriptorWrite writes[kGpuBindingCount] = {};
    for (u32 index = 0; index < kGpuBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = handles[index];
        writes[index].buffer_range = 0;  // the rest of the buffer
    }
    return device_->update_descriptor_set(
        descriptor_set_, Span<const rhi::DescriptorWrite>(writes, kGpuBindingCount));
}

void VfxGpuPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    const rhi::BufferHandle buffers[] = {
        buffers_.particles,
        buffers_.alive,
        buffers_.indices,
        buffers_.counts,
        buffers_.free,
        buffers_.parameters,
        buffers_.keys,
        buffers_.events,
        buffers_.args,
        buffers_.particles_readback,
        buffers_.alive_readback,
        buffers_.indices_readback,
        buffers_.counts_readback,
    };
    // NULL HANDLES ARE SKIPPED, and this is not defensive programming. `destroy()` runs on the
    // failure path of `create()` as well as from the destructor — a pass whose kernel would not
    // compile has no buffers at all — and the RHI validates a destroy of a never-issued handle as
    // an error, which would make a create failure look like a device defect in every log that
    // counted validation errors.
    for (rhi::BufferHandle handle : buffers) {
        if (handle) {
            device_->destroy_buffer(handle);
        }
    }
    buffers_ = Buffers{};
    const rhi::ComputePipelineHandle pipelines[] = {kernel_pipeline_, reset_pipeline_,
                                                    compact_pipeline_, sort_pipeline_};
    for (rhi::ComputePipelineHandle handle : pipelines) {
        if (handle) {
            device_->destroy_compute_pipeline(handle);
        }
    }
    if (pipeline_layout_) {
        device_->destroy_pipeline_layout(pipeline_layout_);
    }
    if (set_layout_) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    const rhi::ShaderModuleHandle modules[] = {kernel_module_, reset_module_, compact_module_,
                                               sort_module_};
    for (rhi::ShaderModuleHandle handle : modules) {
        if (handle) {
            device_->destroy_shader_module(handle);
        }
    }
    kernel_pipeline_ = {};
    reset_pipeline_ = {};
    compact_pipeline_ = {};
    sort_pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    kernel_module_ = {};
    reset_module_ = {};
    compact_module_ = {};
    sort_module_ = {};
    descriptor_set_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
    stepped_ = false;
}

// --- One sub-step
// ---------------------------------------------------------------------------------

Status VfxGpuPass::step(const GpuStepInputs& inputs) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "this VFX GPU pass has not been created");
    }
    if (inputs.parameters.size() > parameter_words_) {
        return fail(ErrorCode::OutOfRange,
                    "more parameter words than the compiled system declares; the packing is four "
                    "words a parameter in declaration order and it is the CPU path's packing too");
    }

    constants_ = GpuPushConstants{};
    constants_.dt = inputs.dt;
    constants_.emitter_age = inputs.emitter_age;
    constants_.block_capacity = block_capacity_;
    constants_.capacity =
        capped(block_capacity_, inputs.levers.count_cap_scale, inputs.reserved_particles);
    constants_.spawn_scale_fixed = gpu_fixed_from_float(inputs.levers.spawn_scale);

    // THE SORT IS THE CONTROLLER'S LEVER AND NOTHING ELSE READS IT. `sorted` false leaves `sort_n`
    // zero, `declare` puts neither sort pass in the graph, and `kGpuCountSortPasses` ends the step
    // at zero — so "the controller dropped the sort" is a number rather than an absence.
    report_.sort_refused = false;
    const u32 sort_n = gpu_round_up_pow2(block_capacity_);
    if (inputs.levers.sorted && sort_n > kGpuSortCapacity) {
        // REFUSED BY NAME rather than sorted partially, which is `GpuCullPass`'s rule for
        // `kGpuCullOcclusion`: a dispatch that quietly covered part of the population would report
        // "sorted" and be indistinguishable from one that sorted everything.
        report_.sort_refused = true;
    } else if (inputs.levers.sorted && has_position_) {
        constants_.sort_n = sort_n;
        constants_.sort_passes = gpu_sort_passes(sort_n);
    }

    if (!inputs.parameters.empty()) {
        auto* target = static_cast<u32*>(device_->buffer_mapped_pointer(buffers_.parameters));
        if (target == nullptr) {
            return fail(ErrorCode::Internal, "the VFX parameter buffer is not mapped");
        }
        std::memcpy(target, inputs.parameters.data(), inputs.parameters.size() * sizeof(f32));
    }
    stepped_ = true;
    return ok();
}

// --- Declaration
// ----------------------------------------------------------------------------------

void VfxGpuPass::record(const PassContext& context, void* user) noexcept {
    const auto* dispatch = static_cast<const Dispatch*>(user);
    VfxGpuPass* self = dispatch->pass;
    context.commands->bind_compute_pipeline(dispatch->pipeline);
    context.commands->bind_descriptor_sets(
        self->pipeline_layout_, 0, Span<const rhi::DescriptorSetHandle>(&self->descriptor_set_, 1));
    context.commands->push_constants(
        self->pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&dispatch->constants),
                       sizeof(GpuPushConstants)));
    if (dispatch->indirect) {
        // THE REQUIREMENT, IN ONE CALL. The group count is three words of device memory that
        // `vfx_compact` wrote; this process never read them and could not have.
        context.commands->dispatch_indirect(self->buffers_.args, dispatch->argument_offset);
        return;
    }
    context.commands->dispatch(dispatch->groups == 0 ? 1 : dispatch->groups, 1, 1);
}

void VfxGpuPass::record_readback(const PassContext& context, void* user) noexcept {
    auto* self = static_cast<VfxGpuPass*>(user);
    struct Copy {
        rhi::BufferHandle source;
        rhi::BufferHandle destination;
        u64 bytes = 0;
    };
    const Copy copies[] = {
        {self->buffers_.counts, self->buffers_.counts_readback,
         at_least_one(kGpuCountWordCount, sizeof(u32))},
        {self->buffers_.particles, self->buffers_.particles_readback,
         at_least_one(self->block_words_, sizeof(u32))},
        {self->buffers_.alive, self->buffers_.alive_readback,
         at_least_one(self->block_capacity_, sizeof(u32))},
        {self->buffers_.indices, self->buffers_.indices_readback,
         at_least_one(gpu_round_up_pow2(self->block_capacity_), sizeof(u32))},
    };
    for (const auto& copy : copies) {
        rhi::BufferCopy region{};
        region.size = copy.bytes;
        context.commands->copy_buffer(copy.source, copy.destination,
                                      Span<const rhi::BufferCopy>(&region, 1));
    }
}

VfxGpuPass::Resources VfxGpuPass::import_all(RenderGraph& graph) noexcept {
    // Every resource is imported rather than graph-owned: the buffers outlive the frame, the
    // descriptor set names them once, and a transient's handle changes every frame.
    const auto import = [&graph, this](const char* name, rhi::BufferHandle handle,
                                       rhi::BufferUsage extra) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = extra;
        return graph.import_buffer(request, handle);
    };
    const auto storage = rhi::BufferUsage::Storage;
    Resources resources;
    resources.particles = import("vfx particles", buffers_.particles, storage);
    resources.alive = import("vfx alive", buffers_.alive, storage);
    resources.indices = import("vfx indices", buffers_.indices, storage);
    resources.counts = import("vfx counts", buffers_.counts, storage);
    resources.free = import("vfx free", buffers_.free, storage);
    resources.parameters = import("vfx parameters", buffers_.parameters, storage);
    resources.keys = import("vfx keys", buffers_.keys, storage);
    resources.events = import("vfx events", buffers_.events, storage);
    resources.args = import("vfx args", buffers_.args, storage | rhi::BufferUsage::Indirect);
    return resources;
}

void VfxGpuPass::declare_dispatch(RenderGraph& graph, const Resources& resources, const char* name,
                                  u32 which) noexcept {
    using rhi::Access;
    Dispatch& dispatch = dispatches_[which];
    // EVERY BUFFER IS READ-MODIFY-WRITE ON EVERY PASS, and that is not laziness. A kernel loads an
    // attribute and stores it; the compaction reads the liveness array and writes it; a kill writes
    // liveness from inside the update. `use()` is the intent `graph.h` provides "for the
    // read-modify-write intents, where neither read nor write is the whole truth", and declaring a
    // read where a write happens is exactly the mistyped declaration that produces a
    // plausible-looking but wrong barrier.
    PassBuilder builder = graph.add_pass(name, queue_);
    builder.use(resources.particles, Access::ComputeStorageReadWrite)
        .use(resources.alive, Access::ComputeStorageReadWrite)
        .use(resources.indices, Access::ComputeStorageReadWrite)
        .use(resources.counts, Access::ComputeStorageReadWrite)
        .use(resources.free, Access::ComputeStorageReadWrite)
        .read(resources.parameters, Access::ComputeStorageRead)
        .use(resources.keys, Access::ComputeStorageReadWrite)
        .use(resources.events, Access::ComputeStorageReadWrite);
    if (dispatch.indirect) {
        // The one hazard an indirect dispatch adds over a direct one: the arguments were written by
        // a compute pass and are read by the command processor, and the two are different pipeline
        // stages. Declaring it is what makes the graph derive that barrier rather than this module
        // emitting one — which it could not, because the RHI exposes no barrier outside the graph.
        builder.read(resources.args, Access::IndirectCommandRead);
    } else {
        builder.use(resources.args, Access::ComputeStorageReadWrite);
    }
    builder.record(&VfxGpuPass::record, &dispatch);
}

Status VfxGpuPass::declare_reset(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "this VFX GPU pass has not been created");
    }
    const Resources resources = import_all(graph);
    dispatch_count_ = 0;
    Dispatch& reset = dispatches_[dispatch_count_];
    reset = Dispatch{};
    reset.pass = this;
    reset.pipeline = reset_pipeline_;
    reset.constants.block_capacity = block_capacity_;
    reset.groups = (block_capacity_ + kGpuGroupSize - 1U) / kGpuGroupSize;
    declare_dispatch(graph, resources, "vfx.reset", dispatch_count_);
    ++dispatch_count_;
    report_.dispatches = dispatch_count_;
    report_.indirect_dispatches = 0;
    report_.queue = queue_;
    return graph.status();
}

Status VfxGpuPass::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "this VFX GPU pass has not been created");
    }
    if (!stepped_) {
        return fail(ErrorCode::InvalidArgument,
                    "step() has not been called for this sub-step; a dispatch declared with no "
                    "constant block would run at a zero cap and report success");
    }
    const Resources resources = import_all(graph);
    dispatch_count_ = 0;
    report_.indirect_dispatches = 0;

    // The order is the CPU executor's order, and it is the reason the two agree: spawn asks, the
    // compaction answers and grants, the initialise takes free slots, the update advances the live.
    const auto push = [this, &graph, &resources](
                          const char* name, rhi::ComputePipelineHandle pipeline, GpuPass pass,
                          u32 groups, bool indirect, u64 argument_offset) noexcept {
        Dispatch& dispatch = dispatches_[dispatch_count_];
        dispatch = Dispatch{};
        dispatch.pass = this;
        dispatch.pipeline = pipeline;
        dispatch.constants = constants_;
        dispatch.constants.pass = pass;
        dispatch.groups = groups;
        dispatch.indirect = indirect;
        dispatch.argument_offset = argument_offset;
        declare_dispatch(graph, resources, name, dispatch_count_);
        ++dispatch_count_;
        report_.indirect_dispatches += indirect ? 1U : 0U;
    };

    push("vfx.spawn", kernel_pipeline_, kGpuPassSpawn, 1, false, 0);
    push("vfx.compact", compact_pipeline_, kGpuPassSpawn, 1, false, 0);
    push("vfx.initialise", kernel_pipeline_, kGpuPassInitialise, 0, true, kGpuArgInitialise);
    push("vfx.update", kernel_pipeline_, kGpuPassUpdate, 0, true, kGpuArgUpdate);
    if (constants_.sort_n != 0) {
        push("vfx.keys", kernel_pipeline_, kGpuPassKeys, 0, true, kGpuArgKeys);
        push("vfx.sort", sort_pipeline_, kGpuPassKeys, 1, false, 0);
    }
    report_.dispatches = dispatch_count_;
    report_.queue = queue_;

    if (!desc_.read_back) {
        return graph.status();
    }
    const auto import_host = [&graph, this](const char* name, rhi::BufferHandle handle) noexcept {
        BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device_->buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        return graph.import_buffer(request, handle);
    };
    const ResourceId counts_out = import_host("vfx counts readback", buffers_.counts_readback);
    const ResourceId particles_out =
        import_host("vfx particles readback", buffers_.particles_readback);
    const ResourceId alive_out = import_host("vfx alive readback", buffers_.alive_readback);
    const ResourceId indices_out = import_host("vfx indices readback", buffers_.indices_readback);

    // THE TRANSFER IS ON THE GRAPHICS QUEUE even when the simulation is on the async one, so the
    // graph derives the cross-queue wait rather than this module assuming submission order is
    // enough — which is the defect `graph.h`'s own header records its spike reproducing.
    graph.add_pass("vfx.readback", rhi::QueueKind::Graphics)
        .read(resources.counts, rhi::Access::TransferRead)
        .read(resources.particles, rhi::Access::TransferRead)
        .read(resources.alive, rhi::Access::TransferRead)
        .read(resources.indices, rhi::Access::TransferRead)
        .write(counts_out, rhi::Access::TransferWrite)
        .write(particles_out, rhi::Access::TransferWrite)
        .write(alive_out, rhi::Access::TransferWrite)
        .write(indices_out, rhi::Access::TransferWrite)
        .record(&VfxGpuPass::record_readback, this);
    // The host boundary is a dependency like any other: declaring it is what makes the graph emit
    // the transfer-to-host barrier, rather than this module relying on coherent memory and a fence.
    graph.add_pass("vfx.host read", rhi::QueueKind::Graphics)
        .read(counts_out, rhi::Access::HostRead)
        .read(particles_out, rhi::Access::HostRead)
        .read(alive_out, rhi::Access::HostRead)
        .read(indices_out, rhi::Access::HostRead)
        .side_effect();
    return graph.status();
}

// --- Read-back
// ------------------------------------------------------------------------------------

Status VfxGpuPass::read_back_counts() noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "this VFX GPU pass has not been created");
    }
    const auto* words =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.counts_readback));
    if (words == nullptr) {
        return fail(ErrorCode::Internal,
                    "the VFX counter read-back buffer is not mapped; a pass created without "
                    "GpuPassDescription::read_back has none, and nothing this class does depends "
                    "on these numbers");
    }
    report_.live = words[kGpuCountLive];
    report_.free = words[kGpuCountFree];
    report_.spawn_request = words[kGpuCountSpawnRequest];
    report_.spawn_granted = words[kGpuCountSpawnGranted];
    report_.spawned = words[kGpuCountSpawned];
    report_.killed = words[kGpuCountKilled];
    report_.reported_live = words[kGpuCountReportedLive];
    report_.sort_passes = words[kGpuCountSortPasses];
    for (u32 channel = 0; channel < kGpuMaxEventChannels; ++channel) {
        report_.events[channel] = words[kGpuCountEventsBase + channel];
    }
    return ok();
}

Expected<Span<const u32>, Error> VfxGpuPass::read_back_particles() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this VFX GPU pass has not been created"});
    }
    const auto* words =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.particles_readback));
    if (words == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal,
                  "the VFX particle read-back buffer is not mapped; a pass created without "
                  "GpuPassDescription::read_back has none, and a frame must never ask for one"});
    }
    return Span<const u32>(words, static_cast<usize>(block_words_));
}

Expected<Span<const u32>, Error> VfxGpuPass::read_back_alive() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this VFX GPU pass has not been created"});
    }
    const auto* words =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.alive_readback));
    if (words == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "the VFX liveness read-back buffer is not mapped"});
    }
    return Span<const u32>(words, block_capacity_);
}

Expected<Span<const u32>, Error> VfxGpuPass::read_back_indices() noexcept {
    if (device_ == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "this VFX GPU pass has not been created"});
    }
    const auto* words =
        static_cast<const u32*>(device_->buffer_mapped_pointer(buffers_.indices_readback));
    if (words == nullptr) {
        return make_unexpected(
            Error{ErrorCode::Internal, "the VFX index read-back buffer is not mapped"});
    }
    return Span<const u32>(words, gpu_round_up_pow2(block_capacity_));
}

}  // namespace cy::vfx::gpu
