#include <cy/rendering/shadows/budget.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {
namespace {

[[nodiscard]] u8 last_position(const ShadowLeverLadder& ladder) noexcept {
    return ladder.positions == 0 ? 0U : static_cast<u8>(ladder.positions - 1U);
}

}  // namespace

const char* shadow_lever_name(ShadowLever lever) noexcept {
    switch (lever) {
        case ShadowLever::Refinement:
            return "Refinement";
        case ShadowLever::FilterQuality:
            return "FilterQuality";
        case ShadowLever::GeometryError:
            return "GeometryError";
        case ShadowLever::RefreshInterval:
            return "RefreshInterval";
        case ShadowLever::PageResolution:
            return "PageResolution";
        case ShadowLever::Count:
            break;
    }
    return "Unknown";
}

ShadowBudgetConfig default_shadow_budget_config() noexcept {
    ShadowBudgetConfig config;

    // Refinement first: it is an addition on top of the paged result, so removing it costs contact
    // detail and nothing else. "Refinement SHALL be reduced before paged shadow quality is."
    ShadowLeverLadder& refinement = config.levers[static_cast<usize>(ShadowLever::Refinement)];
    refinement.declared = true;
    refinement.positions = 3;
    refinement.relative_cost[0] = 1.0F;
    refinement.relative_cost[1] = 0.92F;
    refinement.relative_cost[2] = 0.86F;
    refinement.value[0] = 1.0F;  // full contact refinement
    refinement.value[1] = 0.5F;  // hero receivers only
    refinement.value[2] = 0.0F;  // none

    ShadowLeverLadder& filter = config.levers[static_cast<usize>(ShadowLever::FilterQuality)];
    filter.declared = true;
    filter.positions = 3;
    filter.relative_cost[0] = 1.0F;
    filter.relative_cost[1] = 0.90F;
    filter.relative_cost[2] = 0.82F;
    filter.value[0] = 16.0F;  // filter taps
    filter.value[1] = 8.0F;
    filter.value[2] = 4.0F;

    ShadowLeverLadder& error = config.levers[static_cast<usize>(ShadowLever::GeometryError)];
    error.declared = true;
    error.positions = 4;
    error.relative_cost[0] = 1.0F;
    error.relative_cost[1] = 0.86F;
    error.relative_cost[2] = 0.74F;
    error.relative_cost[3] = 0.66F;
    error.value[0] = 1.0F;  // shadow texels of allowed geometric error
    error.value[1] = 2.0F;
    error.value[2] = 4.0F;
    error.value[3] = 8.0F;

    ShadowLeverLadder& refresh = config.levers[static_cast<usize>(ShadowLever::RefreshInterval)];
    refresh.declared = true;
    refresh.positions = 3;
    refresh.relative_cost[0] = 1.0F;
    refresh.relative_cost[1] = 0.88F;
    refresh.relative_cost[2] = 0.78F;
    refresh.value[0] = 1.0F;  // multiplier on max_stale_frames
    refresh.value[1] = 2.0F;
    refresh.value[2] = 4.0F;

    // Resolution last, because it is the one a viewer reads as "the shadow went soft" rather than
    // as detail thinning. It is still not the renderer's resolution scale — that is the arbiter's
    // last lever of all, across every subsystem (`design.md` §2.2).
    ShadowLeverLadder& resolution = config.levers[static_cast<usize>(ShadowLever::PageResolution)];
    resolution.declared = true;
    resolution.positions = 3;
    resolution.relative_cost[0] = 1.0F;
    resolution.relative_cost[1] = 0.72F;
    resolution.relative_cost[2] = 0.55F;
    resolution.value[0] = 0.0F;  // level bias
    resolution.value[1] = 1.0F;
    resolution.value[2] = 2.0F;

    return config;
}

ShadowBudget::ShadowBudget() noexcept : config_(default_shadow_budget_config()) {}

void ShadowBudget::configure(const ShadowBudgetConfig& config) noexcept {
    config_ = config;
    restore_authored_quality();
}

void ShadowBudget::set_allocation_ms(f32 allocation_ms) noexcept {
    allocation_ms_ = math::max(allocation_ms, 0.0F);
}

void ShadowBudget::report_measured_ms(f32 measured_ms) noexcept {
    const f32 clamped = math::max(measured_ms, 0.0F);
    if (!has_measurement_) {
        filtered_ms_ = clamped;
        has_measurement_ = true;
        return;
    }
    const f32 alpha = math::clamp(config_.filter_alpha, 0.01F, 1.0F);
    filtered_ms_ = (alpha * clamped) + ((1.0F - alpha) * filtered_ms_);
}

void ShadowBudget::grant_relax_step() noexcept {
    relax_granted_ = true;
}

void ShadowBudget::set_pinned(bool pinned) noexcept {
    pinned_ = pinned;
}

u8 ShadowBudget::position(ShadowLever lever) const noexcept {
    const auto index = static_cast<usize>(lever);
    return index < kShadowLeverCount ? positions_[index] : 0U;
}

f32 ShadowBudget::value(ShadowLever lever) const noexcept {
    const auto index = static_cast<usize>(lever);
    if (index >= kShadowLeverCount) {
        return 0.0F;
    }
    const ShadowLeverLadder& ladder = config_.levers[index];
    return ladder.value[math::min(positions_[index], last_position(ladder))];
}

f32 ShadowBudget::predicted_ms() const noexcept {
    if (has_measurement_) {
        // The measurement IS the prediction at the current positions. `base_cost_ms` is only ever
        // the estimate before the first measurement arrives: a declared absolute cost would be
        // wrong the moment the scene changed, and the whole point of `relative_cost` is that the
        // subsystem declares RATIOS and measures the scale.
        return filtered_ms_;
    }
    f32 factor = 1.0F;
    for (usize index = 0; index < kShadowLeverCount; ++index) {
        const ShadowLeverLadder& ladder = config_.levers[index];
        if (!ladder.declared) {
            continue;
        }
        factor *= ladder.relative_cost[math::min(positions_[index], last_position(ladder))];
    }
    return config_.base_cost_ms * factor;
}

f32 ShadowBudget::predicted_at(ShadowLever lever, u8 candidate) const noexcept {
    const auto index = static_cast<usize>(lever);
    if (index >= kShadowLeverCount) {
        return predicted_ms();
    }
    const ShadowLeverLadder& ladder = config_.levers[index];
    const f32 current = ladder.relative_cost[math::min(positions_[index], last_position(ladder))];
    const f32 target = ladder.relative_cost[math::min(candidate, last_position(ladder))];
    if (current <= 0.0F) {
        return predicted_ms();
    }
    return predicted_ms() * (target / current);
}

bool ShadowBudget::at_minimum() const noexcept {
    for (usize index = 0; index < kShadowLeverCount; ++index) {
        const ShadowLeverLadder& ladder = config_.levers[index];
        if (ladder.declared && positions_[index] < last_position(ladder)) {
            return false;
        }
    }
    return true;
}

void ShadowBudget::restore_authored_quality() noexcept {
    for (u8& position : positions_) {
        position = 0;
    }
    relax_granted_ = false;
}

ShadowBudgetUpdate ShadowBudget::update() noexcept {
    ++frame_;
    ShadowBudgetUpdate result;
    result.filtered_ms = filtered_ms_;
    result.predicted_ms = predicted_ms();
    result.at_minimum = at_minimum();
    result.at_reserved_minimum = allocation_ms_ <= config_.reserved_minimum_ms;

    if (pinned_) {
        // Pinned mode is total: nothing moves, and the overrun is reported rather than corrected.
        result.pinned_overrun_ms = math::max(filtered_ms_ - allocation_ms_, 0.0F);
        return result;
    }
    if (allocation_ms_ <= 0.0F || !has_measurement_) {
        return result;
    }

    // TIGHTEN, on the subsystem's own authority and with no permission asked. The margin is the
    // spike's: it must exceed the noise that survives the filter, or the controller ratchets.
    if (filtered_ms_ > allocation_ms_ * config_.tighten_margin) {
        for (usize index = 0; index < kShadowLeverCount; ++index) {
            const ShadowLeverLadder& ladder = config_.levers[index];
            if (!ladder.declared || positions_[index] >= last_position(ladder)) {
                continue;
            }
            result.tightened = true;
            result.lever = static_cast<ShadowLever>(index);
            result.from_position = positions_[index];
            positions_[index] = static_cast<u8>(positions_[index] + 1U);
            result.to_position = positions_[index];
            result.predicted_ms = predicted_ms();
            result.at_minimum = at_minimum();
            return result;
        }
        // Nothing left. `at_minimum` already says so, and the arbiter reads it before it reaches
        // for the resolution scale it owns.
        return result;
    }

    // RELAX, only on the arbiter's grant, only after the dwell, and at most one step — reverse
    // declared order, so the last thing taken away is the first thing given back.
    if (!relax_granted_ || frame_ < last_relax_frame_ + config_.relax_dwell_frames) {
        return result;
    }
    for (usize reverse = kShadowLeverCount; reverse > 0; --reverse) {
        const usize index = reverse - 1;
        const ShadowLeverLadder& ladder = config_.levers[index];
        if (!ladder.declared || positions_[index] == 0) {
            continue;
        }
        const u8 candidate = static_cast<u8>(positions_[index] - 1U);
        const f32 predicted = predicted_at(static_cast<ShadowLever>(index), candidate);
        if (predicted > allocation_ms_ * config_.relax_margin) {
            continue;
        }
        result.relaxed = true;
        result.lever = static_cast<ShadowLever>(index);
        result.from_position = positions_[index];
        positions_[index] = candidate;
        result.to_position = candidate;
        result.predicted_ms = predicted_ms();
        result.at_minimum = at_minimum();
        relax_granted_ = false;
        last_relax_frame_ = frame_;
        return result;
    }
    return result;
}

u8 apply_resolution_bias(const ShadowBudget& budget, u8 level, f32 importance,
                         u8 max_level) noexcept {
    if (importance >= budget.config().protected_importance) {
        // Hero receivers survive pressure. The scenario, as a branch rather than as a promise.
        return level;
    }
    const f32 bias = budget.value(ShadowLever::PageResolution);
    const u32 biased = static_cast<u32>(level) + static_cast<u32>(math::max(bias, 0.0F));
    return static_cast<u8>(math::min(biased, static_cast<u32>(max_level)));
}

u32 budgeted_stale_frames(const ShadowBudget& budget, UpdateClass update_class) noexcept {
    if (update_class == UpdateClass::Critical) {
        // Not negotiable at any lever position: "SHALL NOT be allowed to go stale".
        return 0;
    }
    const f32 multiplier = math::max(budget.value(ShadowLever::RefreshInterval), 1.0F);
    return static_cast<u32>(static_cast<f32>(max_stale_frames(update_class)) * multiplier);
}

}  // namespace cy::rendering
