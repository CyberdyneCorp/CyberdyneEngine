// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_CLOCK_H
#define CY_SAMPLE_PLAY_PLAY_CLOCK_H
// play_clock.h — real time turned into fixed steps: how many ticks a frame runs, and where between
// two ticks the frame it draws falls.
//
// The simulation steps at a fixed rate whatever the display does; the frame takes the wall time
// since the last one, adds it to what the last frame left over, and runs one tick per whole step in
// it. What is left — less than a step — is the INTERPOLATION the ABI hands the game (`CyTime`'s
// "how far between the last committed tick and the next the frame is, in [0, 1)").
//
// Three things this has to get right, each with a case in tests/test_play_clock.cpp:
//
//   * a hitch does not become a burst: a frame longer than `max_frame_seconds` is counted as that
//     long, and a frame that would run more than `max_ticks` drops the rest of its debt rather than
//     falling further behind each frame (the "spiral of death");
//   * a pause stops the simulation and keeps its phase: no tick runs and the remainder is held, so
//     the frame drawn while paused is the frame that was drawn when it paused;
//   * a FIXED clock — one step a frame, whatever the wall says — makes a run with injected input
//     reproducible frame for frame, which is what a capture and a test want.

#include <cy/core/base/types.h>

namespace cy::play {

/// What one frame does.
struct StepPlan {
    /// Ticks to run before the frame.
    u32 ticks = 0;
    /// The frame's place between the last tick and the next, in [0, 1).
    f64 interpolation = 0.0;
    /// The seconds the frame stands for: the wall's, clamped — what presentation advances by.
    f64 frame_seconds = 0.0;
    /// Steps of debt this frame dropped because it was more than `max_ticks` behind.
    u32 dropped = 0;
};

struct ClockSettings {
    f64 step_seconds = 1.0 / 60.0;
    u32 max_ticks = 5;
    f64 max_frame_seconds = 0.25;
    /// One step a frame, whatever the wall time: reproducible runs.
    bool fixed = false;
};

class FixedStepClock {
public:
    explicit FixedStepClock(const ClockSettings& settings = {}) noexcept : settings_(settings) {}

    /// Plan the frame that took `wall_seconds` since the last.
    [[nodiscard]] StepPlan advance(f64 wall_seconds, bool paused) noexcept;

    [[nodiscard]] const ClockSettings& settings() const noexcept { return settings_; }

private:
    ClockSettings settings_;
    f64 remainder_ = 0.0;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_CLOCK_H
