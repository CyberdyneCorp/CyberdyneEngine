#include <cy/core/determinism/clock.h>

namespace cy::determinism {

const char* epoch_reason_name(EpochReason reason) noexcept {
    switch (reason) {
        case EpochReason::SessionStart:
            return "session-start";
        case EpochReason::CheckpointRestore:
            return "checkpoint-restore";
        case EpochReason::WorldReload:
            return "world-reload";
        case EpochReason::SessionRestart:
            return "session-restart";
        case EpochReason::HotReload:
            return "hot-reload";
    }
    return "unknown";
}

Status validate(TickRate rate) noexcept {
    if (rate.numerator == 0 || rate.denominator == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a tick rate is a rational: neither numerator nor denominator may be zero");
    }
    if (rate.numerator > kMaxRateTerm || rate.denominator > kMaxRateTerm) {
        return fail(ErrorCode::OutOfRange,
                    "a tick rate's terms are bounded by kMaxRateTerm; the scaled accumulator's "
                    "range is only proved below it");
    }
    if (rate.numerator > static_cast<u64>(rate.denominator) * kMaxTicksPerSecond) {
        return fail(ErrorCode::OutOfRange,
                    "the tick rate is above kMaxTicksPerSecond ticks per second");
    }
    return ok();
}

Status SimulationClock::configure(const ClockConfig& config) noexcept {
    if (Status valid = validate(config.rate); !valid) {
        return valid;
    }
    if (config.max_ticks_per_frame == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "max_ticks_per_frame must be at least one; a frame that can run no tick cannot "
                    "advance the simulation at all");
    }
    if (config.mode == TickMode::FixedStep && config.fixed_ticks_per_frame == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "fixed-step mode with zero ticks per frame never advances the simulation");
    }
    if (config.mode == TickMode::FixedStep &&
        config.fixed_ticks_per_frame > config.max_ticks_per_frame) {
        // The cap is the loop's safety property, and fixed-step mode is not an exemption from it:
        // a recorded run that quietly exceeds the cap would not reproduce under a realtime one.
        return fail(ErrorCode::InvalidArgument,
                    "fixed_ticks_per_frame is above max_ticks_per_frame; raise the cap explicitly "
                    "rather than letting the mode step around it");
    }

    config_ = config;
    numerator_ = static_cast<i64>(config.rate.numerator);
    denominator_ = static_cast<i64>(config.rate.denominator);
    epochs_.adopt(config.epoch, EpochReason::SessionStart);
    tick_ = config.tick;
    accumulator_ = 0;
    discarded_ns_ = 0;
    capped_frames_ = 0;
    return ok();
}

f64 SimulationClock::seconds_at(u64 tick) const noexcept {
    if (config_.rate.numerator == 0) {
        return 0.0;
    }
    // tick * denominator / numerator, with the multiply first so that a rate like 30000/1001 gives
    // the exact ratio rather than the rounded step accumulated `tick` times.
    return (static_cast<f64>(tick) * static_cast<f64>(denominator_)) / static_cast<f64>(numerator_);
}

FrameTicks SimulationClock::accumulate(Nanoseconds elapsed_ns) noexcept {
    FrameTicks result;

    if (config_.mode == TickMode::FixedStep) {
        // The elapsed time is deliberately unused: this mode exists so that a run reproduces on a
        // machine of a different speed, and reading the argument at all would be the defect.
        result.ticks = config_.fixed_ticks_per_frame;
        result.alpha = 0.0F;
        return result;
    }

    if (elapsed_ns > 0) {
        accumulator_ += elapsed_ns * numerator_;
    }

    const i64 step = step_scaled();
    const i64 available = accumulator_ / step;
    const i64 cap = static_cast<i64>(config_.max_ticks_per_frame);
    result.ticks = static_cast<u32>(available < cap ? available : cap);

    if (available > cap) {
        // `engine-architecture`: exceeding the cap discards the excess so the loop cannot enter a
        // death spiral, with a counter incremented rather than the time silently vanishing. What is
        // discarded is everything past the ticks this frame will run, keeping the residue that
        // becomes the alpha — dropping that too would make the first frame after a stall render at
        // a pose it never simulated.
        const i64 keep = (cap * step) + (accumulator_ % step);
        discarded_ns_ += static_cast<u64>((accumulator_ - keep) / numerator_);
        ++capped_frames_;
        accumulator_ = keep;
    }

    result.discarded_ns = discarded_ns_;
    result.alpha = interpolation_alpha();
    return result;
}

void SimulationClock::advance() noexcept {
    ++tick_;
    if (config_.mode == TickMode::FixedStep) {
        return;
    }
    const i64 step = step_scaled();
    accumulator_ = accumulator_ >= step ? accumulator_ - step : 0;
}

f32 SimulationClock::interpolation_alpha() const noexcept {
    const i64 step = step_scaled();
    if (step <= 0) {
        return 0.0F;
    }
    const i64 residue = accumulator_ % step;
    return static_cast<f32>(static_cast<f64>(residue) / static_cast<f64>(step));
}

Epoch SimulationClock::reset(EpochReason reason, u64 tick) noexcept {
    tick_ = tick;
    accumulator_ = 0;
    return epochs_.advance(reason);
}

void SimulationClock::resume(SimulationPoint point, EpochReason reason) noexcept {
    epochs_.adopt(point.epoch, reason);
    tick_ = point.tick;
    accumulator_ = 0;
}

}  // namespace cy::determinism
