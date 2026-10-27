// SPDX-License-Identifier: MIT
// integration.first_person_sample — samples/15-first-person, end to end, headless.
//
// The host runs as a separate process with its scripted player: synthetic key presses, mouse
// motion and button presses through `InputServer::inject`. The player is the Swift module's; the
// targets are the engine's AI. What is held:
//
//   the scripted run is one run      two runs of the script end in one digest — the player, the
//                                    targets and the props, quantised, folded every frame — and the
//                                    negative control (no game) ends in another
//   the character controller         walking, sprinting and crouched paces are the game's speeds;
//   the
//                                    jump leaves the ground and comes back; the crouch is the
//                                    capsule rebuilt shorter: it passes under the beam a standing
//                                    capsule does not fit under, letting go of the crouch there is
//                                    REFUSED by the headroom test, and once clear the player stands
//   a shot hits what the raycast says every shot the game fired, re-cast by the host along the same
//                                    ray at the same moment, hits the same entity at the same point
//   what a hit does                  a target hit three times goes down — the AI's `fall` — and one
//                                    hit twice is hurt, flinched and engages; a crate shot flies
//   what a shot leaves               a flash, a burst and a kick a shot; sparks a hit; a scorch a
//                                    hit on the arena; a sound each; and both presentation calls
//                                    the game tried from a fixed step were refused
//   the targets see                  the AI's perception traced rays through the physics world and
//                                    saw the player
//
// The numbers it holds to are the game's own (game/Player.swift's exports) and the arena's
// (host/arena.h), not ones read off a run.

#include <cy/test/test.h>

#include <cmath>
#include <cstdlib>
#include <sstream>
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

[[nodiscard]] std::string text_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type begin = found + std::char_traits<char>::length(key);
    return line.substr(begin, line.find(' ', begin) - begin);
}

[[nodiscard]] cy::test::smoke::ProcessResult run(const std::string& arguments) {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_FIRST_PERSON) + " " + arguments +
                                " 2>&1");
}

struct Session {
    cy::test::smoke::ProcessResult result;
    std::string module;
    std::string move;
    std::string weapon;
    std::string effects;
    std::string ai;
    std::string props;

    explicit Session(const std::string& arguments) : result(run(arguments)) {
        module = line_with(result.output, "fps module ");
        move = line_with(result.output, "fps move ");
        weapon = line_with(result.output, "fps weapon ");
        effects = line_with(result.output, "fps effects ");
        ai = line_with(result.output, "fps ai ");
        props = line_with(result.output, "fps props ");
    }

    [[nodiscard]] std::string target(const char* name) const {
        return line_with(result.output, (std::string("fps target   ") + name + " ").c_str());
    }
};

[[nodiscard]] const Session& scripted() {
    static const Session session("--headless");
    return session;
}

}  // namespace

CY_TEST_CASE("first-person: the scripted run is one run, and the game is what makes it") {
    const Session& first = scripted();
    std::fprintf(stderr, "%s\n", first.result.output.c_str());
    CY_REQUIRE_EQ(first.result.exit_code, 0);
    CY_CHECK_EQ(number_after(first.module, "behaviours="), 1.0);
    CY_CHECK_EQ(number_after(first.module, "frames="), 720.0);
    const Session second("--headless");
    CY_REQUIRE_EQ(second.result.exit_code, 0);
    CY_CHECK_EQ(text_after(first.module, "digest="), text_after(second.module, "digest="));
    CY_CHECK_EQ(first.result.output, second.result.output);

    // THE NEGATIVE CONTROL: the same host with no game moves nothing and shoots nothing, and the
    // targets only patrol.
    const Session control("--headless --no-behaviours");
    CY_REQUIRE_EQ(control.result.exit_code, 0);
    CY_CHECK_EQ(number_after(control.module, "behaviours="), 0.0);
    CY_CHECK_NE(text_after(control.module, "digest="), text_after(first.module, "digest="));
    CY_CHECK_EQ(number_after(control.weapon, "shots="), 0.0);
    CY_CHECK_EQ(number_after(control.effects, "lights_spawned="), 0.0);
    CY_CHECK_EQ(number_after(control.effects, "played="), 0.0);
    CY_CHECK_EQ(number_after(control.effects, "decals="), 0.0);
    CY_CHECK_EQ(number_after(control.target("T1"), "hits="), 0.0);
}

CY_TEST_CASE("first-person: walk, sprint, jump and crouch are the character controller's") {
    const Session& session = scripted();
    CY_REQUIRE_EQ(session.result.exit_code, 0);
    const std::string& move = session.move;
    // The game's paces (Player.swift: 4, 7 and 2 m/s), reached on the ground.
    CY_CHECK_GT(number_after(move, "walk="), 3.9);
    CY_CHECK_LT(number_after(move, "walk="), 4.01);
    CY_CHECK_GT(number_after(move, "sprint="), 6.9);
    CY_CHECK_LT(number_after(move, "sprint="), 7.01);
    CY_CHECK_GT(number_after(move, "crouch="), 1.9);
    CY_CHECK_LT(number_after(move, "crouch="), 2.01);
    // One jump at 5 m/s: in the air, and higher than the 0.35 m step a walk climbs.
    CY_CHECK_EQ(number_after(move, "jumps="), 1.0);
    CY_CHECK_GT(number_after(move, "airborne="), 20.0);
    CY_CHECK_GT(number_after(move, "peak="), 1.0);
    // Crouched under the beam, refused a stand there, and standing again at the end: the capsule
    // is the standing one and on the ground.
    CY_CHECK_GT(number_after(move, "crouched="), 30.0);
    CY_CHECK_GT(number_after(move, "under_beam="), 0.0);
    CY_CHECK_GT(number_after(move, "stand_refused="), 0.0);
    CY_CHECK_EQ(number_after(move, "height="), 1.8);
    CY_CHECK_EQ(number_after(move, "grounded="), 1.0);
    // North of the beam: it got through.
    const std::string at = text_after(move, "at=");
    CY_CHECK_LT(std::strtod(at.substr(at.rfind(',') + 1).c_str(), nullptr), 3.0);
}

CY_TEST_CASE("first-person: every shot hits what the host's own raycast says it hits") {
    const Session& session = scripted();
    CY_REQUIRE_EQ(session.result.exit_code, 0);
    const double shots = number_after(session.weapon, "shots=");
    CY_CHECK_GE(shots, 8.0);
    CY_CHECK_EQ(shots, number_after(session.module, "pulls="));
    std::istringstream lines(session.result.output);
    std::string line;
    int seen = 0;
    while (std::getline(lines, line)) {
        if (!line.starts_with("fps shot ")) {
            continue;
        }
        ++seen;
        CY_CHECK_EQ(number_after(line, "confirmed="), 1.0);
        CY_CHECK_NE(number_after(line, "kind="), 0.0);  // the script fires only on its mark
    }
    CY_CHECK_EQ(static_cast<double>(seen), shots);
    // Shots by what they hit: five on targets (three on T1, two on T0), the crate, and the wall and
    // the floor.
    CY_CHECK_EQ(number_after(session.weapon, "target_hits="), 5.0);
    CY_CHECK_EQ(number_after(session.weapon, "prop_hits="), 1.0);
    CY_CHECK_EQ(number_after(session.weapon, "world_hits="), 2.0);
    CY_CHECK_EQ(number_after(session.weapon, "misses="), 0.0);
}

CY_TEST_CASE("first-person: what a hit does, and what a shot leaves behind") {
    const Session& session = scripted();
    CY_REQUIRE_EQ(session.result.exit_code, 0);
    const double shots = number_after(session.weapon, "shots=");
    const double hits = shots - number_after(session.weapon, "misses=");
    // T1, hit three times by 40, is down: the AI's `fall` ran and the body lies.
    const std::string t1 = session.target("T1");
    CY_CHECK_EQ(text_after(t1, "mode="), "down");
    CY_CHECK_EQ(number_after(t1, "hits="), 3.0);
    CY_CHECK_EQ(number_after(t1, "felt="), 3.0);
    CY_CHECK_LE(number_after(t1, "points="), 0.0);
    CY_CHECK_EQ(number_after(t1, "fall="), 1.0);
    CY_CHECK_EQ(number_after(session.weapon, "kills="), 1.0);
    // T0, hit twice, is hurt: it flinched and engages.
    const std::string t0 = session.target("T0");
    CY_CHECK_EQ(number_after(t0, "hits="), 2.0);
    CY_CHECK_EQ(number_after(t0, "points="), 20.0);
    CY_CHECK_GT(number_after(t0, "flinch="), 0.0);
    CY_CHECK_GT(number_after(t0, "engage="), 0.0);
    CY_CHECK_EQ(text_after(t0, "mode="), "engage");
    // The crate the shot pushed flew; the rest of the stack moved less or not at all.
    CY_CHECK_GT(number_after(session.props, "moved="), -1.0);
    const std::string moved = text_after(session.props, "moved=");
    double crate[4] = {};
    const char* cursor = moved.c_str();
    for (double& distance : crate) {
        char* end = nullptr;
        distance = std::strtod(cursor, &end);
        cursor = *end == ',' ? end + 1 : end;
    }
    CY_CHECK_GT(crate[2], 0.5);
    CY_CHECK_EQ(number_after(session.weapon, "impulses="), 1.0);
    // A flash, a burst and a kick a shot; sparks a hit; a scorch a hit on the arena; and the
    // engine's pools counted what the game asked for.
    const std::string& fx = session.effects;
    CY_CHECK_EQ(number_after(fx, "flashes="), shots);
    CY_CHECK_EQ(number_after(fx, "lights_spawned="), shots);
    CY_CHECK_EQ(number_after(fx, "bursts="), shots);
    CY_CHECK_EQ(number_after(fx, "muzzle="), shots);
    CY_CHECK_EQ(number_after(session.weapon, "kicks="), shots);
    CY_CHECK_EQ(number_after(fx, "sparks="), hits);
    CY_CHECK_EQ(number_after(fx, "spark_plays="), hits);
    CY_CHECK_EQ(number_after(fx, "played="), shots + hits);
    CY_CHECK_EQ(number_after(fx, "marks="), number_after(session.weapon, "world_hits="));
    CY_CHECK_EQ(number_after(fx, "decals="), number_after(fx, "marks="));
    CY_CHECK_EQ(number_after(fx, "decals_live="), number_after(fx, "marks="));
    CY_CHECK_GT(number_after(fx, "peak_particles="), 20.0);
    CY_CHECK_GT(number_after(fx, "peak_voices="), 0.0);
    // Shot, impact a hit, hit a target hit, down a kill.
    CY_CHECK_EQ(number_after(fx, "sounds="), shots + hits +
                                                 number_after(session.weapon, "target_hits=") +
                                                 number_after(session.weapon, "kills="));
    // The recoil kicked, and the sights came up for T0.
    CY_CHECK_GT(number_after(session.weapon, "peak_recoil="), 0.01);
    CY_CHECK_GT(number_after(session.weapon, "aimed="), 0.0);
    CY_CHECK_GT(number_after(session.weapon, "peak_aim="), 0.9);
    // PRESENTATION ONLY: the effect and the mark the game asked for from a fixed step were refused.
    CY_CHECK_EQ(number_after(session.weapon, "refused="), 2.0);
}

CY_TEST_CASE("first-person: the targets see through the physics world") {
    const Session& session = scripted();
    CY_REQUIRE_EQ(session.result.exit_code, 0);
    CY_CHECK_GT(number_after(session.ai, "traces="), 0.0);
    CY_CHECK_GT(number_after(session.ai, "sightings="), 0.0);
    // Every target patrolled before it saw anything.
    for (const char* name : {"T0", "T1", "T2", "T3"}) {
        CY_CHECK_GT(number_after(session.target(name), "patrol="), 0.0);
    }
}
