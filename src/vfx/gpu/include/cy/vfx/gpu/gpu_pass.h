#pragma once
// THE GPU COMPUTE DISPATCH M8.c DID NOT BUILD. M10 task 5.1 and 5.2.
//
// ================================================================================================
// WHAT WAS MISSING, IN THE MODULE'S OWN WORDS
// ================================================================================================
//
// `src/vfx/README.md`, under "What is honest about the GPU path", before this module existed:
//
//   > `vfx-system` makes GPU compute the default and this module's decision agrees: `decide_path`
//   > returns `ExecutionPath::Gpu` for every emitter that can have it. WHAT DOES NOT EXIST IN THIS
//   > TREE IS THE COMPUTE DISPATCH THAT WOULD RUN A COMPILED KERNEL, so
//   `device_dispatch_available()` > answers false and every GPU-preferred emitter falls back to the
//   CPU.
//
// This is that dispatch. It is the arrow from a cooked `CompiledEmitter` — whose kernels were
// compiled and whose Slang was generated and self-contained, and which nothing ran — to particle
// state that lives in device memory and is advanced by a compute program the device executed.
//
// ================================================================================================
// THE FOUR SENTENCES OF `vfx-system` THIS FILE IS ACCOUNTABLE FOR
// ================================================================================================
//
//   "Particle state SHALL live in GPU buffers and REMAIN THERE: the CPU SHALL NOT be required to
//    read or write per-particle data during normal operation."
//
// `Buffers::particles` is `MemoryUse::DeviceLocal` and there is no host pointer to it anywhere in
// this class. `read_back_particles()` exists and is OFF unless `GpuPassDescription::read_back` asks
// for it, and it is there so that the dispatch can be compared against the CPU executor BY BUFFER
// rather than by photograph — which is what `tests/test_vfx_gpu_pass.cpp` does. A frame never calls
// it.
//
//   "Simulation SHALL use INDIRECT DISPATCH driven by live particle counts maintained on the GPU,
//    so dispatch size tracks actual population without CPU knowledge of it."
//
// The update and the initialise are `dispatch_indirect` against `Buffers::args`, and the only thing
// that writes `Buffers::args` is `vfx_compact`, on the device. There is no path in this class from
// a counter to a `dispatch()` argument: `step()` takes no population, `declare()` computes no group
// count for those two passes, and the counter read-back is a diagnostic that nothing branches on.
//
//   "Where the device exposes an ASYNCHRONOUS COMPUTE QUEUE, VFX simulation SHALL be schedulable on
//    it [...] Async execution SHALL be capability-gated and SHALL be disableable."
//
// `queue()` answers `AsyncCompute` when the device has one and `GpuPassDescription::async_compute`
// asks for it, and `Graphics` otherwise. The render graph inserts the cross-queue semaphore; this
// module emits no synchronisation of its own, which is `SkinPass`'s rule and for the same reason.
//
//   "Distance sorting SHALL be performed on the GPU over the particle population, with the sort
//    cost budgeted and REDUCIBLE by the budget controller."
//
// `GpuStepInputs::levers` is the controller's own `BudgetLevers`, and `sorted` false does not
// declare the two sort passes at all. `kGpuCountSortPasses` is then zero, so what the controller
// did is a number in the counter block rather than an absence nobody can see.
//
// ================================================================================================
// WHERE THE PROGRAM COMES FROM, AND THE ONE THING THAT IS NOT IN THIS TREE
// ================================================================================================
//
// The three SCHEDULER dispatches are checked-in SPIR-V (`shaders/vfx_support.slang`), because they
// are the same for every effect. The SIMULATION kernel is not: it is generated per effect by
// `assemble_dispatch_unit`, and `create()` compiles it through `cy::shader`'s Slang front end.
//
// That front end exists only where `CY_SHADER_SLANG` is on, which is not a Profile or Shipping
// build — `shader-system` requires a shipping build to contain no Slang compiler. So `create()`
// also accepts PRE-COOKED SPIR-V through `GpuPassDescription::kernel_spirv`, and fails naming the
// missing front end when it is given neither. The cook step that would fill a bundle with that
// SPIR-V is `asset-import-pipeline`'s and is NOT in this tree: a shipping build can run this pass
// only once something cooks for it, and `create()` says so in the error rather than a document.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/graph.h>
#include <cy/vfx/budget.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/gpu_layout.h>

namespace cy::vfx::gpu {

// The render graph's own names, unqualified. `src/rendering/skinning/` gets these for free because
// it lives in `cy::rendering`; this module does not, and a file that spelled `rendering::` on every
// resource would read as though the two were further apart than they are.
using rendering::BufferRequest;
using rendering::kInvalidResource;
using rendering::PassBuilder;
using rendering::PassContext;
using rendering::RenderGraph;
using rendering::ResourceId;

/// How one emitter's device residency is sized, and the two switches a host has over it.
struct GpuPassDescription {
    /// Which emitter of `system` this pass simulates. One pass an emitter, because one emitter is
    /// one kernel and one attribute layout.
    u32 emitter = 0;
    /// Whether the pass may be scheduled on the device's asynchronous compute queue. `vfx-system`
    /// requires async execution to be "capability-gated and disableable"; this is the disable, and
    /// `rhi::Device::has_queue` is the gate.
    bool async_compute = true;
    /// Whether the pass creates host-visible copies of the particle block and the counters and
    /// declares the transfers that fill them.
    ///
    /// The COUNTERS are copied whenever this is on and are the diagnostic `vfx-system` asks for —
    /// "live particle count by effect [...] spawn and kill rates, dispatch count". The PARTICLE
    /// BLOCK is copied too, and that one is for the suite: it is a full copy of every attribute of
    /// every slot and a frame must never ask for it.
    bool read_back = false;
    /// Pre-cooked SPIR-V for the generated kernel, for a build with no Slang front end. Empty means
    /// "compile the generated source", which needs one.
    Span<const u32> kernel_spirv;
};

/// What one GPU step reported, read out of the counter block ONE FRAME LATE.
///
/// Every member here is a diagnostic and none of them is an input to anything: `vfx-system` allows
/// a read-back that "SHALL never stall the frame waiting on the GPU", and the way this class
/// honours that is that nothing it does depends on these numbers. A host that never called
/// `read_back_counts()` would simulate identically.
struct GpuStepReport {
    /// Particles the compaction listed for the update, before the spawn.
    u32 live = 0;
    /// Free slots the compaction listed.
    u32 free = 0;
    u32 spawn_request = 0;
    u32 spawn_granted = 0;
    u32 spawned = 0;
    u32 killed = 0;
    /// The population the step ended with: survivors, promotions and the spawn.
    u32 reported_live = 0;
    /// Compare-exchange stages the sort ran, and zero when the controller dropped it.
    u32 sort_passes = 0;
    u32 events[kGpuMaxEventChannels] = {};
    /// Dispatches the last `declare` put in the graph. The `vfx-system` diagnostic "dispatch count
    /// before and after merging" from this side: this pass does not merge, and a number that says
    /// so is better than one that is absent.
    u32 dispatches = 0;
    /// Dispatches whose group count came from device memory rather than from this process. Two of
    /// them, every step, and zero would mean the indirect path was not taken.
    u32 indirect_dispatches = 0;
    /// The queue `declare` put the passes on.
    rhi::QueueKind queue = rhi::QueueKind::Graphics;
    /// The sort was asked for and refused, because the block is larger than one workgroup's shared
    /// array. Reported rather than silently partial — see `kGpuSortCapacity`.
    bool sort_refused = false;
};

/// What a step needs from the caller, and it is deliberately not a population.
struct GpuStepInputs {
    /// The sub-step, in seconds. The DECOUPLED simulation frequency is the world's decision and
    /// this is its result, not its cause.
    f32 dt = 1.0F / 60.0F;
    f32 emitter_age = 0.0F;
    /// The budget controller's levers for this effect's importance class, already floored by the
    /// effect's own `ScalabilityPolicy`. `BudgetController::levers_for` is what produces one.
    BudgetLevers levers;
    /// Particles this emitter keeps whatever the controller decides.
    u32 reserved_particles = 0;
    /// The system's parameters, four floats each in declaration order — the same packing
    /// `SimulationWorld` gives the CPU executor, so one array drives both paths.
    Span<const f32> parameters;
};

/// One emitter's simulation, resident on the device.
///
/// The lifecycle is: `create` once, then per sub-step `step()` to push the constants and write the
/// parameters, `declare()` into the frame's graph, and execute the graph. Nothing between those
/// calls reads a particle.
class VfxGpuPass {
public:
    VfxGpuPass() = default;
    ~VfxGpuPass();

    VfxGpuPass(const VfxGpuPass&) = delete;
    VfxGpuPass& operator=(const VfxGpuPass&) = delete;
    VfxGpuPass(VfxGpuPass&&) = delete;
    VfxGpuPass& operator=(VfxGpuPass&&) = delete;

    /// Whether this device can run a VFX simulation at all.
    ///
    /// COMPUTE, and compute alone, because indirect dispatch has no capability bit: it is core in
    /// every API this engine targets and `rhi::Capability` therefore has no enumerator for it.
    /// `runtime.h`'s `FallbackReason::DeviceLacksIndirectDispatch` is a value a HOST reports from
    /// its own `DeviceCapability::indirect_dispatch` declaration, and is not something this class
    /// can discover — which is stated here rather than implied by an absent check.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Create the pipelines and the buffers for one emitter of one cooked system.
    ///
    /// Compiles the emitter's generated dispatch unit unless `desc.kernel_spirv` supplies it. Fails
    /// naming the capability when the device cannot, naming the front end when the build has no
    /// Slang compiler and no pre-cooked module, and carrying the generated source's own diagnostics
    /// when the generated program does not compile — because "the generator emitted broken Slang"
    /// and "the shader is broken" are different bug reports.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const CompiledSystem& system,
                                const GpuPassDescription& desc) noexcept;

    /// Push one sub-step's constants and write the instance parameters. Call before `declare`.
    ///
    /// TAKES NO POPULATION, and there is nowhere to put one. The live count, the free count and the
    /// granted spawn are produced by `vfx_compact` on the device and consumed by the indirect
    /// arguments it writes; this call could not size a dispatch if it wanted to.
    [[nodiscard]] Status step(const GpuStepInputs& inputs) noexcept;

    /// Declare the sub-step's passes into `graph`. Between `step` and the graph's execution.
    ///
    /// Four passes, or six with sorting: the Spawn stage's count, the compaction that maintains
    /// every counter and writes the indirect arguments, the indirect initialise, the indirect
    /// update, and — when `BudgetLevers::sorted` — the key pass and the sort.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    /// Zero the liveness array and the counters. Declared like any other pass, because the reset is
    /// a dispatch: the particle block is device-local and there is no host pointer to memset.
    ///
    /// Call it once after `create`, in a frame of its own, before the first `declare`.
    [[nodiscard]] Status declare_reset(RenderGraph& graph) noexcept;

    /// What the last executed step reported. Valid after the frame's fence has been waited on, and
    /// only on a pass created with `read_back`; otherwise every member is zero and
    /// `report().dispatches` still says what was declared.
    [[nodiscard]] const GpuStepReport& report() const noexcept { return report_; }

    /// Refresh `report()` from the read-back copy of the counter block. Must be called after the
    /// fence; never stalls, because it reads a buffer the transfer already filled.
    [[nodiscard]] Status read_back_counts() noexcept;

    /// The particle block as the device holds it, for a comparison against the CPU executor.
    ///
    /// WORDS, in the GPU layout — which is not the CPU pool's layout, so a caller reads it through
    /// `gpu_array_base_words` and `gpu_words_per_particle` and never by `memcmp`. See the note at
    /// the top of <cy/vfx/gpu_layout.h>.
    [[nodiscard]] Expected<Span<const u32>, Error> read_back_particles() noexcept;
    /// The liveness array, same conditions. One word a slot: 0 dead, 1 live, 2 promoted.
    [[nodiscard]] Expected<Span<const u32>, Error> read_back_alive() noexcept;
    /// The compacted live list, same conditions. `report().live` entries are meaningful.
    [[nodiscard]] Expected<Span<const u32>, Error> read_back_indices() noexcept;

    /// The queue the passes are declared on. `AsyncCompute` when the device has one and the
    /// description asked for it.
    [[nodiscard]] rhi::QueueKind queue() const noexcept { return queue_; }
    /// The block this pass was sized for: the emitter's own cooked capacity.
    [[nodiscard]] u32 block_capacity() const noexcept { return block_capacity_; }
    /// The generated Slang this pass compiled, for a diagnostic or an editor. Empty when the pass
    /// was created from pre-cooked SPIR-V.
    [[nodiscard]] Span<const char> generated_source() const noexcept { return source_.span(); }

    void destroy() noexcept;

private:
    struct Buffers {
        rhi::BufferHandle particles;
        rhi::BufferHandle alive;
        rhi::BufferHandle indices;
        rhi::BufferHandle counts;
        rhi::BufferHandle free;
        rhi::BufferHandle parameters;
        rhi::BufferHandle keys;
        rhi::BufferHandle events;
        rhi::BufferHandle args;
        rhi::BufferHandle particles_readback;
        rhi::BufferHandle alive_readback;
        rhi::BufferHandle indices_readback;
        rhi::BufferHandle counts_readback;
    };

    /// One graph resource per buffer, refreshed every `declare` because an imported resource's
    /// identifier is the graph's and a graph is one frame's.
    struct Resources {
        ResourceId particles = kInvalidResource;
        ResourceId alive = kInvalidResource;
        ResourceId indices = kInvalidResource;
        ResourceId counts = kInvalidResource;
        ResourceId free = kInvalidResource;
        ResourceId parameters = kInvalidResource;
        ResourceId keys = kInvalidResource;
        ResourceId events = kInvalidResource;
        ResourceId args = kInvalidResource;
    };

    /// What one recorded dispatch needs. A small value rather than a member, so the four passes'
    /// record functions are one function and the differences are data.
    struct Dispatch {
        VfxGpuPass* pass = nullptr;
        rhi::ComputePipelineHandle pipeline;
        GpuPushConstants constants;
        /// Groups, when the count is this process's to decide. Zero means indirect.
        u32 groups = 0;
        /// Byte offset into the argument buffer, when it is not.
        u64 argument_offset = 0;
        bool indirect = false;
    };

    [[nodiscard]] Status compile_kernel(const CompiledSystem& system,
                                        const GpuPassDescription& desc) noexcept;
    [[nodiscard]] Status create_pipelines(Span<const u32> kernel_spirv) noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;
    [[nodiscard]] Resources import_all(RenderGraph& graph) noexcept;
    /// Declare one dispatch. `reads_args` marks the pass that consumes the indirect arguments, so
    /// the graph derives the barrier between the compaction that wrote them and the dispatch that
    /// is fed by them — which is the one hazard an indirect dispatch adds over a direct one.
    void declare_dispatch(RenderGraph& graph, const Resources& resources, const char* name,
                          u32 which) noexcept;

    static void record(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuPassDescription desc_{};
    rhi::QueueKind queue_ = rhi::QueueKind::Graphics;

    u32 block_capacity_ = 0;
    u32 parameter_words_ = 0;
    u64 block_words_ = 0;
    u32 channel_count_ = 0;
    bool has_position_ = false;

    rhi::ShaderModuleHandle kernel_module_;
    rhi::ShaderModuleHandle reset_module_;
    rhi::ShaderModuleHandle compact_module_;
    rhi::ShaderModuleHandle sort_module_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle kernel_pipeline_;
    rhi::ComputePipelineHandle reset_pipeline_;
    rhi::ComputePipelineHandle compact_pipeline_;
    rhi::ComputePipelineHandle sort_pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;
    Buffers buffers_{};

    /// The dispatches this step declared, in declaration order. Fixed-size because the count is
    /// bounded by the pass list and an allocation a frame is exactly what this module must not do.
    static constexpr u32 kMaxDispatches = 7;
    Dispatch dispatches_[kMaxDispatches];
    u32 dispatch_count_ = 0;

    GpuPushConstants constants_;
    GpuStepReport report_;
    /// The generated Slang, kept after a compile FAILS as well as after it succeeds: it is what an
    /// author is shown beside the diagnostic, and a compiler that threw the text away would report
    /// a line number for a file nobody has.
    Array<char> source_{system_allocator(MemoryDomain::Renderer)};
    bool stepped_ = false;
};

}  // namespace cy::vfx::gpu
