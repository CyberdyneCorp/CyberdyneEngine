// SPDX-License-Identifier: MIT
// smoke.rts_api_play — `--play` in a window, on a device: a whole skirmish played by a written hand
// pushed through SDL's queue, drawn at 1920 by 1080 and presented to the window's swapchain.
//
//   the game is played     the hand's session ends in victory: both of red's buildings fall
//   the frame holds 60     the frame — recorded, submitted and waited for — averages under 16.7 ms
//                          at 1920 by 1080 with the GI, the fog, shadows, ambient occlusion, the
//                          skinned soldiers, the particles, the selection outlines and the HUD
//   it is presented        every frame but those a resize interrupts reaches the window, through
//                          two resizes: the swapchain is rebuilt each time and the next frame
//                          presented — a Vulkan swapchain could be resized only once before #149
//   the outlines           the units the player picked are outlined on the device
//   validation             a shorter run under the validation layer reports no error
//
// Skipped, naming why, on a machine with no display or no device (`--play` says "no requested
// graphics device on this machine"), which is every CI runner.

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

[[nodiscard]] std::string quoted(const std::string& path) {
    return "\"" + path + "\"";
}

[[nodiscard]] cy::test::smoke::ProcessResult play(const std::string& arguments) {
    return cy::test::smoke::run(
        quoted(CY_SAMPLE_RTS_API) + " --play --map " + quoted(CY_RTS_MAP_PACKAGE) + " --input " +
        quoted(std::string(CY_RTS_API_PLAY_DIR) + "/skirmish.input") + " " + arguments);
}

constexpr double kBudgetMs = 1000.0 / 60.0;

}  // namespace

CY_TEST_CASE("rts api play on a device: a skirmish played to victory at 1080p within 16.7 ms") {
    const auto run = play("--width 1920 --height 1080 --frames 1750");
    CY_REQUIRE(run.ran);
    CY_REQUIRE_EQ(run.exit_code, 0);
    const std::string session = line_with(run.output, "rts play");
    if (session.empty()) {
        // No display or no device: `--play` said so on its error stream — which is what the
        // suite's skip expression matches — exited zero and ran nothing.
        return;
    }
    CY_TEST_INFO(run.output);
    CY_CHECK_EQ(number_after(session, "window="), 1.0);
    CY_CHECK_EQ(number_after(session, "missed_aims="), 0.0);
    CY_CHECK_EQ(number_after(line_with(run.output, "rts skirmish"), "outcome="), 1.0);
    // Two resizes, the swapchain rebuilt after each, and every frame but those presented.
    const double frames = number_after(session, "frames=");
    CY_CHECK(number_after(session, "resizes=") >= 2.0);
    CY_CHECK(number_after(session, "presented=") >= frames - 4.0);
    CY_CHECK(number_after(session, "outlined=") > 0.0);
    const double frame_ms = number_after(session, "frame_ms=");
    CY_CHECK(frame_ms > 0.0);
    CY_CHECK(frame_ms < kBudgetMs);
}

CY_TEST_CASE("rts api play on a device: the validation layer reports nothing") {
    const auto run = play("--width 1280 --height 720 --frames 300 --validation");
    CY_REQUIRE(run.ran);
    CY_REQUIRE_EQ(run.exit_code, 0);
    const std::string session = line_with(run.output, "rts play");
    if (session.empty()) {
        return;
    }
    CY_TEST_INFO(run.output);
    CY_CHECK_EQ(number_after(session, "presented="), 300.0);
    CY_CHECK_EQ(number_after(session, "validation_errors="), 0.0);
}
