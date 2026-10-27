// SPDX-License-Identifier: MIT
// smoke.third_person_render — samples/14-third-person drawn by the engine's frame.
//
// The host runs as a separate process with its scripted player, three times:
//
//   headless                  the run with no picture: the gameplay report the drawn runs are
//                             held to
//   --render, 480x270         the run drawn — the hillside, the courtyard, the skinned hero, the
//                             sun's shadows, ambient occlusion, the device GI and the temporal
//                             upscaler — with the validation layer on
//   --render, 1920x1080       the same frames without the validation layer, for what a frame costs
//
// What it asserts:
//
//   drawing does not touch the game   every gameplay line of the drawn run is the headless run's,
//                                     its digest included
//   it ran 1200 ticks and drew them   the seven stills, draws from the skinning pass, the forest,
//                                     the upscaler at its render extent, no validation error
//   the golden image                  frame 880 at 480x270 — the hero running down the ramp —
//                                     against `references/third_person_0880.png`, every texel
//   the frame budget                  at 1080p on this machine's device, a frame — recorded,
//                                     submitted and waited for — within 16.7 ms on average: sixty
//                                     frames a second
//
// It leaves the stills (`third-person-*.png`) in the test's binary directory.
// `CY_RENDER_UPDATE_GOLDEN` rewrites the reference, as for every golden in tests/render/.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include "golden.h"
#include "process.h"

namespace {

/// The budget the frame is held to at 1080p: sixty frames a second.
constexpr double kFrameBudgetMs = 1000.0 / 60.0;

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

/// The gameplay half of a report: every `tps ` line but the level's load and the frame's.
[[nodiscard]] std::string gameplay(const std::string& output) {
    std::istringstream lines(output);
    std::string kept;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("tps ") || line.starts_with("tps level ") ||
            line.starts_with("tps render ")) {
            continue;
        }
        kept += line + "\n";
    }
    return kept;
}

[[nodiscard]] cy::test::smoke::ProcessResult run(const std::string& arguments) {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_THIRD_PERSON) + " " + arguments +
                                " 2>&1");
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

}  // namespace

CY_TEST_CASE("samples/14-third-person: the run drawn, its golden and its budget") {
    const std::string directory = CY_TEST_BINARY_DIR;
    const cy::test::smoke::ProcessResult drawn =
        run("--render " + cy::test::smoke::quoted(directory) + " --width 480 --height 270");
    if (drawn.output.find("no requested graphics device on this machine") != std::string::npos) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    const std::string render = line_with(drawn.output, "tps render ");
    std::fprintf(stderr, "%s\n", render.c_str());
    if (drawn.exit_code != 0 || render.empty()) {
        CY_TEST_MESSAGE(drawn.output);
    }
    CY_REQUIRE_EQ(drawn.exit_code, 0);
    CY_REQUIRE(!render.empty());

    // DRAWING DOES NOT TOUCH THE GAME.
    const cy::test::smoke::ProcessResult headless = run("");
    CY_REQUIRE_EQ(headless.exit_code, 0);
    CY_CHECK_EQ(gameplay(drawn.output), gameplay(headless.output));
    CY_CHECK_NE(gameplay(headless.output).find("tps digest "), std::string::npos);

    // 1200 TICKS, DRAWN, the scene below the output and reconstructed.
    CY_CHECK_EQ(number_after(render, "frames="), 1200.0);
    CY_CHECK_EQ(number_after(render, "stills="), 7.0);
    CY_CHECK_EQ(number_after(render, "validation_errors="), 0.0);
    CY_CHECK_GT(number_after(render, "skinned_draws="), 0.0);
    CY_CHECK_GT(number_after(render, "draws="), 150.0);
    CY_CHECK_GT(number_after(render, "plants="), 500.0);
    CY_CHECK_LT(number_after(render, "render_width="), 480.0);
    CY_CHECK_NE(render.find("upscale=quality"), std::string::npos);

    // THE GOLDEN IMAGE.
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::render_test::Image still(allocator);
    CY_REQUIRE(cy::render_test::read_png((directory + "/third-person-0880.png").c_str(), still)
                   .has_value());
    const std::string reference_path =
        std::string(CY_THIRD_PERSON_TEST_DIR) + "/references/third_person_0880.png";
    if (updating_references()) {
        CY_CHECK(cy::render_test::write_png(reference_path.c_str(), still).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path.c_str());
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    cy::render_test::Image reference(allocator);
    CY_REQUIRE(cy::render_test::read_png(reference_path.c_str(), reference).has_value());
    const cy::render_test::Comparison comparison = cy::render_test::compare(reference, still);
    std::fprintf(stderr, "third_person_0880.png: %u texels differ, the largest by %u at (%u, %u)\n",
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    if (comparison.differing != 0U) {
        (void)cy::render_test::write_difference((directory + "/third-person-0880-diff.png").c_str(),
                                                reference, still);
    }

    // THE BUDGET, at 1080p and without the validation layer.
    const cy::test::smoke::ProcessResult timed =
        run("--render " + cy::test::smoke::quoted(directory) + " --no-validation");
    const std::string cost = line_with(timed.output, "tps render ");
    std::fprintf(stderr, "%s\n", cost.c_str());
    CY_REQUIRE_EQ(timed.exit_code, 0);
    CY_CHECK_EQ(number_after(cost, "width="), 1920.0);
    CY_CHECK_GT(number_after(cost, "timed="), 1000.0);
    CY_CHECK_LT(number_after(cost, "frame_ms="), kFrameBudgetMs);
}
