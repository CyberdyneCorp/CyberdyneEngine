// samples/00-empty — M0's closing artefact. Task 3.6.1.
//
// It opens a window, runs an empty loop through Runtime::tick(), writes a trace, and exits cleanly.
// That is the whole of it, and it is deliberately the first thing a contributor reads: every
// milestone after this one starts from this shape.
//
//   just run-sample empty                    a window on the desktop, until you close it
//   just run-sample empty --headless         no window system at all, which is how CI runs it
//   just run-sample empty --platform native  the SAME window, opened by the native backend rather
//                                            than by SDL3 — M11.d task 4.3, and the cheapest proof
//                                            that the window, the event loop and the runtime all
//                                            came up on an implementation SDL never touched
//   just run-sample empty --platform stub    a platform with no desktop assumption at all
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
#include <cy/platform/stub_display_server.h>
#include <cy/platform/stub_platform.h>
#include <cy/runtime/runtime.h>
#if CY_SAMPLE_LINUX_NATIVE
#    include <cy/platform/linux_platform.h>
#    include <cy/platform/x11_display_server.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

// Which pair of implementations of `Platform` and `DisplayServer` this run uses. FOUR, AND THE
// POINT IS THAT NOTHING BELOW main() KNOWS WHICH: the runtime and the host loop see `cy::Platform`
// and `cy::DisplayServer`, which is what the interfaces are for. Adding `native` and `stub` at
// M11.d changed this file and no other.
enum class Backend {
    Sdl3,      // the desktop, over SDL3 — the default since M0
    Native,    // the desktop, natively: POSIX and Xlib, with no SDL beneath it
    Headless,  // no window system at all, which is how CI runs this sample
    Stub,      // no desktop assumption at all: one unresizable window, one writable directory
};

struct Options {
    Backend backend = Backend::Sdl3;
    cy::u64 frames = 0;  // 0: until the window is closed or the process is interrupted
    const char* trace_path = nullptr;  // null: under the platform's user data directory
    bool trace = true;
};

void print_usage() {
    std::fputs(
        "samples/00-empty — opens a window, ticks the runtime, writes a trace.\n"
        "\n"
        "  --platform <name> sdl3 (default), native, headless or stub\n"
        "  --headless        the same as --platform headless\n"
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
            options.backend = Backend::Headless;
        } else if (argument == "--platform" && has_value) {
            const std::string_view name{arguments[++i]};
            if (name == "sdl3") {
                options.backend = Backend::Sdl3;
            } else if (name == "native") {
                options.backend = Backend::Native;
            } else if (name == "headless") {
                options.backend = Backend::Headless;
            } else if (name == "stub") {
                options.backend = Backend::Stub;
            } else {
                std::fprintf(stderr, "00-empty: unknown platform '%.*s'\n\n",
                             static_cast<int>(name.size()), name.data());
                print_usage();
                return false;
            }
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

namespace {

// Every implementation this build has, constructed and none of them started. Holding all of them by
// value costs nothing — they are empty until initialise() — and it keeps the selection below a
// switch rather than a factory with an allocation in it.
struct Backends {
    cy::Sdl3Platform sdl3_platform;
    cy::StubPlatform stub_platform;
    cy::Sdl3DisplayServer sdl3_display;
    cy::HeadlessDisplayServer headless_display;
    cy::StubDisplayServer stub_display;
#if CY_SAMPLE_LINUX_NATIVE
    cy::LinuxPlatform native_platform;
    cy::X11DisplayServer native_display;
#endif
};

// Starts the pair `backend` names and hands back the two interfaces. The ONLY function in this file
// that knows which implementation is which; everything after it sees `cy::Platform&` and
// `cy::DisplayServer&`.
cy::Status select_backend(Backends& backends, Backend backend, int argument_count, char** arguments,
                          cy::Platform*& platform, cy::DisplayServer*& display) {
    switch (backend) {
        case Backend::Native: {
#if CY_SAMPLE_LINUX_NATIVE
            if (const cy::Status started =
                    backends.native_platform.initialise(argument_count, arguments);
                !started) {
                return started;
            }
            platform = &backends.native_platform;
            display = &backends.native_display;
            return backends.native_display.initialise();
#else
            return cy::fail(cy::ErrorCode::Unsupported,
                            "this build has no native platform backend: platform/linux-native/ is "
                            "built on Linux hosts and nothing else yet. Use --platform sdl3");
#endif
        }
        case Backend::Stub: {
            platform = &backends.stub_platform;
            display = &backends.stub_display;
            return backends.stub_display.initialise();
        }
        case Backend::Headless: {
            // The SDL3 Platform serves a headless run unchanged: process services, clocks and paths
            // need no window system, so only the display side is replaced. platform/headless/ says
            // the same thing in its own CMakeLists.
            if (const cy::Status started =
                    backends.sdl3_platform.initialise(argument_count, arguments);
                !started) {
                return started;
            }
            platform = &backends.sdl3_platform;
            display = &backends.headless_display;
            return backends.headless_display.initialise();
        }
        case Backend::Sdl3:
        default: {
            if (const cy::Status started =
                    backends.sdl3_platform.initialise(argument_count, arguments);
                !started) {
                return started;
            }
            platform = &backends.sdl3_platform;
            display = &backends.sdl3_display;
            return backends.sdl3_display.initialise();
        }
    }
}

// The mirror of select_backend(), and deliberately written next to it: a shutdown path that forgot
// one of the four is a leak nobody sees until a second run in the same process.
void shutdown_backend(Backends& backends, Backend backend) {
    switch (backend) {
        case Backend::Native:
#if CY_SAMPLE_LINUX_NATIVE
            backends.native_display.shutdown();
            backends.native_platform.shutdown();
#endif
            return;
        case Backend::Stub:
            backends.stub_display.shutdown();
            return;
        case Backend::Headless:
            backends.headless_display.shutdown();
            backends.sdl3_platform.shutdown();
            return;
        case Backend::Sdl3:
        default:
            backends.sdl3_display.shutdown();
            backends.sdl3_platform.shutdown();
            return;
    }
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }

    Backends backends;
    cy::Platform* platform_ptr = nullptr;
    cy::DisplayServer* display = nullptr;
    if (const cy::Status started = select_backend(backends, options.backend, argument_count,
                                                  arguments, platform_ptr, display);
        !started) {
        report("platform", started.error());
        shutdown_backend(backends, options.backend);
        return 1;
    }
    cy::Platform& platform = *platform_ptr;

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
        shutdown_backend(backends, options.backend);
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
    shutdown_backend(backends, options.backend);

    const char* reason = exit_reason(result);
    std::fprintf(stdout, "00-empty: %llu frames, %llu simulation ticks, exit %d (%s)\n",
                 static_cast<unsigned long long>(result.frames),
                 static_cast<unsigned long long>(frame.total_ticks), result.exit_code, reason);
    return result.exit_code;
}
