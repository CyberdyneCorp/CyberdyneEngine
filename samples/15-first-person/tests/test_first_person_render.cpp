// SPDX-License-Identifier: MIT
// smoke.first_person_render — samples/15-first-person drawn on a device.
//
// The host runs as a separate process with its scripted player:
//
//   --render, 480x270              the session drawn through the engine's frame with validation
//                                  on: the GI, the upscaler, shadows, ambient occlusion, the
//                                  weapon view, the decals, the particles and the flashes
//   --render, 480x270, controls    the same session without the decals, and without the particles
//   headless                       the same session with no picture
//   --render, 1920x1080            without the validation layer, for what a frame costs
//
// What it asserts:
//
//   drawing does not touch the game   the drawn run's game report is the headless run's, digest
//                                     and all
//   the frame drew what the game made  the weapon view's five parts through their own lens, the
//                                     marks, the sparks and the flash, with no validation error
//   a mark is where the raycast hit   every texel the decals change lies within a few pixels of
//                                     where a mark's centre — the game's raycast hit point —
//                                     projects, and every mark changed some texel there
//   the sparks are where it hit       the frame after the wall is struck, the particles change
//                                     texels at the hit point's projection
//   the golden image                  frame 450 at 480x270 against `references/fps_450.png`
//   the frame budget                  at 1080p on this machine's device, the game's frame and the
//                                     drawn frame together within 16.7 ms on average: sixty
//                                     frames a second
//
// It leaves the stills (`fps-*.png`) in the test's binary directory. `CY_RENDER_UPDATE_GOLDEN`
// rewrites the reference, as for every golden in tests/render/.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "golden.h"
#include "process.h"

namespace {

/// The budget the frame is held to at 1080p: sixty frames a second.
constexpr double kFrameBudgetMs = 1000.0 / 60.0;
/// How far from a mark's projected centre the texels it changes may lie, at 480x270: the scorch's
/// 0.14 m radius seen from 1.5 m is about 25 pixels, and its noisy rim and the upscaler's kernel a
/// few more.
constexpr double kMarkRadiusPixels = 40.0;

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

/// Every `fps ` line but the renderer's own: the game's report.
[[nodiscard]] std::string gameplay(const std::string& output) {
    std::istringstream lines(output);
    std::string kept;
    std::string line;
    while (std::getline(lines, line)) {
        if (line.starts_with("fps ") && !line.starts_with("fps render ") &&
            !line.starts_with("fps mark ")) {
            kept += line + "\n";
        }
    }
    return kept;
}

/// The projected points the renderer printed at `frame`: the marks' centres, or the last shot's.
[[nodiscard]] std::vector<std::pair<double, double>> marks_at(const std::string& output,
                                                              unsigned frame, const char* kind) {
    std::vector<std::pair<double, double>> points;
    std::istringstream lines(output);
    std::string line;
    const std::string wanted = "frame=" + std::to_string(frame) + " " + kind;
    while (std::getline(lines, line)) {
        if (line.starts_with("fps mark ") && line.find(wanted) != std::string::npos) {
            points.emplace_back(number_after(line, " x="), number_after(line, " y="));
        }
    }
    return points;
}

[[nodiscard]] cy::test::smoke::ProcessResult run(const std::string& arguments) {
    return cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_FIRST_PERSON) + " " + arguments +
                                " 2>&1");
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// A still the run wrote, read back.
class Still {
public:
    explicit Still(const std::string& path)
        : image_(cy::system_allocator(cy::MemoryDomain::Renderer)) {
        CY_REQUIRE(cy::render_test::read_png(path.c_str(), image_).has_value());
    }
    [[nodiscard]] const cy::render_test::Image& image() const noexcept { return image_; }

private:
    cy::render_test::Image image_;
};

/// The texels of two stills that differ by more than a rounding step, as (x, y).
[[nodiscard]] std::vector<std::pair<int, int>> differing(const cy::render_test::Image& a,
                                                         const cy::render_test::Image& b) {
    std::vector<std::pair<int, int>> out;
    CY_REQUIRE_EQ(a.width, b.width);
    CY_REQUIRE_EQ(a.height, b.height);
    for (cy::u32 y = 0; y < a.height; ++y) {
        for (cy::u32 x = 0; x < a.width; ++x) {
            const cy::u32 left = a.texels[(y * a.width) + x];
            const cy::u32 right = b.texels[(y * b.width) + x];
            int largest = 0;
            for (cy::u32 channel = 0; channel < 3U; ++channel) {
                const int l = static_cast<int>((left >> (channel * 8U)) & 0xFFU);
                const int r = static_cast<int>((right >> (channel * 8U)) & 0xFFU);
                largest = std::max(largest, std::abs(l - r));
            }
            if (largest > 2) {
                out.emplace_back(static_cast<int>(x), static_cast<int>(y));
            }
        }
    }
    return out;
}

[[nodiscard]] double nearest(std::pair<int, int> texel,
                             const std::vector<std::pair<double, double>>& points) {
    double best = 1e30;
    for (const auto& [x, y] : points) {
        best = std::min(best, std::hypot(texel.first - x, texel.second - y));
    }
    return best;
}

}  // namespace

CY_TEST_CASE("samples/15-first-person: drawn, its marks where the shots hit, golden and budget") {
    const std::string root = CY_TEST_BINARY_DIR;
    const std::string drawn_dir = root + "/fps-drawn";
    const std::string no_decals_dir = root + "/fps-no-decals";
    const std::string no_particles_dir = root + "/fps-no-particles";
    for (const std::string& directory : {drawn_dir, no_decals_dir, no_particles_dir}) {
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
    }
    const std::string small = " --width 480 --height 270";
    const cy::test::smoke::ProcessResult drawn =
        run("--render " + cy::test::smoke::quoted(drawn_dir) + small);
    if (drawn.output.find("no requested graphics device on this machine") != std::string::npos) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    const std::string render = line_with(drawn.output, "fps render ");
    std::fprintf(stderr, "%s\n", render.c_str());
    CY_REQUIRE_EQ(drawn.exit_code, 0);
    CY_REQUIRE(!render.empty());

    // DRAWING DOES NOT TOUCH THE GAME.
    const cy::test::smoke::ProcessResult headless = run("--headless");
    CY_REQUIRE_EQ(headless.exit_code, 0);
    CY_CHECK_EQ(gameplay(drawn.output), gameplay(headless.output));

    // THE FRAME DREW WHAT THE GAME MADE, with no validation error.
    CY_CHECK_EQ(number_after(render, "frames="), 720.0);
    CY_CHECK_EQ(number_after(render, "stills="), 7.0);
    CY_CHECK_EQ(number_after(render, "validation_errors="), 0.0);
    CY_CHECK_EQ(number_after(render, "viewmodel_draws="), 5.0);
    CY_CHECK_EQ(number_after(render, "decals="), 2.0);
    CY_CHECK_GT(number_after(render, "particles="), 20.0);
    CY_CHECK_GE(number_after(render, "lights="), 2.0);
    CY_CHECK_LT(number_after(render, "render_width="), 480.0);

    // A MARK IS WHERE THE RAYCAST HIT: the frame after the floor is struck both marks are on
    // screen — the wall's and the floor's — and the frame without the decals differs from it only
    // round where the marks' centres project, and round each.
    const cy::test::smoke::ProcessResult no_decals =
        run("--render " + cy::test::smoke::quoted(no_decals_dir) + small + " --no-decals");
    CY_REQUIRE_EQ(no_decals.exit_code, 0);
    const std::vector<std::pair<double, double>> marks = marks_at(drawn.output, 428, "decal=");
    CY_REQUIRE_EQ(marks.size(), 2U);
    const std::vector<std::pair<int, int>> marked = differing(
        Still(drawn_dir + "/fps-428.png").image(), Still(no_decals_dir + "/fps-428.png").image());
    CY_CHECK_GT(marked.size(), 20U);
    for (const auto& texel : marked) {
        CY_CHECK_LE(nearest(texel, marks), kMarkRadiusPixels);
    }
    for (const auto& mark : marks) {
        bool touched = false;
        for (const auto& texel : marked) {
            touched = touched || std::hypot(texel.first - mark.first, texel.second - mark.second) <
                                     kMarkRadiusPixels;
        }
        CY_CHECK(touched);
    }

    // THE SPARKS ARE WHERE IT HIT: the frame after the wall is struck, the particles change texels
    // at the hit point's projection.
    const cy::test::smoke::ProcessResult no_particles =
        run("--render " + cy::test::smoke::quoted(no_particles_dir) + small + " --no-particles");
    CY_REQUIRE_EQ(no_particles.exit_code, 0);
    const std::vector<std::pair<double, double>> struck = marks_at(drawn.output, 415, "shot=");
    CY_REQUIRE_EQ(struck.size(), 1U);
    const std::vector<std::pair<int, int>> sparked =
        differing(Still(drawn_dir + "/fps-415.png").image(),
                  Still(no_particles_dir + "/fps-415.png").image());
    std::size_t at_hit = 0;
    for (const auto& texel : sparked) {
        at_hit += nearest(texel, struck) < 12.0 ? 1U : 0U;
    }
    CY_CHECK_GT(at_hit, 0U);

    // THE GOLDEN IMAGE.
    const Still drawn_still(drawn_dir + "/fps-450.png");
    const cy::render_test::Image& still = drawn_still.image();
    const std::string reference_path =
        std::string(CY_FIRST_PERSON_TEST_DIR) + "/references/fps_450.png";
    if (updating_references()) {
        CY_CHECK(cy::render_test::write_png(reference_path.c_str(), still).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path.c_str());
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    const Still reference_still(reference_path);
    const cy::render_test::Image& reference = reference_still.image();
    const cy::render_test::Comparison comparison = cy::render_test::compare(reference, still);
    std::fprintf(stderr, "fps_450.png: %u texels differ, the largest by %u at (%u, %u)\n",
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    if (comparison.differing != 0U) {
        (void)cy::render_test::write_difference((root + "/fps-450-diff.png").c_str(), reference,
                                                still);
    }

    // THE BUDGET, at 1080p and without the validation layer: the game's frame and the drawn
    // frame together.
    const cy::test::smoke::ProcessResult timed =
        run("--render " + cy::test::smoke::quoted(root) + " --no-validation");
    const std::string cost = line_with(timed.output, "fps render ");
    std::fprintf(stderr, "%s\n", cost.c_str());
    CY_REQUIRE_EQ(timed.exit_code, 0);
    CY_CHECK_EQ(number_after(cost, "width="), 1920.0);
    CY_CHECK_GT(number_after(cost, "timed="), 600.0);
    CY_CHECK_LT(number_after(cost, "total_ms="), kFrameBudgetMs);
}
