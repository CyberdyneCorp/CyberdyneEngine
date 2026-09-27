#pragma once
// GPU-FIRST SIMULATION, THE DECLARED CPU FALLBACK, AND THE SEAM TO THE RENDERER. M8.c tasks 2.3
// and 2.5.
//
// ================================================================================================
// THE FALLBACK IS DECLARED, WHICH MEANS IT IS A VALUE RATHER THAN A SILENCE
// ================================================================================================
//
// `design.md` §4 of this milestone, in as many words: "the CPU path is the half that must therefore
// be a DECLARED fallback that reports itself, never a silent one."
//
// So there is no boolean anywhere in this module that quietly means "we ended up on the CPU".
// `decide_path` returns a `PathDecision` carrying the path, the REASON, and an explanation string,
// and `StepReport::cpu_fallbacks` counts every emitter that wanted the GPU and did not get it. A
// caller that never looks still cannot lose the fact: the number is in the step report every frame.
//
// The two purposes are kept apart for the reason `vfx-system` insists on: "A CPU simulation path
// SHALL exist for two explicitly distinct purposes [...] The CPU path SHALL NOT be presented as
// merely a degraded GPU path, because its use cases differ." `FallbackReason::EffectRequiresCpu` is
// not a fallback at all and is not counted as one — it is an effect whose per-particle results
// gameplay reads, running where it asked to run.
//
// ================================================================================================
// WHAT RUNS TODAY, STATED PLAINLY
// ================================================================================================
//
// **The kernel executor in this header is the CPU path.** It steps through `VfxKernel::program()` —
// the flat, compiled form — and never sees a `cy::graph::Graph`, which is the whole of "the runtime
// SHALL contain no graph interpreter".
//
// **The GPU path is `cy::vfx-gpu`'s `VfxGpuPass`** and it exists since M10 task 5.1: the emitter's
// generated dispatch unit compiled to a module, particle state resident in device memory, and the
// update and the spawn dispatched indirectly from counts the device maintains. It is a SEPARATE
// OBJECT a host drives from its frame, not something `SimulationWorld::step` can reach — this world
// holds no device and must not, which is what keeps `integration.vfx` headless.
// `device_dispatch_available()` answers that honestly, `decide_path` reports
// `FallbackReason::NoDeviceInThisWorld` when it is the cause, and the report says so every frame
// rather than once in a document.
//
// ================================================================================================
// THE SEAM TO THE RENDERER IS THIRTY-TWO BYTES
// ================================================================================================
//
// `publish_sprites` fills `cy::rendering::particles::ParticleInstance` — a camera-relative
// position, a size and a linear radiance — and that is the entire interface between this module and
// anything that draws. This module creates no pipeline, declares no pass and holds no device
// handle; the renderer's own README says the same thing from its side. A simulation that had to
// know how a frame is assembled would be a second renderer, which is what this milestone exists to
// stop.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/vfx/events.h>
#include <cy/vfx/world.h>

namespace cy::vfx {

enum class ExecutionPath : u8 { Gpu = 0, Cpu };

/// Why an emitter is not on the GPU. `None` means it is.
enum class FallbackReason : u8 {
    None = 0,
    /// The effect declares `SimulationPath::CpuRequired`. NOT A FALLBACK — see the note above.
    EffectRequiresCpu,
    /// `vfx-system`'s "Device lacks compute" scenario.
    DeviceLacksCompute,
    /// Indirect dispatch is how population tracks the live count without a readback; without it the
    /// GPU path's central property is gone.
    DeviceLacksIndirectDispatch,
    /// The host turned the GPU path off — profiling, a capture, a bisection.
    DisabledByHost,
    /// THE HONEST ONE. The build HAS a compute dispatch — `cy::vfx-gpu`'s `VfxGpuPass` since M10 —
    /// and THIS SIMULATION WORLD has no device to run it on.
    ///
    /// `SimulationWorld` holds no device handle and never has: its own README's claim that "a
    /// simulation that had to know how a frame is assembled would be a second renderer" is exactly
    /// that property, and it is what lets every case in `integration.vfx` run headless. A host that
    /// wants the GPU path creates a `VfxGpuPass` per emitter and drives it from its frame; a host
    /// that steps a `SimulationWorld` is on the CPU path and this value is why.
    ///
    /// It replaced `DeviceDispatchUnimplemented`, which was true until M10 task 5.1 and is not any
    /// more — and a reason that had stayed behind after the thing it names was built would be the
    /// worst kind of report: a specific one that is wrong.
    NoDeviceInThisWorld,
};

[[nodiscard]] const char* fallback_reason_name(FallbackReason reason) noexcept;
/// A sentence an author reads in a diagnostic. Never null.
[[nodiscard]] const char* fallback_explanation(FallbackReason reason) noexcept;

/// What the device can do, as the host reports it. A plain value so the decision can be tested
/// without a device — which is the point: `vfx-system`'s "Device lacks compute" scenario has to be
/// checkable on a machine that has one.
struct DeviceCapability {
    bool compute = true;
    bool indirect_dispatch = true;
    bool async_compute = true;
    u32 max_gpu_particles = 1000000;
    /// The host's switch. `vfx-system` requires async execution to be "capability-gated and
    /// disableable"; this is the same idea one level up.
    bool gpu_path_enabled = true;
};

struct PathDecision {
    ExecutionPath path = ExecutionPath::Gpu;
    FallbackReason reason = FallbackReason::None;
    /// Never null.
    const char* explanation = "";
    /// True only when the emitter WANTED the GPU and did not get it. An effect that declared the
    /// CPU path is not a fallback and is not counted as one.
    bool is_fallback = false;
};

/// Whether a compute dispatch that could run a compiled VFX kernel exists in this build.
///
/// TRUE since M10 task 5.1: `cy::vfx-gpu`'s `VfxGpuPass` is it. What this does NOT say is that any
/// particular `SimulationWorld` will use one — a world holds no device, so an emitter stepped by
/// one runs on the CPU and reports `FallbackReason::NoDeviceInThisWorld`. The two facts are
/// separate and this function answers only the first.
[[nodiscard]] bool device_dispatch_available() noexcept;

[[nodiscard]] PathDecision decide_path(const CompiledEmitter& emitter,
                                       const DeviceCapability& capability) noexcept;

// --- The CPU path's executor ---------------------------------------------------------------------

/// The register file one kernel evaluation needs: four floats per slot. Reused across particles by
/// the caller, so a million-particle step allocates once.
using KernelRegisters = Array<f32>;

/// What a kernel evaluation is given.
struct KernelContext {
    const AttributeLayout* layout = nullptr;
    Span<u8> storage;
    /// The system's parameters, packed four floats each in declaration order.
    Span<const f32> parameters;
    Span<const ParameterDecl> parameter_decls;
    f32 dt = 1.0F / 60.0F;
    f32 emitter_age = 0.0F;
    f32 normalised_age = 0.0F;
    u32 spawn_index = 0;
    /// Where a kernel's event raises go. Null is legitimate — a cook-time evaluation has no world —
    /// and a raise then evaluates its predicate and puts nothing anywhere.
    EventRouter* events = nullptr;
};

/// What it decided.
struct KernelResult {
    bool killed = false;
    u32 spawn_count = 0;
    /// Raises whose predicate was non-zero. Not the number that SURVIVED: the channel's own bounds
    /// decide that, and `ChannelReport::dropped` is where the difference is.
    u32 events_raised = 0;
};

/// Evaluate one kernel for one particle and apply its writes.
///
/// NOT A GRAPH INTERPRETER. What it walks is `VfxKernel::program()`, the flat compiled form: an
/// operation, three operand slots and a destination, with no pointer to follow and no virtual call
/// to make. The authored graph is not reachable from here and there is no type in this header that
/// could name one.
[[nodiscard]] Status execute_kernel(const VfxKernel& kernel, const KernelContext& context,
                                    u32 particle, KernelRegisters& registers,
                                    KernelResult& out) noexcept;

// --- Publication to the renderer -----------------------------------------------------------------

struct PublishReport {
    u32 particles = 0;
    u32 emitters = 0;
    /// Particles the caller's capacity could not hold. Counted, never silently dropped: which tail
    /// is dropped is an importance decision and a number here is what makes it measurable.
    u32 dropped = 0;
};

/// Fill `out` with one record per live particle, camera-relative.
///
/// `capacity` is the renderer's ring capacity; a world with more live particles than that fills the
/// ring in importance order — `Critical` first — and counts the rest.
[[nodiscard]] Status publish_sprites(const SimulationWorld& world, const Vec3& camera_position,
                                     u32 capacity,
                                     Array<rendering::particles::ParticleInstance>& out,
                                     PublishReport& report) noexcept;

/// One mesh particle, as the renderer's GPU scene consumes it.
///
/// `vfx-system`: mesh particles publish "per particle at minimum: a transform, a mesh reference, a
/// material reference, and instance flags", and "SHALL NOT require ECS entities, per-particle CPU
/// submission, or CPU readback". There is no entity type in this header and no way to make one:
/// what this produces is rows, and the host appends them to the same instance buffer every other
/// source writes into, so GPU-driven culling and LOD selection apply with no VFX-specific logic.
struct MeshParticleInstance {
    /// Row-major 3x4, camera-relative — the same convention `InstanceTransform` uses, because a
    /// second convention here would be a second place to get the camera-relative rebase wrong.
    f32 rows[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    u32 mesh = 0;
    u32 material = 0;
    u32 flags = 0;
    /// The particle's projected radius, which is what the GPU scene's LOD selection reads. Supplied
    /// rather than computed here for the reason above: LOD is the scene's rule, not VFX's.
    f32 radius = 0.0F;
};

[[nodiscard]] Status publish_mesh_instances(const SimulationWorld& world,
                                            const Vec3& camera_position, u32 capacity,
                                            Array<MeshParticleInstance>& out,
                                            PublishReport& report) noexcept;

}  // namespace cy::vfx
