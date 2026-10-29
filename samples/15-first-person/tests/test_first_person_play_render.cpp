// SPDX-License-Identifier: MIT
// smoke.first_person_play — samples/15-first-person PLAYED and DRAWN: the arena in a window on a
// device, the shooting script's hands pushed through the platform. See samples/play/tests/
// play_render.h for the two runs and what each is held to. Vulkan; serial, because one run is timed.

#include <cy/test/test.h>

#include <string>

#include "play_render.h"

namespace {

[[nodiscard]] std::string script() {
    return std::string(CY_FIRST_PERSON_PLAY_DIR) + "/shoot.input";
}

}  // namespace

CY_TEST_CASE("first-person play drawn: every frame presented, valid, and the game unchanged") {
    const cy::test::smoke::ProcessResult drawn = cy::play::test::play_drawn(
        CY_SAMPLE_FIRST_PERSON, script(), "--window 1280x720 --validation");
    if (cy::play::test::skipped(drawn)) {
        return;
    }
    const cy::test::smoke::ProcessResult headless =
        cy::play::test::play(CY_SAMPLE_FIRST_PERSON, script());
    cy::play::test::hold_drawn_to_headless(drawn, headless, "fps weapon");
}

CY_TEST_CASE("first-person play drawn: a frame's work at 1080p is under 16.7 ms") {
    const cy::test::smoke::ProcessResult timed =
        cy::play::test::play_drawn(CY_SAMPLE_FIRST_PERSON, script(), "--fullscreen --no-vsync");
    if (cy::play::test::skipped(timed)) {
        return;
    }
    cy::play::test::hold_to_budget(timed);
}

CY_TEST_CASE("first-person play drawn: a window resized twice keeps presenting every frame") {
    // The swapchain is rebuilt in the handle the presenter holds: on the Vulkan device's old
    // resize — destroy, create under a new handle it never returned — the first resize left the
    // presenter a stale handle and the next frame failed.
    const cy::test::smoke::ProcessResult resized = cy::play::test::play_drawn(
        CY_SAMPLE_FIRST_PERSON, std::string(CY_FIRST_PERSON_PLAY_DIR) + "/resize.input",
        "--window 1280x720 --validation");
    if (cy::play::test::skipped(resized)) {
        return;
    }
    CY_REQUIRE_MESSAGE(resized.exit_code == 0, resized.output);
    const std::string loop = cy::play::test::line_with(resized.output, "play loop");
    const std::string frame = cy::play::test::line_with(resized.output, "play frame");
    CY_CHECK(cy::play::test::number_after(loop, "resizes") >= 2.0);
    CY_CHECK_EQ(cy::play::test::number_after(frame, "presented"),
                cy::play::test::number_after(loop, "frames"));
    CY_CHECK_EQ(cy::play::test::number_after(frame, "validation_errors"), 0.0);
}
