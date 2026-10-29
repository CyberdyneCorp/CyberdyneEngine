// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_TESTS_PLAY_RENDER_H
#define CY_SAMPLE_PLAY_TESTS_PLAY_RENDER_H
// play_render.h — a played sample DRAWN, in a window on a device: the two runs every such smoke
// suite makes, and what it holds of each.
//
//   drawn     the validation layer on, a 1280x720 window: every frame presented, no validation
//             error, and the game's report the headless play run's — the picture, the swapchain and
//             the interface over it change nothing the game does
//   timed     full screen at 1920x1080 — the frame's 1080p, presented at 1080p — v-sync off, no
//             validation layer: the frame's work, the ticks, the game's frame and the draw with the
//             device's time for it, under 16.7 ms on average: sixty frames a second
//
// A machine with no display or no device skips, saying so: the sample prints `--play: skipped:`.

#include <cy/test/test.h>

#include <cstdio>
#include <string>

#include "play_report.h"

namespace cy::play::test {

/// Sixty frames a second.
inline constexpr double kFrameBudgetMs = 16.7;

/// A drawn run: `sample` played in a window with `script` and `extra`.
[[nodiscard]] inline cy::test::smoke::ProcessResult play_drawn(const char* sample,
                                                               const std::string& script,
                                                               const std::string& extra) {
    return cy::test::smoke::run(cy::test::smoke::quoted(sample) +
                                " --play --fixed-clock --no-focus-pause " +
                                "--input-script " + cy::test::smoke::quoted(script) + " " + extra +
                                " 2>&1");
}

/// True, and the sample's reason printed, when the machine cannot play drawn.
[[nodiscard]] inline bool skipped(const cy::test::smoke::ProcessResult& result) {
    if (result.output.find("--play: skipped:") == std::string::npos) {
        return false;
    }
    std::fputs(result.output.c_str(), stdout);
    return true;
}

/// The drawn run held to the headless one: every frame presented, no validation error, and the
/// lines of the game's report that start with `prefix` identical.
inline void hold_drawn_to_headless(const cy::test::smoke::ProcessResult& drawn,
                                   const cy::test::smoke::ProcessResult& headless,
                                   const char* game_line) {
    CY_REQUIRE_MESSAGE(drawn.exit_code == 0, drawn.output);
    CY_REQUIRE_MESSAGE(headless.exit_code == 0, headless.output);
    const std::string loop = line_with(drawn.output, "play loop");
    const std::string frame = line_with(drawn.output, "play frame");
    CY_CHECK_EQ(number_after(frame, "presented"), number_after(loop, "frames"));
    CY_CHECK_EQ(number_after(frame, "validation_errors"), 0.0);
    // A window on a desktop holds the mouse — relative mode — while the game runs.
    CY_CHECK_EQ(number_after(loop, "relative_mouse"), 1.0);
    CY_CHECK_EQ(line_with(drawn.output, game_line), line_with(headless.output, game_line));
}

/// The timed run: the frame's work under the budget.
inline void hold_to_budget(const cy::test::smoke::ProcessResult& timed) {
    CY_REQUIRE_MESSAGE(timed.exit_code == 0, timed.output);
    const std::string loop = line_with(timed.output, "play loop");
    const std::string frame = line_with(timed.output, "play frame");
    std::printf("%s\n%s\n", loop.c_str(), frame.c_str());
    CY_CHECK_EQ(number_after(loop, "window"), 1920.0);
    CY_CHECK(number_after(frame, "timed") > 100.0);
    CY_CHECK_LT(number_after(frame, "work_ms"), kFrameBudgetMs);
}

}  // namespace cy::play::test

#endif  // CY_SAMPLE_PLAY_TESTS_PLAY_RENDER_H
