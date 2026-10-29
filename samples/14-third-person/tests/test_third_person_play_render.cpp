// SPDX-License-Identifier: MIT
// smoke.third_person_play — samples/14-third-person PLAYED and DRAWN: the hillside in a window on a
// device, the running script's hands pushed through the platform. See samples/play/tests/
// play_render.h for the two runs and what each is held to. Vulkan; serial, because one run is
// timed.

#include <cy/test/test.h>

#include <string>

#include "play_render.h"

namespace {

[[nodiscard]] std::string script() {
    return std::string(CY_THIRD_PERSON_PLAY_DIR) + "/run.input";
}

}  // namespace

CY_TEST_CASE("third-person play drawn: every frame presented, valid, and the game unchanged") {
    const cy::test::smoke::ProcessResult drawn = cy::play::test::play_drawn(
        CY_SAMPLE_THIRD_PERSON, script(), "--window 1280x720 --validation");
    if (cy::play::test::skipped(drawn)) {
        return;
    }
    const cy::test::smoke::ProcessResult headless =
        cy::play::test::play(CY_SAMPLE_THIRD_PERSON, script());
    cy::play::test::hold_drawn_to_headless(drawn, headless, "tps hero");
}

CY_TEST_CASE("third-person play drawn: a frame's work at 1080p is under 16.7 ms") {
    const cy::test::smoke::ProcessResult timed =
        cy::play::test::play_drawn(CY_SAMPLE_THIRD_PERSON, script(), "--fullscreen --no-vsync");
    if (cy::play::test::skipped(timed)) {
        return;
    }
    cy::play::test::hold_to_budget(timed);
}
