// SPDX-License-Identifier: MIT
// integration.menus_sample — samples/16-menus, end to end, headless.
//
// The host runs as a separate process twice, over a copy of the project and a fresh player
// directory, driven by a scripted player with a gamepad, a keyboard and a mouse (navigate.script,
// persist.script). Every assertion is on what the ENGINE holds after the Swift game acted — the
// focus the toolkit has, the display settings the video adapter took, the GI quality the system
// runs, the buses' gains, the controls the input user resolves — or on the game's own report where
// only the game knows (an inventory move):
//
//   navigation      the d-pad skips a disabled button, enters a screen on its first control, the
//                   shoulder buttons change tabs, B and Escape go back one screen
//   settings        drop-downs changed and applied reach the video adapter (resolution, quality,
//                   upscaler, and the render extent the upscaler draws at) and the GI; a slider is
//                   the bus it binds, with no Apply
//   rebinding       a gamepad and a keyboard rebind through the input system, a conflict swapped,
//                   a rebind cancelled by Escape changing nothing and leaving the screen open
//   persistence     a second start finds the settings and the bindings, enables Continue, and the
//                   rebound controls fire the game's own action
//   inventory       pick up and put down by d-pad, a held item put back by B, a pointer drag
//   hot reload      a document saved over (as the editor saves) changes under the running game

#include <cy/test/test.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "process.h"

namespace {

namespace fs = std::filesystem;

/// The values of one `menus:` line, by key.
class Report {
public:
    explicit Report(std::string line) : line_(std::move(line)) {}

    [[nodiscard]] double number(const char* key) const {
        const std::string::size_type at = find(key);
        return at == std::string::npos ? -1e30 : std::strtod(line_.c_str() + at, nullptr);
    }

    [[nodiscard]] std::string text(const char* key) const {
        const std::string::size_type at = find(key);
        if (at == std::string::npos) {
            return "<missing>";
        }
        if (line_[at] == '"') {
            const std::string::size_type end = line_.find('"', at + 1);
            return line_.substr(at + 1, end - at - 1);
        }
        const std::string::size_type end = line_.find(' ', at);
        return line_.substr(at, end == std::string::npos ? end : end - at);
    }

private:
    [[nodiscard]] std::string::size_type find(const char* key) const {
        const std::string marker = std::string(" ") + key + "=";
        const std::string::size_type found = line_.find(marker);
        return found == std::string::npos ? found : found + marker.size();
    }

    std::string line_;
};

struct Run {
    cy::test::smoke::ProcessResult process;
    std::vector<Report> reports;
};

/// A copy of the project the run may edit, and an empty player directory, both its own.
struct Workspace {
    fs::path project;
    fs::path user_data;

    static Workspace make(const char* name) {
        Workspace made;
        const fs::path root = fs::path(CY_TEST_BINARY_DIR) / name;
        std::error_code ignored;
        fs::remove_all(root, ignored);
        made.project = root / "project";
        made.user_data = root / "user-data";
        fs::create_directories(made.user_data);
        fs::copy(CY_MENUS_PROJECT, made.project, fs::copy_options::recursive);
        return made;
    }
};

Run play(const Workspace& space, const char* script) {
    const std::string command =
        cy::test::smoke::quoted(CY_SAMPLE_MENUS) + " --script " +
        cy::test::smoke::quoted(std::string(CY_MENUS_TEST_DIR) + "/" + script) + " --project " +
        cy::test::smoke::quoted(space.project.string()) + " --user-data " +
        cy::test::smoke::quoted(space.user_data.string()) + " 2>&1";
    Run run;
    run.process = cy::test::smoke::run(command);
    std::string::size_type at = 0;
    while ((at = run.process.output.find("menus: ", at)) != std::string::npos) {
        const std::string::size_type end = run.process.output.find('\n', at);
        run.reports.emplace_back(run.process.output.substr(at, end - at));
        at = end;
    }
    return run;
}

constexpr double kMainMenu = 1;
constexpr double kSettings = 2;
constexpr double kPlaying = 3;
constexpr double kPaused = 4;
constexpr double kInventory = 5;
constexpr double kQuit = 6;

}  // namespace

CY_TEST_CASE("menus sample: a gamepad plays every screen, and a second start keeps what it set") {
    const Workspace space = Workspace::make("menus-sample");
    const Run first = play(space, "navigate.script");
    CY_REQUIRE(first.process.ran);
    CY_REQUIRE_EQ(first.process.exit_code, 0);
    // One report a `report` step, and one at the end.
    CY_REQUIRE_EQ(first.reports.size(), 22U);
    const std::vector<Report>& seen = first.reports;

    // The main menu, from a first start: nothing to continue, so the d-pad skips Continue.
    CY_CHECK_EQ(seen[0].number("found"), 1.0);
    CY_CHECK_EQ(seen[0].number("screen"), kMainMenu);
    CY_CHECK_EQ(seen[0].text("focused"), "new-game");
    CY_CHECK_EQ(seen[0].number("loaded_settings"), 0.0);
    CY_CHECK_EQ(seen[1].number("screen"), kSettings);
    CY_CHECK_EQ(seen[1].text("focused"), "resolution");

    // Graphics changed in the widgets only: the engine still has the defaults.
    CY_CHECK_EQ(seen[2].text("focused"), "gi-quality");
    CY_CHECK_EQ(seen[2].text("video"), "1280x720");
    CY_CHECK_EQ(seen[2].number("quality"), 2.0);
    CY_CHECK_EQ(seen[2].number("gi"), 2.0);

    // Audio: two steps down on Master and one on Music, heard at once — the slider is the bus.
    CY_CHECK_EQ(seen[3].text("focused"), "volume-music");
    CY_CHECK_EQ(seen[3].number("master"), 0.9);
    CY_CHECK_EQ(seen[3].number("music"), 0.95);
    CY_CHECK_EQ(seen[3].number("sfx"), 1.0);

    // Controls: listening, then Jump on the gamepad's north button — the Inventory's, swapped.
    CY_CHECK_EQ(seen[4].number("rebinds"), 0.0);
    CY_CHECK_EQ(seen[5].number("rebinds"), 1.0);
    CY_CHECK_EQ(seen[5].text("jump_gamepad"), "gamepad/north");
    CY_CHECK_EQ(seen[5].text("jump_keyboard"), "keyboard/space");
    CY_CHECK_EQ(seen[5].text("focused"), "bind-jump-gamepad");
    // And on E, which Interact had: Interact takes Space.
    CY_CHECK_EQ(seen[6].number("rebinds"), 2.0);
    CY_CHECK_EQ(seen[6].text("jump_keyboard"), "keyboard/e");
    CY_CHECK_EQ(seen[6].text("interact_keyboard"), "keyboard/space");
    // A rebind cancelled by Escape changes nothing, and the Escape does not also leave settings.
    CY_CHECK_EQ(seen[7].number("rebinds"), 2.0);
    CY_CHECK_EQ(seen[7].text("jump_keyboard"), "keyboard/e");
    CY_CHECK_EQ(seen[7].number("screen"), kSettings);
    CY_CHECK_EQ(seen[7].number("backs"), 0.0);
    CY_CHECK_EQ(seen[7].text("focused"), "bind-jump-keyboard");

    // Apply: the video adapter has 1920x1080 at Medium with Quality upscaling — 1.5x, so the
    // scene renders at the upscaler's internal extent — and the GI runs at Medium.
    CY_CHECK_EQ(seen[8].number("applied"), 1.0);
    CY_CHECK_EQ(seen[8].text("video"), "1920x1080");
    CY_CHECK_EQ(seen[8].number("quality"), 1.0);
    CY_CHECK_EQ(seen[8].number("upscale"), 1.0);
    CY_CHECK_EQ(seen[8].text("render"), "1286x724");
    CY_CHECK_EQ(seen[8].number("gi"), 1.0);
    CY_CHECK_EQ(seen[9].number("screen"), kMainMenu);
    CY_CHECK_EQ(seen[9].number("backs"), 1.0);

    // New game, Start pauses, the inventory from the pause menu.
    CY_CHECK_EQ(seen[10].number("screen"), kPlaying);
    CY_CHECK_EQ(seen[10].text("focused"), "-");
    CY_CHECK_EQ(seen[11].number("screen"), kPaused);
    CY_CHECK_EQ(seen[11].text("focused"), "resume");
    CY_CHECK_EQ(seen[12].number("screen"), kInventory);
    CY_CHECK_EQ(seen[12].text("focused"), "slot-0");
    CY_CHECK_EQ(seen[12].number("potion_slot"), 0.0);
    // A, right, A: the potion and the sword swap.
    CY_CHECK_EQ(seen[13].number("picks"), 1.0);
    CY_CHECK_EQ(seen[13].number("drops"), 1.0);
    CY_CHECK_EQ(seen[13].number("potion_slot"), 1.0);
    CY_CHECK_EQ(seen[13].number("first_slot"), 1.0);
    // Picked up and put back with B, which closes nothing while something is held.
    CY_CHECK_EQ(seen[14].number("picks"), 2.0);
    CY_CHECK_EQ(seen[14].number("drops"), 1.0);
    CY_CHECK_EQ(seen[14].number("screen"), kInventory);
    CY_CHECK_EQ(seen[14].number("potion_slot"), 1.0);
    // The pointer drags the potion from slot 1 to slot 5.
    CY_CHECK_EQ(seen[15].number("drags"), 1.0);
    CY_CHECK_EQ(seen[15].number("potion_slot"), 5.0);
    // B twice: the pause menu, then the game; Escape pauses.
    CY_CHECK_EQ(seen[16].number("screen"), kPlaying);
    CY_CHECK_EQ(seen[16].number("backs"), 3.0);
    CY_CHECK_EQ(seen[17].number("screen"), kPaused);
    CY_CHECK_EQ(seen[18].number("screen"), kMainMenu);

    // The editor's save, under the running game: the subtitle is the new one, the focus kept.
    CY_CHECK_EQ(seen[18].text("subtitle"), "Menus made in the editor");
    CY_CHECK_EQ(seen[19].text("subtitle"), "Menus remade in the editor");
    CY_CHECK_EQ(seen[19].text("focused"), "new-game");
    CY_CHECK_EQ(seen[20].number("screen"), kQuit);

    // The second start, over the same player files.
    const Run second = play(space, "persist.script");
    CY_REQUIRE(second.process.ran);
    CY_REQUIRE_EQ(second.process.exit_code, 0);
    CY_REQUIRE_EQ(second.reports.size(), 5U);
    const std::vector<Report>& again = second.reports;
    CY_CHECK_EQ(again[0].number("loaded_settings"), 1.0);
    CY_CHECK_EQ(again[0].number("loaded_bindings"), 1.0);
    CY_CHECK_EQ(again[0].text("video"), "1920x1080");
    CY_CHECK_EQ(again[0].number("quality"), 1.0);
    CY_CHECK_EQ(again[0].number("upscale"), 1.0);
    CY_CHECK_EQ(again[0].number("gi"), 1.0);
    CY_CHECK_EQ(again[0].number("master"), 0.9);
    CY_CHECK_EQ(again[0].number("music"), 0.95);
    CY_CHECK_EQ(again[0].text("jump_gamepad"), "gamepad/north");
    CY_CHECK_EQ(again[0].text("jump_keyboard"), "keyboard/e");
    CY_CHECK_EQ(again[0].text("interact_keyboard"), "keyboard/space");
    // Continue is enabled now, and the keyboard reaches it.
    CY_CHECK_EQ(again[1].text("focused"), "continue");
    CY_CHECK_EQ(again[2].number("screen"), kPlaying);
    // North and E jump; South, the Inventory's since the swap, opens it.
    CY_CHECK_EQ(again[3].number("jumps"), 2.0);
    CY_CHECK_EQ(again[3].number("screen"), kInventory);
}
