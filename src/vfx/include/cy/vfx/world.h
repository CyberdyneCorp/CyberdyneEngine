#pragma once
// THE UNIFIED SIMULATION WORLD AND ITS GLOBAL SCHEDULER. M8.c task 2.4.
//
// ================================================================================================
// ONE WORLD, ONE POOL, FEW DISPATCHES
// ================================================================================================
//
// `vfx-system`: "All active emitters SHALL be simulated within a single VFX simulation world backed
// by SHARED PARTICLE MEMORY, rather than each effect instance owning isolated buffers and
// dispatches." So there is exactly one `ParticlePool` per world and an instance owns a BLOCK of it,
// never an allocation of its own.
//
// "A global scheduler SHALL, each simulation step: collect active emitters, group those sharing a
// compiled kernel and compatible bindings, and issue MERGED INDIRECT DISPATCHES over the grouped
// population." `StepReport::dispatches_unmerged` and `dispatches_merged` are the two numbers that
// requirement's own scenario asks for — "400 instances of the same explosion effect [...] SHALL be
// simulated by a small number of merged dispatches, not 400 separate ones" — and they are read off
// the grouping rather than predicted from it.
//
// ================================================================================================
// RESERVATIONS ARE WHAT STOP A DECORATIVE EFFECT STARVING A CRITICAL ONE
// ================================================================================================
//
// "Allocation from the shared pool SHALL be subject to per-importance-class reservations, so a
// high-volume decorative effect cannot starve a critical one", and "WHEN the shared pool is
// exhausted THEN spawn requests SHALL be REDUCED BY IMPORTANCE RANK and the shortfall reported,
// rather than overwriting live particles or failing the frame."
//
// The rule is one line and it is in `ParticlePool::acquire`: a class may take the free bytes MINUS
// whatever every other class still has unclaimed of its own reservation. A `Decorative` request
// therefore cannot touch `Critical`'s reservation even when the pool is otherwise empty, and a
// request that cannot be met in full is REDUCED and counted — never refused, because a frame that
// dropped an effect entirely is worse than a frame that drew a smaller one.
//
// ================================================================================================
// DECOUPLED SIMULATION FREQUENCY
// ================================================================================================
//
// "Effects SHALL simulate at a frequency selected independently of the render frame rate" and
// "Rendering SHALL interpolate particle attributes between simulation steps so a reduced rate is
// not visible as stepping." Each instance carries its own accumulator and its own hertz — chosen
// from its importance and its distance, and adjustable by the budget controller — and
// `EffectInstance::interpolation_alpha` is what the renderer interpolates with.
//
// "WHEN an effect's particles change discontinuously (spawn, kill, teleport) THEN interpolation
// SHALL be SUPPRESSED for those particles rather than smearing them." `ParticleSlot::discontinuous`
// is that suppression, set on spawn and on kill and cleared by the first step that does not move a
// particle discontinuously.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/vfx/budget.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/events.h>

namespace cy::vfx {

/// A block of the shared pool.
struct PoolBlock {
    u64 offset = 0;
    u64 bytes = 0;
    u32 particles = 0;
    ImportanceClass importance = ImportanceClass::Ambient;
};

struct PoolReport {
    u64 total_bytes = 0;
    u64 used_bytes = 0;
    u64 reserved_bytes[kImportanceCount] = {0, 0, 0, 0};
    u64 used_by_class[kImportanceCount] = {0, 0, 0, 0};
    /// Particles asked for and not granted, cumulative. "the shortfall reported".
    u32 shortfall_particles = 0;
    /// Acquisitions that were reduced rather than met in full.
    u32 reduced_requests = 0;
};

/// The one particle allocation in a world. Structure-of-arrays storage, addressed by the emitter's
/// derived `AttributeLayout` — there is no `struct Particle` here and there cannot be one.
class ParticlePool {
public:
    explicit ParticlePool(Allocator& allocator) noexcept : memory_(allocator) {}

    ParticlePool(const ParticlePool&) = delete;
    ParticlePool& operator=(const ParticlePool&) = delete;

    /// `reserved_fraction` is per importance class and sums to at most one; what is left over is
    /// the unreserved pool every class competes for.
    [[nodiscard]] Status initialize(u64 bytes,
                                    const f32 reserved_fraction[kImportanceCount]) noexcept;

    /// Ask for `wanted` particles. Returns a block of AT MOST that many: a request that cannot be
    /// met in full is reduced by importance rank and the shortfall counted.
    [[nodiscard]] Expected<PoolBlock, Error> acquire(ImportanceClass importance, u32 wanted,
                                                     u32 bytes_per_particle) noexcept;
    void release(const PoolBlock& block) noexcept;

    [[nodiscard]] Span<u8> bytes_at(const PoolBlock& block) noexcept;
    [[nodiscard]] Span<const u8> bytes_at(const PoolBlock& block) const noexcept;

    [[nodiscard]] const PoolReport& report() const noexcept { return report_; }
    [[nodiscard]] u64 free_bytes() const noexcept;
    /// What a class may still take, after every other class's unclaimed reservation is set aside.
    [[nodiscard]] u64 available_to(ImportanceClass importance) const noexcept;

private:
    Array<u8> memory_;
    /// A bump arena with a free list of released blocks. A pool whose blocks are all the same
    /// emitter's layout does not need a general allocator, and a general one here would be a second
    /// allocator to reason about in a frame budget.
    struct FreeBlock {
        u64 offset = 0;
        u64 bytes = 0;
    };
    Array<FreeBlock> free_list_{memory_.allocator()};
    u64 high_water_ = 0;
    PoolReport report_;
};

using EffectHandle = u32;
inline constexpr EffectHandle kInvalidEffect = 0;

/// What a caller asks for when it plays an effect.
struct EffectSpawn {
    /// World position. Particle positions are stored relative to it, which is what lets an effect
    /// follow an entity without gameplay updating a transform per frame.
    Vec3 position{0.0F, 0.0F, 0.0F};
    f32 scale = 1.0F;
    /// Parameter overrides, in the compiled system's declaration order. A shorter span leaves the
    /// remaining parameters at their authored values.
    Span<const f32> parameter_values;
    /// The simulation frequency the caller wants, before the budget controller has its say. Zero
    /// takes the importance class's default.
    f32 simulation_hz = 0.0F;
    /// FIRE AND FORGET. `vfx-system`: "A fire-and-forget spawn SHALL be available that requires no
    /// handle management." The instance releases itself when its last particle dies.
    bool release_on_completion = false;
};

/// One playing effect. One per system instance, not one per emitter: the emitters of a system share
/// its parameters and its lifetime.
struct EffectInstance {
    EffectHandle handle = kInvalidEffect;
    const CompiledSystem* system = nullptr;
    Vec3 position{0.0F, 0.0F, 0.0F};
    f32 scale = 1.0F;
    ImportanceClass importance = ImportanceClass::Ambient;
    /// One block per emitter of the system, in emitter order.
    u32 first_block = 0;
    u32 block_count = 0;
    /// Where this instance's parameter words start. Four floats a parameter, in the compiled
    /// system's declaration order — per instance, because "settable per effect instance" is what
    /// `vfx-system` asks of a parameter and a shared block would make one effect's Intensity every
    /// effect's.
    u32 parameter_base = 0;
    f32 simulation_hz = 60.0F;
    f32 accumulator = 0.0F;
    /// Where between the last two simulation steps the current frame sits, for the renderer's
    /// interpolation. Never used across a discontinuity — see the note at the top of this file.
    f32 interpolation_alpha = 0.0F;
    f32 age = 0.0F;
    u32 live_particles = 0;
    bool spawning = true;
    bool active = true;
    bool release_on_completion = false;
};

/// What one step did.
struct StepReport {
    u32 instances = 0;
    u32 active_emitters = 0;
    /// One per (instance, kernel) pair: what an unmerged scheduler would issue.
    u32 dispatches_unmerged = 0;
    /// One per group of emitters sharing a compiled kernel and compatible bindings.
    u32 dispatches_merged = 0;
    u32 live_particles = 0;
    u32 spawned = 0;
    u32 killed = 0;
    u32 particles_by_importance[kImportanceCount] = {0, 0, 0, 0};
    /// Simulation sub-steps run this frame, summed over instances. An 8 Hz effect at 120 FPS
    /// contributes a step every fifteenth frame, which is what "decoupled" means as a number.
    u32 substeps = 0;
    /// Emitters that ran on the GPU path and on the CPU path. A fallback is a NUMBER here, never a
    /// silence — see `runtime.h`.
    u32 gpu_emitters = 0;
    u32 cpu_emitters = 0;
    u32 cpu_fallbacks = 0;
};

struct WorldDescription {
    u64 pool_bytes = 8ULL * 1024ULL * 1024ULL;
    u32 max_instances = 256;
    /// Per importance class. `Critical` is reserved most heavily because it is the class the
    /// budget controller degrades last.
    f32 reserved_fraction[kImportanceCount] = {0.30F, 0.20F, 0.10F, 0.0F};
    /// The readback byte budget for the whole world.
    u64 readback_bytes_per_frame = 4096;
};

class SimulationWorld {
public:
    explicit SimulationWorld(Allocator& allocator) noexcept;

    SimulationWorld(const SimulationWorld&) = delete;
    SimulationWorld& operator=(const SimulationWorld&) = delete;

    [[nodiscard]] Status initialize(const WorldDescription& description) noexcept;
    void shutdown() noexcept;

    /// Play one system. The compiled system must outlive the instance: the world holds a pointer,
    /// not a copy, because a cooked effect is shared by every instance of it.
    [[nodiscard]] Expected<EffectHandle, Error> play(const CompiledSystem& system,
                                                     const EffectSpawn& spawn) noexcept;
    /// Stop. `allow_completion` ceases spawning and lets existing particles live out their
    /// lifetimes, which is `vfx-system`'s "Graceful stop"; false releases the block immediately.
    [[nodiscard]] Status stop(EffectHandle handle, bool allow_completion) noexcept;
    [[nodiscard]] Status set_parameter(EffectHandle handle, Name parameter,
                                       Span<const f32> value) noexcept;
    [[nodiscard]] Status set_transform(EffectHandle handle, const Vec3& position) noexcept;
    [[nodiscard]] const EffectInstance* find(EffectHandle handle) const noexcept;

    /// Advance every active instance by `dt` seconds at its own simulation frequency.
    [[nodiscard]] Status step(f32 dt, StepReport& report) noexcept;

    [[nodiscard]] Span<const EffectInstance> instances() const noexcept {
        return instances_.span();
    }
    [[nodiscard]] ParticlePool& pool() noexcept { return pool_; }
    [[nodiscard]] const ParticlePool& pool() const noexcept { return pool_; }
    [[nodiscard]] BudgetController& budget() noexcept { return budget_; }
    [[nodiscard]] const BudgetController& budget() const noexcept { return budget_; }
    [[nodiscard]] EventRouter& events() noexcept { return events_; }
    [[nodiscard]] ReadbackQueue& readback() noexcept { return readback_; }
    [[nodiscard]] const StepReport& last_step() const noexcept { return last_step_; }

    /// Read one live particle's attribute, in f32 regardless of the precision it is stored at. The
    /// renderer's publication and the statistical tests both go through this, so a quantised
    /// attribute reads back quantised — which is what makes the derived layout observable rather
    /// than reported.
    [[nodiscard]] f32 read_attribute(const EffectInstance& instance, u32 emitter, u32 particle,
                                     Name attribute, u32 component) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return instances_.allocator(); }

    // --- What `runtime.cpp` needs and nothing else does. Public because the executor is a free
    // function in the runtime target rather than a member: a simulation world that knew how to
    // evaluate a kernel would be a world that had to change when the IR did.

    /// The per-emitter blocks, parallel to the instance's `first_block .. first_block + count`.
    [[nodiscard]] Span<const PoolBlock> blocks() const noexcept { return blocks_.span(); }
    [[nodiscard]] Span<PoolBlock> blocks() noexcept { return blocks_.span(); }
    /// The alive flag of every particle of one emitter block.
    [[nodiscard]] Span<u8> alive_flags(u32 block) noexcept;
    /// The same, for a reader. `renderers.cpp`'s publications take the world by const reference —
    /// a publication that could mutate the simulation would be a publication that had to be
    /// ordered against it — and liveness is exactly what they must see.
    [[nodiscard]] Span<const u8> alive_flags(u32 block) const noexcept;

    /// The scheduler's grouping key: emitters sharing a compiled kernel and compatible bindings
    /// become one dispatch. Public because `world.cpp`'s grouping helper is a free function — a
    /// scheduler that had to be a member to see its own key would be a scheduler nothing else can
    /// test.
    struct DispatchGroup {
        u64 kernel_digest = 0;
        u64 layout_digest = 0;
        u32 members = 0;
    };

private:
    /// `groups` is the WORLD's, not the instance's: merging that only looked inside one instance
    /// would report four hundred dispatches for four hundred copies of one effect, which is the
    /// number the requirement exists to reduce.
    [[nodiscard]] Status simulate_instance(EffectInstance& instance, f32 dt, StepReport& report,
                                           Array<DispatchGroup>& groups) noexcept;
    /// One emitter of one instance, for one sub-step: the spawn count, the initialise (fused with
    /// the update where the compiler could fuse them) and the update over the live set.
    [[nodiscard]] Status simulate_emitter(EffectInstance& instance, u32 which,
                                          const BudgetLevers& levers, f32 substep,
                                          Array<f32>& registers, StepReport& report,
                                          Array<DispatchGroup>& groups, u32& live) noexcept;
    [[nodiscard]] Status release_instance(EffectInstance& instance) noexcept;
    /// One pool block per emitter. `reusing` replaces a retired instance's blocks in place rather
    /// than appending, which is what keeps the block, alive-flag and parameter arrays bounded.
    [[nodiscard]] Status acquire_blocks(const CompiledSystem& system, u32 first_block,
                                        bool reusing) noexcept;
    /// The authored parameter defaults, then the caller's overrides, into this instance's own
    /// words. Per instance because "settable per effect instance" is what `vfx-system` asks of a
    /// parameter, and a shared block would make one effect's Intensity every effect's.
    void write_parameters(const EffectInstance& instance, Span<const f32> overrides) noexcept;

    ParticlePool pool_;
    BudgetController budget_;
    EventRouter events_;
    ReadbackQueue readback_;
    Array<EffectInstance> instances_;
    Array<PoolBlock> blocks_;
    /// One alive byte per particle of every block, in block order. Outside the pool because it is
    /// not an attribute: `vfx-system`'s layout is derived from the graphs, and a liveness bit no
    /// graph mentions has no business inflating the per-particle byte size an author is shown.
    Array<u8> alive_;
    Array<u32> alive_offset_;
    Array<f32> parameters_;
    Array<u32> parameter_offset_;
    StepReport last_step_;
    WorldDescription description_;
    EffectHandle next_handle_ = 1;
    bool ready_ = false;
};

}  // namespace cy::vfx
