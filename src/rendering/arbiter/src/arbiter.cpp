#include <cy/rendering/arbiter/arbiter.h>

namespace cy::rendering {
namespace {

/// A forced allocation must be far enough under what the current position costs that the
/// subsystem's own tighten test fires. The controllers' margin is 1.06, so anything at or below
/// 1/1.06 = 0.943 of the current cost does it; 0.94 is that with a digit of room.
///
/// This is the second half of `design.md` §2.3's "by setting its allocation just under what its
/// current position costs". The first half — "forced allocation at target cost x 1.08" — is
/// `ArbiterConfig::forced_allocation_margin`, and the arbiter takes the SMALLER of the two. Taking
/// only the second is a defect on a fine ladder: a step that is 5% cheaper priced at x1.08 lands
/// ABOVE the current cost, the controller never tightens, and the arbiter forces a step every tick
/// that nothing ever takes.
constexpr f32 kForceUnderCurrent = 0.94F;

[[nodiscard]] f32 max_of(f32 lhs, f32 rhs) noexcept {
    return lhs > rhs ? lhs : rhs;
}

[[nodiscard]] f32 min_of(f32 lhs, f32 rhs) noexcept {
    return lhs < rhs ? lhs : rhs;
}

}  // namespace

const char* adjustment_cause_name(AdjustmentCause cause) noexcept {
    switch (cause) {
        case AdjustmentCause::FrameOverBudget:
            return "frame-over-budget";
        case AdjustmentCause::HeadroomAvailable:
            return "headroom-available";
        case AdjustmentCause::EverySubsystemAtMinimum:
            return "every-subsystem-at-minimum";
        case AdjustmentCause::Count:
            break;
    }
    return "unknown";
}

BudgetArbiter::BudgetArbiter() noexcept {
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        entries_[index].declaration.subsystem = static_cast<BudgetSubsystem>(index);
    }
}

Status BudgetArbiter::configure(const ArbiterConfig& config) noexcept {
    if (!(config.frame_budget_ms > 0.0F) || config.non_allocatable_ms < 0.0F ||
        config.non_allocatable_ms >= config.frame_budget_ms) {
        return fail(
            ErrorCode::InvalidArgument,
            "BudgetArbiter: the non-allocatable part must be smaller than the frame budget");
    }
    if (config.period_frames == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "BudgetArbiter: the arbiter period must be at "
                    "least one frame");
    }
    if (!(config.gain > 0.0F) || config.deadband_multiple < 0.0F || !(config.filter_alpha > 0.0F) ||
        config.filter_alpha > 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "BudgetArbiter: gain must be positive, the deadband non-negative and the "
                    "filter alpha in (0, 1]");
    }
    if (!(config.relax_allocation_margin > config.forced_allocation_margin)) {
        return fail(ErrorCode::InvalidArgument,
                    "BudgetArbiter: the relax allocation margin must exceed the forced one, or a "
                    "granted step is an allocation the controller refuses to use");
    }
    if (config.resolution_positions == 0 || config.resolution_positions > kMaxLadderPositions ||
        config.resolution_scale[0] != 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "BudgetArbiter: the resolution ladder starts at 1.0 and has between one and "
                    "kMaxLadderPositions positions");
    }
    for (u8 index = 1; index < config.resolution_positions; ++index) {
        const f32 previous = config.resolution_scale[index - 1U];
        const f32 current = config.resolution_scale[index];
        if (!(current > 0.0F) || current >= previous) {
            return fail(ErrorCode::InvalidArgument,
                        "BudgetArbiter: the resolution ladder must descend strictly from 1.0");
        }
    }
    config_ = config;
    return {};
}

Status BudgetArbiter::declare(const SubsystemDeclaration& declaration) noexcept {
    if (declaration.subsystem >= BudgetSubsystem::Count) {
        return fail(ErrorCode::InvalidArgument, "BudgetArbiter: subsystem is out of range");
    }
    // The ladder rules are the controller's and there is exactly one copy of them: a declaration
    // the arbiter accepted and a controller refused would be a budget over a ladder nothing holds.
    SubsystemController probe;
    if (auto accepted = probe.declare(declaration); !accepted) {
        return accepted;
    }
    if (declaration.resolution_sensitivity < 0.0F || declaration.resolution_sensitivity > 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "BudgetArbiter: resolution_sensitivity is a fraction in [0, 1]");
    }

    Entry& entry = entries_[static_cast<u32>(declaration.subsystem)];
    entry.declaration = declaration;
    entry.registered = true;
    entry.filtered_ms = declaration.base_cost_ms;
    entry.scale_ms = declaration.base_cost_ms;
    entry.allocation_ms = declaration.base_cost_ms * config_.forced_allocation_margin;
    entry.position = 0;
    entry.at_minimum = declaration.ladder.positions <= 1U;
    entry.measured = false;
    entry.frames_since_grant = config_.relax_grant_dwell_frames;
    rebuild_order();
    return {};
}

void BudgetArbiter::rebuild_order() noexcept {
    order_count_ = 0;
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (entries_[index].registered) {
            order_[order_count_++] = static_cast<u8>(index);
        }
    }
    // Insertion sort by declared reduction order; ties keep enumerator order, which is the
    // specification's own list and therefore a stable, reviewable tie-break.
    for (u32 index = 1; index < order_count_; ++index) {
        const u8 candidate = order_[index];
        const u32 key = entries_[candidate].declaration.reduction_order;
        u32 slot = index;
        while (slot > 0 && entries_[order_[slot - 1U]].declaration.reduction_order > key) {
            order_[slot] = order_[slot - 1U];
            --slot;
        }
        order_[slot] = candidate;
    }
}

bool BudgetArbiter::registered(BudgetSubsystem subsystem) const noexcept {
    const auto index = static_cast<u32>(subsystem);
    return index < kBudgetSubsystemCount && entries_[index].registered;
}

void BudgetArbiter::report_frame_ms(f32 frame_ms) noexcept {
    measured_frame_ms_ = frame_ms > 0.0F ? frame_ms : 0.0F;
    if (!frame_measured_) {
        filtered_frame_ms_ = measured_frame_ms_;
        frame_measured_ = true;
        return;
    }
    filtered_frame_ms_ += config_.filter_alpha * (measured_frame_ms_ - filtered_frame_ms_);
}

void BudgetArbiter::report_subsystem(BudgetSubsystem subsystem, f32 measured_ms, u8 position,
                                     bool at_minimum) noexcept {
    const auto index = static_cast<u32>(subsystem);
    if (index >= kBudgetSubsystemCount || !entries_[index].registered) {
        return;
    }
    Entry& entry = entries_[index];
    const f32 sample = measured_ms > 0.0F ? measured_ms : 0.0F;
    if (!entry.measured) {
        entry.filtered_ms = sample;
        entry.measured = true;
    } else {
        entry.filtered_ms += config_.filter_alpha * (sample - entry.filtered_ms);
    }
    entry.position = position < entry.declaration.ladder.positions
                         ? position
                         : entry.declaration.ladder.last_position();
    entry.at_minimum = at_minimum;
    const f32 price = entry.declaration.ladder.cost_at(entry.position);
    const f32 scale = entry.filtered_ms / price;
    entry.scale_ms = scale > 0.0F ? scale : entry.scale_ms;
}

f32 BudgetArbiter::predicted_at(const Entry& entry, u8 position) noexcept {
    return entry.scale_ms * entry.declaration.ladder.cost_at(position);
}

f32 BudgetArbiter::allocation_ms(BudgetSubsystem subsystem) const noexcept {
    const auto index = static_cast<u32>(subsystem);
    return index < kBudgetSubsystemCount ? entries_[index].allocation_ms : 0.0F;
}

f32 BudgetArbiter::resolution_scale() const noexcept {
    return config_.resolution_scale[resolution_position_];
}

f32 BudgetArbiter::allocatable_ms() const noexcept {
    return config_.frame_budget_ms - config_.non_allocatable_ms;
}

f32 BudgetArbiter::coarsest_quantum_ms() const noexcept {
    // The deadband is sized against the coarsest step REACHABLE FROM HERE, in either direction —
    // not against a fixed number of milliseconds. What makes a discrete ladder oscillate is a
    // correction smaller than the step it would have to take, and which step that is depends on
    // where every subsystem currently stands.
    f32 coarsest = 0.0F;
    for (u32 slot = 0; slot < order_count_; ++slot) {
        const Entry& entry = entries_[order_[slot]];
        const u8 position = entry.position;
        const f32 here = predicted_at(entry, position);
        if (position + 1U < entry.declaration.ladder.positions) {
            coarsest = max_of(coarsest, here - predicted_at(entry, static_cast<u8>(position + 1U)));
        }
        if (position > 0) {
            coarsest = max_of(coarsest, predicted_at(entry, static_cast<u8>(position - 1U)) - here);
        }
    }
    // Resolution scale counts towards the quantum only when it is REACHABLE: downwards once every
    // peer is at its minimum, upwards once it has already been taken. Counting it unconditionally
    // sizes the deadband against a step of 15% of the whole frame from the first frame onwards,
    // which wastes about a millisecond of budget on a lever that cannot be pulled.
    if (everything_at_minimum() && resolution_position_ + 1U < config_.resolution_positions) {
        coarsest = max_of(coarsest,
                          -resolution_step_increase_ms(static_cast<u8>(resolution_position_ + 1U)));
    }
    if (resolution_position_ > 0) {
        coarsest = max_of(coarsest,
                          resolution_step_increase_ms(static_cast<u8>(resolution_position_ - 1U)));
    }
    return coarsest;
}

f32 BudgetArbiter::resolution_step_increase_ms(u8 target) const noexcept {
    // Cost scales with pixel COUNT, and the ladder's numbers are a linear scale per axis — so the
    // ratio is of the squares. A subsystem contributes only its `resolution_sensitivity` share:
    // an acceleration-structure build on a fixed grid does not get cheaper because the target got
    // smaller, and an arbiter that assumed it did would credit a step it cannot pay for.
    const f32 now = config_.resolution_scale[resolution_position_];
    const f32 then =
        config_.resolution_scale[target < config_.resolution_positions
                                     ? target
                                     : static_cast<u8>(config_.resolution_positions - 1U)];
    const f32 ratio = (then * then) / (now * now);
    f32 delta = 0.0F;
    for (u32 slot = 0; slot < order_count_; ++slot) {
        const Entry& entry = entries_[order_[slot]];
        const f32 sensitivity = entry.declaration.resolution_sensitivity;
        delta += entry.filtered_ms * sensitivity * (ratio - 1.0F);
    }
    return delta;
}

void BudgetArbiter::cap_and_floor(Entry& entry) const noexcept {
    // `design.md` §2.5 defect 1: cap an allocation at what the subsystem can spend at its best
    // declared position. Without the cap every under-budget frame scales the allocations up,
    // nothing changes because everything is already at position 0, and the next spike is absorbed
    // by accumulated slack instead of by the levers.
    // The ceiling uses the RELAX margin because that is the largest allocation any branch here can
    // legitimately hand out; capping at the smaller forced margin would clamp the last step back to
    // authored quality and leave the subsystem one rung short of where it started.
    const f32 ceiling = predicted_at(entry, 0) * config_.relax_allocation_margin;
    entry.allocation_ms = min_of(entry.allocation_ms, ceiling);
    entry.allocation_ms = max_of(entry.allocation_ms, entry.declaration.reserved_minimum_ms);
}

void BudgetArbiter::push(ArbiterReport& report, const BudgetAdjustment& adjustment) noexcept {
    if (report.adjustment_count < kMaxAdjustmentsPerTick) {
        report.adjustments[report.adjustment_count++] = adjustment;
    }
}

bool BudgetArbiter::everything_at_minimum() const noexcept {
    if (order_count_ == 0) {
        return false;
    }
    for (u32 slot = 0; slot < order_count_; ++slot) {
        if (!entries_[order_[slot]].at_minimum) {
            return false;
        }
    }
    return true;
}

void BudgetArbiter::tighten(f32 error_ms, ArbiterReport& report) noexcept {
    f32 remaining = config_.gain * error_ms;

    // The actuator: ascending declared reduction order, ONE step forced per subsystem, until the
    // gain's share of the deficit is covered. Nothing is scaled (`design.md` §2.3).
    for (u32 slot = 0; slot < order_count_ && remaining > 0.0F; ++slot) {
        Entry& entry = entries_[order_[slot]];
        const u8 target = static_cast<u8>(entry.position + 1U);
        if (target >= entry.declaration.ladder.positions) {
            continue;
        }
        const f32 here = predicted_at(entry, entry.position);
        const f32 there = predicted_at(entry, target);
        const f32 saving = here - there;
        if (!(saving > 0.0F)) {
            continue;  // a rung that changes quality without changing cost covers no deficit
        }
        const f32 forced =
            min_of(there * config_.forced_allocation_margin, here * kForceUnderCurrent);
        if (forced < entry.declaration.reserved_minimum_ms) {
            // The declared reserved minimum already holds this subsystem above what the step would
            // allocate it, so forcing the step would mean allocating below a floor the subsystem
            // was promised. It has nothing to give and the arbiter takes the shortfall elsewhere,
            // which is `rendering-architecture`'s "a subsystem with nothing left to give".
            continue;
        }
        const f32 previous = entry.allocation_ms;
        entry.allocation_ms = forced;
        cap_and_floor(entry);
        push(report, BudgetAdjustment{entry.declaration.subsystem, AdjustmentCause::FrameOverBudget,
                                      entry.position, target, previous, entry.allocation_ms});
        remaining -= saving;
    }

    if (remaining > 0.0F && everything_at_minimum() &&
        resolution_position_ + 1U < config_.resolution_positions) {
        // The last lever, and only here: resolution multiplies every raster-bound subsystem at
        // once, so it is not a peer allocation and is reached only when no peer has anything left.
        const u8 target = static_cast<u8>(resolution_position_ + 1U);
        const f32 from = config_.resolution_scale[resolution_position_];
        push(report, BudgetAdjustment{
                         BudgetSubsystem::Count, AdjustmentCause::EverySubsystemAtMinimum,
                         resolution_position_, target, from, config_.resolution_scale[target]});
        resolution_position_ = target;
        frames_since_resolution_grant_ = 0;
    }
}

void BudgetArbiter::relax(f32 headroom_ms, ArbiterReport& report) noexcept {
    // Headroom is MEASURED and it is what is left after one deadband is set aside, so a granted
    // step leaves the frame at least one deadband inside the budget (`design.md` §2.5 defects 2
    // and 4).
    if (!(headroom_ms > 0.0F)) {
        return;
    }

    // Resolution scale came down last, so it goes back up first.
    if (resolution_position_ > 0) {
        if (frames_since_resolution_grant_ < config_.relax_grant_dwell_frames) {
            return;
        }
        const u8 target = static_cast<u8>(resolution_position_ - 1U);
        const f32 increase = resolution_step_increase_ms(target);
        if (increase > headroom_ms) {
            return;  // all or nothing: a partial grant spends the surplus and changes nothing
        }
        const f32 from = config_.resolution_scale[resolution_position_];
        push(report, BudgetAdjustment{BudgetSubsystem::Count, AdjustmentCause::HeadroomAvailable,
                                      resolution_position_, target, from,
                                      config_.resolution_scale[target]});
        resolution_position_ = target;
        frames_since_resolution_grant_ = 0;
        return;
    }

    // Reverse declared order — last reduced, first restored — and STOP at the first subsystem that
    // has a step to take. Skipping it to grant something further down the list gives quality back
    // in an order nobody declared, which is the same objection §2.3 makes to a uniform scale.
    for (u32 slot = order_count_; slot-- > 0;) {
        Entry& entry = entries_[order_[slot]];
        if (entry.position == 0) {
            continue;
        }
        if (entry.frames_since_grant < config_.relax_grant_dwell_frames) {
            return;
        }
        const u8 target = static_cast<u8>(entry.position - 1U);
        const f32 increase = predicted_at(entry, target) - predicted_at(entry, entry.position);
        if (increase > headroom_ms) {
            return;
        }
        const f32 previous = entry.allocation_ms;
        entry.allocation_ms = predicted_at(entry, target) * config_.relax_allocation_margin;
        cap_and_floor(entry);
        entry.frames_since_grant = 0;
        report.relax_granted[order_[slot]] = true;
        push(report,
             BudgetAdjustment{entry.declaration.subsystem, AdjustmentCause::HeadroomAvailable,
                              entry.position, target, previous, entry.allocation_ms});
        return;
    }
}

ArbiterReport BudgetArbiter::update() noexcept {
    ArbiterReport report;
    report.pinned = pinned_;
    report.measured_frame_ms = measured_frame_ms_;
    report.filtered_frame_ms = filtered_frame_ms_;
    report.resolution_scale = resolution_scale();

    const f32 deadband = coarsest_quantum_ms() * config_.deadband_multiple;
    const f32 setpoint = config_.frame_budget_ms - deadband;
    report.deadband_ms = deadband;
    report.setpoint_ms = setpoint;

    if (pinned_) {
        // "Pinned mode is total ... budget overruns SHALL be reported rather than corrected."
        report.pinned_overrun_ms = max_of(0.0F, filtered_frame_ms_ - config_.frame_budget_ms);
    } else {
        for (u32 slot = 0; slot < order_count_; ++slot) {
            Entry& entry = entries_[order_[slot]];
            if (entry.frames_since_grant < config_.relax_grant_dwell_frames) {
                ++entry.frames_since_grant;
            }
        }
        if (frames_since_resolution_grant_ < config_.relax_grant_dwell_frames) {
            ++frames_since_resolution_grant_;
        }

        ++frame_counter_;
        if (frame_counter_ >= config_.period_frames) {
            frame_counter_ = 0;
            report.arbitrated = true;
            const f32 error = filtered_frame_ms_ - setpoint;
            if (error > deadband) {
                tighten(error, report);
            } else if (-error > deadband) {
                relax(-error - deadband, report);
            }
        }
    }

    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        const Entry& entry = entries_[index];
        report.registered[index] = entry.registered;
        report.allocation_ms[index] = entry.allocation_ms;
        report.measured_ms[index] = entry.filtered_ms;
        report.at_minimum[index] = entry.registered && entry.at_minimum;
    }
    report.resolution_scale = resolution_scale();
    return report;
}

void BudgetArbiter::restore_authored_quality() noexcept {
    for (Entry& entry : entries_) {
        if (!entry.registered) {
            continue;
        }
        entry.position = 0;
        entry.at_minimum = entry.declaration.ladder.positions <= 1U;
        entry.allocation_ms = predicted_at(entry, 0) * config_.forced_allocation_margin;
        cap_and_floor(entry);
        entry.frames_since_grant = config_.relax_grant_dwell_frames;
    }
    resolution_position_ = 0;
    frames_since_resolution_grant_ = config_.relax_grant_dwell_frames;
}

}  // namespace cy::rendering
