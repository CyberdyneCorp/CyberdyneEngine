// SPDX-License-Identifier: MIT
// integration.first_person_play — samples/15-first-person PLAYED, headless: the desktop play host
// under SDL's dummy video driver, with no device, one step a frame.
//
// Every input here is a person's, written down (play/*.input) and pushed onto SDL's own queue
// (`cy::Sdl3EventInjector`): through the input source's translation, the game's action maps and
// the play host's loop — never a call into the game, and never `InputServer::inject`. What is held:
//
//   the look          a mouse count turns the camera by the first-person camera's one sensitivity,
//                     0.0022 radians, and pushing the mouse away looks up
//   the walk          W walks the game's four metres a second the way the camera faces
//   the shot          the trigger pulled with the mouse hits a target, and every shot the game
//                     fired is the one the host's own raycast along the same ray confirms
//   the focus         a window that loses the focus pauses: no tick runs, the mouse turns nothing,
//                     the keys walk nowhere; getting the focus back does not resume, Esc does
//   the menu          Esc shows it; its Resume resumes and fires nothing, its Quit ends the run
//   the pad           SDL's own virtual joystick: the left stick walks, the right stick turns at
//                     the game's look rate, the right trigger fires
//   the rebinding     a player's file moves jump from Space to F
//   the magazine      thirty rounds; the thirty-first pull clicks and reloads; R reloads
//   the respawn       a target shot down lies the game's ten seconds and the AI stands it up
//
// The numbers are the game's own (game/Player.swift, the engine's `FirstPersonSettings`) and the
// arena's (host/arena.h), not ones read off a run.

#include <cy/test/test.h>

#include <string>

#include "play_report.h"

namespace {

using cy::play::test::component_after;
using cy::play::test::line_with;
using cy::play::test::near;
using cy::play::test::number_after;

/// The first-person camera's sensitivity, radians a mouse count (`FirstPersonSettings`).
constexpr double kRadiansPerCount = 0.0022;
/// The game's walking pace and the pad's look rate (game/Player.swift's exports).
constexpr double kWalk = 4.0;
constexpr double kStickRate = 3.2;
/// Where the feet start (host/arena.h's `kPlayerStart`).
constexpr double kStartX = 5.0;
constexpr double kStartZ = 10.0;

[[nodiscard]] std::string script(const char* name) {
    return std::string(CY_FIRST_PERSON_PLAY_DIR) + "/" + name;
}

struct Played {
    cy::test::smoke::ProcessResult result;
    std::string move;
    std::string weapon;
    std::string loop;
};

[[nodiscard]] Played play(const char* name, const std::string& extra = {}) {
    Played out;
    out.result = cy::play::test::play(CY_SAMPLE_FIRST_PERSON, script(name), extra);
    out.move = line_with(out.result.output, "fps move");
    out.weapon = line_with(out.result.output, "fps weapon");
    out.loop = line_with(out.result.output, "play loop");
    CY_REQUIRE(out.result.ran);
    const bool played = out.result.exit_code == 0 && !out.loop.empty();
    CY_REQUIRE_MESSAGE(played, out.result.output);
    return out;
}

}  // namespace

CY_TEST_CASE("first-person play: a mouse count turns the camera by the camera's one sensitivity") {
    const Played run = play("look.input");
    // 200 counts right, then 50 pushed away: +x turns right, and away looks up.
    CY_CHECK(near(number_after(run.weapon, "yaw"), 200.0 * kRadiansPerCount, 5e-4));
    CY_CHECK(near(number_after(run.weapon, "pitch"), 50.0 * kRadiansPerCount, 5e-4));
    // Through the platform, and nothing else: the source forwarded what SDL queued.
    CY_CHECK(number_after(run.loop, "forwarded") >= 2.0);
    // A look and no step: the character stands where it started.
    CY_CHECK(near(component_after(run.move, "at", 0), kStartX, 1e-3));
    CY_CHECK(near(component_after(run.move, "at", 2), kStartZ, 1e-3));
}

CY_TEST_CASE("first-person play: W walks the game's pace the way the camera faces") {
    const Played run = play("walk.input");
    // A second north at four metres a second; a quarter turn right (714 counts); a second east.
    CY_CHECK(near(number_after(run.weapon, "yaw"), 714.0 * kRadiansPerCount, 5e-4));
    CY_CHECK(near(component_after(run.move, "at", 0), kStartX + kWalk, 0.05));
    CY_CHECK(near(component_after(run.move, "at", 2), kStartZ - kWalk, 0.05));
    CY_CHECK(near(number_after(run.move, "walk"), kWalk, 1e-3));
}

CY_TEST_CASE("first-person play: a shot fired with the mouse hits a target the host's ray agrees") {
    const Played run = play("shoot.input");
    const double shots = number_after(run.weapon, "shots");
    CY_CHECK(shots >= 9.0);
    CY_CHECK(number_after(run.weapon, "target_hits") >= 2.0);
    // EVERY SHOT, re-cast by the host along the game's ray at the same moment, hit the same thing.
    std::string::size_type at = 0;
    int confirmed = 0;
    while ((at = run.result.output.find("fps shot", at)) != std::string::npos) {
        const std::string line = line_with(run.result.output.substr(at), "fps shot");
        confirmed += number_after(line, "confirmed") == 1.0 ? 1 : 0;
        at += line.size();
    }
    CY_CHECK_EQ(static_cast<double>(confirmed), shots);
}

CY_TEST_CASE("first-person play: a lost focus pauses, and only the player resumes") {
    const Played run = play("pause.input");
    // Paused from frame 30 to the Esc at 60: thirty frames with no tick.
    CY_CHECK_EQ(number_after(run.loop, "focus_pauses"), 1.0);
    CY_CHECK_EQ(number_after(run.loop, "paused_frames"), 30.0);
    CY_CHECK_EQ(number_after(run.loop, "resumes"), 1.0);
    CY_CHECK_EQ(number_after(run.loop, "ticks"),
                number_after(run.loop, "frames") - number_after(run.loop, "paused_frames"));
    // The mouse moved 300 counts during the pause and turned nothing.
    CY_CHECK_EQ(number_after(run.weapon, "yaw"), 0.0);
    // W held from 10 to 90 walked only the fifty ticks that ran: 20 before, 30 after.
    CY_CHECK(near(component_after(run.move, "at", 2), kStartZ - (kWalk * 50.0 / 60.0), 0.05));
}

CY_TEST_CASE("first-person play: the pause menu's Resume resumes and fires nothing, Quit quits") {
    const Played run = play("menu.input");
    CY_CHECK_EQ(number_after(run.loop, "pauses"), 2.0);
    CY_CHECK_EQ(number_after(run.loop, "resumes"), 1.0);
    CY_CHECK_EQ(number_after(run.loop, "quit_by_menu"), 1.0);
    // The run ended at the click on frame 52, not at the script's frame 300.
    CY_CHECK(number_after(run.loop, "frames") < 60.0);
    // The left button that clicked Resume pulled no trigger.
    CY_CHECK_EQ(number_after(run.weapon, "shots"), 0.0);
}

CY_TEST_CASE("first-person play: a pad walks, turns and fires through SDL's own joystick") {
    const Played run = play("pad.input");
    // The left stick forward for sixty ticks: four metres north.
    CY_CHECK(near(component_after(run.move, "at", 2), kStartZ - kWalk, 0.05));
    // The right stick full right for thirty frames at the game's rate: 3.2 rad/s for half a second.
    CY_CHECK(near(number_after(run.weapon, "yaw"), kStickRate * 0.5, 1e-3));
    // The right trigger past half way: one shot.
    CY_CHECK_EQ(number_after(run.weapon, "shots"), 1.0);
}

CY_TEST_CASE("first-person play: a player's rebinding moves jump from Space to F") {
    const Played rebound = play("rebind.input", "--bindings " + cy::test::smoke::quoted(
                                                                     script("rebind.txt")));
    CY_CHECK_EQ(number_after(rebound.move, "jumps"), 2.0);
    // The same hands with the authored bindings: F does nothing, Space jumps.
    const Played authored = play("rebind.input");
    CY_CHECK_EQ(number_after(authored.move, "jumps"), 1.0);
}

CY_TEST_CASE("first-person play: thirty rounds, a click on empty, and the reloads") {
    const Played run = play("magazine.input");
    const std::string played = line_with(run.result.output, "fps played");
    // Thirty shots empty the magazine, the thirty-first pull clicks and starts the reload, a shot
    // once it is in, and R reloads the one round that shot spent.
    CY_CHECK_EQ(number_after(run.weapon, "shots"), 31.0);
    CY_CHECK_EQ(number_after(played, "dry_fires"), 1.0);
    CY_CHECK_EQ(number_after(played, "reloads"), 2.0);
    CY_CHECK_EQ(number_after(played, "ammo"), 30.0);
}

CY_TEST_CASE("first-person play: a target shot down gets back up ten seconds later") {
    const Played run = play("respawn.input");
    const std::string played = line_with(run.result.output, "fps played");
    const std::string target = line_with(run.result.output, "fps target   T2");
    CY_CHECK_EQ(number_after(played, "respawns"), 1.0);
    // It went down, lay the game's ten seconds (600 ticks, the tick it fell on aside), and the
    // engine's AI stood it up whole when the game gave its health back.
    CY_CHECK_EQ(number_after(target, "down"), 599.0);
    CY_CHECK_EQ(number_after(target, "points"), 100.0);
    CY_CHECK_EQ(number_after(target, "fall"), 0.0);
    CY_CHECK_EQ(number_after(target, "hits"), 3.0);
}
