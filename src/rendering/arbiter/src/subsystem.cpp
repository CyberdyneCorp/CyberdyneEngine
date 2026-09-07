#include <cy/rendering/arbiter/subsystem.h>

namespace cy::rendering {
namespace {

[[nodiscard]] f32 clamp_positive(f32 value, f32 fallback) noexcept {
    return value > 0.0F ? value : fallback;
}

}  // namespace

const char* budget_subsystem_name(BudgetSubsystem subsystem) noexcept {
    switch (subsystem) {
        case BudgetSubsystem::Geometry:
            return "geometry";
        case BudgetSubsystem::Shadows:
            return "shadows";
        case BudgetSubsystem::GlobalIllumination:
            return "global-illumination";
        case BudgetSubsystem::Reflections:
            return "reflections";
        case BudgetSubsystem::MaterialEvaluation:
            return "material-evaluation";
        case BudgetSubsystem::Vfx:
            return "vfx";
        case BudgetSubsystem::PostProcessing:
            return "post-processing";
        case BudgetSubsystem::Count:
            break;
    }
    return "unknown";
}

Status SubsystemController::declare(const SubsystemDeclaration& declaration,
                                    const SubsystemControllerConfig& config) noexcept {
    if (declaration.subsystem >= BudgetSubsystem::Count) {
        return fail(ErrorCode::InvalidArgument, "SubsystemController: subsystem is out of range");
    }
    if (declaration.ladder.positions == 0 || declaration.ladder.positions > kMaxLadderPositions) {
        return fail(ErrorCode::InvalidArgument,
                    "SubsystemController: a ladder has between one and kMaxLadderPositions "
                    "positions");
    }
    if (declaration.ladder.relative_cost[0] != 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "SubsystemController: position 0 is authored quality and costs 1.0 by "
                    "construction");
    }
    // A ladder whose price rises along it is not a reduction ladder, and an arbiter walking it
    // would cover a deficit by making the frame more expensive. Refuse it here rather than
    // discover it as an oscillation. Equality is allowed: a position that changes quality without
    // changing cost is a legitimate declaration and simply never covers a deficit.
    for (u8 index = 1; index < declaration.ladder.positions; ++index) {
        const f32 previous = declaration.ladder.relative_cost[index - 1U];
        const f32 current = declaration.ladder.relative_cost[index];
        if (!(current > 0.0F) || current > previous) {
            return fail(ErrorCode::InvalidArgument,
                        "SubsystemController: relative_cost must be positive and non-increasing "
                        "along the ladder");
        }
    }
    if (!(declaration.base_cost_ms > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "SubsystemController: base_cost_ms must be positive — it is the estimate used "
                    "before the first measurement arrives");
    }

    declaration_ = declaration;
    config_ = config;
    scale_ms_ = declaration.base_cost_ms;
    filtered_ms_ = declaration.base_cost_ms;
    allocation_ms_ = declaration.base_cost_ms;
    position_ = 0;
    frames_since_relax_ = config.relax_dwell_frames;
    measured_ = false;
    relax_granted_ = false;
    return {};
}

void SubsystemController::set_allocation_ms(f32 allocation_ms) noexcept {
    const f32 floor = declaration_.reserved_minimum_ms;
    allocation_ms_ = allocation_ms > floor ? allocation_ms : floor;
}

void SubsystemController::report_measured_ms(f32 measured_ms) noexcept {
    const f32 sample = measured_ms > 0.0F ? measured_ms : 0.0F;
    if (!measured_) {
        filtered_ms_ = sample;
        measured_ = true;
    } else {
        const f32 alpha = config_.filter_alpha;
        filtered_ms_ = filtered_ms_ + alpha * (sample - filtered_ms_);
    }
    // The ladder declares ratios and the controller measures the scale. Dividing the filtered cost
    // by the price of the position it was measured at recovers what position 0 would cost now,
    // which is the only quantity a prediction of another position can be built from.
    scale_ms_ = clamp_positive(filtered_ms_ / declaration_.ladder.cost_at(position_), scale_ms_);
}

f32 SubsystemController::predicted_at(u8 candidate) const noexcept {
    return scale_ms_ * declaration_.ladder.cost_at(candidate);
}

bool SubsystemController::at_minimum() const noexcept {
    return position_ >= declaration_.ladder.last_position();
}

void SubsystemController::restore_authored_quality() noexcept {
    position_ = 0;
    frames_since_relax_ = config_.relax_dwell_frames;
    relax_granted_ = false;
}

SubsystemUpdate SubsystemController::update() noexcept {
    SubsystemUpdate result;
    result.filtered_ms = filtered_ms_;
    result.from_position = position_;
    result.to_position = position_;

    if (pinned_) {
        // "Pinned mode is total": the overrun is reported rather than corrected.
        result.predicted_ms = predicted_at(position_);
        result.at_minimum = at_minimum();
        result.at_reserved_minimum = allocation_ms_ <= declaration_.reserved_minimum_ms;
        const f32 overrun = filtered_ms_ - allocation_ms_;
        result.pinned_overrun_ms = overrun > 0.0F ? overrun : 0.0F;
        return result;
    }

    if (frames_since_relax_ < config_.relax_dwell_frames) {
        ++frames_since_relax_;
    }

    // Tightening needs nobody's permission: the cost overrun is the subsystem's own.
    if (filtered_ms_ > allocation_ms_ * config_.tighten_margin && !at_minimum()) {
        position_ = static_cast<u8>(position_ + 1U);
        result.tightened = true;
        result.to_position = position_;
        frames_since_relax_ = 0;
    } else if (relax_granted_ && position_ > 0 &&
               frames_since_relax_ >= config_.relax_dwell_frames) {
        // Relaxing is the arbiter's to grant, and it is spent whether or not the step fires: a
        // grant that is kept until the prediction happens to fit is a step taken at a moment
        // nobody chose (`design.md` §2.5, defect 3).
        const u8 candidate = static_cast<u8>(position_ - 1U);
        if (predicted_at(candidate) <= allocation_ms_ * config_.relax_margin) {
            position_ = candidate;
            result.relaxed = true;
            result.to_position = position_;
            frames_since_relax_ = 0;
        }
        relax_granted_ = false;
    }

    result.predicted_ms = predicted_at(position_);
    result.at_minimum = at_minimum();
    result.at_reserved_minimum = allocation_ms_ <= declaration_.reserved_minimum_ms;
    return result;
}

}  // namespace cy::rendering
