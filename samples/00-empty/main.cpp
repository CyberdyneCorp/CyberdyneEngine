// samples/00-empty — M0's closing artefact. Task 3.6.1.
//
// It opens a window, runs an empty loop through Runtime::tick(), writes a trace, and exits cleanly.
// That is the whole of it, and it is deliberately the first thing a contributor reads: every
// milestone after this one starts from this shape.
//
//   just run-sample empty                    a window on the desktop, until you close it
//   just run-sample empty --headless         no window system at all, which is how CI runs it
//   just run-sample empty --frames 120       stop after 120 frames, which is what makes it testable
//   just diagnose-trace <path>               read the trace it wrote
//
// FOUR OBJECTS AND ONE LOOP. The host — this file — owns the platform, the display server, the
// runtime and the loop. The runtime owns the frame. Nothing owns the other direction: the runtime
// exposes tick() and never calls back into here, which is what lets a platform that drives frames
// itself replace run_host_loop() and change nothing else (design.md §3).

#include <cy/platform/headless_display_server.h>
#include <cy/platform/host_loop.h>
#include <cy/platform/sdl3_display_server.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

struct Options {
    bool headless = false;
    cy::u64 frames = 0;  // 0: until the window is closed or the process is interrupted
    const char* trace_path = nullptr;  // null: under the platform's user data directory
    bool trace = true;
};

void print_usage() {
    std::fputs(
        "samples/00-empty — opens a window, ticks the runtime, writes a trace.\n"
        "\n"
        "  --headless        run under the headless display server, with no window system\n"
        "  --frames <n>      stop after n frames (0, the default, runs until asked to exit)\n"
        "  --trace <path>    write the trace here (default: the user data directory)\n"
        "  --no-trace        do not open a trace\n",
        stderr);
}

// Returns false when the command line is not one this sample understands, having said so.
bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int i = 1; i < argument_count; ++i) {
        const std::string_view argument{arguments[i]};
        const bool has_value = i + 1 < argument_count;

        if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--no-trace") {
            options.trace = false;
        } else if (argument == "--frames" && has_value) {
            options.frames = std::strtoull(arguments[++i], nullptr, 10);
        } else if (argument == "--trace" && has_value) {
            options.trace_path = arguments[++i];
        } else {
            std::fprintf(stderr, "00-empty: unrecognised argument '%s'\n\n", arguments[i]);
            print_usage();
            return false;
        }
    }
    return true;
}

// The trace goes under the user data directory, because the three user directories are the only
// paths the engine may write to. A path the caller gave is used unchanged.
//
// Data rather than cache, which is where a capture belongs: the platform creates the user data
// directory but not its config/ and cache/ subdirectories, and nothing at M0 can create a directory
// — the virtual filesystem is M1. This is one call to change when it lands.
const char* resolve_trace_path(const cy::Platform& platform, char* buffer, cy::usize capacity) {
    char directory[768];
    const auto written = platform.user_data_directory(directory, sizeof(directory));
    if (!written) {
        return "00-empty.cytrace";
    }
    std::snprintf(buffer, capacity, "%s00-empty.cytrace", directory);
    return buffer;
}

// Which of the three clean exits this was — the window closing, an interrupt, or the frame limit.
const char* exit_reason(const cy::HostLoopResult& result) {
    if (result.interrupted) {
        return "interrupted";
    }
    if (result.frame_limit_reached) {
        return "frame limit";
    }
    return "window closed";
}

void report(const char* label, const cy::Error& error) {
    std::fprintf(stderr, "00-empty: %s failed: %s (%s)\n", label, error.message,
                 cy::error_code_name(error.code));
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }

    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(argument_count, arguments); !started) {
        report("platform", started.error());
        return 1;
    }

    // Two display servers, one chosen. Nothing below this point knows which: the runtime and the
    // loop see cy::DisplayServer, which is the whole point of the interface.
    cy::HeadlessDisplayServer headless;
    cy::Sdl3DisplayServer desktop;
    cy::DisplayServer* display = nullptr;
    const cy::Status display_started =
        options.headless ? headless.initialise() : desktop.initialise();
    if (!display_started) {
        report("display", display_started.error());
        platform.shutdown();
        return 1;
    }
    display = options.headless ? static_cast<cy::DisplayServer*>(&headless) : &desktop;

    char trace_storage[1024];
    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = display;
    config.window.title = "CyberdyneEngine — 00-empty";
    config.build_identity = "cyberdyne 0.0.0 m0 sample 00-empty";
    if (options.trace) {
        config.trace_path =
            options.trace_path != nullptr
                ? options.trace_path
                : resolve_trace_path(platform, trace_storage, sizeof(trace_storage));
    }

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        report("runtime startup", started.error());
        platform.shutdown();
        return 1;
    }

    std::fprintf(stdout, "00-empty: display=%.*s window=%u trace=%s\n",
                 static_cast<int>(display->name().size()), display->name().data(),
                 runtime.main_window(), config.trace_path != nullptr ? config.trace_path : "none");

    // The loop. It belongs to the host; tick() belongs to the runtime.
    cy::HostLoopOptions loop;
    loop.frame_limit = options.frames;
    // Nothing presents until M3, so the loop paces itself at the simulation step rather than
    // spinning a core for a window that is only sitting there. The step is derived from the tick
    // rate, which M2 made an exact rational: `RuntimeConfig::fixed_step_ns` is gone because 1/60 s
    // is not a whole number of nanoseconds and accumulating the rounded value drifts.
    loop.frame_interval_ns = cy::determinism::step_nanoseconds(config.tick_rate);
    const cy::HostLoopResult result =
        cy::run_host_loop(platform, loop, [&runtime] { (void)runtime.tick(); });

    const cy::FrameStats frame = runtime.frame();
    runtime.shutdown();
    if (options.headless) {
        headless.shutdown();
    } else {
        desktop.shutdown();
    }
    platform.shutdown();

    const char* reason = exit_reason(result);
    std::fprintf(stdout, "00-empty: %llu frames, %llu simulation ticks, exit %d (%s)\n",
                 static_cast<unsigned long long>(result.frames),
                 static_cast<unsigned long long>(frame.total_ticks), result.exit_code, reason);
    return result.exit_code;
}
