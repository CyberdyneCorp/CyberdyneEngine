// SPDX-License-Identifier: MIT
// A game's launch from a packaged installation. See launch.h.

#include <cy/core/diagnostics/crash.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/ship/launch.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace cy::ship {

namespace fs = std::filesystem;

namespace {

/// Asks the launch to crash once the handler is installed, so that a test can see a report land
/// where a player's would. Consumed by the launch; the game never sees it.
constexpr const char* kCrashFlag = "--ship-crash";

using Clock = std::chrono::steady_clock;

[[nodiscard]] f64 milliseconds_since(Clock::time_point started) {
    return std::chrono::duration<f64, std::milli>(Clock::now() - started).count();
}

/// The crash handler keeps pointers to its version and identity and reads them from a signal
/// handler, so they live in static storage that a second launch overwrites in place.
char g_crash_version[64];
char g_crash_identity[256];

[[nodiscard]] bool install_crash_reports(const LaunchDirectories& directories,
                                         const Manifest& manifest, std::string& where) {
    std::error_code code;
    const fs::path crashes = directories.data / "crashes";
    fs::create_directories(crashes, code);
    where = crashes.string();
    std::snprintf(g_crash_version, sizeof(g_crash_version), "%s", manifest.version.c_str());
    std::snprintf(g_crash_identity, sizeof(g_crash_identity), "%s-%s-%s-%s", manifest.game.c_str(),
                  manifest.version.c_str(), manifest.target.c_str(), manifest.revision.c_str());
    diag::CrashConfig config;
    config.directory = where.c_str();
    config.engine_version = g_crash_version;
    config.build_identity = g_crash_identity;
    return static_cast<bool>(diag::install_crash_handler(config));
}

void print_refusal(const Location& location, const Verification& verification) {
    std::fprintf(stderr, "ship: refusing to start: the installation at %s does not verify\n",
                 location.root.string().c_str());
    for (const Problem& problem : verification.problems) {
        std::fprintf(stderr, "ship:   %s\n", problem.describe().c_str());
    }
}

/// The player's arguments with the launch's own flag taken out; whether it was there.
[[nodiscard]] bool take_crash_flag(std::vector<std::string>& arguments) {
    const auto found = std::ranges::find(arguments, kCrashFlag);
    if (found == arguments.end()) {
        return false;
    }
    arguments.erase(found);
    return true;
}

/// The arguments a packaged game runs with: its settings, then the player's — or its defaults.
[[nodiscard]] std::vector<std::string> packaged_arguments(const LaunchSpec& spec,
                                                          std::vector<std::string> player,
                                                          Settings& settings) {
    if (player.empty()) {
        player.assign(spec.default_arguments, spec.default_arguments + spec.default_argument_count);
    }
    const bool applies = spec.settings_when == nullptr ||
                         std::ranges::find(player, spec.settings_when) != player.end();
    std::vector<std::string> arguments;
    if (applies) {
        settings_arguments(spec.settings, spec.setting_count, settings, arguments);
    }
    arguments.insert(arguments.end(), player.begin(), player.end());
    return arguments;
}

/// The user directories under one base, which is what CY_SHIP_USER_DIR asks for.
[[nodiscard]] LaunchDirectories directories_under(const fs::path& base) {
    return LaunchDirectories{base / "data", base / "config", base / "cache"};
}

[[nodiscard]] bool platform_directories(Sdl3Platform& platform, const LaunchSpec& spec,
                                        LaunchDirectories& out) {
    char buffer[4096];
    if (auto overridden = platform.environment_variable("CY_SHIP_USER_DIR", buffer, sizeof(buffer));
        overridden && *overridden > 0) {
        out = directories_under(fs::path(buffer) / spec.application);
        return true;
    }
    using Query = Expected<usize, Error> (Sdl3Platform::*)(char*, usize) const;
    const std::pair<fs::path*, Query> queries[] = {
        {&out.data, &Sdl3Platform::user_data_directory},
        {&out.config, &Sdl3Platform::user_config_directory},
        {&out.cache, &Sdl3Platform::user_cache_directory}};
    for (const auto& [target, query] : queries) {
        if (!(platform.*query)(buffer, sizeof(buffer))) {
            return false;
        }
        *target = fs::path(buffer);
    }
    return true;
}

}  // namespace

bool requires_installation() noexcept {
#if defined(CY_SHIP_REQUIRE_INSTALLATION)
    return true;
#else
    return false;
#endif
}

void Launch::set_arguments(std::vector<std::string> arguments) {
    pointers_.clear();
    for (std::string& argument : arguments) {
        pointers_.push_back(const_cast<char*>(keep(std::move(argument))));
    }
    pointers_.push_back(nullptr);
}

const char* Launch::keep(std::string text) {
    storage_.push_back(std::move(text));
    return storage_.back().c_str();
}

const char* Launch::library(const char* development, const char* name) {
    if (!packaged_) {
        return development;
    }
    for (const char* directory : {"lib", "Frameworks", "bin"}) {
        const fs::path candidate = location_.root / directory / name;
        std::error_code code;
        if (fs::is_regular_file(candidate, code)) {
            return keep(candidate.string());
        }
    }
    return keep((location_.root / "lib" / name).string());
}

const char* Launch::content(const char* development, const char* name) {
    if (!packaged_) {
        return development;
    }
    return keep((content_ / fs::path(name)).string());
}

bool start_at(const fs::path& executable, int argc, char** argv, const LaunchSpec& spec,
              const LaunchDirectories* directories, Launch& out) {
    const auto started = Clock::now();
    std::vector<std::string> player(argv + (argc > 0 ? 1 : 0), argv + (argc > 0 ? argc : 0));
    const std::string program = argc > 0 ? argv[0] : spec.game;
    out.packaged_ = locate_installation(executable, out.location_);
    if (!out.packaged_) {
        if (requires_installation()) {
            std::fprintf(stderr,
                         "ship: refusing to start: this build runs only from a verified "
                         "installation, and there is no %s beside %s\n",
                         kManifestName, executable.string().c_str());
            return false;
        }
        player.insert(player.begin(), program);
        out.set_arguments(std::move(player));
        return true;
    }
    if (directories == nullptr) {
        std::fprintf(stderr, "ship: refusing to start: the user directories are unknown\n");
        return false;
    }
    out.directories_ = *directories;
    out.verification_ = verify_installation(out.location_, trusted_key());
    if (!out.verification_.ok()) {
        print_refusal(out.location_, out.verification_);
        return false;
    }
    const Manifest& manifest = out.verification_.manifest;
    const auto materialising = Clock::now();
    std::string error;
    const fs::path cache =
        out.directories_.cache / "content" / out.verification_.manifest_digest.substr(0, 16);
    if (!materialise_content(out.location_, manifest, cache, out.content_, error)) {
        std::fprintf(stderr, "ship: refusing to start: the content cannot be made ready: %s\n",
                     error.c_str());
        return false;
    }
    const f64 materialise_ms = milliseconds_since(materialising);
    std::string crashes;
    if (!install_crash_reports(out.directories_, manifest, crashes)) {
        std::fprintf(stderr, "ship: crash reports are off: the handler could not be installed\n");
    }
    out.settings_file_ = out.directories_.config / "settings.cfg";
    Settings settings;
    if (!load_settings(out.settings_file_, spec.settings, spec.setting_count, settings, error)) {
        std::fprintf(stderr, "ship: the settings are the defaults: %s\n", error.c_str());
    }
    const bool crash = take_crash_flag(player);
    std::vector<std::string> arguments = packaged_arguments(spec, std::move(player), settings);
    for (const std::string& warning : settings.warnings) {
        std::fprintf(stderr, "ship: %s: %s\n", out.settings_file_.string().c_str(),
                     warning.c_str());
    }
    arguments.insert(arguments.begin(), program);
    out.set_arguments(std::move(arguments));
    std::printf(
        "ship launch game=%s version=%s target=%s profile=%s files=%llu chunks=%llu "
        "content_bytes=%llu signer=%s trusted=%s verify_ms=%.2f materialise_ms=%.2f "
        "startup_ms=%.2f content=%s settings=%s%s crashes=%s\n",
        manifest.game.c_str(), manifest.version.c_str(), manifest.target.c_str(),
        manifest.profile.c_str(), static_cast<unsigned long long>(out.verification_.files),
        static_cast<unsigned long long>(out.verification_.chunks),
        static_cast<unsigned long long>(manifest.content_bytes), manifest.signer.c_str(),
        trusts_development_key() ? "development" : "configured", out.verification_.milliseconds,
        materialise_ms, milliseconds_since(started), out.content_.string().c_str(),
        out.settings_file_.string().c_str(), settings.created ? " (written)" : "", crashes.c_str());
    std::fflush(stdout);
    if (crash) {
        std::raise(SIGSEGV);
    }
    return true;
}

bool start(int argc, char** argv, const LaunchSpec& spec, Launch& out) {
    Sdl3Platform platform;
    platform.set_application_identity("CyberdyneEngine", spec.application);
    char buffer[4096];
    fs::path executable = argc > 0 ? fs::path(argv[0]) : fs::path();
    const bool initialised = static_cast<bool>(platform.initialise(argc, argv));
    if (initialised) {
        if (const auto written = platform.executable_path(buffer, sizeof(buffer)); written) {
            executable = fs::path(buffer);
        }
    }
    Location probe;
    LaunchDirectories directories;
    const bool packaged = locate_installation(executable, probe);
    const bool known = packaged && initialised && platform_directories(platform, spec, directories);
    // The platform is the game's to initialise for itself: this one is shut down before it starts.
    if (initialised) {
        platform.shutdown();
    }
    return start_at(executable, argc, argv, spec, known ? &directories : nullptr, out);
}

}  // namespace cy::ship
