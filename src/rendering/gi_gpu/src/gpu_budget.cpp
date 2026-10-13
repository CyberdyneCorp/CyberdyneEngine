// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_budget.h>

#include <cmath>

namespace cy::rendering::gi_gpu {

u8 arbiter_position(const gi::GiBudget& budget, const QualityLadder& ladder) noexcept {
    const f32 allocation = budget.settings().allocation_ms;
    if (allocation <= 0.0F) {
        return 0;
    }
    // The levers' shares sum to the allocation at authored quality, so this is the fraction of
    // authored cost the levers now predict.
    const f32 fraction = budget.predicted_cost_ms() / allocation;
    u8 nearest = 0;
    f32 best = 1.0e9F;
    for (u8 position = 0; position <= ladder.last_position(); ++position) {
        const f32 distance = std::abs(ladder.cost_at(position) - fraction);
        if (distance < best) {
            best = distance;
            nearest = position;
        }
    }
    return nearest;
}

u32 tracing_divisor(f32 scale) noexcept {
    return scale < 0.99F ? 2U : 1U;
}

f32 tracing_density(f32 scale) noexcept {
    return scale < 0.6F ? 0.5F : 1.0F;
}

void apply_levers(const gi::IlluminationSystem& system, gi::ScreenResolveSettings settings,
                  GpuScreenResolve& resolve) noexcept {
    const f32 scale = system.budget().value(gi::GiLever::TracingResolution);
    (void)resolve.set_divisor(tracing_divisor(scale));
    settings.ray_density = tracing_density(scale);
    resolve.set_settings(settings);
    resolve.set_denoiser(system.denoiser());
}

void GpuGiBudget::observe(const GpuGiCost& cost) noexcept {
    measured_ = cost.any();
    measured_ms_ = measured_ ? static_cast<f32>(cost.total_ms()) : 0.0F;
}

void GpuGiBudget::submit(BudgetArbiter& arbiter, const gi::GiBudget& budget) const noexcept {
    const SubsystemDeclaration* declared = arbiter.declaration(subsystem_);
    if (!measured_ || declared == nullptr) {
        return;
    }
    arbiter.report_subsystem(subsystem_, measured_ms_, arbiter_position(budget, declared->ladder),
                             budget.at_reserved_minimum(), CostSource::Measured);
}

void GpuGiBudget::apply(const ArbiterReport& report, gi::GiBudget& budget) const noexcept {
    const auto index = static_cast<u32>(subsystem_);
    budget.set_pinned(report.pinned);
    if (index >= kBudgetSubsystemCount || !report.registered[index]) {
        return;
    }
    budget.set_allocation_ms(report.allocation_ms[index]);
    if (report.relax_granted[index]) {
        budget.permit_relaxation();
    }
}

}  // namespace cy::rendering::gi_gpu
