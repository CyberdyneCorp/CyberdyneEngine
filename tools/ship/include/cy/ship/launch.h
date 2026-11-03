// SPDX-License-Identifier: MIT
#ifndef CY_SHIP_LAUNCH_H
#define CY_SHIP_LAUNCH_H
// The first thing a game's main() does: find out whether it is running from a packaged
// installation, and if it is, refuse to start unless that installation verifies. Issue #87.
//
//     int main(int argc, char** argv) {
//         cy::ship::Launch launch;
//         if (!cy::ship::start(argc, argv, kShipping, launch)) {
//             return cy::ship::kRefusedExit;
//         }
//         options.module = launch.library(CY_GAME_MODULE_LIBRARY, "libCyGame_g0.so");
//         options.map = launch.content(CY_GAME_MAP_PACKAGE, "frontier.cypak");
//         ... parse launch.argc() and launch.argv() as before ...
//
// OUT OF A BUILD TREE, NOTHING CHANGES: no manifest is beside the executable, `library` and
// `content` return the build tree's paths, and the arguments are the process's own. PACKAGED:
//
//   1. the installation is verified against its signed manifest (installation.h), and any problem
//      — a modified chunk, a missing file, a file the manifest does not list, a signature by a key
//      this build does not trust — is printed by name and the game does not start;
//   2. the content is materialised into the user's cache directory, keyed by the manifest;
//   3. the crash handler is installed, writing reports to the user's data directory;
//   4. the settings file in the user's configuration directory is read, or written with its
//      defaults, and translated to the game's own flags ahead of the player's arguments;
//   5. with no arguments at all — a double-click — the game's default arguments are used.
//
// One line, `ship launch ...`, reports what it did and how long it took.
//
// A BUILD CONFIGURED WITH `-D CY_SHIP_REQUIRE_INSTALLATION=ON` REFUSES TO RUN FROM A BUILD TREE:
// it is a game that only starts from a verified installation.

#include <cy/ship/installation.h>
#include <cy/ship/settings.h>

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace cy::ship {

/// What a refused launch exits with: not 1, which a game's own failure already means.
inline constexpr int kRefusedExit = 3;

struct LaunchSpec {
    /// The game's short name, as `just package` knows it: `rts`, `fps`, `tps`, `ship`.
    const char* game = "";
    /// The directory the user's data, configuration and cache live under.
    const char* application = "";
    const Setting* settings = nullptr;
    usize setting_count = 0;
    /// Settings are applied only when this flag is among the arguments (`--play`); null always.
    const char* settings_when = nullptr;
    /// The arguments a packaged game is started with when it is given none.
    const char* const* default_arguments = nullptr;
    usize default_argument_count = 0;
};

struct LaunchDirectories {
    std::filesystem::path data;
    std::filesystem::path config;
    std::filesystem::path cache;
};

class Launch {
public:
    Launch() = default;
    Launch(const Launch&) = delete;
    Launch& operator=(const Launch&) = delete;

    [[nodiscard]] bool packaged() const noexcept { return packaged_; }
    [[nodiscard]] int argc() const noexcept { return static_cast<int>(pointers_.size()) - 1; }
    [[nodiscard]] char** argv() noexcept { return pointers_.data(); }

    /// A shared library of the installation by its file name — in `lib/` (Linux), `Frameworks/`
    /// (a macOS bundle) or `bin/` (Windows) — or `development` when not packaged. The pointer
    /// lives as long as this object.
    [[nodiscard]] const char* library(const char* development, const char* name);
    /// A content entry, materialised, or `development` when not packaged.
    [[nodiscard]] const char* content(const char* development, const char* name);

    [[nodiscard]] const Verification& verification() const noexcept { return verification_; }
    [[nodiscard]] const LaunchDirectories& directories() const noexcept { return directories_; }
    [[nodiscard]] const std::filesystem::path& settings_file() const noexcept {
        return settings_file_;
    }

private:
    friend bool start(int argc, char** argv, const LaunchSpec& spec, Launch& out);
    friend bool start_at(const std::filesystem::path& executable, int argc, char** argv,
                         const LaunchSpec& spec, const LaunchDirectories* directories, Launch& out);

    void set_arguments(std::vector<std::string> arguments);
    const char* keep(std::string text);

    bool packaged_ = false;
    Location location_;
    Verification verification_;
    LaunchDirectories directories_;
    std::filesystem::path content_;
    std::filesystem::path settings_file_;
    std::deque<std::string> storage_;
    std::vector<char*> pointers_;
};

/// The launch, from the process's own executable path and the platform's user directories. False
/// when the game must not start; why has been printed.
[[nodiscard]] bool start(int argc, char** argv, const LaunchSpec& spec, Launch& out);

/// `start` from a given executable path and, when not null, given user directories. What the
/// tests drive, so that no test writes into the real home directory.
[[nodiscard]] bool start_at(const std::filesystem::path& executable, int argc, char** argv,
                            const LaunchSpec& spec, const LaunchDirectories* directories,
                            Launch& out);

/// Whether this build refuses to run outside a verified installation.
[[nodiscard]] bool requires_installation() noexcept;

}  // namespace cy::ship

#endif  // CY_SHIP_LAUNCH_H
