// SPDX-License-Identifier: MIT
// integration.third_person_play — samples/14-third-person PLAYED, headless: the desktop play host
// under SDL's dummy video driver, with no device, one step a frame.
//
// Every input is a person's, written down (play/*.input) and pushed onto SDL's own queue — through
// the input source, the game's action maps and the play host's loop, never into the game. What is
// held:
//
//   the orbit     a mouse count turns the camera by the director's 0.0025 radians, and the camera
//                 holds where the mouse left it while the hero stands
//   the run       W and Shift run the hero at the game's run speed the way the camera looks
//   the pause     Esc pauses: no tick runs, the hero stands, the mouse turns nothing; Esc resumes
//   the pad       SDL's own virtual joystick: the right stick orbits at the director's look rate,
//                 the left stick walks the hero
//
// The numbers are the game's (game/CameraDirector.swift, game/Hero.swift), not read off a run.

#include <cy/test/test.h>

#include <cmath>
#include <string>

#include "play_report.h"

namespace {

using cy::play::test::line_with;
using cy::play::test::near;
using cy::play::test::number_after;

/// game/CameraDirector.swift's `mouseRadians` and `lookSpeed`; game/Hero.swift's speeds.
constexpr double kRadiansPerCount = 0.0025;
constexpr double kLookSpeed = 2.4;
constexpr double kRunSpeed = 5.5;
constexpr double kWalkSpeed = 1.8;
/// Where the hero starts (game/Hero.swift's `spawn`).
constexpr double kSpawnX = 0.0;
constexpr double kSpawnZ = 9.0;

struct Played {
    cy::test::smoke::ProcessResult result;
    std::string hero;
    std::string camera;
    std::string loop;
};

[[nodiscard]] Played play(const char* name) {
    Played out;
    out.result = cy::play::test::play(CY_SAMPLE_THIRD_PERSON,
                                      std::string(CY_THIRD_PERSON_PLAY_DIR) + "/" + name);
    out.hero = line_with(out.result.output, "tps hero");
    out.camera = line_with(out.result.output, "tps camera");
    out.loop = line_with(out.result.output, "play loop");
    CY_REQUIRE(out.result.ran);
    const bool played = out.result.exit_code == 0 && !out.loop.empty();
    CY_REQUIRE_MESSAGE(played, out.result.output);
    return out;
}

}  // namespace

CY_TEST_CASE("third-person play: a mouse count orbits the camera by the director's rate") {
    const Played run = play("orbit.input");
    // 400 counts right: the yaw falls by 400 x 0.0025 — the director's sign, a turn to the right.
    CY_CHECK(near(number_after(run.camera, "yaw"), -400.0 * kRadiansPerCount, 1e-3));
    // The hero stood: the camera held where the mouse left it rather than swinging back behind.
    CY_CHECK(near(number_after(run.hero, "end_x"), kSpawnX, 1e-3));
    CY_CHECK(near(number_after(run.hero, "end_z"), kSpawnZ, 1e-3));
}

CY_TEST_CASE("third-person play: W and Shift run the hero the way the camera looks") {
    const Played run = play("run.input");
    // A quarter turn left (628 counts): the camera looks west, and the hero runs west.
    CY_CHECK(near(number_after(run.camera, "yaw"), 628.0 * kRadiansPerCount, 0.01));
    CY_CHECK(near(number_after(run.hero, "fastest"), kRunSpeed, 1e-3));
    CY_CHECK(number_after(run.hero, "end_x") < kSpawnX - 6.0);
    CY_CHECK(near(number_after(run.hero, "end_z"), kSpawnZ, 0.1));
    CY_CHECK(number_after(run.hero, "run_ticks") > 100.0);
}

CY_TEST_CASE("third-person play: Esc pauses the hero and the camera, and Esc resumes") {
    const Played run = play("pause.input");
    CY_CHECK_EQ(number_after(run.loop, "pauses"), 1.0);
    CY_CHECK_EQ(number_after(run.loop, "resumes"), 1.0);
    CY_CHECK_EQ(number_after(run.loop, "paused_frames"), 40.0);
    CY_CHECK_EQ(number_after(run.hero, "ticks"),
                number_after(run.loop, "frames") - number_after(run.loop, "paused_frames"));
    // The mouse moved 400 counts during the pause and turned nothing.
    CY_CHECK_EQ(number_after(run.camera, "yaw"), 0.0);
    // W walked the ticks that ran, north, at the walking pace: never as far as a minute's worth.
    CY_CHECK(number_after(run.hero, "end_z") < kSpawnZ - 1.0);
    CY_CHECK(near(number_after(run.hero, "fastest"), kWalkSpeed, 1e-3));
}

CY_TEST_CASE("third-person play: a pad orbits the camera and walks the hero") {
    const Played run = play("pad.input");
    // The right stick full right for half a second at the director's 2.4 radians a second.
    CY_CHECK(near(number_after(run.camera, "yaw"), -kLookSpeed * 0.5, 0.01));
    // The left stick forward for a second: the hero walked, the way the camera looked.
    CY_CHECK(near(number_after(run.hero, "fastest"), kWalkSpeed, 1e-3));
    const double moved = std::hypot(number_after(run.hero, "end_x") - kSpawnX,
                                    number_after(run.hero, "end_z") - kSpawnZ);
    CY_CHECK(moved > 1.0);
}
