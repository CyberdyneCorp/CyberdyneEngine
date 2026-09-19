// The engine runs frames on a platform that shares no desktop assumption. M11.d task 4.4.
//
// ================================================================================================
// WHY THIS IS THE PORTING SURFACE'S PROOF AND platform/linux-native/ IS NOT
// ================================================================================================
//
// `platform/desktop-sdl3/` and `platform/linux-native/` disagree about which library opens a window
// and agree about everything else a desktop takes for granted. Two desktops do not discover that a
// porting surface is a desktop interface; only something that is not a desktop does.
//
// `platform/stub/` answers **false to every `Feature`**, has **one** unresizable window that is the
// whole display, **one** writable directory, **no** command line, **no** subprocesses, **no**
// dynamic loader, **one** core and **no** SIMD — and this file starts the runtime on it and runs
// frames. What it asserts is not that the stub works: it is that **`Runtime` does not need any of
// the things the stub does not have.**
//
// ================================================================================================
// AND IT DRIVES THE LOOP ITSELF
// ================================================================================================
//
// `core-platform-abstraction` requires the main loop to be the platform's and not the engine's.
// `platform/host/`'s `run_host_loop()` is the desktop's answer; a console's presentation callback
// and a browser's animation frame are others. This case is the cheapest possible demonstration that
// the engine does not care which: a plain `for` calling `tick()`, with no host loop anywhere in it.
// If `Runtime` ever grew a dependency on being driven by `run_host_loop()`, this is the file that
// would stop compiling.

#include <cy/core/base/expected.h>
#include <cy/platform/stub_display_server.h>
#include <cy/platform/stub_platform.h>
#include <cy/runtime/runtime.h>
#include <cy/test/test.h>

#include <cstring>

using cy::u32;
using cy::u64;
using cy::usize;

namespace {

/// The runtime, started on the stub platform and shut down with the case.
struct StubRuntime {
    ~StubRuntime() {
        if (started) {
            runtime.shutdown();
        }
        display.shutdown();
    }

    [[nodiscard]] bool start() {
        if (!display.initialise().has_value()) {
            return false;
        }
        cy::RuntimeConfig config;
        config.platform = &platform;
        config.display = &display;
        config.window.title = "stub";
        config.build_identity = "cyberdyne stub platform test";
        // No trace: the stub's one writable directory is a path that need not exist, and a trace
        // file is not what this case is about.
        config.trace_path = nullptr;
        started = runtime.startup(config).has_value();
        return started;
    }

    cy::StubPlatform platform;
    cy::StubDisplayServer display;
    cy::Runtime runtime;
    bool started = false;
};

}  // namespace

CY_TEST_CASE("stub platform: the runtime starts and runs frames with no desktop beneath it") {
    StubRuntime host;
    // STARTUP SUCCEEDS. It is worth stating plainly what that means: the runtime brought up its
    // subsystems on a platform with one core, no environment, no executable path, no dynamic
    // loader and no way to spawn anything — and asked for none of them.
    CY_REQUIRE(host.start());
    CY_CHECK(host.runtime.main_window() != cy::kInvalidWindow);

    // The loop is the caller's. Nothing in here is run_host_loop().
    constexpr u64 kFrames = 8;
    for (u64 frame = 0; frame < kFrames; ++frame) {
        (void)host.runtime.tick();
    }

    const cy::FrameStats stats = host.runtime.frame();
    CY_CHECK_EQ(stats.frame_index, kFrames);
    // Simulation ticks advanced too, which is what separates "the runtime did not refuse to start"
    // from "the engine ran".
    CY_CHECK(stats.total_ticks > 0);
}

CY_TEST_CASE("stub platform: every capability is absent, and every absence is reported") {
    cy::StubDisplayServer display;
    CY_REQUIRE(display.initialise(cy::Extent{640, 480}).has_value());

    // FALSE FOR EVERY ENUMERATOR, walked rather than sampled. This is the only implementation in
    // the tree that says no to all of them, and a backend that quietly answered true for one it had
    // not implemented is exactly what has_feature() exists to prevent.
    const cy::Feature features[] = {
        cy::Feature::WindowResizable,   cy::Feature::WindowBorderless,
        cy::Feature::WindowAlwaysOnTop, cy::Feature::WindowTransparency,
        cy::Feature::WindowNoFocus,     cy::Feature::WindowPopup,
        cy::Feature::MousePassthrough,  cy::Feature::HighDpi,
        cy::Feature::PerScreenDpiScale, cy::Feature::ExclusiveFullscreen,
        cy::Feature::VSyncAdaptive,     cy::Feature::VSyncMailbox,
        cy::Feature::ScreenRefreshRate, cy::Feature::VulkanSurface,
        cy::Feature::MetalSurface,      cy::Feature::D3D12Surface,
        cy::Feature::Clipboard,         cy::Feature::NativeFileDialog,
        cy::Feature::NativeMessageDialog, cy::Feature::CustomCursor,
        cy::Feature::ImePositioning,    cy::Feature::OnScreenKeyboard,
        cy::Feature::ScreenOrientation, cy::Feature::KeepAwake,
        cy::Feature::SystemTray,
    };
    for (const cy::Feature feature : features) {
        CY_CHECK_FALSE(display.has_feature(feature));
    }

    cy::WindowDescription description;
    description.size = cy::Extent{1920, 1080};
    description.flags = cy::WindowFlags::Resizable | cy::WindowFlags::Borderless;
    const auto window = display.create_window(description);
    CY_REQUIRE(window.has_value());

    // EVERY REQUESTED FLAG WAS DROPPED and the window was created anyway — the specification's
    // degradation rule at its limit. A caller finds out by asking, which is why the return of
    // create_window() is a window and not a report.
    const auto flags = display.window_flags(window.value());
    CY_REQUIRE(flags.has_value());
    CY_CHECK_EQ(static_cast<u32>(flags.value()), static_cast<u32>(cy::WindowFlags::None));

    // The requested size was dropped too: the window IS the display.
    const auto size = display.window_size(window.value());
    CY_REQUIRE(size.has_value());
    CY_CHECK_EQ(size.value().width, 640);
    CY_CHECK_EQ(size.value().height, 480);

    // A second window is refused rather than queued, and the refusal says why.
    const auto second = display.create_window(description);
    CY_REQUIRE_FALSE(second.has_value());
    CY_CHECK_EQ(second.error().code, cy::ErrorCode::Unsupported);

    // Not resizable, and it says so rather than accepting the call and doing nothing.
    const cy::Status resized = display.set_window_size(window.value(), cy::Extent{800, 600});
    CY_CHECK_FALSE(resized.has_value());
    CY_CHECK_EQ(resized.error().code, cy::ErrorCode::Unsupported);

    // No native handle for any API, which is the path `Runtime::enter_display()` treats as a
    // warning rather than a startup failure. No other implementation in this tree exercises it.
    const auto surface =
        display.create_surface(window.value(), cy::SurfaceDescription{cy::GraphicsApi::None,
                                                                     nullptr});
    CY_REQUIRE_FALSE(surface.has_value());
    CY_CHECK_EQ(surface.error().code, cy::ErrorCode::Unavailable);

    display.shutdown();
}

CY_TEST_CASE("stub platform: no command line, one writable directory, and no process table") {
    cy::StubPlatform platform;

    // No command line. Code that reads its configuration only from argv does not run on a target
    // like this one, and the way to find that out is for there to be none.
    CY_CHECK_EQ(platform.argument_count(), usize{0});
    CY_CHECK(platform.argument(0).empty());

    // ONE writable directory: all three answer the same path. A desktop has three and they are
    // distinct; a target with a single mount does not, and anything that relies on clearing the
    // cache without touching the save data behaves differently here.
    char data[256];
    char config[256];
    char cache[256];
    CY_REQUIRE(platform.user_data_directory(data, sizeof(data)).has_value());
    CY_REQUIRE(platform.user_config_directory(config, sizeof(config)).has_value());
    CY_REQUIRE(platform.user_cache_directory(cache, sizeof(cache)).has_value());
    CY_CHECK_EQ(std::strcmp(data, config), 0);
    CY_CHECK_EQ(std::strcmp(data, cache), 0);
    const usize length = std::strlen(data);
    CY_REQUIRE(length > 0);
    CY_CHECK_EQ(data[length - 1], '/');

    // Unsupported, each with a sentence. `Unsupported` and not `NotImplemented`: the second says
    // "later" and the first says "not on this target", and a caller deciding whether to wait for a
    // feature or do without it needs the difference.
    char path[256];
    CY_CHECK_EQ(platform.executable_path(path, sizeof(path)).error().code,
                cy::ErrorCode::Unsupported);
    CY_CHECK_EQ(platform.load_library("libanything.so").error().code, cy::ErrorCode::Unsupported);

    const char* argv[] = {"true", nullptr};
    cy::ProcessOptions options;
    options.arguments = argv;
    options.argument_count = 1;
    CY_CHECK_EQ(platform.spawn_process(options).error().code, cy::ErrorCode::Unsupported);

    // One core and no instruction-set extensions — the interesting answer rather than the lazy one.
    CY_CHECK_EQ(platform.cpu_count(), u32{1});
    const cy::CpuFeatures cpu = platform.cpu_features();
    CY_CHECK_FALSE(cpu.sse42);
    CY_CHECK_FALSE(cpu.avx2);
    CY_CHECK_FALSE(cpu.neon);

    // The clocks are real, because no target can be without one and a platform that answered zero
    // would make every frame take no time and every test that measures one vacuous.
    const cy::Nanoseconds first = platform.monotonic_nanoseconds();
    CY_CHECK(platform.monotonic_nanoseconds() >= first);
    CY_CHECK(platform.wall_nanoseconds() > 0);

    // Exit is requested, never taken: the first request wins and the host's loop observes it.
    platform.request_exit(3);
    platform.request_exit(9);
    CY_CHECK(platform.exit_requested());
    CY_CHECK_EQ(platform.exit_code(), 3);
}
