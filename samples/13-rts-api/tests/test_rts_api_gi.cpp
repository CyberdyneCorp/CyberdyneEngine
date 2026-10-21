// SPDX-License-Identifier: MIT
// smoke.rts_api_gi — samples/13-rts-api drawn with the engine's device GI. Issue #35, the live
// half.
//
// The host runs as a separate process with its scripted player, drawing every frame on a device
// (`--render`): once lit by `gi_gpu::GpuGiFrame` over the level's GI, and once without it
// (`--gi off`), the control. What it asserts:
//
//   the GI lights the game's frame      the last frame with the GI differs from the one without it
//                                       in a tenth of its pixels or more
//   the game's ABI 1.9 calls are in it  round the beacon's foot the frame is darker after the Swift
//                                       game put the beacon out and brighter again after it lit
//                                       it; without the GI the beacon's light is not in the frame
//                                       at all, and the three readings agree
//   the cost                            a whole frame — the game, the GI's host update, the scene,
//                                       the GI's device passes and the readback — under 50 ms, and
//                                       the GI's own device time under 8 ms
//
// It leaves the stills (`rts-gi-*.png`, `rts-nogi-*.png`) in the test's binary directory.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdlib>
#include <string>

#include "golden.h"
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

struct Drawn {
    cy::test::smoke::ProcessResult process;
    std::string render;
    std::string gi;
    bool skipped = false;
};

[[nodiscard]] Drawn draw(const char* gi) {
    Drawn run;
    run.process =
        cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_RTS_API) + " --render " +
                             cy::test::smoke::quoted(CY_TEST_BINARY_DIR) + " --gi " + gi + " 2>&1");
    run.skipped = run.process.output.find("no requested graphics device on this machine") !=
                  std::string::npos;
    run.render = line_with(run.process.output, "rts render ");
    run.gi = line_with(run.process.output, "rts gi ");
    return run;
}

[[nodiscard]] cy::usize differing(const char* a, const char* b) {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::render_test::Image left(allocator);
    cy::render_test::Image right(allocator);
    const std::string directory = CY_TEST_BINARY_DIR;
    CY_REQUIRE(cy::render_test::read_png((directory + a).c_str(), left).has_value());
    CY_REQUIRE(cy::render_test::read_png((directory + b).c_str(), right).has_value());
    const cy::render_test::Comparison comparison = cy::render_test::compare(left, right);
    CY_REQUIRE(comparison.comparable);
    return comparison.differing;
}

}  // namespace

CY_TEST_CASE("samples/13-rts-api: the Swift game's frame lit by the device GI, and what it costs") {
    const Drawn lit = draw("on");
    if (lit.skipped) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    std::fprintf(stderr, "%s\n%s\n", lit.render.c_str(), lit.gi.c_str());
    CY_REQUIRE_EQ(lit.process.exit_code, 0);
    CY_REQUIRE(!lit.render.empty());
    const Drawn plain = draw("off");
    std::fprintf(stderr, "%s\n", plain.render.c_str());
    CY_REQUIRE_EQ(plain.process.exit_code, 0);
    CY_REQUIRE(!plain.render.empty());

    CY_CHECK_EQ(number_after(lit.render, "gi="), 1.0);
    CY_CHECK_EQ(number_after(plain.render, "gi="), 0.0);
    CY_CHECK_EQ(number_after(lit.render, "stills="), 4.0);
    CY_CHECK_EQ(number_after(lit.render, "validation_errors="), 0.0);
    CY_CHECK_EQ(number_after(plain.render, "validation_errors="), 0.0);

    // THE GI LIGHTS THE FRAME: the last frame with it and without it.
    const cy::usize changed = differing("/rts-gi-420.png", "/rts-nogi-420.png");
    std::fprintf(stderr, "the last frame with the GI differs from the one without in %zu pixels\n",
                 changed);
    CY_CHECK_GT(changed, static_cast<cy::usize>(480U * 270U / 10U));

    // THE GAME'S CALLS ARE IN THE FRAME: the beacon put out at 101 and lit again at 201.
    const double on = number_after(lit.render, "beacon_lit=");
    const double out = number_after(lit.render, "beacon_dark=");
    const double again = number_after(lit.render, "beacon_relit=");
    CY_CHECK_GT(on, out * 1.05);
    CY_CHECK_GT(again, out * 1.05);
    // Without the GI the beacon's light never reaches the frame: the three readings are the
    // frame's direct light and sky, unmoved by the game's switch.
    const double plain_on = number_after(plain.render, "beacon_lit=");
    const double plain_out = number_after(plain.render, "beacon_dark=");
    CY_CHECK_LT(plain_on - plain_out, (0.02 * plain_on) + 0.5);
    CY_CHECK_LT(plain_out - plain_on, (0.02 * plain_on) + 0.5);

    // AND THE GAME READ IT: in the drawn run the caches are shaded and gathered on the device, and
    // the light the game reads through ABI 1.9 is theirs — out, it falls; lit again, it returns.
    const double read_lit = number_after(lit.gi, "lit=");
    const double read_dark = number_after(lit.gi, "dark=");
    CY_CHECK_GT(read_lit, read_dark * 1.25);
    CY_CHECK_GT(number_after(lit.gi, "relit="), read_dark * 1.25);

    // THE COST, in the game's own frame at 480x270: the game's frame (the GI's host update in it)
    // and the drawn frame together hold 20 frames a second with room to spare.
    CY_CHECK_LT(number_after(lit.render, "frame_ms=") + number_after(lit.render, "game_ms="), 50.0);
    CY_CHECK_LT(number_after(lit.render, "gi_device_ms="), 8.0);
    CY_CHECK_GT(number_after(lit.render, "gi_device_ms="), 0.0);
}
