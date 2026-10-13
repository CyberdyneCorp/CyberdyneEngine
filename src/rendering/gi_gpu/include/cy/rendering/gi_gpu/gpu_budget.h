// SPDX-License-Identifier: MIT
#pragma once
// The device GI's cost into the GI budget, the budget's levers onto the device, and the GI budget
// into the renderer's arbiter. Issue #35, stage 5.
//
// ================================================================================================
// THREE ARROWS, AND WHICH WAY EACH POINTS
// ================================================================================================
//
//   device -> GiBudget    `measured_ms`: the GPU time of this frame's GI work — the resolve's
//                         stages, the probe gather and the card shading where they ran on the
//                         device — timed with timestamps around the dispatches. It is what a frame
//                         hands `FrameContext::measured_gi_ms`, and `GiBudget::update` tightens on
//                         it in its declared reduction order. THE GI'S OWN COST, NEVER A FRAME TIME:
//                         budget.h's rule, kept.
//   GiBudget -> device    `apply_levers`: the two levers the device resolve reads.
//                         `TracingResolution` is a linear scale, so the pixels it pays for are its
//                         square — the resolve's grid and its ray density; `DenoiserQuality` is the
//                         shared denoiser's quality position, which `IlluminationSystem::update`
//                         sets from it and the device's cascade reads off the denoiser. The probe
//                         and card budgets reach the device through the system's own update,
//                         unchanged.
//   GiBudget <-> arbiter  `submit` reports the measured cost and the rung of the arbiter's declared
//                         GI ladder the levers add up to; `apply` hands back the allocation and,
//                         only when the arbiter granted it, the permission to relax one step. A
//                         controller tightens on its own authority and relaxes on the arbiter's —
//                         the M7 spike's finding, kept.

#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/subsystem.h>
#include <cy/rendering/gi/budget.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>

namespace cy::rendering::gi_gpu {

/// One frame's device GI cost, in milliseconds. A part that did not run on the device, or was not
/// timed, is zero and says so in `valid`.
struct GpuGiCost {
    f64 resolve_ms = 0.0;
    f64 gather_ms = 0.0;
    f64 cards_ms = 0.0;
    bool resolve_valid = false;
    bool gather_valid = false;
    bool cards_valid = false;

    [[nodiscard]] f64 total_ms() const noexcept { return resolve_ms + gather_ms + cards_ms; }
    [[nodiscard]] bool any() const noexcept {
        return resolve_valid || gather_valid || cards_valid;
    }
};

/// The rung of an arbiter ladder a GI budget's levers add up to: the one whose declared relative
/// cost is nearest the budget's predicted cost over its allocation.
[[nodiscard]] u8 arbiter_position(const gi::GiBudget& budget, const QualityLadder& ladder) noexcept;

/// `TracingResolution`'s value as the resolve's grid and ray density. The ladder's three positions
/// are the frame (1.0); a grid of every other pixel each way, a quarter of them resolved, traced
/// and denoised and the rest upsampled (0.71); and that grid with half its cells tracing a ray a
/// frame (0.5). MEASURED (`render.gi_gpu_resolve`): at 1080p the first costs four times the
/// second, and the rays are the smaller part of either — the cache query and the cascade run per
/// cell — which is why the grid, not the ray density, is the step the lever's first move takes.
[[nodiscard]] u32 tracing_divisor(f32 scale) noexcept;
[[nodiscard]] f32 tracing_density(f32 scale) noexcept;

/// The levers onto the device resolve: the grid and ray density `TracingResolution` asks for,
/// `settings` otherwise unchanged, and the system's denoiser, whose quality `DenoiserQuality` set.
void apply_levers(const gi::IlluminationSystem& system, gi::ScreenResolveSettings settings,
                  GpuScreenResolve& resolve) noexcept;

/// The GI budget's link to the renderer's arbiter.
class GpuGiBudget {
public:
    /// The subsystem the GI reports as. The arbiter's profile declares its ladder.
    explicit GpuGiBudget(BudgetSubsystem subsystem = BudgetSubsystem::GlobalIllumination) noexcept
        : subsystem_(subsystem) {}

    /// Record this frame's device cost. What `measured_ms` answers until the next one.
    void observe(const GpuGiCost& cost) noexcept;
    /// The last observed cost, for `FrameContext::measured_gi_ms`.
    [[nodiscard]] f32 measured_ms() const noexcept { return measured_ms_; }
    [[nodiscard]] bool measured() const noexcept { return measured_; }

    /// Report the measured cost, and where the levers stand, to `arbiter`. A frame that measured
    /// nothing reports nothing: a zero would tell the arbiter the GI was free.
    void submit(BudgetArbiter& arbiter, const gi::GiBudget& budget) const noexcept;

    /// The arbiter's answer: the allocation and the pinned state into `budget`, and the relax step
    /// it granted, which `budget` may spend on its next update and on no other.
    void apply(const ArbiterReport& report, gi::GiBudget& budget) const noexcept;

private:
    BudgetSubsystem subsystem_;
    f32 measured_ms_ = 0.0F;
    bool measured_ = false;
};

}  // namespace cy::rendering::gi_gpu
