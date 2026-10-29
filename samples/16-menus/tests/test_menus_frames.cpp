// SPDX-License-Identifier: MIT
// smoke.menus_frames — samples/16-menus drawn on a Vulkan device.
//
// The host runs navigate.script with `--render`: the menus over the RTS level, lit by CyberGI, at
// the display settings the game applies. Its `shot` steps write frames; this test holds them
// against committed goldens and reads the host's frame report:
//
//   the settings reach the renderer   the frame drawn after Apply is 1920x1080 output, rendered at
//                                     the upscaler's internal extent — the report lists both
//   the screens                       the graphics and controls tabs, the inventory with a drag in
//                                     progress and a tooltip, the pause menu over the game, and
//                                     the main menu after a save in the editor — each against a
//                                     golden at a size small enough to commit
//   the frame cost                    mean and worst wall time of a frame — game, interface and
//                                     device — and the interface's own submit time, within budget
//   validation                        no Vulkan validation message
//
// `CY_RENDER_UPDATE_GOLDEN=1` rewrites the goldens.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "golden.h"
#include "process.h"

namespace {

namespace fs = std::filesystem;

[[nodiscard]] double number_after(const std::string& line, const char* key) {
    const std::string marker = std::string(" ") + key + "=";
    const std::string::size_type found = line.find(marker);
    return found == std::string::npos ? -1e30
                                      : std::strtod(line.c_str() + found + marker.size(), nullptr);
}

[[nodiscard]] std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

/// The frames held against `references/<name>.png`: one at each output the run draws at, a modal
/// dialog, and a drag in progress over the lit level. The script writes four more for the guide.
constexpr const char* kGoldens[] = {
    "settings-graphics",
    "settings-rebind",
    "inventory-drag",
    "main-menu-reloaded",
};

/// The mean frame — game, interface and device — within a 60 Hz frame on the machine that drew
/// it, and the interface's own share of it within a millisecond.
constexpr double kFrameBudgetMs = 1000.0 / 60.0;
constexpr double kUiSubmitBudgetMs = 1.0;

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

}  // namespace

CY_TEST_CASE("menus frames: the menus on a device, at the settings the game applied") {
    const fs::path root = fs::path(CY_TEST_BINARY_DIR) / "menus-frames";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "user-data");
    fs::create_directories(root / "shots");
    fs::copy(CY_MENUS_PROJECT, root / "project", fs::copy_options::recursive);
    const std::string command =
        cy::test::smoke::quoted(CY_SAMPLE_MENUS) + " --script " +
        cy::test::smoke::quoted(std::string(CY_MENUS_TEST_DIR) + "/navigate.script") +
        " --project " + cy::test::smoke::quoted((root / "project").string()) + " --user-data " +
        cy::test::smoke::quoted((root / "user-data").string()) + " --render " +
        cy::test::smoke::quoted((root / "shots").string()) + " --shot-size 480x270 2>&1";
    const cy::test::smoke::ProcessResult run = cy::test::smoke::run(command);
    CY_REQUIRE(run.ran);
    if (run.exit_code == 3 &&
        run.output.find("no requested graphics device") != std::string::npos) {
        std::printf("no requested graphics device on this machine\n");
        return;
    }
    CY_REQUIRE_EQ(run.exit_code, 0);
    const std::string report = line_with(run.output, "menus render:");
    CY_REQUIRE_FALSE(report.empty());
    std::printf("%s\n", report.c_str());

    // The output followed the settings: 1280x720 native first, then 1920x1080 at the Quality
    // upscaler's internal extent.
    CY_CHECK(report.find("extents=1280x720@1280x720,1920x1080@1286x724") != std::string::npos);
    CY_CHECK_EQ(number_after(report, "validation"), 0.0);
    CY_CHECK_EQ(number_after(report, "stills"), 8.0);
    CY_CHECK(number_after(report, "frame_ms") < kFrameBudgetMs);
    CY_CHECK(number_after(report, "ui_submit_ms") < kUiSubmitBudgetMs);

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    for (const char* shot : kGoldens) {
        const std::string image = (root / "shots" / (std::string(shot) + ".png")).string();
        const std::string golden = std::string(CY_MENUS_TEST_DIR) + "/references/" + shot + ".png";
        cy::render_test::Image drawn(allocator);
        CY_REQUIRE_MESSAGE(cy::render_test::read_png(image.c_str(), drawn).has_value(), shot);
        if (updating_references()) {
            CY_CHECK(cy::render_test::write_png(golden.c_str(), drawn).has_value());
            std::fprintf(stderr, "wrote %s\n", golden.c_str());
            continue;
        }
        cy::render_test::Image reference(allocator);
        CY_REQUIRE_MESSAGE(cy::render_test::read_png(golden.c_str(), reference).has_value(), shot);
        const cy::render_test::Comparison comparison = cy::render_test::compare(reference, drawn);
        std::fprintf(stderr, "%s: %u texels differ, the largest by %u at (%u, %u)\n", shot,
                     comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                     comparison.worst_y);
        CY_CHECK_MESSAGE(comparison.comparable, shot);
        CY_CHECK_MESSAGE(comparison.differing_off_edge == 0U, shot);
        CY_CHECK_MESSAGE(comparison.differing <= comparison.edge_texels, shot);
    }
    if (updating_references()) {
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
    }
}
