#pragma once
// THE BINDING CONTRACT BETWEEN A GENERATED KERNEL AND THE HOST THAT DISPATCHES IT. M10 task 5.1.
//
// ================================================================================================
// WHY THIS FILE EXISTS AT ALL, AND WHY IT IS IN THE COMPILER TARGET
// ================================================================================================
//
// `vfx-system`: "Particle state SHALL live in GPU buffers and remain there: the CPU SHALL NOT be
// required to read or write per-particle data during normal operation", and "Simulation SHALL use
// INDIRECT DISPATCH driven by live particle counts MAINTAINED ON THE GPU, so dispatch size tracks
// actual population without CPU knowledge of it."
//
// Two pieces of code have to agree for that to be true: the Slang `emit_slang.cpp` generates, and
// the C++ in `src/vfx/gpu/` that creates the buffers and records the dispatches. They agree about
// nine binding numbers, one push-constant block, a table of counter words and the word offset of
// every attribute array. Every one of those is HERE and spelled once, because the failure mode of
// spelling them twice is a dispatch that reads the wrong words and reports success.
//
// It lives in `cy::vfx-compiler` rather than in the GPU target because the emitter needs it and the
// emitter may not name a device. Nothing in this header names one either: it is arithmetic and
// constants.
//
// ================================================================================================
// THE GPU BLOCK IS NOT THE CPU BLOCK, AND CONFLATING THEM IS THE BUG THIS COMMENT PREVENTS
// ================================================================================================
//
// `AttributeLayout::array_offset` is a BYTE offset into the CPU pool's block, where a 1-component
// `Unorm8` attribute costs one byte a particle. The generated Slang addresses 32-bit words, and a
// 1-component `Unorm8` attribute costs one WORD a particle there, because three quarters of that
// word belong to components the attribute does not have. So the two layouts have different strides
// and different offsets and the same VALUES, and `gpu_block_words` below computes the GPU one.
//
// The consequence is a rule for every test: the CPU path and the GPU path are compared BY VALUE —
// through `SimulationWorld::read_attribute` on one side and a read-back plus `load_component` on
// the other — and never by `memcmp` of the two blocks. A byte comparison would fail on a correct
// pair and pass on a pair that agreed about nothing but its float32 attributes.

#include <cy/core/base/types.h>
#include <cy/vfx/layout.h>

namespace cy::vfx {

/// The descriptor set every VFX compute dispatch binds — the generated kernels and the fixed
/// support dispatches alike, so one set layout serves both and a support dispatch can read what a
/// kernel wrote without a second set to keep in step.
///
/// FIXED, and fixed is the point: a set whose bindings depended on the effect would need reflection
/// to build, and a reflected layout that drifted from the generator would bind the wrong buffer to
/// a kernel that still compiled.
enum GpuBinding : u32 {
    /// The shared particle memory. Structure of arrays, addressed in words — see the note above.
    kGpuBindingParticles = 0,
    /// One word a particle: 0 dead, 1 live, 2 initialised by the fused kernel this step. The third
    /// value is the CPU path's own marker and it means the same thing here, so that a particle the
    /// fused kernel already advanced is not advanced twice on its first step.
    kGpuBindingAlive = 1,
    /// The compacted live list, ascending. What an indirect simulate dispatch indexes through.
    kGpuBindingIndices = 2,
    /// The scheduler's counters. GPU-maintained; see `GpuCountWord`.
    kGpuBindingCounts = 3,
    /// The compacted free list, ascending. What a spawn takes its slots from.
    kGpuBindingFree = 4,
    /// The instance's parameters, four words each in declaration order — the same packing
    /// `SimulationWorld::write_parameters` uses, so one array serves both paths.
    kGpuBindingParameters = 5,
    /// One sort key a live particle, parallel to `kGpuBindingIndices`.
    kGpuBindingKeys = 6,
    /// The event ring this emitter raises into. One region per channel; see `kGpuEventStride`.
    kGpuBindingEvents = 7,
    /// The indirect dispatch arguments a GPU pass wrote for the next one to be dispatched by.
    kGpuBindingArgs = 8,
    kGpuBindingCount = 9,
};

/// The counter words. Every one of them is written by a dispatch and read by a dispatch; the host
/// reads them only for the diagnostic `vfx-system` asks for, one frame late, and never to size a
/// dispatch.
enum GpuCountWord : u32 {
    /// Live particles, and the length of the compacted list at `kGpuBindingIndices`.
    kGpuCountLive = 0,
    /// Free slots, and the length of the list at `kGpuBindingFree`.
    kGpuCountFree = 1,
    /// What the Spawn stage's kernel asked for this step.
    kGpuCountSpawnRequest = 2,
    /// What it got: the request scaled by the budget controller's spawn lever and clamped to the
    /// free list. The clamp is on the GPU because the free count is.
    kGpuCountSpawnGranted = 3,
    /// Particles initialised this step.
    kGpuCountSpawned = 4,
    /// Particles killed this step, by a kernel's kill root or by the particle-count cap.
    kGpuCountKilled = 5,
    /// The population the sub-step ENDS with: the compaction sets it to `live + granted` and every
    /// kill decrements it, so it is not `kGpuCountLive` — that one was measured before the spawn
    /// and
    /// before the update had a chance to kill anything. It is the number `EffectInstance::
    /// live_particles` means on the CPU path, and the two are compared for equality.
    kGpuCountReportedLive = 6,
    /// Bitonic passes the sort ran, or zero when `BudgetLevers::sorted` was false. "the sort SHALL
    /// be a reportable cost".
    kGpuCountSortPasses = 7,
    /// The first of `kGpuMaxEventChannels` per-channel event counters.
    kGpuCountEventsBase = 8,
    kGpuCountWordCount = kGpuCountEventsBase + 8,
};

/// Channels one emitter may raise on from the GPU. Eight because the event ring is sized
/// `channels * kGpuEventStride` and a generous bound on a small record is cheaper than a resize.
inline constexpr u32 kGpuMaxEventChannels = 8;

/// Events one channel's region of the ring holds. The channel's own declared
/// `max_events_per_frame` is enforced in the generated append and may be lower; this is the
/// region's physical size and the append is bounded by BOTH, because a channel declared larger than
/// the ring must not write past it.
inline constexpr u32 kGpuEventStride = 256;

/// `CyVfxEvent` in the generated prelude: four words.
inline constexpr u32 kGpuEventWords = 4;

/// The thread-group size every VFX dispatch uses. One place, so a change to the generated
/// `[numthreads]` that is not reflected here covers the wrong population rather than a comment.
inline constexpr u32 kGpuGroupSize = 64;

/// The single workgroup the compaction runs in. Ascending order is what makes the GPU's live list
/// the same list the CPU path builds, and an atomic append would make it a lottery — the same
/// decision, for the same reason, as `gpu_cull.slang`'s `compact_draws`.
inline constexpr u32 kGpuCompactGroupSize = 64;

/// Which of the entry point's branches a dispatch is running. Pushed, not stored, because it
/// changes between two dispatches in one command buffer and a device-memory word cannot.
enum GpuPass : u32 {
    /// One thread. Evaluates the Spawn stage and leaves its answer in `kGpuCountSpawnRequest`.
    kGpuPassSpawn = 0,
    /// Indirect over `kGpuCountSpawnGranted`. Takes a slot from the free list, marks it alive and
    /// runs the Initialise kernel — which, when the compiler fused the stages, has already advanced
    /// the particle by this step and marks it `2` so the update does not advance it again.
    kGpuPassInitialise = 1,
    /// Indirect over `kGpuCountLive`. Runs the Update kernel over the compacted live list.
    kGpuPassUpdate = 2,
    /// Indirect over `kGpuCountLive`. Writes one sort key a live particle. Generated rather than
    /// fixed because the key is read out of the attribute layout, and the layout is the effect's.
    kGpuPassKeys = 3,
};

/// The push-constant block. ONE block for the generated kernels and the fixed support dispatches
/// alike, because one pipeline layout serves both and a pipeline layout carries one push range.
///
/// Its first five members are the `CyVfxInput` fields a generated kernel body already reads by
/// name, so a body compiles against it unchanged. `particle_index` and `spawn_index` are per-thread
/// and therefore NOT really pushed: the entry point overwrites them in its static copy after
/// reading the block. They are members so that the struct the generator emits and the struct the
/// host pushes are one struct, and the host leaves them zero.
struct GpuPushConstants {
    f32 dt = 1.0F / 60.0F;
    f32 emitter_age = 0.0F;
    f32 particle_index = 0.0F;
    f32 spawn_index = 0.0F;
    f32 normalised_age = 0.0F;
    /// A `GpuPass`. Pushed rather than stored because it changes between two dispatches in one
    /// command buffer and a word in device memory cannot.
    u32 pass = kGpuPassSpawn;
    /// The particle-count cap this step runs under — the budget controller's `count_cap_scale`
    /// applied to the block, floored by the effect's reservation. A slot at or above it is killed
    /// by the compaction, which is what the CPU path's `particle >= pass.capacity` branch does.
    u32 capacity = 0;
    /// The spawn lever, as a 16.16 fixed-point multiplier. Fixed point rather than a float because
    /// the clamp that consumes it runs in the compaction's integer arithmetic, and a float there
    /// would make the granted count depend on a rounding mode.
    u32 spawn_scale_fixed = 1U << 16;
    /// The physical block, which is what the compaction scans — as against `capacity`, which is
    /// what it kills above. Two numbers because the cap moves every frame and the block does not.
    u32 block_capacity = 0;
    /// The power of two the bitonic sort runs over, and zero when `BudgetLevers::sorted` is false.
    /// The array is padded to it with a maximal key, so the sort needs no knowledge of the live
    /// count — which is the property that keeps it off the CPU's books.
    u32 sort_n = 0;
    /// What that sort costs in compare-exchange stages. Written into `kGpuCountSortPasses` by the
    /// compaction so the cost is REPORTED rather than inferred: "the sort SHALL be a reportable
    /// cost".
    u32 sort_passes = 0;
    u32 reserved = 0;
};

static_assert(sizeof(GpuPushConstants) == 48,
              "the push block is pushed as bytes and its size is in the pipeline layout");

/// The largest population one sort dispatch orders. The bitonic sort is ONE workgroup over group-
/// shared memory — one graph pass rather than the log^2(n) passes a global bitonic sort needs — and
/// this is the size of that shared array. An emitter whose block exceeds it is REFUSED the sort and
/// says so in `GpuStepReport::sort_refused`, rather than being sorted partially.
inline constexpr u32 kGpuSortCapacity = 2048;

/// The workgroup the sort runs in.
inline constexpr u32 kGpuSortGroupSize = 256;

/// Byte offsets into the indirect-argument buffer. Three `uint`s each, which is what
/// `vkCmdDispatchIndirect` reads.
enum GpuArgOffset : u32 {
    kGpuArgUpdate = 0,
    kGpuArgInitialise = 12,
    kGpuArgKeys = 24,
    kGpuArgBytes = 36,
};

/// The number of compare-exchange stages a bitonic sort over `n` costs: log2(n) * (log2(n) + 1)
/// / 2. Reported, and the same arithmetic the shader's loop performs.
[[nodiscard]] u32 gpu_sort_passes(u32 n) noexcept;

/// The smallest power of two at least `value`, and at least one.
[[nodiscard]] u32 gpu_round_up_pow2(u32 value) noexcept;

/// 16.16 fixed point, saturating. The one spelling of the conversion.
[[nodiscard]] inline u32 gpu_fixed_from_float(f32 value) noexcept {
    if (!(value > 0.0F)) {
        return 0;
    }
    const f32 scaled = value * 65536.0F;
    return scaled >= 4.294e9F ? 0xFFFFFFFFU : static_cast<u32>(scaled);
}

/// How many 32-bit words one particle's value of `slot` occupies in the GPU block. The generated
/// accessors pack `32 / precision_bits` components to a word and index `particle * words`; this is
/// the same arithmetic and it is the reason the two blocks differ in size.
[[nodiscard]] u32 gpu_words_per_particle(const AttributeSlot& slot) noexcept;

/// The word offset of `slot`'s array within the GPU block, for a block of `capacity` particles.
/// Slots are laid out in the order `AttributeLayout::slots()` returns them, elided ones skipped —
/// the same traversal `emit_prelude` makes, because the two must produce the same numbers and the
/// cheapest way to guarantee that is for both to be this function.
[[nodiscard]] u32 gpu_array_base_words(const AttributeLayout& layout, const AttributeSlot& slot,
                                       u32 capacity) noexcept;

/// The whole GPU block, in words, for `capacity` particles.
[[nodiscard]] u64 gpu_block_words(const AttributeLayout& layout, u32 capacity) noexcept;

}  // namespace cy::vfx
