// SPDX-License-Identifier: MIT
// integration.third_person_sample — samples/14-third-person, end to end, headless.
//
// The host runs as a separate process with its scripted player (host/script.h): keys through
// `InputServer::inject`, nothing else. Every decision is the Swift module's, so every assertion is
// about something the game did through the engine — read back from the game's `HeroReport` and
// from the servers:
//
//   the hero climbed the stairs     character_move over the Jolt world: stepped up eight risers
//                                   onto the platform at 1.44 m
//   it ran, and up a hill           the sprint action, the locomotion blend space's speed; the
//                                   hill's flank is terrain, a triangle mesh
//   it jumped twice and landed      the jump action with coyote time; jump, fall and land played
//   its feet were placed            ABI 1.14's foot placement, on the stairs and the slope
//   it was heard                    footfalls from the clips' events, on stone and on grass,
//                                   through audio_play at the foot; the wind and the birds
//   the camera kept off the wall    the game's request through camera_set_target, pulled in by the
//                                   rig's collision node against the north wall, answered by the
//                                   physics world
//   twice is the same               the digest of every frame's hero, camera and pose
//
// and the negative control: the same host with no behaviours has no hero, no sound and a camera
// that never moved.

#include <cy/test/test.h>

#include <cstdlib>
#include <string>

#include "process.h"

namespace {

[[nodiscard]] std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

[[nodiscard]] double number_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return -1e30;
    }
    return std::strtod(line.c_str() + found + std::char_traits<char>::length(key), nullptr);
}

struct Run {
    cy::test::smoke::ProcessResult process;
    std::string module;
    std::string hero;
    std::string steps;
    std::string feet;
    std::string camera;
    std::string digest;

    [[nodiscard]] bool parsed() const noexcept {
        return !module.empty() && !hero.empty() && !steps.empty() && !feet.empty() &&
               !camera.empty() && !digest.empty();
    }
};

[[nodiscard]] Run run_sample(const std::string& arguments) {
    Run run;
    run.process =
        cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_THIRD_PERSON) + arguments + " 2>&1");
    run.module = line_with(run.process.output, "tps module ");
    run.hero = line_with(run.process.output, "tps hero ");
    run.steps = line_with(run.process.output, "tps steps ");
    run.feet = line_with(run.process.output, "tps feet ");
    run.camera = line_with(run.process.output, "tps camera ");
    run.digest = line_with(run.process.output, "tps digest ");
    return run;
}

void report_if_broken(const Run& run) {
    if (!run.process.ran || run.process.exit_code != 0 || !run.parsed()) {
        CY_TEST_MESSAGE(run.process.output);
    }
}

}  // namespace

CY_TEST_CASE("samples/14-third-person: the Swift hero climbs, runs, jumps and lands") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    CY_CHECK_EQ(number_after(run.module, "behaviours="), 3.0);
    CY_CHECK_EQ(number_after(run.module, "report="), 1.0);
    CY_CHECK_EQ(number_after(run.hero, "ticks="), 1200.0);

    // THE STAIRS: the controller lifted the capsule onto a tread at least once a riser, and the
    // feet reached the platform's 1.44 m — and, on the hill, higher still.
    CY_CHECK(number_after(run.hero, "stepped_up=") >= 8.0);
    CY_CHECK(number_after(run.hero, "highest=") > 1.44);
    // THE RUN: the sprint reached the run's speed, and it spent ticks in both gaits.
    CY_CHECK(number_after(run.hero, "fastest=") > 5.0);
    CY_CHECK(number_after(run.hero, "run_ticks=") > 60.0);
    CY_CHECK(number_after(run.hero, "walk_ticks=") > 120.0);
    // THE JUMPS: two taps, two jumps, two landings, and every state of the program played
    // (locomotion 1, jump 2, fall 4, land 8).
    CY_CHECK_EQ(number_after(run.hero, "jumps="), 2.0);
    CY_CHECK_EQ(number_after(run.hero, "landings="), 2.0);
    CY_CHECK(number_after(run.hero, "airborne=") > 60.0);
    CY_CHECK_EQ(number_after(run.hero, "states="), 15.0);
    // IT ENDS ON THE HILL, east of the courtyard's wall and above its floor.
    CY_CHECK(number_after(run.hero, "end_x=") > 16.0);
    CY_CHECK(number_after(run.hero, "end_y=") > 1.0);
}

CY_TEST_CASE(
    "samples/14-third-person: feet placed, footfalls heard, the camera kept off the wall") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    // FOOT PLACEMENT solved while the hero stood and walked, moved a foot by more than four
    // centimetres on the stairs and the slope, and lowered the hips — what the game read
    // (`Animator.footPlacement`) and what the animation system reported, frame for frame.
    CY_CHECK(number_after(run.feet, "solved=") > 600.0);
    CY_CHECK(number_after(run.feet, "adjusted=") > 30.0);
    CY_CHECK(number_after(run.feet, "pelvis_drop=") > 0.03);
    // The game reads, in its frame update, the placement the previous frame's pose solved: one
    // frame behind the system's own count.
    CY_CHECK_EQ(number_after(run.feet, "placed_frames="), number_after(run.feet, "solved=") + 1.0);
    CY_CHECK(number_after(run.feet, "departure=") > 0.1);

    // FOOTFALLS: the clips' events, heard on the flagstones and the stairs, and on the grass.
    CY_CHECK(number_after(run.steps, "footsteps=") > 20.0);
    CY_CHECK(number_after(run.steps, "stone=") > 8.0);
    CY_CHECK(number_after(run.steps, "grass=") >= 3.0);
    CY_CHECK_EQ(number_after(run.steps, "dropped="), 0.0);
    CY_CHECK(number_after(run.steps, "peak_voices=") >= 2.0);
    // The wind's loop and at least one bird.
    CY_CHECK(number_after(run.steps, "ambience=") >= 2.0);

    // THE CAMERA: the physics world answered its collision sweeps, the north wall pulled it in
    // while the player turned it round the hero, and the game saw it closer than it asked for.
    CY_CHECK(number_after(run.camera, "collision_frames=") > 10.0);
    CY_CHECK(number_after(run.camera, "nearest_fraction=") < 0.8);
    CY_CHECK(number_after(run.camera, "pulled=") > 10.0);
    CY_CHECK(number_after(run.camera, "nearest=") < 0.8);
    // A sweep and a ray a frame, from the first frame the game framed the hero: every frame but
    // the first, whose framing the director asks for after the rig has been evaluated.
    CY_CHECK_EQ(number_after(run.camera, "queries="), 2.0 * 1199.0);
}

CY_TEST_CASE("samples/14-third-person: two runs are the same run") {
    const Run first = run_sample("");
    const Run second = run_sample("");
    report_if_broken(first);
    report_if_broken(second);
    CY_REQUIRE(first.parsed());
    CY_REQUIRE(second.parsed());
    CY_CHECK_EQ(first.digest, second.digest);
    CY_CHECK_EQ(first.hero, second.hero);
    CY_CHECK_EQ(first.feet, second.feet);
    CY_CHECK_EQ(first.camera, second.camera);
}

CY_TEST_CASE("samples/14-third-person: with no behaviours nothing moves and nothing is heard") {
    const Run run = run_sample(" --no-behaviours");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());
    // Every line of C++ ran — the level, the colliders, the camera, the audio, the animation
    // system — and with no game there is no hero, no step, no sound and no collision to answer.
    CY_CHECK_EQ(number_after(run.module, "behaviours="), 0.0);
    CY_CHECK_EQ(number_after(run.module, "report="), 0.0);
    CY_CHECK_EQ(number_after(run.hero, "ticks="), 0.0);
    CY_CHECK_EQ(number_after(run.steps, "footsteps="), 0.0);
    CY_CHECK_EQ(number_after(run.steps, "peak_voices="), 0.0);
    CY_CHECK_EQ(number_after(run.feet, "placed_frames="), 0.0);
    CY_CHECK_EQ(number_after(run.camera, "collision_frames="), 0.0);
}
