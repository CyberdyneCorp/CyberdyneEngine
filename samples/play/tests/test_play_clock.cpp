// SPDX-License-Identifier: MIT
// `unit.play_host` — the play loop's clock and its pause. See play_clock.h and pause.h.

#include <cy/test/test.h>

#include <cmath>
#include <initializer_list>

#include "pause.h"
#include "play_clock.h"
#include "play_motion.h"

using cy::play::ClockSettings;
using cy::play::FixedStepClock;
using cy::play::PauseCause;
using cy::play::PauseState;
using cy::play::StepPlan;

namespace {

constexpr double kStep = 1.0 / 60.0;

[[nodiscard]] bool near(double a, double b) noexcept {
    return std::fabs(a - b) < 1e-9;
}

}  // namespace

CY_TEST_CASE("a sixty-hertz display over a sixty-hertz simulation runs one tick a frame") {
    FixedStepClock clock;
    for (int frame = 0; frame < 120; ++frame) {
        const StepPlan plan = clock.advance(kStep, false);
        CY_CHECK_EQ(plan.ticks, 1U);
        CY_CHECK(plan.interpolation < 1e-6);
    }
}

CY_TEST_CASE("a fast display runs no tick on some frames and interpolates between them") {
    // 144 hertz: 2.4 frames a tick. Over a second the ticks add up to sixty, no frame runs two,
    // and the interpolation climbs within each tick and wraps when the next one runs.
    FixedStepClock clock;
    unsigned ticks = 0;
    unsigned idle = 0;
    double last = -1.0;
    for (int frame = 0; frame < 144; ++frame) {
        const StepPlan plan = clock.advance(1.0 / 144.0, false);
        CY_CHECK(plan.ticks <= 1U);
        CY_CHECK(plan.interpolation >= 0.0);
        CY_CHECK(plan.interpolation < 1.0);
        if (plan.ticks == 0U) {
            ++idle;
            CY_CHECK(plan.interpolation > last);
        }
        ticks += plan.ticks;
        last = plan.interpolation;
    }
    CY_CHECK(ticks >= 59U);
    CY_CHECK(ticks <= 60U);
    CY_CHECK(idle > 70U);
}

CY_TEST_CASE("a hitch is clamped and a frame far behind drops its debt instead of spiralling") {
    ClockSettings settings;
    settings.max_ticks = 5;
    settings.max_frame_seconds = 0.25;
    FixedStepClock clock(settings);
    // A two-second stall is counted as a quarter of a second: fifteen steps, five run, ten dropped.
    const StepPlan stalled = clock.advance(2.0, false);
    CY_CHECK(near(stalled.frame_seconds, 0.25));
    CY_CHECK_EQ(stalled.ticks, 5U);
    CY_CHECK_EQ(stalled.dropped, 10U);
    // And the next ordinary frame is ordinary: no debt carried over.
    const StepPlan next = clock.advance(kStep, false);
    CY_CHECK_EQ(next.ticks, 1U);
    CY_CHECK_EQ(next.dropped, 0U);
}

CY_TEST_CASE("a paused clock runs nothing and holds its phase") {
    FixedStepClock clock;
    (void)clock.advance(kStep * 1.5, false);
    const StepPlan before = clock.advance(0.0, false);
    for (int frame = 0; frame < 30; ++frame) {
        const StepPlan paused = clock.advance(kStep * 3.0, true);
        CY_CHECK_EQ(paused.ticks, 0U);
        CY_CHECK(near(paused.interpolation, before.interpolation));
    }
    // Resumed, it picks up from the phase it held, not from the time that passed.
    const StepPlan resumed = clock.advance(kStep * 0.5, false);
    CY_CHECK_EQ(resumed.ticks, 1U);
}

CY_TEST_CASE("a fixed clock runs one step a frame whatever the wall says") {
    ClockSettings settings;
    settings.fixed = true;
    FixedStepClock clock(settings);
    for (const double wall : {0.0, 0.001, 0.5, 3.0}) {
        const StepPlan plan = clock.advance(wall, false);
        CY_CHECK_EQ(plan.ticks, 1U);
        CY_CHECK(near(plan.frame_seconds, kStep));
        CY_CHECK(near(plan.interpolation, 0.0));
    }
    CY_CHECK_EQ(clock.advance(0.5, true).ticks, 0U);
}

CY_TEST_CASE("Esc pauses and resumes; a lost focus pauses and only the player resumes") {
    PauseState pause;
    CY_CHECK_FALSE(pause.paused());
    CY_CHECK(pause.hold_mouse());

    pause.toggle();
    CY_CHECK(pause.paused());
    CY_CHECK(pause.cause() == PauseCause::Player);
    CY_CHECK_FALSE(pause.hold_mouse());
    pause.toggle();
    CY_CHECK_FALSE(pause.paused());

    // The window loses the focus: paused, the mouse released. Getting it back does not resume —
    // a click back into the window must not land in the fight.
    pause.focus_lost();
    CY_CHECK(pause.paused());
    CY_CHECK(pause.cause() == PauseCause::Focus);
    CY_CHECK_FALSE(pause.hold_mouse());
    pause.focus_gained();
    CY_CHECK(pause.paused());
    CY_CHECK_FALSE(pause.hold_mouse());
    pause.resume();
    CY_CHECK_FALSE(pause.paused());
    CY_CHECK(pause.hold_mouse());

    // A focus lost while already paused is not a second pause.
    pause.toggle();
    pause.focus_lost();
    CY_CHECK_EQ(pause.pauses(), 3U);
    CY_CHECK_EQ(pause.focus_pauses(), 1U);
}

CY_TEST_CASE("a frame on its tick is drawn where the tick left things, exactly") {
    // The scripted runs' every frame: an interpolation of zero changes no bit of what is drawn, so
    // their pictures and digests are what they were before the play host existed.
    const cy::Vec3 before{1.0F, 2.0F, 3.0F};
    const cy::Vec3 now{1.25F, 2.0F, 2.5F};
    const cy::Vec3 drawn = cy::play::carried_forward(before, now, 0.0F);
    CY_CHECK_EQ(drawn.x, now.x);
    CY_CHECK_EQ(drawn.y, now.y);
    CY_CHECK_EQ(drawn.z, now.z);
    cy::Transform from = cy::Transform::from_translation(before);
    cy::Transform to = cy::Transform::from_translation(now);
    to.rotation = cy::Quat::from_axis_angle(cy::Vec3{0.0F, 1.0F, 0.0F}, 0.5F);
    const cy::Transform placed = cy::play::carried_forward(from, to, 0.0F);
    CY_CHECK_EQ(placed.rotation.x, to.rotation.x);
    CY_CHECK_EQ(placed.rotation.y, to.rotation.y);
    CY_CHECK_EQ(placed.rotation.w, to.rotation.w);
}

CY_TEST_CASE("a frame between ticks carries things forward by the last tick's motion") {
    // Halfway to the next tick: half of the last step again, the turn too.
    const cy::Vec3 drawn =
        cy::play::carried_forward(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{1.0F, 0.0F, -2.0F}, 0.5F);
    CY_CHECK(std::fabs(drawn.x - 1.5F) < 1e-6F);
    CY_CHECK(std::fabs(drawn.z + 3.0F) < 1e-6F);
    cy::Transform from = cy::Transform::identity();
    cy::Transform to = cy::Transform::identity();
    to.rotation = cy::Quat::from_axis_angle(cy::Vec3{0.0F, 1.0F, 0.0F}, 0.2F);
    const cy::Transform placed = cy::play::carried_forward(from, to, 0.5F);
    const cy::Quat expected = cy::Quat::from_axis_angle(cy::Vec3{0.0F, 1.0F, 0.0F}, 0.3F);
    CY_CHECK(std::fabs(placed.rotation.y - expected.y) < 1e-3F);
    CY_CHECK(std::fabs(placed.rotation.w - expected.w) < 1e-3F);
}
