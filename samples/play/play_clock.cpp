// SPDX-License-Identifier: MIT
// play_clock.cpp — see play_clock.h.

#include "play_clock.h"

namespace cy::play {

StepPlan FixedStepClock::advance(f64 wall_seconds, bool paused) noexcept {
    const f64 step = settings_.step_seconds;
    StepPlan plan;
    if (settings_.fixed) {
        plan.frame_seconds = step;
        plan.ticks = paused ? 0U : 1U;
        return plan;
    }
    f64 seconds = wall_seconds < 0.0 ? 0.0 : wall_seconds;
    seconds = seconds > settings_.max_frame_seconds ? settings_.max_frame_seconds : seconds;
    plan.frame_seconds = seconds;
    if (paused) {
        plan.interpolation = remainder_ / step;
        return plan;
    }
    f64 debt = remainder_ + seconds;
    while (debt >= step && plan.ticks < settings_.max_ticks) {
        debt -= step;
        ++plan.ticks;
    }
    if (debt >= step) {
        // Further behind than a frame may catch up: what is past the limit is dropped, keeping the
        // phase, rather than carried into the next frame and the one after.
        while (debt >= step) {
            debt -= step;
            ++plan.dropped;
        }
    }
    remainder_ = debt;
    plan.interpolation = remainder_ / step;
    return plan;
}

}  // namespace cy::play
