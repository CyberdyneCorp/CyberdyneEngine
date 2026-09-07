#include "spike.h"

#include <cy/rendering/arbiter/subsystem.h>

#include <algorithm>
#include <cmath>

namespace cy::sample::fidelity {
namespace {

using rendering::ArbiterConfig;
using rendering::ArbiterReport;
using rendering::BudgetArbiter;
using rendering::BudgetSubsystem;
using rendering::kBudgetSubsystemCount;
using rendering::QualityLadder;
using rendering::SubsystemController;
using rendering::SubsystemDeclaration;

/// The scene's cost model. One row per subsystem: what it spends at authored quality, how much of
/// that follows pixel count, the order it gives quality up in, and what each rung of its own ladder
/// costs relative to the top one.
struct Row {
    BudgetSubsystem subsystem;
    f32 base_ms;
    f32 resolution_sensitivity;
    u32 reduction_order;
    f32 reserved_minimum_ms;
    u8 positions;
    f32 relative_cost[rendering::kMaxLadderPositions];
    /// The spike multiplies this subsystem's cost by the magnitude. The scripted event is a bank of
    /// dynamic lights coming on in a reflective interior, so it lands on the three systems that
    /// pay for lights and on nothing else.
    bool struck_by_spike;
};

// Nominal total 9.70 ms against an allocatable 12.70 ms — `design.md` §2.11's headroom, and the
// reason this model does not produce a limit cycle that would read as a control-law defect.
//
// THE SIX MODELLED ROWS ARE SMALL ENOUGH THAT THE MEASURED GEOMETRY COST FITS BESIDE THEM. They
// sum to 6.60 ms, so a device frame up to 5.08 ms is substituted for the geometry row and the
// nominal state still keeps its headroom. That ceiling is derived in `geometry_cost` rather than
// written here, but it is why these six numbers are what they are: an earlier draft summed to
// 7.90 ms, the measured frame on this machine landed within four MICROseconds of the ceiling, and
// `geometry_measured` flipped between runs — a figure that changes with the weather is a figure a
// driver cannot check.
constexpr Row kRows[kBudgetSubsystemCount] = {
    {BudgetSubsystem::Geometry, 3.10F, 1.00F, 5, 0.90F, 4, {1.00F, 0.82F, 0.66F, 0.52F}, false},
    {BudgetSubsystem::Shadows, 1.80F, 0.60F, 2, 0.50F, 4, {1.00F, 0.78F, 0.60F, 0.46F}, true},
    {BudgetSubsystem::GlobalIllumination, 1.50F, 0.35F, 1, 0.35F, 4,
     {1.00F, 0.70F, 0.50F, 0.34F}, true},
    {BudgetSubsystem::Reflections, 1.10F, 0.80F, 0, 0.24F, 4, {1.00F, 0.64F, 0.42F, 0.28F}, true},
    {BudgetSubsystem::MaterialEvaluation, 1.00F, 1.00F, 6, 0.42F, 3, {1.00F, 0.84F, 0.70F}, false},
    {BudgetSubsystem::Vfx, 0.60F, 0.90F, 3, 0.14F, 3, {1.00F, 0.66F, 0.40F}, false},
    {BudgetSubsystem::PostProcessing, 0.60F, 1.00F, 4, 0.20F, 3, {1.00F, 0.80F, 0.62F}, false},
};

/// The headroom the nominal state must keep inside the allocatable budget before the spike starts.
///
/// `design.md` §2.11's modelling trap, as a rule rather than as advice: the arbiter spike's first
/// model had a 17.1 ms baseline against a 13.9 ms budget and produced a 54-frame limit cycle that
/// looked like a control-law defect and was a CONTENT defect. So the device's measured frame is
/// substituted for the geometry subsystem's authored cost only when the nominal state that results
/// still fits with this much room; otherwise the model is used and BOTH numbers are reported, which
/// is what makes the substitution visible.
constexpr f32 kNominalHeadroom = 0.92F;

/// Deterministic per-frame noise, so a re-run of this artefact reproduces exactly. +/-2%, which is
/// inside the +/-3% `design.md` §2.7 says the filter is tuned for.
[[nodiscard]] f32 noise_at(u32 frame, u32 subsystem) noexcept {
    u32 state = (frame * 2654435761U) ^ ((subsystem + 1U) * 2246822519U);
    state ^= state >> 15U;
    state *= 2246822519U;
    state ^= state >> 13U;
    return 1.0F + (0.02F * ((static_cast<f32>(state & 0xFFFFU) / 32767.5F) - 1.0F));
}

/// The scripted event, as a multiplier on the subsystems it strikes. It ramps in over twenty
/// frames rather than stepping, because a renderer's load does: a light bank fades up.
[[nodiscard]] f32 spike_at(const SpikeOptions& options, u32 frame, f32 magnitude) noexcept {
    const u32 begins = options.settle_frames;
    const u32 ends = options.settle_frames + options.spike_frames;
    if (frame < begins || frame >= ends) return 1.0F;
    const f32 into = static_cast<f32>(frame - begins);
    const f32 ramp = std::min(1.0F, into / 20.0F);
    const f32 out_of = static_cast<f32>(ends - frame);
    const f32 fall = std::min(1.0F, out_of / 20.0F);
    return 1.0F + ((magnitude - 1.0F) * std::min(ramp, fall));
}

/// Frames of quiet after the arbiter's last adjustment before a frame over budget counts against
/// the loop.
///
/// THE WINDOW IS MEASURED, NOT CHOSEN, AND THAT IS THE SECOND TIME THIS FILE LEARNED IT. A fixed
/// window was tried twice — forty-eight frames, then a hundred and twenty-eight — and both are a
/// guess at how long absorbing a spike takes, which depends on how deep the ladders have to walk
/// and therefore on the magnitude and on the measured baseline. The second guess passed on one
/// machine and failed on the same machine when the device's own frame came out 0.19 ms slower.
///
/// So the question the artefact actually wants to ask is asked directly: HAS THE LOOP STOPPED
/// ACTING, AND IS THE FRAME STILL OVER BUDGET? A frame counts as late only when the arbiter has
/// made no adjustment for this many frames — four of its eight-frame periods — which is a loop
/// that has decided it is finished. While it is still walking a ladder down, a frame over budget
/// is the spike being absorbed, which is what the arbiter is for and not a failure of it.
constexpr u32 kQuietFrames = 32;

[[nodiscard]] SubsystemDeclaration declaration_for(const Row& row, f32 base_ms) noexcept {
    SubsystemDeclaration declaration;
    declaration.subsystem = row.subsystem;
    declaration.reduction_order = row.reduction_order;
    declaration.reserved_minimum_ms = row.reserved_minimum_ms;
    declaration.base_cost_ms = base_ms;
    declaration.resolution_sensitivity = row.resolution_sensitivity;
    declaration.ladder.positions = row.positions;
    for (u32 position = 0; position < rendering::kMaxLadderPositions; ++position) {
        declaration.ladder.relative_cost[position] =
            position < row.positions ? row.relative_cost[position]
                                     : row.relative_cost[row.positions - 1U];
    }
    return declaration;
}

/// The whole loop, once, for one spike magnitude. Everything about the artefact's claim on task
/// 11.3 happens in here, and the four lines the arbiter's own header prescribes are the four lines
/// in the middle of it.
class Loop {
public:
    [[nodiscard]] Status configure(const SpikeOptions& options, f32 geometry_ms) noexcept {
        ArbiterConfig config;
        config.frame_budget_ms = options.frame_budget_ms;
        if (Status configured = arbiter_.configure(config); !configured) return configured;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            const Row& row = kRows[index];
            base_ms_[index] =
                row.subsystem == BudgetSubsystem::Geometry ? geometry_ms : row.base_ms;
            const SubsystemDeclaration declaration = declaration_for(row, base_ms_[index]);
            if (Status declared = arbiter_.declare(declaration); !declared) return declared;
            if (Status declared = controllers_[index].declare(declaration); !declared) {
                return declared;
            }
            nominal_ms_ += base_ms_[index];
        }
        return ok();
    }

    [[nodiscard]] f32 nominal_ms() const noexcept { return nominal_ms_; }
    [[nodiscard]] f32 allocatable_ms() const noexcept { return arbiter_.allocatable_ms(); }

    void reset() noexcept {
        arbiter_.restore_authored_quality();
        quiet_frames_ = 0;
        for (SubsystemController& controller : controllers_) {
            controller.restore_authored_quality();
        }
        scale_ = 1.0F;
    }

    /// Frames since the arbiter last moved anything. `kQuietFrames` of it means settled.
    [[nodiscard]] u32 quiet_frames() const noexcept { return quiet_frames_; }

    /// One frame. Returns the modelled frame time and fills in what changed.
    f32 step(const SpikeOptions& options, u32 frame, f32 magnitude, u32& changes,
             u32& adjustments, u32& resolution_steps) noexcept {
        const f32 event = spike_at(options, frame, magnitude);
        const f32 pixels = scale_ * scale_;

        f32 measured[kBudgetSubsystemCount] = {};
        f32 frame_ms = arbiter_.config().non_allocatable_ms;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            const Row& row = kRows[index];
            const f32 ladder =
                controllers_[index].declaration().ladder.cost_at(controllers_[index].position());
            const f32 resolution =
                (1.0F - row.resolution_sensitivity) + (row.resolution_sensitivity * pixels);
            measured[index] = base_ms_[index] * ladder * resolution * noise_at(frame, index) *
                              (row.struck_by_spike ? event : 1.0F);
            frame_ms += measured[index];
        }

        // The four lines `arbiter.h`'s class comment prescribes, and nothing between them.
        arbiter_.report_frame_ms(frame_ms);
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            arbiter_.report_subsystem(kRows[index].subsystem, measured[index],
                                      controllers_[index].position(),
                                      controllers_[index].at_minimum());
        }
        const ArbiterReport report = arbiter_.update();
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            controllers_[index].set_allocation_ms(report.allocation_ms[index]);
            if (report.relax_granted[index]) controllers_[index].grant_relax_step();
            controllers_[index].report_measured_ms(measured[index]);
            const rendering::SubsystemUpdate update = controllers_[index].update();
            changes += (update.tightened || update.relaxed) ? 1U : 0U;
        }
        adjustments += report.adjustment_count;
        quiet_frames_ = report.adjustment_count > 0U ? 0U : quiet_frames_ + 1U;
        for (u32 index = 0; index < report.adjustment_count; ++index) {
            resolution_steps +=
                report.adjustments[index].subsystem == BudgetSubsystem::Count ? 1U : 0U;
        }
        scale_ = report.resolution_scale;
        filtered_ms_ = report.filtered_frame_ms;
        setpoint_ms_ = report.setpoint_ms;
        return frame_ms;
    }

    [[nodiscard]] bool restored() const noexcept {
        for (const SubsystemController& controller : controllers_) {
            if (controller.position() != 0U) return false;
        }
        return scale_ >= 1.0F;
    }

    [[nodiscard]] u32 positions() const noexcept {
        u32 total = 0;
        for (const SubsystemController& controller : controllers_) {
            total += controller.position();
        }
        return total;
    }

    [[nodiscard]] u32 at_minimum() const noexcept {
        u32 total = 0;
        for (const SubsystemController& controller : controllers_) {
            total += controller.at_minimum() ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] u32 below_reserved_minimum() const noexcept {
        u32 total = 0;
        for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
            total += arbiter_.allocation_ms(kRows[index].subsystem) <
                             kRows[index].reserved_minimum_ms - 1.0e-4F
                         ? 1U
                         : 0U;
        }
        return total;
    }

    [[nodiscard]] f32 resolution_scale() const noexcept { return scale_; }
    [[nodiscard]] f32 filtered_ms() const noexcept { return filtered_ms_; }
    [[nodiscard]] f32 setpoint_ms() const noexcept { return setpoint_ms_; }

private:
    BudgetArbiter arbiter_;
    SubsystemController controllers_[kBudgetSubsystemCount];
    f32 base_ms_[kBudgetSubsystemCount] = {};
    f32 nominal_ms_ = 0.0F;
    f32 scale_ = 1.0F;
    u32 quiet_frames_ = 0;
    f32 filtered_ms_ = 0.0F;
    f32 setpoint_ms_ = 0.0F;
};

[[nodiscard]] f32 median_of(Array<f32>& values) noexcept {
    if (values.size() == 0) return 0.0F;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

[[nodiscard]] f32 geometry_cost(const SpikeOptions& options, f32 allocatable_ms,
                                bool& measured) noexcept {
    f32 others_ms = 0.0F;
    for (const Row& row : kRows) {
        others_ms += row.subsystem == BudgetSubsystem::Geometry ? 0.0F : row.base_ms;
    }
    const f32 ceiling = (allocatable_ms * kNominalHeadroom) - others_ms;
    if (options.measured_geometry_ms > 0.0F && options.measured_geometry_ms <= ceiling) {
        measured = true;
        return options.measured_geometry_ms;
    }
    measured = false;
    return options.modelled_geometry_ms;
}

}  // namespace

Status run_spike(const SpikeOptions& options, SpikeReport& out) noexcept {
    // The allocatable budget is a property of the config, so it is read from a configured arbiter
    // rather than recomputed here — two subtractions of `non_allocatable_ms` would be two places to
    // change it.
    Loop probe;
    if (Status configured = probe.configure(options, options.modelled_geometry_ms); !configured) {
        return configured;
    }
    out.allocatable_ms = probe.allocatable_ms();
    out.budget_ms = options.frame_budget_ms;
    out.geometry_ms = geometry_cost(options, out.allocatable_ms, out.geometry_measured);
    out.device_ms = options.measured_geometry_ms;

    Loop loop;
    if (Status configured = loop.configure(options, out.geometry_ms); !configured) return configured;
    out.nominal_ms = loop.nominal_ms() + options.frame_budget_ms - out.allocatable_ms;
    out.subsystems = kBudgetSubsystemCount;

    const u32 total_frames = options.settle_frames + options.spike_frames + options.release_frames;
    const u32 steps = options.sweep_steps > 0U ? options.sweep_steps : 1U;
    Array<f32> every_frame(out.runs.allocator());
    if (Status sized = every_frame.reserve(static_cast<usize>(total_frames) * steps); !sized) {
        return sized;
    }
    if (Status sized = out.runs.reserve(steps); !sized) return sized;

    for (u32 step = 0; step < steps; ++step) {
        const f32 fraction =
            steps > 1U ? static_cast<f32>(step) / static_cast<f32>(steps - 1U) : 0.0F;
        SpikeRun run;
        run.magnitude = options.sweep_from + (fraction * (options.sweep_to - options.sweep_from));

        loop.reset();
        Array<f32> frames(out.runs.allocator());
        if (Status sized = frames.reserve(total_frames); !sized) return sized;
        u32 changes = 0;
        for (u32 frame = 0; frame < total_frames; ++frame) {
            u32 frame_changes = 0;
            const f32 frame_ms = loop.step(options, frame, run.magnitude, frame_changes,
                                           run.adjustments, run.resolution_steps);
            changes += frame_changes;
            // The last two hundred frames of the release phase: the loop has been settled at
            // authored load for at least a hundred and sixty frames by then.
            if (frame + 200U >= total_frames) run.settled_changes += frame_changes;
            run.deepest_positions = std::max(run.deepest_positions, loop.positions());
            if (frame_ms > options.frame_budget_ms) {
                ++run.frames_over_budget;
                if (loop.quiet_frames() >= kQuietFrames) ++run.late_frames_over_budget;
            }
            run.worst_ms = std::max(run.worst_ms, frame_ms);
            // The last frame at full magnitude, before the load starts to lift: where the loop
            // settled under the spike is the number the claim is about.
            if (frame + 1U == options.settle_frames + options.spike_frames - 20U) {
                run.settled_filtered_ms = loop.filtered_ms();
                run.settled_setpoint_ms = loop.setpoint_ms();
            }
            if (Status added = frames.push_back(frame_ms); !added) return added;
            if (Status added = every_frame.push_back(frame_ms); !added) return added;
        }
        (void)changes;
        run.median_ms = median_of(frames);
        run.restored = loop.restored();

        out.magnitudes_oscillating += run.settled_changes > 4U ? 1U : 0U;
        out.magnitudes_over_budget += run.settled_filtered_ms > options.frame_budget_ms ? 1U : 0U;
        out.settled_frames_over_budget += run.late_frames_over_budget;
        out.magnitudes_restored += run.restored ? 1U : 0U;
        out.frames += total_frames;
        if (Status added = out.runs.push_back(run); !added) return added;
    }
    out.magnitudes = steps;
    out.median_frame_ms = median_of(every_frame);
    return ok();
}

Status run_starvation(const SpikeOptions& options, StarvationReport& out) noexcept {
    Loop loop;
    bool measured = false;
    Loop probe;
    if (Status configured = probe.configure(options, options.modelled_geometry_ms); !configured) {
        return configured;
    }
    const f32 geometry_ms = geometry_cost(options, probe.allocatable_ms(), measured);
    if (Status configured = loop.configure(options, geometry_ms); !configured) return configured;

    // A load nothing can absorb: eight times the authored cost of the three subsystems the spike
    // strikes, held long enough for every ladder to bottom out and for resolution scale — the last
    // lever — to be reached.
    SpikeOptions starving = options;
    starving.settle_frames = 8;
    starving.spike_frames = 4000;
    starving.release_frames = 0;
    u32 changes = 0;
    u32 adjustments = 0;
    u32 resolution_steps = 0;
    for (u32 frame = 0; frame < 1200U; ++frame) {
        (void)loop.step(starving, frame + starving.settle_frames, 8.0F, changes, adjustments,
                        resolution_steps);
    }
    out.subsystems = kBudgetSubsystemCount;
    out.at_minimum = loop.at_minimum();
    out.below_reserved_minimum = loop.below_reserved_minimum();
    out.resolution_scale = loop.resolution_scale();

    // 450 more frames at the bottom of every ladder. `design.md` §2.2's own starvation case asserts
    // the loop moves ZERO times here: there is nothing left to move, and a loop that pulses anyway
    // is a loop whose deadband is not doing its job.
    out.changes_at_the_bottom = 0;
    for (u32 frame = 0; frame < 450U; ++frame) {
        (void)loop.step(starving, frame + 1200U + starving.settle_frames, 8.0F,
                        out.changes_at_the_bottom, adjustments, resolution_steps);
    }
    return ok();
}

}  // namespace cy::sample::fidelity
