// samples/04-character — M4's closing artefact. Tasks 5.1, 5.2, 5.4.
//
// A third-person character controller written entirely in Swift: it moves, jumps, collides with a
// level, is heard, and is followed by a camera. This file and the two beside it are the HOST — the
// engine's side of the boundary — and they contain no gameplay. The game is
// samples/04-character/game/, five Swift files, built into a shared library and loaded over the C
// ABI at run time.
//
//   just run-sample character                        the default: 900 fixed ticks of scripted play
//   just run-sample character --ticks 120            a short run, which is what the smoke test does
//   just run-sample character --no-behaviours        THE NEGATIVE CONTROL — see below
//   just run-sample character --jolt                 the same run over the Jolt backend
//   just run-sample character --verbose              one line per tick
//   just run-sample character --device-input         a window, and your own keyboard
//
// --- WHY THIS SAMPLE DRAWS NOTHING
// ------------------------------------------------------------------
//
// It cannot, and the gap is M3's rather than M4's: `cy::rhi::Device` exposes no way to obtain the
// graphics-API instance a window surface must be created against, so a host can create a window and
// a device and cannot join them. tools/roadmap/milestones/m3.toml records that as an open gap and
// samples/03-first-light renders offscreen for the same reason. What this sample does instead is
// produce, every tick, the `cy::render::ViewDescription` a renderer would draw — which is the seam
// `camera-system`'s "Render view production" actually names — and count it. A camera that framed
// the character correctly and a renderer that never received the frame would be two different
// failures, and only the first is this milestone's.
//
// --- THE NEGATIVE CONTROL, AND WHY IT IS THE REAL CHECK FOR TASK 5.3
// ----------------------------------
//
// `--no-behaviours` loads the same module, builds the same level out of it, brings up the same five
// servers, resolves the same contract, runs the same scripted input through the same command
// stream, and creates neither `Character` nor `CameraDirector`. Every line of C++ in this sample
// runs. The character does not move, does not jump, makes no sound, and the camera does not turn.
//
// That is what "the sample contains no C++ gameplay code" means as a measurement rather than as a
// claim, and it is why the run is worth more than tools/check_no_cpp_gameplay.py beside it: a
// static check can only fail on the words somebody thought to forbid, and this one fails on any
// decision at all having leaked to this side of the boundary.

#include <cy/core/memory/system_allocator.h>
#include <cy/platform/headless_display_server.h>
#include <cy/platform/host_loop.h>
#include <cy/platform/sdl3_display_server.h>
#include <cy/platform/sdl3_input_source.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>
#include <cy/runtime/simulation.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "game.h"

namespace {

constexpr const char* kTag = "04-character";

struct Options {
    cy::u64 ticks = 900;  // fifteen seconds at sixty hertz: the whole scripted timeline
    cy::u32 ticks_per_frame = 1;
    sample::GameOptions game;
    bool trace = false;
    const char* trace_path = nullptr;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/04-character — a third-person character controller written in Swift.\n"
        "\n"
        "  --ticks <n>            simulation ticks to run          (default 900)\n"
        "  --ticks-per-frame <n>  fixed ticks per frame, max 8     (default 1)\n"
        "  --no-behaviours        load the module and the level but create no deciding\n"
        "                         behaviours: the negative control for 'no C++ gameplay'\n"
        "  --device-input         open a window and take input from the keyboard\n"
        "  --jolt                 run physics on the Jolt backend rather than the reference one\n"
        "  --verbose              one line per tick\n"
        "  --trace <path>         write a trace here\n"
        "  --help                 this\n",
        stderr);
}

bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int i = 1; i < argument_count; ++i) {
        const std::string_view argument{arguments[i]};
        const bool has_value = i + 1 < argument_count;

        if (argument == "--help" || argument == "-h") {
            options.help = true;
            print_usage();
        } else if (argument == "--no-behaviours") {
            options.game.behaviours = false;
        } else if (argument == "--device-input") {
            options.game.device_input = true;
        } else if (argument == "--jolt") {
            options.game.jolt = true;
        } else if (argument == "--verbose") {
            options.game.verbose = true;
        } else if (argument == "--ticks" && has_value) {
            options.ticks = std::strtoull(arguments[++i], nullptr, 10);
        } else if (argument == "--ticks-per-frame" && has_value) {
            options.ticks_per_frame =
                static_cast<cy::u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--trace" && has_value) {
            options.trace = true;
            options.trace_path = arguments[++i];
        } else if (argument == "--headless") {
            // Accepted and ignored: this sample is headless unless `--device-input` asks for a
            // window, so `just run-sample character --headless` means what a reader expects rather
            // than failing on an argument every other sample takes.
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, arguments[i]);
            print_usage();
            return false;
        }
    }
    if (options.ticks_per_frame == 0 || options.ticks_per_frame > 8) {
        std::fprintf(stderr, "%s: --ticks-per-frame must be between 1 and 8\n", kTag);
        return false;
    }
    return true;
}

void report(const char* label, const cy::Error& error) {
    std::fprintf(stderr, "%s: %s failed: %s (%s)\n", kTag, label, error.message,
                 cy::error_code_name(error.code));
}

/// The summary. Every number is one a test can assert on, which is why there is no prose in it.
void print_report(const sample::GameReport& r, const sample::GameOptions& options) {
    std::fprintf(stdout, "%s: module   behaviours=%u level=%u bodies physics=%s\n", kTag,
                 r.behaviours, r.level_bodies, r.physics_backend);
    std::fprintf(stdout, "%s: input    committed=%u rejected=%u log=%016llx frames=%016llx\n", kTag,
                 r.commands_committed, r.commands_rejected,
                 static_cast<unsigned long long>(r.command_log_hash),
                 static_cast<unsigned long long>(r.input_hash));
    std::fprintf(stdout,
                 "%s: motion   ticks=%llu travelled=%.2f m  start=(%.2f %.2f %.2f) "
                 "end=(%.2f %.2f %.2f)\n",
                 kTag, static_cast<unsigned long long>(r.ticks),
                 static_cast<double>(r.distance_travelled), static_cast<double>(r.start.x),
                 static_cast<double>(r.start.y), static_cast<double>(r.start.z),
                 static_cast<double>(r.end.x), static_cast<double>(r.end.y),
                 static_cast<double>(r.end.z));
    std::fprintf(stdout,
                 "%s: ground   grounded=%u airborne=%u stepped=%u wall=%u high=%.2f low=%.2f\n",
                 kTag, r.grounded_ticks, r.airborne_ticks, r.stepped_up_ticks, r.wall_ticks,
                 static_cast<double>(r.highest_point), static_cast<double>(r.lowest_point));
    std::fprintf(stdout, "%s: audio    footsteps=%u landings=%u jumps=%u voices=%u frames=%llu\n",
                 kTag, r.footsteps, r.landings, r.jumps, r.voices_started,
                 static_cast<unsigned long long>(r.audio_frames));
    std::fprintf(stdout, "%s: camera   views=%u travelled=%.2f m  end=(%.2f %.2f %.2f)\n", kTag,
                 r.views_produced, static_cast<double>(r.camera_travelled),
                 static_cast<double>(r.camera_end.x), static_cast<double>(r.camera_end.y),
                 static_cast<double>(r.camera_end.z));
    std::fprintf(stdout, "%s: gameplay %s\n", kTag,
                 options.behaviours
                     ? "swift behaviours decided every tick"
                     : "NO BEHAVIOURS — the control run: nothing above decided anything");
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }
    if (options.help) {
        return 0;
    }

    // The build knows where the game module is; this program does not go looking for it. Both paths
    // are absolute and both are produced by the same custom command that compiles the Swift.
    options.game.module_library = CY_CHARACTER_MODULE_LIBRARY;
    options.game.module_manifest = CY_CHARACTER_MODULE_MANIFEST;

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Ecs);

    // --- The simulation, before the runtime ------------------------------------------------------
    //
    // The order <cy/runtime/simulation.h> documents: a game registers what it has between
    // constructing the simulation and starting the runtime, because the runtime's Boot stage is
    // what closes registration. Here "what it has" is a Swift module, which registers its own
    // components from inside `Game::start`.
    cy::runtime::SimulationConfig simulation_config;
    simulation_config.world_name = "character";
    simulation_config.session_seed = 0xC4A5AC7EULL;
    simulation_config.clock.mode = cy::determinism::TickMode::FixedStep;
    simulation_config.clock.fixed_ticks_per_frame = options.ticks_per_frame;
    cy::runtime::Simulation simulation(allocator, simulation_config);
    if (const cy::Status ready = simulation.initialize(); !ready) {
        report("simulation", ready.error());
        return 1;
    }

    sample::Game game(allocator, simulation.world(), options.game);
    const char* detail = "";
    if (const cy::Status started = game.start(&detail); !started) {
        std::fprintf(stderr, "%s: bring-up failed at '%s': %s (%s)\n", kTag, detail,
                     started.error().message, cy::error_code_name(started.error().code));
        return 1;
    }
    if (const cy::Status installed = game.install(simulation.schedule()); !installed) {
        report("schedule", installed.error());
        return 1;
    }

    // --- The platform ----------------------------------------------------------------------------
    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(argument_count, arguments); !started) {
        report("platform", started.error());
        return 1;
    }

    cy::HeadlessDisplayServer headless;
    cy::Sdl3DisplayServer desktop;
    const cy::Status display_started =
        options.game.device_input ? desktop.initialise() : headless.initialise();
    if (!display_started) {
        report("display", display_started.error());
        platform.shutdown();
        return 1;
    }
    cy::DisplayServer* display = options.game.device_input
                                     ? static_cast<cy::DisplayServer*>(&desktop)
                                     : static_cast<cy::DisplayServer*>(&headless);
    // `DisplayServer` declares no `shutdown` — it is the interface a runtime consumes, and bringing
    // one down is the host's own business with the concrete server it created. Same in samples/00.
    const auto shutdown_display = [&] {
        if (options.game.device_input) {
            desktop.shutdown();
        } else {
            headless.shutdown();
        }
    };

    // A real keyboard, when one was asked for. ASSIGNMENT IS ALWAYS EXPLICIT — `Sdl3InputSource`
    // connects the devices and gives them to nobody, because `input-and-actions` forbids a platform
    // layer from deciding that the keyboard belongs to player one.
    cy::Sdl3InputSource input_source;
    if (options.game.device_input) {
        if (const cy::Status attached = input_source.attach(game.input()); !attached) {
            report("input source", attached.error());
            platform.shutdown();
            return 1;
        }
        if (const cy::Status assigned = game.input().assign(input_source.keyboard(), 0, 0);
            !assigned) {
            report("device assignment", assigned.error());
            platform.shutdown();
            return 1;
        }
    }

    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = display;
    config.create_window = options.game.device_input;
    config.create_surface = false;
    config.install_crash_handler = false;
    config.window.title = "CyberdyneEngine — 04-character";
    config.build_identity = "cyberdyne 0.0.0 m4 sample 04-character";
    config.tick_mode = cy::determinism::TickMode::FixedStep;
    config.fixed_ticks_per_frame = options.ticks_per_frame;
    config.simulation = &simulation;
    if (options.trace) {
        config.trace_path = options.trace_path;
    }

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        report("runtime", started.error());
        input_source.detach();
        shutdown_display();
        platform.shutdown();
        return 1;
    }

    std::fprintf(stdout, "%s: module   %s\n", kTag, options.game.module_library);

    // --- The loop --------------------------------------------------------------------------------
    //
    // `Runtime::tick()` owns the split between fixed steps and the frame; the host only says how
    // many frames it wants. In fixed-step mode the tick count is exact rather than a function of
    // how fast this machine is, which is what makes the report reproducible and the smoke test an
    // assertion rather than a hope.
    cy::HostLoopOptions loop;
    loop.frame_limit = (options.ticks + options.ticks_per_frame - 1) / options.ticks_per_frame;
    loop.frame_interval_ns = 0;
    const cy::HostLoopResult result =
        cy::run_host_loop(platform, loop, [&runtime] { (void)runtime.tick(); });

    runtime.shutdown();
    game.shutdown();
    print_report(game.report(), options.game);

    input_source.detach();
    shutdown_display();
    platform.shutdown();

    std::fprintf(stdout, "%s: %llu frames, exit %d (%s)\n", kTag,
                 static_cast<unsigned long long>(result.frames), result.exit_code,
                 result.interrupted ? "interrupted" : "frame limit");
    return result.exit_code;
}
