#pragma once
// samples/07-fidelity — the load spike, and the arbiter that has to absorb it. Task 11.3.
//
// ================================================================================================
// THE MODEL IS THE SCENE'S, THE LOOP IS THE SHIPPED ONE
// ================================================================================================
//
// `cy::rendering::BudgetArbiter` and `cy::rendering::SubsystemController` are the engine's, not
// this sample's: the sample declares seven subsystems, feeds the arbiter a frame time, and hands
// each controller its allocation. What the sample supplies is the COST MODEL — what each subsystem
// spends at its authored position, and what the spike does to that.
//
// The geometry subsystem's authored cost is the frame the device just measured, so one of the seven
// numbers is a measurement of this machine and the other six are the scene's. Where a number is
// modelled it is named as modelled, here and in the driver's report.
//
// ================================================================================================
// design.md §2.11's MODELLING TRAP, AND WHY THE NOMINAL STATE HAS HEADROOM
// ================================================================================================
//
// The arbiter spike's first model had a 17.1 ms baseline against a 13.9 ms budget and produced a
// 54-frame limit cycle that looked like a control-law defect and was a CONTENT defect: a scene that
// cannot fit its budget at authored quality gives the loop nothing to converge to. So the nominal
// state here is checked rather than assumed — `SpikeReport::nominal_ms` is the sum of the authored
// costs and the run asserts it is inside the allocatable budget before the spike starts.
//
// ================================================================================================
// ONE SPIKE IS NOT A CERTIFICATION, AND THIS RUN DOES NOT CLAIM TO BE ONE
// ================================================================================================
//
// design.md §2.1: a control law over a discrete ladder must be certified over a SWEEP of loads,
// because where the equilibrium lands relative to a ladder boundary is what decides whether it
// oscillates. That sweep is `integration.render_arbiter_sweep` — 71 step magnitudes — and it is the
// evidence for the control law. What this artefact shows is the other thing: that the loop holds a
// budget on a real scene with a real measured baseline, under a scripted spike a person can read.
// It runs a small sweep of spike magnitudes for the same reason, and reports how many of them
// oscillate rather than reporting one.

#include <cy/core/memory/array.h>
#include <cy/rendering/arbiter/arbiter.h>

namespace cy::sample::fidelity {

struct SpikeOptions {
    /// The frame budget. 13.90 ms is 72 Hz with a margin, which is `design.md` §2.2's model.
    f32 frame_budget_ms = 13.90F;
    /// What the geometry subsystem costs at authored quality. The device's measured frame, when
    /// there was a device; the modelled figure below when there was not.
    f32 measured_geometry_ms = 0.0F;
    f32 modelled_geometry_ms = 3.10F;
    /// Frames before the spike, during the spike, and after it.
    u32 settle_frames = 120;
    u32 spike_frames = 240;
    u32 release_frames = 360;
    /// The spike magnitudes swept, as a multiple of the nominal cost of the subsystems it hits.
    u32 sweep_steps = 9;
    f32 sweep_from = 1.8F;
    f32 sweep_to = 4.0F;
};

/// One spike magnitude's outcome.
struct SpikeRun {
    f32 magnitude = 0.0F;
    /// Frames whose modelled frame time exceeded the budget, and the worst overshoot.
    u32 frames_over_budget = 0;
    /// Of those, the ones the arbiter had already stopped acting on: it made no adjustment for
    /// four of its own periods and the frame was over budget anyway. Reported as a figure rather
    /// than asserted on — see `settled_filtered_ms`.
    u32 late_frames_over_budget = 0;
    /// THE FILTERED FRAME TIME WHERE THE LOOP SETTLED, at the end of the spike and before it lifts.
    ///
    /// This is what the arbiter controls and it is therefore what "the budget is held" can mean.
    /// The loop drives the FILTERED frame to the setpoint — `budget - deadband` — and an individual
    /// frame is that plus this scene's own +/-2% noise, so asserting that no single frame ever
    /// exceeds the budget is asserting something the control law does not claim and does not
    /// provide. Measured, and this is why the assertion moved: at a 3.62 ms measured geometry cost
    /// no frame at any magnitude exceeded the budget, and at 3.82 ms — the same machine, twenty
    /// minutes later — the deepest magnitude put a handful of noise excursions over it while the
    /// filtered frame sat comfortably below. A claim that flips on the weather is a claim about the
    /// weather.
    f32 settled_filtered_ms = 0.0F;
    f32 settled_setpoint_ms = 0.0F;
    f32 worst_ms = 0.0F;
    f32 median_ms = 0.0F;
    /// Lever changes over the last 200 frames of the release phase. A settled loop makes none.
    u32 settled_changes = 0;
    /// Adjustments the arbiter made, and how many were caused by resolution scale.
    u32 adjustments = 0;
    u32 resolution_steps = 0;
    /// Every subsystem back at authored quality by the end of the release phase.
    bool restored = false;
    /// The lowest position the loop had to reach, summed over subsystems.
    u32 deepest_positions = 0;
};

struct SpikeReport {
    explicit SpikeReport(Allocator& allocator) noexcept : runs(allocator) {}

    Array<SpikeRun> runs;
    f32 nominal_ms = 0.0F;
    f32 allocatable_ms = 0.0F;
    f32 budget_ms = 0.0F;
    /// Median over every frame of every magnitude — the figure the artefact leads with, because it
    /// is the one that reproduces.
    f32 median_frame_ms = 0.0F;
    u32 frames = 0;
    u32 magnitudes = 0;
    u32 magnitudes_oscillating = 0;
    u32 magnitudes_over_budget = 0;
    /// Frames over budget while the loop was settled, summed over every magnitude, and the total.
    u32 settled_frames_over_budget = 0;
    /// The geometry subsystem's authored cost came from the device rather than from the model.
    bool geometry_measured = false;
    /// What the authored cost actually is, and what the device measured — both, always, so that a
    /// substitution is visible rather than implied by one number changing.
    f32 geometry_ms = 0.0F;
    f32 device_ms = 0.0F;
    u32 magnitudes_restored = 0;
    /// A subsystem that could not be declared is a defect in this sample, not in the arbiter.
    u32 subsystems = 0;
};

[[nodiscard]] Status run_spike(const SpikeOptions& options, SpikeReport& out) noexcept;

/// The starvation case: every subsystem forced to its minimum at once. `delivery-roadmap`'s
/// "degrades rather than disappears" is a check on the allocations, not on a picture.
struct StarvationReport {
    u32 subsystems = 0;
    u32 at_minimum = 0;
    u32 below_reserved_minimum = 0;
    /// Lever changes over 450 frames at the bottom of every ladder. A loop that pulses there is a
    /// loop that never settles.
    u32 changes_at_the_bottom = 0;
    f32 resolution_scale = 1.0F;
};

[[nodiscard]] Status run_starvation(const SpikeOptions& options, StarvationReport& out) noexcept;

}  // namespace cy::sample::fidelity
