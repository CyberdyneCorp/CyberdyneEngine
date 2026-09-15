// The sky's subsystem controller: the observation, the unit cost it learns, and the tier it moves.

#include <cy/rendering/sky/budget.h>

#include <cy/core/math/math.h>

#include <chrono>

namespace cy::rendering::sky {
namespace {

/// What one re-integrated sky-view row is charged, in samples.
///
/// The row IS a march — `IncrementalSkyView` re-integrates a row of the sky view table per update
/// and each entry of it is a transmittance lookup along a ray — so it is counted in the same
/// currency as everything else here rather than weighted against it. The number is the table's own
/// width at the medium quality the engine defaults to; a row is not a sample and pretending it were
/// one would be the declared price this class exists not to contain.
inline constexpr u64 kSamplesPerSkyViewRow = 64;

/// EMA on the learned nanoseconds-per-sample.
///
/// The arbiter's own `filter_alpha`, deliberately: this signal feeds the same loop at the same rate
/// and a second constant would be a second time constant nobody tuned.
inline constexpr f32 kUnitCostAlpha = 0.25F;

[[nodiscard]] u64 monotonic_nanoseconds() noexcept {
    // steady_clock, never system_clock: a duration measured against a clock the user can set
    // backwards is not a duration. The same choice `src/core/jobs/src/types.cpp` records.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace

u64 SkyWorkload::samples() const noexcept {
    return cloud_density_samples + cloud_light_samples + shadow_density_samples +
           (sky_view_rows * kSamplesPerSkyViewRow);
}

Status SkyBudget::declare(BudgetArbiter& arbiter, BudgetSubsystem subsystem,
                          u32 reduction_order) noexcept {
    SubsystemDeclaration declaration;
    declaration.subsystem = subsystem;
    declaration.reduction_order = reduction_order;
    declaration.ladder = sky_quality_ladder();
    // The estimate the loop runs on until the first observation. It is a declared number and it is
    // reported as one — `CostSource::Estimated` — for exactly as long as it is the answer.
    declaration.base_cost_ms = 2.0F;
    // The cloud march is a screen-space integration: its cost is per pixel almost entirely, and the
    // table lookups and the radiance map that are not are the rounding error `sky_quality_ladder()`
    // already says they are.
    declaration.resolution_sensitivity = 0.9F;
    declaration.reserved_minimum_ms = 0.1F;

    if (auto status = arbiter.declare(declaration); !status) {
        return status;
    }
    if (auto status = controller_.declare(declaration); !status) {
        return status;
    }
    subsystem_ = subsystem;
    stats_ = SkyBudgetStats{};
    began_nanoseconds_ = 0;
    timing_ = false;
    declared_ = true;
    return {};
}

void SkyBudget::begin_frame() noexcept {
    began_nanoseconds_ = monotonic_nanoseconds();
    timing_ = true;
}

void SkyBudget::end_frame(const SkyWorkload& work) noexcept {
    if (!timing_) {
        // Not an error and not silent: a frame that never started the clock did not observe
        // anything, and `unpriced` is where that is visible.
        ++stats_.unpriced;
        return;
    }
    const u64 now = monotonic_nanoseconds();
    timing_ = false;
    observe(work, now > began_nanoseconds_ ? now - began_nanoseconds_ : 0);
}

void SkyBudget::observe(const SkyWorkload& work, u64 elapsed_nanoseconds) noexcept {
    ++stats_.observations;
    const u64 samples = work.samples();
    stats_.last_samples = samples;
    stats_.last_elapsed_ms = static_cast<f32>(elapsed_nanoseconds) * 1.0e-6F;

    if (samples > 0 && elapsed_nanoseconds > 0) {
        const f32 unit = static_cast<f32>(elapsed_nanoseconds) / static_cast<f32>(samples);
        stats_.unit_cost_ns = stats_.unit_cost_ns > 0.0F
                                  ? stats_.unit_cost_ns + (kUnitCostAlpha * (unit - stats_.unit_cost_ns))
                                  : unit;
    }

    if (stats_.unit_cost_ns <= 0.0F) {
        // Nothing has been priced yet, so there is nothing to report that would be a measurement.
        // The controller keeps its declared estimate and says so; reporting a zero here would tell
        // the arbiter the sky was free, which is the single most expensive lie this class could
        // tell it.
        ++stats_.unpriced;
        return;
    }

    stats_.reported_ms = static_cast<f32>(samples) * stats_.unit_cost_ns * 1.0e-6F;
    controller_.report_measured_ms(stats_.reported_ms);
}

void SkyBudget::submit(BudgetArbiter& arbiter) const noexcept {
    arbiter.report_subsystem(subsystem_, controller_.filtered_ms(), controller_.position(),
                             controller_.at_minimum(), controller_.cost_source());
}

SubsystemUpdate SkyBudget::apply(const ArbiterReport& report) noexcept {
    const auto index = static_cast<u32>(subsystem_);
    controller_.set_pinned(report.pinned);
    if (index < kBudgetSubsystemCount) {
        controller_.set_allocation_ms(report.allocation_ms[index]);
        if (report.relax_granted[index]) {
            controller_.grant_relax_step();
        }
    }
    return controller_.update();
}

SkyQualityTier SkyBudget::tier() const noexcept {
    return tier_at_ladder_position(controller_.position());
}

}  // namespace cy::rendering::sky
