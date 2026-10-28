// SPDX-License-Identifier: MIT
// smoke.rts_api_map — samples/13-rts-api's session drawn on the RTS test map.
//
// The host runs as a separate process with its scripted player, four times:
//
//   headless                      the session with no picture and no map: every fog decision the
//                                 map run makes is held to it
//   headless --map                the session with no picture on the map: its GI boxes and the fog
//                                 grid over its ground, the gameplay report the drawn run is held
//                                 to
//   --render --map, 480x270       the session drawn on the cooked map — loaded through the asset
//                                 system, units skinned and animated, the sun's shadows, ambient
//                                 occlusion, the GI and blue's fog — with validation on
//   --render --map, 1920x1080     the same frames without the validation layer, for what a frame
//                                 costs
//
// What it asserts:
//
//   the map does not touch the game   every line of the gameplay report is the headless map run's,
//                                     the lockstep company's digest is the committed one; against
//                                     the run without a map, every line but the GI's light
//                                     readings, which the map's buildings are part of, and the fog
//                                     grid's extent, digest and the cells blue sees past the
//                                     level's edge, which the map's ground is
//   the fog covers the map            the fog grid is the map's 128 m, and the company's second
//                                     group, far from blue's sight, is not drawn
//   it ran N ticks and drew them      420 frames, the four stills, draws from the skinning pass,
//                                     the forest, and no validation error
//   the golden image                  frame 420 at 480x270 against `references/rts_map_420.png`
//   the frame budget                  at 1080p on this machine's device, a frame — recorded,
//                                     submitted and waited for — within 16.7 ms on average: sixty
//                                     frames a second
//
// and, ABI 1.16, the battle (`--battle`) drawn on the same map:
//
//   the battle, drawn               validation on at 480x270: all 1200 frames, the battle's
//                                   digest the committed one, its effects, particles and lights
//                                   drawn, red's buildings and the battle's figures among the
//                                   draws, the topographic minimap composed and redrawn only where
//                                   blue's fog moved, no validation error
//   the battle's frame budget       at 1080p without the validation layer, a battle of forty units
//                                   — blows, arrows, catapult shot, explosions, smoke and their
//                                   lights — within 16.7 ms a frame on average
//
// It leaves the stills (`rts-map-*.png`) in the test's binary directory. `CY_RENDER_UPDATE_GOLDEN`
// rewrites the reference, as for every golden in tests/render/.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include "battle.h"
#include "golden.h"
#include "process.h"

namespace {

/// The budget the map's frame is held to at 1080p: sixty frames a second.
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

/// The gameplay half of a report: every `rts ` line but the GI's light, the map's and the frame's.
[[nodiscard]] std::string gameplay(const std::string& output) {
    std::istringstream lines(output);
    std::string kept;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("rts ") || line.starts_with("rts gi ") ||
            line.starts_with("rts map ") || line.starts_with("rts render ")) {
            continue;
        }
        kept += line + "\n";
    }
    return kept;
}

/// The gameplay half with what depends on the fog grid's extent taken out of its line — the extent,
/// the digest, and the cells blue sees, which on the map include those past the level's edge: what
/// a run on the map shares with a run on the level alone, whose grid is the level's square.
[[nodiscard]] std::string fog_decisions(const std::string& output) {
    std::string kept = gameplay(output);
    const std::string::size_type line = kept.find("rts fog ");
    if (line == std::string::npos) {
        return kept;
    }
    for (const char* key : {" width=", " digest=", " cells=", " game_cells="}) {
        const std::string::size_type at = kept.find(key, line);
        if (at == std::string::npos) {
            continue;
        }
        const std::string::size_type end = kept.find_first_of(" \n", at + 1);
        kept.erase(at, end - at);
    }
    return kept;
}

[[nodiscard]] cy::test::smoke::ProcessResult run(const std::string& arguments) {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_RTS_API) + " " + arguments +
                                " 2>&1");
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

}  // namespace

CY_TEST_CASE("samples/13-rts-api: the session on the RTS test map, its golden and its budget") {
    const std::string directory = CY_TEST_BINARY_DIR;
    const std::string package = cy::test::smoke::quoted(CY_RTS_MAP_PACKAGE);
    const cy::test::smoke::ProcessResult drawn =
        run("--render " + cy::test::smoke::quoted(directory) + " --map " + package +
            " --width 480 --height 270");
    if (drawn.output.find("no requested graphics device on this machine") != std::string::npos) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    const std::string render = line_with(drawn.output, "rts map render ");
    std::fprintf(stderr, "%s\n%s\n", line_with(drawn.output, "rts map load ").c_str(),
                 render.c_str());
    CY_REQUIRE_EQ(drawn.exit_code, 0);
    CY_REQUIRE(!render.empty());

    // THE MAP DOES NOT TOUCH THE GAME.
    const cy::test::smoke::ProcessResult on_map = run("--map " + package);
    CY_REQUIRE_EQ(on_map.exit_code, 0);
    CY_CHECK_EQ(gameplay(drawn.output), gameplay(on_map.output));
    const cy::test::smoke::ProcessResult headless = run("");
    CY_REQUIRE_EQ(headless.exit_code, 0);
    CY_CHECK_EQ(fog_decisions(drawn.output), fog_decisions(headless.output));
    CY_CHECK_NE(fog_decisions(headless.output).find("rts fog "), std::string::npos);

    // THE FOG COVERS THE MAP: the grid is the terrain's 128 m, not the level's 32, and the
    // company's second group — eight units on their field north of the level, far from blue's sight
    // — is withheld on the last frame.
    CY_CHECK_EQ(number_after(line_with(drawn.output, "rts fog "), "width="), 128.0);
    CY_CHECK_EQ(number_after(line_with(headless.output, "rts fog "), "width="), 32.0);
    // Sight is no longer cut off at the level's edge: blue sees at least what it saw on the level.
    CY_CHECK_GE(number_after(line_with(drawn.output, "rts fog "), " cells="),
                number_after(line_with(headless.output, "rts fog "), " cells="));
    CY_CHECK_GE(number_after(render, "enemies_hidden="), 8.0);
    const std::string lockstep = line_with(drawn.output, "rts lockstep ");
    CY_CHECK_NE(lockstep.find("digest=a61e5414c179f54a follower=a61e5414c179f54a"),
                std::string::npos);

    // N TICKS, DRAWN.
    CY_CHECK_EQ(number_after(render, "frames="), 420.0);
    CY_CHECK_EQ(number_after(render, "stills="), 4.0);
    CY_CHECK_EQ(number_after(render, "validation_errors="), 0.0);
    CY_CHECK_GT(number_after(render, "skinned_draws="), 0.0);
    CY_CHECK_GT(number_after(render, "draws="), 300.0);
    CY_CHECK_GT(number_after(render, "plants="), 500.0);

    // THE GOLDEN IMAGE.
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::render_test::Image still(allocator);
    CY_REQUIRE(
        cy::render_test::read_png((directory + "/rts-map-420.png").c_str(), still).has_value());
    const std::string reference_path =
        std::string(CY_RTS_API_TEST_DIR) + "/references/rts_map_420.png";
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
    std::fprintf(stderr, "rts_map_420.png: %u texels differ, the largest by %u at (%u, %u)\n",
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    if (comparison.differing != 0U) {
        (void)cy::render_test::write_difference((directory + "/rts-map-420-diff.png").c_str(),
                                                reference, still);
    }

    // THE BUDGET, at 1080p and without the validation layer.
    const cy::test::smoke::ProcessResult timed =
        run("--render " + cy::test::smoke::quoted(directory) + " --map " + package +
            " --no-validation");
    const std::string cost = line_with(timed.output, "rts map render ");
    std::fprintf(stderr, "%s\n", cost.c_str());
    CY_REQUIRE_EQ(timed.exit_code, 0);
    CY_CHECK_EQ(number_after(cost, "width="), 1920.0);
    CY_CHECK_GT(number_after(cost, "timed="), 300.0);
    CY_CHECK_LT(number_after(cost, "frame_ms="), kFrameBudgetMs);
}

CY_TEST_CASE("samples/13-rts-api: the battle on the RTS test map, drawn, and its frame budget") {
    const std::string directory = std::string(CY_TEST_BINARY_DIR) + "/battle";
    std::error_code made;
    std::filesystem::create_directories(directory, made);
    CY_REQUIRE(!made);
    const std::string package = cy::test::smoke::quoted(CY_RTS_MAP_PACKAGE);
    const cy::test::smoke::ProcessResult drawn =
        run("--battle --render " + cy::test::smoke::quoted(directory) + " --map " + package +
            " --width 480 --height 270");
    if (drawn.output.find("no requested graphics device on this machine") != std::string::npos) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    const std::string render = line_with(drawn.output, "rts map render ");
    const std::string look = line_with(drawn.output, "rts map battle ");
    const std::string battle = line_with(drawn.output, "rts battle ");
    std::fprintf(stderr, "%s\n%s\n", render.c_str(), look.c_str());
    CY_REQUIRE_EQ(drawn.exit_code, 0);
    CY_REQUIRE(!render.empty());
    CY_REQUIRE(!look.empty());

    // THE DRAWING DOES NOT TOUCH THE BATTLE: the session ends in the committed digest.
    char committed[32];
    (void)std::snprintf(committed, sizeof(committed), "digest=%016llx",
                        static_cast<unsigned long long>(sample::rts::battle::kBattleDigest));
    CY_CHECK_NE(battle.find(committed), std::string::npos);
    CY_CHECK_EQ(number_after(battle, "standing="), 0.0);

    // ALL OF IT, DRAWN: every frame, the four stills, a figure for every unit and red's buildings
    // among the draws, the effects' particles and the blasts' lights, and no validation error.
    CY_CHECK_EQ(number_after(render, "frames="), 1200.0);
    CY_CHECK_EQ(number_after(render, "stills="), 4.0);
    CY_CHECK_EQ(number_after(render, "validation_errors="), 0.0);
    CY_CHECK_EQ(number_after(look, "figures="), 58.0);
    CY_CHECK_GT(number_after(look, "particles_peak="), 100.0);
    CY_CHECK_GT(number_after(look, "lights_peak="), 0.0);
    // The topographic minimap: composed every sixth frame, its fog redrawn where it moved.
    CY_CHECK_EQ(number_after(look, "minimap_composed="), 200.0);
    CY_CHECK_GT(number_after(look, "minimap_fog_passes="), 0.0);

    // THE BUDGET: forty units fighting, at 1080p and without the validation layer.
    const cy::test::smoke::ProcessResult timed =
        run("--battle --render " + cy::test::smoke::quoted(directory) + " --map " + package +
            " --no-validation");
    const std::string cost = line_with(timed.output, "rts map render ");
    std::fprintf(stderr, "%s\n%s\n", cost.c_str(),
                 line_with(timed.output, "rts map battle ").c_str());
    CY_REQUIRE_EQ(timed.exit_code, 0);
    CY_CHECK_EQ(number_after(cost, "width="), 1920.0);
    CY_CHECK_GT(number_after(cost, "timed="), 1000.0);
    CY_CHECK_LT(number_after(cost, "frame_ms="), kFrameBudgetMs);
}
