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
// and the negative control: the same host with no `Commander` does none of it.

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

    [[nodiscard]] bool parsed() const noexcept {
        return !module.empty() && !camera.empty() && !select.empty() && !order.empty() &&
               !audio.empty() && !spawn.empty();
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

    CY_CHECK_EQ(number_after(run.module, "behaviours="), 1.0);
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

    // The build key made a third worker through the prefab, and it is a navigation agent with a
    // body like the first two.
    CY_CHECK_EQ(number_after(run.spawn, "spawns="), 1.0);
    CY_CHECK_EQ(number_after(run.spawn, "workers="), 3.0);
    CY_CHECK_EQ(number_after(run.spawn, "agents="), 3.0);
    CY_CHECK_EQ(number_after(run.spawn, "units="), 3.0);
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
}
