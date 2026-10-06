// SPDX-License-Identifier: MIT
// integration.rts_api_sample — samples/13-rts-api, end to end, headless. `add-swift-game-api`.
//
// The host runs as a separate process with its scripted player: synthetic key presses and pointer
// events through `InputServer::inject`. Every decision is the Swift module's, so every assertion
// here is about something the game did through an ABI 1.3 entry:
//
//   the camera panned            input_action_state, input_pointer, camera_view, camera_set_target
//   the clicked unit is selected input_pointer, camera_screen_to_ray, physics_raycast (unit layer)
//   it reached the ground point  physics_raycast (ground layer), nav_agent_move_to, nav_agent_state
//   the arrival cue fired        nav_agent_state's one-tick event, audio_find_cue, audio_play
//   the build key spawned        input_action_state, spawn_resolve, spawn_instantiate,
//   nav_agent_configure
//
// and, at ABI 1.5:
//
//   the tree reached Swift       the scene tree's pump -> enter_tree, ready; @Node -> node_find
//   a Swift system was scheduled register_system; ScriptSystems ran `trainUnits` every fixed tick,
//                                ordered against a native reader of the same column
//   the hero walked and jumped   character_create, character_move, character_state
//   the crate was kicked         physics_apply_impulse, physics_get_velocity
//
// and, at ABI 1.6:
//
//   the game built its HUD       ui_root, ui_create, ui_set_layout, ui_set_style, ui_set_text,
//                                ui_set_progress, ui_set_visibility — from Swift's CyberdyneKit
//   the HUD follows the game     the store's texts, rows, health fill and minimap dots after 420
//                                frames are what the game's state says
//   the Build click built        the adapter routed a press and a release on the button into a
//                                click, ui_event delivered it to the commander, a worker followed
//   the click was the HUD's      ui_hit_test kept it from deselecting the unit beneath
//
// and, at ABI 1.7:
//
//   the units animate            animation_attach over the host's cooked `worker` rig, then
//                                animation_play from the agent's state: walk while it follows a
//                                path, a cheer on arrival, idle again on the cheer's own event
//                                (animation_events); the engine's AnimationSystem ran them
//
// and the negative controls: the same host with no behaviours does none of the behaviour work and
// animates nothing, the same host with the systems left out of the schedule never runs
// `trainUnits`, and the same host with no interface has no HUD and its Build click lands on the
// world.

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

[[nodiscard]] unsigned long long entity_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return 0;
    }
    return std::strtoull(line.c_str() + found + std::char_traits<char>::length(key), nullptr, 10);
}

struct Run {
    cy::test::smoke::ProcessResult process;
    std::string module;
    std::string camera;
    std::string select;
    std::string order;
    std::string audio;
    std::string spawn;
    std::string tree;
    std::string system;
    std::string hero;
    std::string body;
    std::string hud;
    std::string anim;

    [[nodiscard]] bool parsed() const noexcept {
        return !module.empty() && !camera.empty() && !select.empty() && !order.empty() &&
               !audio.empty() && !spawn.empty() && !tree.empty() && !system.empty() &&
               !hero.empty() && !body.empty() && !hud.empty() && !anim.empty();
    }
};

[[nodiscard]] Run run_sample(const std::string& arguments) {
    Run run;
    run.process =
        cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_RTS_API) + arguments + " 2>&1");
    run.module = line_with(run.process.output, "rts module ");
    run.camera = line_with(run.process.output, "rts camera ");
    run.select = line_with(run.process.output, "rts select ");
    run.order = line_with(run.process.output, "rts order ");
    run.audio = line_with(run.process.output, "rts audio ");
    run.spawn = line_with(run.process.output, "rts spawn ");
    run.tree = line_with(run.process.output, "rts tree ");
    run.system = line_with(run.process.output, "rts system ");
    run.hero = line_with(run.process.output, "rts hero ");
    run.body = line_with(run.process.output, "rts body ");
    run.hud = line_with(run.process.output, "rts hud ");
    run.anim = line_with(run.process.output, "rts anim ");
    return run;
}

/// The whole output, when a run did not end in a parseable report.
void report_if_broken(const Run& run) {
    if (!run.process.ran || run.process.exit_code != 0 || !run.parsed()) {
        CY_TEST_MESSAGE(run.process.output);
    }
}

}  // namespace

CY_TEST_CASE("samples/13-rts-api: a Swift RTS selects, orders, hears and builds through ABI 1.3") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    CY_CHECK_EQ(number_after(run.module, "behaviours="), 2.0);  // the commander and the scout
    CY_CHECK_EQ(number_after(run.module, "missed_aims="), 0.0);

    // The camera: 30 frames of D at the game's 12 m/s is +6 m, 30 frames against the left edge is
    // -6 m. Loose bounds, because the claim is the direction and that both inputs reached it.
    CY_CHECK(number_after(run.camera, "keyboard=") > 4.0);
    CY_CHECK(number_after(run.camera, "edge=") < -4.0);

    // The click on the second unit selected that unit and not the other one.
    const unsigned long long clicked = entity_after(run.select, "clicked=");
    CY_CHECK(clicked != 0ULL);
    CY_CHECK_EQ(entity_after(run.select, "selected="), clicked);

    // One order, the unit arrived within its arrival distance of the clicked ground point, and the
    // unit that was not selected stayed where it was.
    CY_CHECK_EQ(number_after(run.order, "orders="), 1.0);
    CY_CHECK(number_after(run.order, "arrived_frame=") > 92.0);
    CY_CHECK(number_after(run.order, "miss=") >= 0.0);
    CY_CHECK(number_after(run.order, "miss=") < 0.5);
    CY_CHECK_EQ(number_after(run.order, "bystander_moved="), 0.0);

    // The arrival was heard: the game saw one arrival event, the audio adapter accepted the cue,
    // and the audio server had a voice playing.
    CY_CHECK_EQ(number_after(run.audio, "arrivals="), 1.0);
    CY_CHECK_EQ(number_after(run.audio, "cues="), 1.0);
    CY_CHECK(number_after(run.audio, "peak_voices=") >= 1.0);

    // The build key made a third worker through the prefab, and the HUD's Build button a fourth;
    // each is a navigation agent with a body like the first two.
    CY_CHECK_EQ(number_after(run.spawn, "spawns="), 2.0);
    CY_CHECK_EQ(number_after(run.spawn, "workers="), 4.0);
    CY_CHECK_EQ(number_after(run.spawn, "agents="), 4.0);
    CY_CHECK_EQ(number_after(run.spawn, "units="), 4.0);
}

CY_TEST_CASE("samples/13-rts-api: ABI 1.5 — tree callbacks, a scheduled system, a hero, a push") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    // The commander is a node: the pump delivered onEnterTree and onReady exactly once, and both
    // @Node paths resolved against the level before onReady read them.
    CY_CHECK_EQ(number_after(run.tree, "entered="), 1.0);
    CY_CHECK_EQ(number_after(run.tree, "readied="), 1.0);
    CY_CHECK_EQ(number_after(run.tree, "barracks="), 1.0);
    CY_CHECK_EQ(number_after(run.tree, "crate_found="), 1.0);

    // `trainUnits` is a Swift system the ENGINE ran: once per fixed tick (420 frames, one tick
    // each), over every unit's Veterancy column; the native roll that reads the column saw four
    // units, the first of which has served every tick but the one the roll ran before; and the
    // scheduler ordered the two by their declarations.
    CY_CHECK_EQ(number_after(run.system, "installed="), 1.0);
    CY_CHECK_EQ(number_after(run.system, "runs="), 420.0);
    CY_CHECK_EQ(number_after(run.system, "rows="), 4.0);
    CY_CHECK_EQ(number_after(run.system, "most="), 419.0);
    CY_CHECK_EQ(number_after(run.system, "ordered="), 1.0);

    // The hero walked 120 ticks at 2 m/s from x = 4 (about 4 m), jumped once (it was airborne),
    // and stands on the ground at the end (CY_GROUND_GROUNDED is 0).
    CY_CHECK(number_after(run.hero, "x=") > 7.5);
    CY_CHECK(number_after(run.hero, "x=") < 8.5);
    CY_CHECK_EQ(number_after(run.hero, "airborne="), 1.0);
    CY_CHECK_EQ(number_after(run.hero, "ground="), 0.0);

    // One 100 N s impulse on a 20 kg crate: 5 m/s at once, and it slid along +Z.
    CY_CHECK_EQ(number_after(run.body, "kicks="), 1.0);
    CY_CHECK(number_after(run.body, "crate_speed=") > 4.5);
    CY_CHECK(number_after(run.body, "crate_moved=") > 0.5);
}

CY_TEST_CASE("samples/13-rts-api: ABI 1.7 — units walk, cheer on arrival and stand down") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    // Every unit the game enlisted carries an animator: the two it started with, the one the build
    // key built and the one the HUD's Build button built.
    CY_CHECK_EQ(number_after(run.anim, "animated="), 4.0);
    // The ordered unit walked while its agent followed the path, cheered when it arrived, and was
    // stood down to idle by the cheer's own event — three decisions the game made in Swift.
    CY_CHECK_EQ(number_after(run.anim, "walked="), 1.0);
    CY_CHECK_EQ(number_after(run.anim, "cheered="), 1.0);
    CY_CHECK_EQ(number_after(run.anim, "idle_after="), 1.0);
    // The bystander and the two built units never left idle.
    CY_CHECK_EQ(number_after(run.anim, "only_idle="), 3.0);
    // The pose moved: the walk swings the legs by more than half a radian.
    CY_CHECK(number_after(run.anim, "departure=") > 0.2);
    // The game read the walk's footfalls and the one cheer finishing, each once.
    CY_CHECK(number_after(run.anim, "footsteps=") >= 2.0);
    CY_CHECK_EQ(number_after(run.anim, "cheer_events="), 1.0);
}

CY_TEST_CASE("samples/13-rts-api: with the systems left out of the schedule, none of them runs") {
    const Run control = run_sample(" --no-systems");
    report_if_broken(control);
    CY_REQUIRE(control.process.ran);
    CY_REQUIRE_EQ(control.process.exit_code, 0);
    CY_REQUIRE(control.parsed());

    // Registered with the engine, never installed: the scheduler is what runs `trainUnits`, so
    // without it nothing serves a tick — and nothing else changed.
    CY_CHECK_EQ(number_after(control.system, "installed="), 0.0);
    CY_CHECK_EQ(number_after(control.system, "runs="), 0.0);
    CY_CHECK_EQ(number_after(control.system, "rows="), 4.0);
    CY_CHECK_EQ(number_after(control.system, "most="), 0.0);
    CY_CHECK_EQ(number_after(control.tree, "readied="), 1.0);
    CY_CHECK_EQ(number_after(control.body, "kicks="), 1.0);
}

CY_TEST_CASE("samples/13-rts-api: with no Swift behaviour, the host decides nothing") {
    const Run control = run_sample(" --no-behaviours");
    report_if_broken(control);
    CY_REQUIRE(control.process.ran);
    CY_REQUIRE_EQ(control.process.exit_code, 0);
    CY_REQUIRE(control.parsed());

    // The same host, the same content, the same scripted hand: no units, no camera move, no order,
    // no sound, no spawn. Each of those was the game's decision.
    CY_CHECK_EQ(number_after(control.module, "behaviours="), 0.0);
    CY_CHECK_EQ(number_after(control.camera, "keyboard="), 0.0);
    CY_CHECK_EQ(number_after(control.camera, "edge="), 0.0);
    CY_CHECK_EQ(entity_after(control.select, "selected="), 0ULL);
    CY_CHECK_EQ(number_after(control.audio, "peak_voices="), 0.0);
    CY_CHECK_EQ(number_after(control.spawn, "workers="), 0.0);
    CY_CHECK_EQ(number_after(control.spawn, "agents="), 0.0);
    // No tree callback reached anything, no hero walked, the crate was never pushed, and the
    // scheduled system — registered by the module, installed by the host — had no unit to train.
    CY_CHECK_EQ(number_after(control.tree, "entered="), 0.0);
    CY_CHECK_EQ(number_after(control.tree, "readied="), 0.0);
    CY_CHECK_EQ(number_after(control.hero, "x="), 0.0);
    CY_CHECK_EQ(number_after(control.body, "kicks="), 0.0);
    CY_CHECK(number_after(control.body, "crate_moved=") < 0.01);
    CY_CHECK_EQ(number_after(control.system, "rows="), 0.0);
    // The interface is up, and nobody built anything in it.
    CY_CHECK_EQ(number_after(control.hud, "mounted="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "elements="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "button="), 0.0);
    // The engine's animation system ran every tick and had nothing to animate: no animator, no
    // pose moved from its reference, no event read.
    CY_CHECK_EQ(number_after(control.anim, "animated="), 0.0);
    CY_CHECK_EQ(number_after(control.anim, "departure="), 0.0);
    CY_CHECK_EQ(number_after(control.anim, "footsteps="), 0.0);
    CY_CHECK_EQ(number_after(control.anim, "cheer_events="), 0.0);
}

CY_TEST_CASE("samples/13-rts-api: ABI 1.6 — a Swift HUD follows the game and its button builds") {
    const Run run = run_sample("");
    report_if_broken(run);
    CY_REQUIRE(run.process.ran);
    CY_REQUIRE_EQ(run.process.exit_code, 0);
    CY_REQUIRE(run.parsed());

    // The commander mounted the HUD: the resource bar (7), the minimap (19), the selection panel
    // and its title (2) with three rows of four (12), and the Build button — 41 elements the
    // module made, every one in the store.
    CY_CHECK_EQ(number_after(run.hud, "mounted="), 1.0);
    CY_CHECK_EQ(number_after(run.hud, "elements="), 41.0);
    CY_CHECK_EQ(number_after(run.hud, "button="), 1.0);
    CY_CHECK_EQ(number_after(run.hud, "aimed="), 1.0);

    // The click: the adapter routed one press and release on the button into one click, the
    // engine delivered it to the commander (its `onUIEvent` heard it), the button's action asked
    // for a worker, and the next fixed step built it.
    CY_CHECK_EQ(number_after(run.hud, "clicks="), 1.0);
    CY_CHECK_EQ(number_after(run.hud, "heard="), 1.0);
    CY_CHECK_EQ(number_after(run.hud, "builds="), 1.0);

    // The HUD shows the game as it ended. Two workers cost 50 gold each from 1250; four units of
    // food against a cap of 10; the selected unit is the hurt one of the starting pair.
    const double spawns = number_after(run.spawn, "spawns=");
    CY_CHECK_EQ(spawns, 2.0);
    CY_CHECK(run.hud.find("gold=1150 ") != std::string::npos);
    CY_CHECK(run.hud.find("wood=830 ") != std::string::npos);
    CY_CHECK(run.hud.find("food=4/10 ") != std::string::npos);
    CY_CHECK(run.hud.find("title='Selected: 1 unit'") != std::string::npos);
    CY_CHECK_EQ(number_after(run.hud, "rows="), 1.0);
    CY_CHECK(run.hud.find("health=64/100 ") != std::string::npos);
    // The health bar is 50 wide and 64% full.
    CY_CHECK(number_after(run.hud, "fill=") > 31.99);
    CY_CHECK(number_after(run.hud, "fill=") < 32.01);
    // One dot per unit on the minimap.
    CY_CHECK_EQ(number_after(run.hud, "dots="), number_after(run.spawn, "units="));

    // The click on the button was the HUD's: `UI.hitTest` kept it from reaching the world, so the
    // unit selected at frame 81 is still the one selected.
    const unsigned long long clicked = entity_after(run.select, "clicked=");
    CY_CHECK(clicked != 0ULL);
    CY_CHECK_EQ(entity_after(run.select, "selected="), clicked);
}

CY_TEST_CASE(
    "samples/13-rts-api: with no interface there is no HUD, and the Build click is the "
    "world's") {
    const Run control = run_sample(" --no-ui");
    report_if_broken(control);
    CY_REQUIRE(control.process.ran);
    CY_REQUIRE_EQ(control.process.exit_code, 0);
    CY_REQUIRE(control.parsed());

    // The game found no interface (UNAVAILABLE), said so, and played on without a HUD.
    CY_CHECK_EQ(number_after(control.hud, "mounted="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "button="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "aimed="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "clicks="), 0.0);
    CY_CHECK_EQ(number_after(control.hud, "builds="), 0.0);
    // The same click on the same pixel built nothing: only the key's worker exists.
    CY_CHECK_EQ(number_after(control.spawn, "spawns="), 1.0);
    CY_CHECK_EQ(number_after(control.spawn, "workers="), 3.0);
    // With no HUD to take it, the left click went to the world and found no unit there, so the
    // selection made at frame 81 was dropped.
    CY_CHECK_EQ(entity_after(control.select, "selected="), 0ULL);
    // Everything that is not the interface is unchanged, and the one worker the HUD did not build
    // is one animator fewer.
    CY_CHECK_EQ(number_after(control.anim, "animated="), 3.0);
    CY_CHECK_EQ(number_after(control.order, "orders="), 1.0);
    CY_CHECK_EQ(number_after(control.audio, "arrivals="), 1.0);
    CY_CHECK_EQ(number_after(control.tree, "readied="), 1.0);
}

CY_TEST_CASE("samples/13-rts-api reproduces exactly across two runs") {
    const Run first = run_sample("");
    const Run second = run_sample("");
    CY_REQUIRE(first.parsed());
    CY_REQUIRE(second.parsed());
    CY_CHECK_EQ(first.select, second.select);
    CY_CHECK_EQ(first.order, second.order);
    CY_CHECK_EQ(first.audio, second.audio);
    CY_CHECK_EQ(first.spawn, second.spawn);
    CY_CHECK_EQ(first.system, second.system);
    CY_CHECK_EQ(first.hero, second.hero);
    CY_CHECK_EQ(first.body, second.body);
    CY_CHECK_EQ(first.hud, second.hud);
    CY_CHECK_EQ(first.anim, second.anim);
}
