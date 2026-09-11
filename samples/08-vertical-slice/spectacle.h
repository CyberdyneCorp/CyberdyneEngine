#pragma once
// THE SPECTACLE HALF OF THE SLICE: particles and a cut, inside the game rather than beside it.
// M8.c tasks 5.1, 5.2 and 5.3.
//
// ================================================================================================
// WHY THIS IS ONE OBJECT AND WHY IT WRITES NOTHING THE SIMULATION READS
// ================================================================================================
//
// M8.c's proposal is explicit that the slice is EXTENDED rather than replaced, because "a second
// slice would prove these systems work beside a copy of the game rather than inside it". So this
// object attaches at the two seams `samples/08-vertical-slice/README.md` recorded at the end of
// M8.b and adds no third one:
//
//   * `ActivationPipeline::cues()` — every committed activation emits a cue, and `Slice::act()`
//     already counts them. `on_cue()` plays a compiled VFX system at the caster instead.
//   * the camera — the gameplay camera is a compiled `cy::graph::camera` rig program evaluated in
//     `Slice::publish()`. The CUT does not touch it: it drives `cy::camera::CameraServer`'s own
//     stack through `CameraStackBridge`, and while the cinematic is live the view comes from
//     `evaluate_stack()`. `sequencing-and-cinematics`' "a sequence does not write camera
//     transforms" is therefore a fact about the link graph — this file contains no assignment to a
//     camera pose — and a MEASURED ZERO besides: `SpectacleReport::cut_camera_property_writes`
//     counts arbitrated writes addressed to `SubsystemId::Camera` and must stay at nothing.
//
// ================================================================================================
// AND WHY TASK 5.3 IS A PROPERTY OF THE WIRING RATHER THAN A TEST BESIDE IT
// ================================================================================================
//
// `Slice::digest_` folds character placements, the AI's per-tick state hash, the effect system's
// digest, the attribute store's digest and every activation identity. NOTHING BELOW IS IN THAT
// LIST and nothing below can reach it: `cy::vfx::SimulationWorld` writes only its own pool and its
// own event router, and the sequence player produces batches this file consumes. `--no-spectacle`
// is the control that turns the whole of this off, and act 3 of the driver requires the two runs
// to fold the SAME digest. If a later change gives VFX a way into gameplay state, the ECS write
// firewall (`cy/ecs/firewall.h`, `WriteOrigin::Vfx`) refuses it and this act notices.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

namespace cy::sample::slice {

using cy::Allocator;
using cy::Array;
using cy::Error;
using cy::Expected;
using cy::f32;
using cy::f64;
using cy::Span;
using cy::Status;
using cy::u32;
using cy::u64;
using cy::Vec3;

/// What the two new systems did over a run. Printed by `main.cpp`; checked by `slice.py`.
struct SpectacleReport {
    // --- The particles.
    /// Effects played from a committed activation's cue.
    u32 effects_played = 0;
    u32 effects_refused = 0;
    /// The last step's population, and its importance split. Reported on every frame including one
    /// that ran no sub-step — an 8 Hz effect at 60 FPS simulates on one frame in seven, and a
    /// number that said zero on the other six would be a function of which frame it was read on.
    u32 vfx_live_particles = 0;
    u32 vfx_peak_particles = 0;
    u32 vfx_by_importance[4] = {};
    u32 vfx_spawned = 0;
    u32 vfx_killed = 0;
    u32 vfx_substeps = 0;
    /// The scheduler's two numbers. `vfx-system` asks for merged indirect dispatches over the
    /// grouped population rather than one per instance, so a run with many copies of one effect
    /// must show the second well below the first.
    u32 vfx_dispatches_unmerged = 0;
    u32 vfx_dispatches_merged = 0;
    /// A FALLBACK IS A NUMBER, NEVER A SILENCE. `vfx-system` requires the CPU path to declare
    /// itself; this is where the slice reports it.
    u32 vfx_cpu_emitters = 0;
    u32 vfx_gpu_emitters = 0;
    u32 vfx_cpu_fallbacks = 0;
    /// What reached the renderer's ring, and what the ring could not hold.
    u32 vfx_published = 0;
    u32 vfx_dropped = 0;
    u64 vfx_pool_used_bytes = 0;
    u64 vfx_pool_total_bytes = 0;

    // --- The cut.
    bool cut_ran = false;
    /// Frames on which the camera stack produced this frame's view.
    u32 cut_frames = 0;
    /// Frames on which BOTH shots contributed. The blend, counted rather than asserted — the
    /// picture task 5.5 asks for is taken inside this window.
    u32 cut_blend_frames = 0;
    u32 cut_pushed = 0;
    u32 cut_released = 0;
    u32 cut_cuts = 0;
    u32 cut_anticipated_cuts = 0;
    u32 cut_unresolved_rigs = 0;
    /// THE ZERO TASK 5.2 IS ABOUT. Arbitrated property writes addressed at `SubsystemId::Camera`.
    /// A sequence that wrote a camera transform would have to write it as a property, and this
    /// would stop being zero.
    u32 cut_camera_property_writes = 0;
    /// Frames on which the camera server reported a pose override — a sequence forcing a pose past
    /// the rig. Also zero, and for the same reason.
    u32 cut_pose_overrides = 0;
    u32 cut_segments = 0;
    u32 cut_channels = 0;
    /// The two shots' weights on the frame the capture is taken on.
    f32 cut_wide_weight = 0.0F;
    f32 cut_tight_weight = 0.0F;
    /// The lens the stack produced on that frame, in radians. The two rigs declare different ones,
    /// so a blend that did nothing would read as one of the two ends.
    f32 cut_fov = 0.0F;
    /// THE MOST BALANCED FRAME OF THE BLEND — the one where the smaller of the two weights is
    /// largest — and the lens the stack produced on it. This is what the picture is taken at and
    /// what `slice.py` checks: a lens STRICTLY BETWEEN the two rigs' own is a blend that happened,
    /// and either end is one that did not.
    f32 cut_blend_wide = 0.0F;
    f32 cut_blend_tight = 0.0F;
    f32 cut_blend_fov = 0.0F;
    /// The two shots' authored lenses, printed so the driver compares the blend against the
    /// SOURCE rather than against numbers copied into a script.
    f32 cut_wide_fov = 0.0F;
    f32 cut_tight_fov = 0.0F;
};

/// What the frame is told about the view. Declared here rather than in `presentation.h` so that
/// this file does not depend on the presentation half.
struct CameraPose {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 target{0.0F, 0.0F, 0.0F};
    f32 vertical_fov = 1.0471975512F;
    /// True when the cut produced it. False means the gameplay rig did.
    bool from_cut = false;
};

/// The particles and the cinematic. Owns no ECS component, no entity and no camera transform.
class Spectacle {
public:
    explicit Spectacle(Allocator& allocator) noexcept;
    ~Spectacle();

    Spectacle(const Spectacle&) = delete;
    Spectacle& operator=(const Spectacle&) = delete;

    /// Cook the effect, author and compile the cut, and create the camera stack and its two rigs.
    /// `cut_start_tick` is the tick the cinematic begins on.
    [[nodiscard]] Status build(u32 cut_start_tick, u32 particle_ring) noexcept;

    /// One committed activation, at the caster. Fire and forget: the instance releases itself when
    /// its last particle dies, which is what `vfx-system` asks a gameplay cue to cost.
    [[nodiscard]] Status on_cue(const Vec3& at) noexcept;

    /// Advance the effect world and, while it is live, the cinematic. `gameplay` is the camera the
    /// compiled rig program produced; it is returned unchanged on every frame the cut is not
    /// driving, which is what makes the cut an ADDITION rather than a replacement.
    [[nodiscard]] Status step(u32 tick, f32 dt, const Vec3& subject, const CameraPose& gameplay,
                              CameraPose& out) noexcept;

    /// One record per live particle, camera-relative to `camera`. Filled by `step`; call
    /// `publish` after it with the eye the frame will actually use.
    [[nodiscard]] Status publish(const Vec3& camera) noexcept;
    [[nodiscard]] Span<const cy::rendering::particles::ParticleInstance> particles() const noexcept;

    [[nodiscard]] const SpectacleReport& report() const noexcept { return report_; }
    /// Microseconds the last `step` spent, split so a budget can name which half.
    [[nodiscard]] f64 particles_us() const noexcept { return particles_us_; }
    [[nodiscard]] f64 cinematic_us() const noexcept { return cinematic_us_; }

private:
    struct State;

    Allocator* allocator_ = nullptr;
    State* state_ = nullptr;
    SpectacleReport report_;
    f64 particles_us_ = 0.0;
    f64 cinematic_us_ = 0.0;
};

}  // namespace cy::sample::slice
