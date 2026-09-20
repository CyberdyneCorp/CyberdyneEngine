// samples/03-first-light — M3's closing artefact. Tasks 6.1 and 6.2.
//
// A lit, textured, shadowed scene with a moving camera, rendered through the render graph, in about
// four hundred lines across three files. It is the first program in this repository that draws
// anything, and it is deliberately the thing a contributor reads to find out how a frame is built.
//
//   just run-sample first-light                          sixty frames, and a report per run
//   just run-sample first-light --frames 1               one frame
//   just run-sample first-light --capture /tmp/a.ppm     write the last frame as an image
//   just run-sample first-light --origin 1000000         the same scene a million units out
//   just run-sample first-light --no-shadows             the control: the sun casts nothing
//   just run-sample first-light --no-aliasing            what the frame's targets cost unaliased
//   just run-sample first-light --backend null           the frame with no device at all
//   just run-sample first-light --platform native        the same frame, on platform/linux-native
//   just run-sample first-light --help
//
// ================================================================================================
// FOUR OBJECTS AND ONE LOOP, STILL
// ================================================================================================
//
// `samples/00-empty` established the shape and this file keeps it: the host owns the platform, the
// display server, the runtime and the loop; the runtime owns the frame. What M3 adds is a fifth
// object — a `cy::rhi::Device` — and a renderer that is handed one. Nothing in `scene.cpp` or
// `renderer.cpp` knows which backend it got, which is the whole of design.md §1 in one sentence.
//
// The renderer is driven from the loop body, immediately after `Runtime::tick()` — which is where
// the fixed ticks happen and where the variable-rate render belongs. It is NOT inside the runtime:
// `RuntimeConfig` has no renderer field and `Runtime::tick()`'s render step is still the empty seam
// M2 left, so the honest thing is for the host to own the call and for this comment to say that the
// seam is unfilled. Wiring `cy::render::RenderServer` into `runtime::ServerRegistry` is the piece
// that closes it, and it is a four-line adapter at layer 5 that this sample does not own.
//
// ================================================================================================
// EVERY LINE THIS PRINTS IS A FUNCTION OF THE CONTENT
// ================================================================================================
//
// The same discipline as `samples/02-headless-sim`, and for the same reason: `tests/render/` runs
// this renderer and compares numbers. Nothing here is timed, nothing here is a pointer, and the
// camera is a pure function of the frame index — so two runs on two machines print the same
// report, and a difference is a defect rather than a Tuesday.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/system_allocator.h>
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

#include <cy/core/determinism/clock.h>

// The build's own feature table, which is where CY_RENDERER_VULKAN comes from. A `#if` on a macro
// nobody defined is silently false, and it was: without this include the sample compiled, linked
// cy::rhi-vulkan, and then reported "the requested backend is not registered in this build" —
// because the registration below is the only thing that references the backend's translation unit,
// and a static initialiser in an archive nobody references is a static initialiser the linker
// drops. That is the exact failure the registration function exists to prevent, and it only works
// when the guard around it is true.
#include <cy_features.h>

#if defined(CY_RENDERER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#if defined(CY_RENDERER_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#endif

#include "renderer.h"
#include "scene.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::usize;

constexpr const char* kTag = "03-first-light";

// WHICH IMPLEMENTATION OF `Platform` AND `DisplayServer` HOSTS THE M3 FRAME. M11.d task 4.3.
//
// The frame itself is rendered OFFSCREEN and always has been — there is no swapchain here, and
// `samples/11-ship` is the artefact that presents. So none of these four can change one texel of
// the picture, and THAT IS THE CLAIM: `golden_legs.py` runs this program on two of them, captures
// the frame each time, and checks the two captures against each other and against the committed M3
// reference `tests/render/references/first_light.png`. A platform backend that had leaked into the
// frame would show up as a difference; the measurement is worth nothing unless the selector exists.
//
// `samples/00-empty` has had the same four since task 4.3 opened, and NOTHING BELOW main() KNOWS
// WHICH: the runtime, the host loop and the renderer see `cy::Platform` and `cy::DisplayServer`.
//
// The default is HEADLESS rather than sdl3, which is the pair this sample has used since M3 and the
// reason README.md's "Why there is no window" is still true. A default that opened a window would
// change what `just run-sample first-light` and every suite that drives this binary do.
enum class PlatformBackend {
    Headless,  // SDL3 for process services, no window system at all — the default since M3
    Sdl3,      // the desktop, over SDL3
    Native,    // the desktop, natively: POSIX and Xlib, with no SDL beneath it
    Stub,      // no desktop assumption at all: one unresizable window, one writable directory
};

struct Options {
    PlatformBackend platform_backend = PlatformBackend::Headless;
    u64 frames = 60;
    u32 width = 192;
    u32 height = 108;
    u32 boxes = 6;
    double origin = 0.0;
    bool shadows = true;
    bool aliasing = true;
    /// Null means "whichever backend answers", which is what `cy::rhi::create_device` is for.
    const char* backend = nullptr;
    const char* capture_path = nullptr;
    bool validation = true;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/03-first-light — a lit, textured, shadowed scene through the render graph.\n"
        "\n"
        "  --frames <n>       frames to render                     (default 60)\n"
        "  --width <n>        colour target width                  (default 192)\n"
        "  --height <n>       colour target height                 (default 108)\n"
        "  --boxes <n>        boxes in the ring                    (default 6)\n"
        "  --origin <units>   move the whole scene this far out    (default 0)\n"
        "  --no-shadows       the sun casts no shadow (the control)\n"
        "  --no-aliasing      place every transient at its own offset\n"
        "  --no-validation    do not ask the backend for its validation layers\n"
        "  --backend <name>   'vulkan' or 'null'; the default asks for the best available\n"
        "  --platform <name>  headless (default), sdl3, native or stub; the frame is the same\n"
        "  --capture <path>   write the last frame as a binary PPM\n"
        "  --help             this text\n",
        stderr);
}

/// Returns false when the command line is not one this sample understands, having said so.
bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int i = 1; i < argument_count; ++i) {
        const std::string_view argument{arguments[i]};
        const bool has_value = i + 1 < argument_count;

        if (argument == "--help") {
            options.help = true;
        } else if (argument == "--no-shadows") {
            options.shadows = false;
        } else if (argument == "--no-aliasing") {
            options.aliasing = false;
        } else if (argument == "--no-validation") {
            options.validation = false;
        } else if (argument == "--frames" && has_value) {
            options.frames = std::strtoull(arguments[++i], nullptr, 10);
        } else if (argument == "--width" && has_value) {
            options.width = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--height" && has_value) {
            options.height = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--boxes" && has_value) {
            options.boxes = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--origin" && has_value) {
            options.origin = std::strtod(arguments[++i], nullptr);
        } else if (argument == "--backend" && has_value) {
            options.backend = arguments[++i];
        } else if (argument == "--platform" && has_value) {
            const std::string_view name{arguments[++i]};
            if (name == "headless") {
                options.platform_backend = PlatformBackend::Headless;
            } else if (name == "sdl3") {
                options.platform_backend = PlatformBackend::Sdl3;
            } else if (name == "native") {
                options.platform_backend = PlatformBackend::Native;
            } else if (name == "stub") {
                options.platform_backend = PlatformBackend::Stub;
            } else {
                std::fprintf(stderr, "%s: unknown platform '%.*s'\n\n", kTag,
                             static_cast<int>(name.size()), name.data());
                print_usage();
                return false;
            }
        } else if (argument == "--capture" && has_value) {
            options.capture_path = arguments[++i];
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, arguments[i]);
            print_usage();
            return false;
        }
    }
    return true;
}

void report(const char* label, const cy::Error& error) {
    std::fprintf(stderr, "%s: %s failed: %s (%s)\n", kTag, label, error.message,
                 cy::error_code_name(error.code));
}

const char* severity_name(cy::rhi::ValidationSeverity severity) noexcept {
    switch (severity) {
        case cy::rhi::ValidationSeverity::Error:
            return "error";
        case cy::rhi::ValidationSeverity::Warning:
            return "warning";
        case cy::rhi::ValidationSeverity::Info:
            break;
    }
    return "info";
}

void print_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* /*user*/) noexcept {
    std::fprintf(stderr, "%s: validation %s: %s\n", kTag, severity_name(severity),
                 message != nullptr ? message : "");
}

/// Write the colour target as a binary PPM.
///
/// PPM rather than PNG because it is six lines and no dependency, and a sample should not carry an
/// image codec to prove that it drew something. `tests/render/` writes PNGs, because a reference
/// image and its difference have to be readable in a review and a PPM is not.
bool write_ppm(const char* path, cy::Span<const u32> texels, u32 width, u32 height) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "%s: cannot write '%s'\n", kTag, path);
        return false;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (const u32 texel : texels) {
        // Rgba8Unorm is little-endian byte order in a u32: red in the low byte.
        const unsigned char rgb[3] = {static_cast<unsigned char>(texel & 0xFFU),
                                      static_cast<unsigned char>((texel >> 8U) & 0xFFU),
                                      static_cast<unsigned char>((texel >> 16U) & 0xFFU)};
        (void)std::fwrite(rgb, 1, 3, file);
    }
    (void)std::fclose(file);
    return true;
}

// Every implementation this build has, constructed and none of them started. Holding all of them by
// value costs nothing — they are empty until initialise() — and it keeps the selection below a
// switch rather than a factory with an allocation in it. `samples/00-empty` holds the same four the
// same way, and the duplication is deliberate: a shared factory would be a fifth thing that knows
// which backend is which, in a layer that is not a host.
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

/// Starts the pair `backend` names and hands back the two interfaces.
cy::Status select_platform(Backends& backends, PlatformBackend backend, int argument_count,
                           char** arguments, cy::Platform*& platform, cy::DisplayServer*& display) {
    switch (backend) {
        case PlatformBackend::Native: {
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
                            "built on Linux hosts and nothing else yet. Use --platform headless");
#endif
        }
        case PlatformBackend::Stub: {
            platform = &backends.stub_platform;
            display = &backends.stub_display;
            return backends.stub_display.initialise();
        }
        case PlatformBackend::Sdl3: {
            if (const cy::Status started =
                    backends.sdl3_platform.initialise(argument_count, arguments);
                !started) {
                return started;
            }
            platform = &backends.sdl3_platform;
            display = &backends.sdl3_display;
            return backends.sdl3_display.initialise();
        }
        case PlatformBackend::Headless:
        default: {
            // The SDL3 Platform serves a headless run unchanged: process services, clocks and paths
            // need no window system, so only the display side is replaced. This is the pair this
            // sample has used since M3 and it is still the default.
            if (const cy::Status started =
                    backends.sdl3_platform.initialise(argument_count, arguments);
                !started) {
                return started;
            }
            platform = &backends.sdl3_platform;
            display = &backends.headless_display;
            return backends.headless_display.initialise();
        }
    }
}

/// The mirror of select_platform(), and deliberately written next to it: a shutdown path that
/// forgot one of the four is a leak nobody sees until a second run in the same process.
void shutdown_platform(Backends& backends, PlatformBackend backend) {
    switch (backend) {
        case PlatformBackend::Native:
#if CY_SAMPLE_LINUX_NATIVE
            backends.native_display.shutdown();
            backends.native_platform.shutdown();
#endif
            return;
        case PlatformBackend::Stub:
            backends.stub_display.shutdown();
            return;
        case PlatformBackend::Sdl3:
            backends.sdl3_display.shutdown();
            backends.sdl3_platform.shutdown();
            return;
        case PlatformBackend::Headless:
        default:
            backends.headless_display.shutdown();
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
    if (options.help) {
        print_usage();
        return 0;
    }

    // The pair `--platform` named. This sample renders OFFSCREEN whichever pair it is — see
    // README.md, "Why there is no window" — so the selector is not about the picture: it is about
    // who owns the process, the clock, the paths and the loop underneath it. `golden_legs.py`
    // measures that the picture does not notice.
    Backends backends;
    cy::Platform* platform_ptr = nullptr;
    cy::DisplayServer* display_ptr = nullptr;
    if (const cy::Status started =
            select_platform(backends, options.platform_backend, argument_count, arguments,
                            platform_ptr, display_ptr);
        !started) {
        report("platform", started.error());
        return 1;
    }
    cy::Platform& platform = *platform_ptr;
    cy::DisplayServer& display = *display_ptr;
    std::fprintf(stdout, "%s: platform platform=%.*s display=%.*s\n", kTag,
                 static_cast<int>(platform.name().size()), platform.name().data(),
                 static_cast<int>(display.name().size()), display.name().data());

    // FIXED-STEP, so the report is a function of the content rather than of how fast this machine
    // is. Four ticks a frame and no wall-clock pacing: the accumulator has no residue, so the
    // interpolation alpha is exactly zero — which is what samples/02-headless-sim's README
    // predicted would still be true until something asks for a variable rate.
    cy::RuntimeConfig runtime_config;
    runtime_config.platform = &platform;
    runtime_config.display = &display;
    runtime_config.window.title = "CyberdyneEngine — 03-first-light";
    runtime_config.build_identity = "cyberdyne 0.0.0 m3 sample 03-first-light";
    runtime_config.tick_mode = cy::determinism::TickMode::FixedStep;
    runtime_config.fixed_ticks_per_frame = 4;

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(runtime_config); !started) {
        report("runtime startup", started.error());
        shutdown_platform(backends, options.platform_backend);
        return 1;
    }

    // Both backends register themselves when linked; naming them makes it a statement rather than a
    // link-order property, which is the kind of thing that works everywhere except the one platform
    // nobody tested.
    (void)cy::rhi::null::register_null_backend();
#if defined(CY_RENDERER_VULKAN)
    (void)cy::rhi::vulkan::register_vulkan_backend();
#endif
#if defined(CY_RENDERER_METAL)
    (void)cy::rhi::metal::register_metal_backend();
#endif

    cy::rhi::DeviceDescription description;
    description.application_name = "cy_sample_first-light";
    description.enable_validation = options.validation;
    description.enable_synchronisation_validation = options.validation;
    // One queue. The frame declares four passes on graphics and nothing else, so asking for an
    // async-compute queue would only add a capability nothing in this sample uses.
    description.request_async_compute = false;

    cy::rhi::BackendSelection selection;
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::Expected<cy::rhi::Device*, cy::Error> device =
        cy::rhi::create_device(allocator, options.backend, description, selection);
    if (!device.has_value()) {
        report("device", device.error());
        return 1;
    }
    device.value()->set_validation_callback(&print_validation, nullptr);
    // The reason is empty when nothing had to be explained — `create_device` fills it in when it
    // FELL BACK, which is the case worth reading. Printing "because" with nothing after it is how a
    // successful run comes to look like a truncated line.
    const bool fell_back = selection.reason != nullptr && selection.reason[0] != '\0';
    std::fprintf(stdout, "%s: device   backend=%s asked=%s%s%s\n", kTag,
                 selection.selected != nullptr ? selection.selected : "(none)",
                 options.backend != nullptr ? options.backend : "(default)",
                 fell_back ? " because " : "", fell_back ? selection.reason : "");

    cy::sample::first_light::SceneDescription scene_description;
    scene_description.box_count = options.boxes;
    scene_description.origin = options.origin;
    scene_description.sun_shadows = options.shadows;

    cy::sample::first_light::Scene scene(allocator);
    if (const cy::Status built = scene.build(scene_description); !built) {
        report("scene", built.error());
        cy::rhi::destroy_device(allocator, device.value());
        runtime.shutdown();
        return 1;
    }
    std::fprintf(stdout,
                 "%s: scene    objects=%zu vertices=%zu indices=%zu origin=%.0f shadows=%s\n", kTag,
                 scene.objects().size(), scene.vertices().size(), scene.indices().size(),
                 options.origin, options.shadows ? "yes" : "no");

    cy::sample::first_light::RendererOptions renderer_options;
    renderer_options.width = options.width;
    renderer_options.height = options.height;
    renderer_options.aliasing = options.aliasing;

    // Scoped so the renderer's destructor runs before the device is destroyed. The renderer holds
    // the device's objects and cannot outlive it, and a scope says so more reliably than an order
    // of statements does.
    int exit_code = 0;
    {
        cy::sample::first_light::Renderer renderer(allocator, *device.value());
        if (const cy::Status prepared = renderer.prepare(scene, renderer_options); !prepared) {
            report("renderer", prepared.error());
            cy::rhi::destroy_device(allocator, device.value());
            runtime.shutdown();
            return 1;
        }

        cy::sample::first_light::FrameReport last{};
        u64 rendered = 0;
        cy::HostLoopOptions loop_options;
        loop_options.frame_limit = options.frames;
        const cy::HostLoopResult result = cy::run_host_loop(platform, loop_options, [&] {
            // The fixed ticks first, then one render. That order is `engine-architecture`'s loop
            // and M2's commit boundary: by the time the renderer reads anything, the tick's state
            // is authoritative.
            (void)runtime.tick();
            // The camera is a pure function of the frame index: one full orbit over the run,
            // whatever the run's length. That is what makes `--frames 1` and frame 0 of a
            // sixty-frame run the same image, which is what the golden suite relies on.
            const f32 phase = options.frames == 0
                                  ? 0.0F
                                  : static_cast<f32>(rendered) / static_cast<f32>(options.frames);
            const auto camera = scene.camera_at(phase);
            cy::Expected<cy::sample::first_light::FrameReport, cy::Error> frame =
                renderer.render(scene, camera);
            if (!frame.has_value()) {
                report("frame", frame.error());
                platform.request_exit(1);
                return;
            }
            last = *frame;
            ++rendered;
        });
        exit_code = result.exit_code;

        std::fprintf(stdout,
                     "%s: frame    passes=%u culled=%u submits=%u barriers=%u batches=%u "
                     "transfers=%u\n",
                     kTag, last.passes_recorded, last.passes_culled, last.submits, last.barriers,
                     last.barrier_batches, last.queue_ownership_transfers);
        std::fprintf(stdout, "%s: draws    draws=%u triangles=%u viewport=%ux%u\n", kTag,
                     last.draws, last.triangles, options.width, options.height);
        std::fprintf(stdout, "%s: memory   transients=%llu B unaliased=%llu B aliasing=%s\n", kTag,
                     static_cast<unsigned long long>(last.transient_bytes),
                     static_cast<unsigned long long>(last.transient_bytes_without_aliasing),
                     options.aliasing ? "on" : "off");
        std::fprintf(stdout, "%s: plan     hash=%016llx frames=%llu\n", kTag,
                     static_cast<unsigned long long>(last.plan_hash),
                     static_cast<unsigned long long>(rendered));
        const cy::FrameStats frame_stats = runtime.frame();
        std::fprintf(stdout, "%s: loop     ticks=%llu alpha=%.3f events=%u dropped=%u\n", kTag,
                     static_cast<unsigned long long>(frame_stats.total_ticks),
                     static_cast<double>(frame_stats.interpolation_alpha),
                     frame_stats.events_handled, frame_stats.events_dropped);

        if (options.capture_path != nullptr) {
            const cy::Span<const u32> texels = renderer.color_texels();
            if (texels.empty()) {
                std::fprintf(stdout,
                             "%s: capture  nothing to write — the %s backend executes no draws\n",
                             kTag, selection.selected != nullptr ? selection.selected : "(none)");
            } else if (write_ppm(options.capture_path, texels, options.width, options.height)) {
                std::fprintf(stdout, "%s: capture  %s (%ux%u)\n", kTag, options.capture_path,
                             options.width, options.height);
            } else {
                exit_code = 1;
            }
        }
    }

    cy::rhi::destroy_device(allocator, device.value());
    runtime.shutdown();
    shutdown_platform(backends, options.platform_backend);
    std::fprintf(stdout, "%s: exit %d (clean)\n", kTag, exit_code);
    return exit_code;
}
