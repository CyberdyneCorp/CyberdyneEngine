// The VFX budget controller. M8.c task 2.6. See budget.h for every requirement this implements.

#include <cy/vfx/budget.h>

#include <algorithm>

namespace cy::vfx {
namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

[[nodiscard]] f32 mix(f32 from, f32 to, f32 t) noexcept {
    return from + ((to - from) * t);
}

/// A quality in [0, 1] mapped onto four discrete tiers, highest first. One function rather than a
/// nested conditional per lever, because three levers step the same way and a fourth would be a
/// fourth chance to get the thresholds subtly different.
[[nodiscard]] u8 tier_of(f32 quality, f32 high, f32 middle, f32 low) noexcept {
    if (quality > high) {
        return 3;
    }
    if (quality > middle) {
        return 2;
    }
    if (quality > low) {
        return 1;
    }
    return 0;
}

[[nodiscard]] f32 floor_for(ImportanceClass importance) noexcept {
    // `Critical` never reaches zero. Everything else may.
    return importance == ImportanceClass::Critical ? kCriticalFloor : 0.0F;
}

}  // namespace

u32 AppliedAdjustments::count() const noexcept {
    return (spawn_rate ? 1U : 0U) + (simulation_frequency ? 1U : 0U) + (count_cap ? 1U : 0U) +
           (collision ? 1U : 0U) + (feature_level ? 1U : 0U) + (lod ? 1U : 0U) +
           (sorting ? 1U : 0U);
}

BudgetController::BudgetController() noexcept {
    recompute_levers();
}

void BudgetController::set_allocation(f32 milliseconds) noexcept {
    state_.allocation_ms = milliseconds > 0.0F ? milliseconds : 0.001F;
}

void BudgetController::set_reserved_minimum(f32 milliseconds) noexcept {
    state_.reserved_minimum_ms = milliseconds > 0.0F ? milliseconds : 0.0F;
}

void BudgetController::set_pinned(bool pinned) noexcept {
    state_.pinned = pinned;
}

void BudgetController::measure(f32 vfx_milliseconds) noexcept {
    state_.measured_ms = vfx_milliseconds > 0.0F ? vfx_milliseconds : 0.0F;
}

void BudgetController::update(f32 dt) noexcept {
    applied_ = AppliedAdjustments{};
    const f32 ratio = state_.measured_ms / state_.allocation_ms;
    state_.over_budget = ratio > 1.0F;

    if (state_.pinned) {
        // PINNED. No adaptive adjustment; the excess is reported and nothing else happens.
        return;
    }

    const f32 step = kAdjustRate * (dt > 0.0F ? dt : 0.0F);
    if (ratio > kReduceThreshold) {
        // LEAST IMPORTANT FIRST. The enumerator order is the rank, so this walks it backwards.
        for (u32 which = kImportanceCount; which > 0; --which) {
            const auto importance = static_cast<ImportanceClass>(which - 1);
            f32& quality = state_.quality[which - 1];
            const f32 floor = floor_for(importance);
            if (quality <= floor) {
                continue;
            }
            quality = std::max(quality - step, floor);
            break;
        }
    } else if (ratio < kRestoreThreshold) {
        // MOST IMPORTANT FIRST on the way back up: what was degraded last is restored first.
        for (f32& quality : state_.quality) {
            if (quality >= 1.0F) {
                continue;
            }
            quality = std::min(quality + step, 1.0F);
            break;
        }
    }

    state_.at_reserved_minimum = true;
    state_.lowest_reduced = ImportanceClass::Count;
    for (u32 which = 0; which < kImportanceCount; ++which) {
        const auto importance = static_cast<ImportanceClass>(which);
        if (state_.quality[which] > floor_for(importance)) {
            state_.at_reserved_minimum = false;
        }
        if (state_.quality[which] < 1.0F && state_.lowest_reduced == ImportanceClass::Count) {
            state_.lowest_reduced = importance;
        }
    }
    recompute_levers();
}

void BudgetController::recompute_levers() noexcept {
    for (u32 which = 0; which < kImportanceCount; ++which) {
        const f32 quality = clamp01(state_.quality[which]);
        BudgetLevers& levers = levers_[which];
        const BudgetLevers before = levers;

        levers.spawn_scale = mix(0.15F, 1.0F, quality);
        levers.simulation_hz = mix(8.0F, 60.0F, quality);
        levers.count_cap_scale = mix(0.2F, 1.0F, quality);
        levers.collision_quality = tier_of(quality, 0.75F, 0.5F, 0.25F);
        levers.feature_level = tier_of(quality, 0.8F, 0.55F, 0.3F);
        levers.lod_bias = static_cast<u8>((1.0F - quality) * 3.0F);
        levers.sorted = quality > 0.4F;

        applied_.spawn_rate = applied_.spawn_rate || levers.spawn_scale != before.spawn_scale;
        applied_.simulation_frequency =
            applied_.simulation_frequency || levers.simulation_hz != before.simulation_hz;
        applied_.count_cap = applied_.count_cap || levers.count_cap_scale != before.count_cap_scale;
        applied_.collision =
            applied_.collision || levers.collision_quality != before.collision_quality;
        applied_.feature_level =
            applied_.feature_level || levers.feature_level != before.feature_level;
        applied_.lod = applied_.lod || levers.lod_bias != before.lod_bias;
        applied_.sorting = applied_.sorting || levers.sorted != before.sorted;
    }
}

const BudgetLevers& BudgetController::levers(ImportanceClass importance) const noexcept {
    const u32 index =
        static_cast<u32>(importance) < kImportanceCount ? static_cast<u32>(importance) : 0U;
    return levers_[index];
}

BudgetLevers BudgetController::levers_for(ImportanceClass importance,
                                          const ScalabilityPolicy& policy) const noexcept {
    BudgetLevers result = levers(importance);
    // THE EFFECT'S OWN FLOORS WIN. A controller that could reduce an effect below what its asset
    // declared would make `ScalabilityPolicy` advisory, and `vfx-system` declares it part of the
    // asset rather than part of the controller.
    result.spawn_scale = std::max(result.spawn_scale, policy.min_spawn_scale);
    result.simulation_hz = std::max(result.simulation_hz, policy.min_simulation_hz);
    if (!policy.may_drop_sorting) {
        result.sorted = true;
    }
    if (!policy.may_reduce_collision) {
        result.collision_quality = 3;
    }
    if (!policy.may_reduce_feature_level) {
        result.feature_level = 3;
    }
    return result;
}

void BudgetController::reset() noexcept {
    for (f32& quality : state_.quality) {
        quality = 1.0F;
    }
    state_.at_reserved_minimum = false;
    state_.lowest_reduced = ImportanceClass::Count;
    recompute_levers();
    applied_ = AppliedAdjustments{};
}

}  // namespace cy::vfx
