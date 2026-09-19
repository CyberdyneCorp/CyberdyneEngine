// SPDX-License-Identifier: MIT
#include "present.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/platform.h>
#include <cy/platform/headless_display_server.h>
#include <cy/platform/sdl3_display_server.h>
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
#    include <cy/platform/x11_display_server.h>
#endif
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#ifdef CY_SHIP_HAS_VULKAN
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include <cstdio>
#include <cstring>

namespace cy::sample::ship {
namespace {

using rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

/// What the copy pass needs, handed through the record callback's `void*`.
struct BlitState {
    rendering::GraphExecutor* executor = nullptr;
    ResourceId staging = rendering::kInvalidResource;
    ResourceId target = rendering::kInvalidResource;
    ResourceId readback = rendering::kInvalidResource;
    /// The copy's extent, which is the swapchain's where the compositor gave us something smaller
    /// than we asked for.
    u32 width = 0;
    u32 height = 0;
    /// The STAGING BUFFER's row length in texels, which is the card's width and not the copy's. A
    /// copy whose row length followed its extent would read every row at the wrong offset the
    /// moment the two differ, and the picture would shear rather than fail.
    u32 row_length = 0;
};

void record_blit(const rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<BlitState*>(user);
    rhi::BufferTextureCopy region;
    region.buffer_row_length = state->row_length;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_buffer_to_texture(state->executor->buffer(state->staging),
                                             state->executor->texture(state->target),
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void record_capture(const rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<BlitState*>(user);
    rhi::BufferTextureCopy region;
    region.buffer_row_length = state->row_length;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->target),
                                             state->executor->buffer(state->readback),
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// WHAT KIND OF DEVICE ANSWERED — task 8.3, and the spike's sharpest finding applied to this host.
///
/// M11.d's spike (design.md §1.4.1) found two D3D12 adapters on a hosted Windows runner, both
/// `Microsoft Basic Render Driver`, and **adapter 0 did not set the software flag**: "the label has
/// to come from the adapter's identity, not from its flag". This engine has no device-type query at
/// all — `DeviceCapabilities` carries `device_name()` and `driver_version()` and nothing that says
/// discrete, integrated or software — so the identity is the only thing there is to read, and this
/// function reads it and says that is what it did. Guessing "hardware" from the absence of evidence
/// is the failure the spike named.
[[nodiscard]] std::string classify_device(rhi::BackendKind backend, const char* name) noexcept {
    if (backend == rhi::BackendKind::Null) {
        return "null backend (no device; a complete implementation that cannot present)";
    }
    static const char* const kSoftwareNames[] = {"llvmpipe",  "lavapipe",           "swiftshader",
                                                 "softpipe",  "Basic Render",       "WARP",
                                                 "Microsoft", "Software Rasterizer"};
    for (const char* needle : kSoftwareNames) {
        if (std::strstr(name, needle) != nullptr) {
            return "software (named by the device's own identity)";
        }
    }
    return "hardware by name (this engine has no device-type query: M11.d finding, present.cpp)";
}

[[nodiscard]] const char* swapchain_format_name(rhi::Format format) noexcept {
    switch (format) {
        case rhi::Format::Bgra8Srgb:
            return "Bgra8Srgb";
        case rhi::Format::Bgra8Unorm:
            return "Bgra8Unorm";
        case rhi::Format::Rgba8Srgb:
            return "Rgba8Srgb";
        case rhi::Format::Rgba8Unorm:
            return "Rgba8Unorm";
        default:
            return "other";
    }
}

[[nodiscard]] bool is_bgra(rhi::Format format) noexcept {
    return format == rhi::Format::Bgra8Srgb || format == rhi::Format::Bgra8Unorm;
}

/// Copy the card into the staging buffer in the SWAPCHAIN's channel order.
///
/// The swapchain negotiates its own format — `vulkan_swapchain.cpp` says so in as many words, "a
/// swapchain is a negotiation, and the honest answer is what it got" — and on this host it settles
/// on BGRA. Swizzling here rather than asking for RGBA and hoping is the difference between a
/// picture and a blue-and-red picture.
void fill_staging(u8* destination, const Image& image, bool swap_red_and_blue) noexcept {
    const usize texels = static_cast<usize>(image.width) * image.height;
    for (usize index = 0; index < texels; ++index) {
        const u8* source = image.pixels.data() + (index * 4U);
        u8* target = destination + (index * 4U);
        target[0] = swap_red_and_blue ? source[2] : source[0];
        target[1] = source[1];
        target[2] = swap_red_and_blue ? source[0] : source[2];
        target[3] = source[3];
    }
}

void read_photograph(Image& out, const u8* source, u32 width, u32 height, u32 row_length,
                     bool swap_red_and_blue) noexcept {
    out.width = width;
    out.height = height;
    out.pixels.assign(out.byte_size(), 255);
    for (u32 row = 0; row < height; ++row) {
        const u8* line = source + (static_cast<usize>(row) * row_length * 4U);
        u8* target_line = out.pixels.data() + (static_cast<usize>(row) * width * 4U);
        for (u32 column = 0; column < width; ++column) {
            const u8* texel = line + (static_cast<usize>(column) * 4U);
            u8* target = target_line + (static_cast<usize>(column) * 4U);
            target[0] = swap_red_and_blue ? texel[2] : texel[0];
            target[1] = texel[1];
            target[2] = swap_red_and_blue ? texel[0] : texel[2];
            target[3] = 255;  // a screenshot is opaque; the swapchain's alpha is the compositor's
        }
    }
}

void report_validation(rhi::ValidationSeverity severity, const char* message,
                       void* /*user*/) noexcept {
    const char* label = severity == rhi::ValidationSeverity::Error     ? "error"
                        : severity == rhi::ValidationSeverity::Warning ? "warning"
                                                                       : "info";
    std::fprintf(stderr, "11-ship: vulkan validation %s: %s\n", label,
                 message != nullptr ? message : "");
}

}  // namespace

const char* platform_choice_name(PlatformChoice choice) noexcept {
    switch (choice) {
        case PlatformChoice::Auto:
            return "auto";
        case PlatformChoice::Sdl3:
            return "sdl3";
        case PlatformChoice::Native:
            return "native";
        case PlatformChoice::Headless:
            return "headless";
    }
    return "?";
}

std::vector<PlatformChoice> available_platforms() noexcept {
    std::vector<PlatformChoice> choices;
    choices.push_back(PlatformChoice::Sdl3);
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
    // THE NATIVE LEG. `platform/linux-native/` — M11.d section 4 — implements the same
    // `cy::DisplayServer` SDL3 implements, with Xlib and XRandR beneath it and no SDL anywhere.
    // This sample reaches it through the interface and through nothing else: the only line in this
    // file that knows which one it got is the `case` below, and everything after
    // `DisplayServers::start` returns is written against `DisplayServer&`. That is the proof
    // `core-platform-abstraction`'s Complete cell rests on, and an artefact is where it is made.
    choices.push_back(PlatformChoice::Native);
#endif
    choices.push_back(PlatformChoice::Headless);
    return choices;
}

namespace {

/// The display servers this binary was built with, one of which is started.
///
/// All three are constructed and exactly one is INITIALISED, which is the arrangement
/// samples/00-empty uses for its two: construction is free, and `initialise()` is what opens a
/// connection to a window system that may not be there. `started` is the one the rest of this file
/// sees, and it sees it as a `cy::DisplayServer&`.
class DisplayServers {
public:
    [[nodiscard]] Expected<DisplayServer*, Error> start(PlatformChoice choice) noexcept {
        switch (choice) {
            case PlatformChoice::Auto:
            case PlatformChoice::Sdl3:
                if (const Status started = sdl3_.initialise(); !started) {
                    return make_unexpected(started.error());
                }
                started_ = &sdl3_;
                break;
            case PlatformChoice::Headless:
                if (const Status started = headless_.initialise(); !started) {
                    return make_unexpected(started.error());
                }
                started_ = &headless_;
                break;
            case PlatformChoice::Native:
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
                if (const Status started = native_.initialise(); !started) {
                    return make_unexpected(started.error());
                }
                started_ = &native_;
                break;
#else
                return fail(ErrorCode::Unsupported,
                            "this binary was built without platform/linux-native, so it has no "
                            "native display server; see available_platforms() in present.cpp");
#endif
        }
        return started_;
    }

    void stop() noexcept {
        if (started_ == &sdl3_) {
            sdl3_.shutdown();
        } else if (started_ == &headless_) {
            headless_.shutdown();
        }
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
        else if (started_ == &native_) {
            native_.shutdown();
        }
#endif
        started_ = nullptr;
    }

private:
    Sdl3DisplayServer sdl3_;
    HeadlessDisplayServer headless_;
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
    X11DisplayServer native_;
#endif
    DisplayServer* started_ = nullptr;
};

}  // namespace

PresentReport present_card(Platform& platform, Image& image,
                           const PresentOptions& options) noexcept {
    PresentReport report;
    // The platform is the process's own abstraction — user directories, exit, standard streams —
    // and this function needs none of them: everything here is the DISPLAY server's. It is taken
    // anyway because `main` chose the two together and a leg that opened an X11 window through an
    // SDL3 platform would be a leg that had not replaced SDL3 at all.
    (void)platform;

    const PlatformChoice choice =
        options.platform == PlatformChoice::Auto ? PlatformChoice::Sdl3 : options.platform;
    DisplayServers servers;
    const Expected<DisplayServer*, Error> started = servers.start(choice);
    if (!started) {
        report.not_evaluated = std::string("the '") + platform_choice_name(choice) +
                               "' display server did not start: " + started.error().message;
        return report;
    }
    DisplayServer* display = *started;
    report.display_server = std::string(display->name());

    WindowDescription window_description;
    window_description.title = "CyberEngine — samples/11-ship (M11.d)";
    window_description.size = Extent{static_cast<i32>(image.width), static_cast<i32>(image.height)};
    window_description.flags = WindowFlags::None;  // a fixed-size card; resizing is not the claim
    const Expected<WindowId, Error> window = display->create_window(window_description);
    if (!window) {
        report.not_evaluated = std::string("no window: ") + window.error().message;
        servers.stop();
        return report;
    }
    report.window_opened = true;

    // The surface is queried before it is created, which is what `has_feature` exists for. A
    // headless display server answers false here and the leg reports itself unevaluated rather
    // than failing — `samples/00-empty --headless` is how continuous integration runs this engine.
    if (!display->has_feature(Feature::VulkanSurface)) {
        report.not_evaluated = std::string("the '") + report.display_server +
                               "' display server reports no VulkanSurface feature, so there is no "
                               "surface to present to";
        display->destroy_window(*window);
        servers.stop();
        return report;
    }

    Allocator& allocator = system_allocator(MemoryDomain::Gpu);
#ifdef CY_SHIP_HAS_VULKAN
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription device_description;
    device_description.application_name = "cy_sample_ship";
    device_description.enable_validation = options.validation;
    device_description.enable_synchronisation_validation = options.validation;
    rhi::BackendSelection selection;
    Expected<rhi::Device*, Error> device =
        rhi::create_device(allocator, "vulkan", device_description, selection);
    if (!device) {
        report.not_evaluated = std::string("no device: ") + device.error().message;
        display->destroy_window(*window);
        servers.stop();
        return report;
    }
    rhi::Device& gpu = **device;
    if (options.validation) {
        gpu.set_validation_callback(&report_validation, nullptr);
    }
    report.backend = selection.selected;
    report.device_name = gpu.capabilities().device_name();
    report.device_class =
        classify_device(gpu.capabilities().backend(), gpu.capabilities().device_name());

    // The device onto the card, before the first frame. Written here rather than by the caller
    // because here is the first place it is known, and the presented image has to be the one that
    // says it.
    draw_text(image, 48, static_cast<i32>(image.height) - 44, 2, options.device_line_colour,
              std::string("DEVICE: ") + report.device_name);
    draw_text(image, 48, static_cast<i32>(image.height) - 20, 2, options.device_line_colour,
              report.device_class);

    if (gpu.capabilities().backend() != rhi::BackendKind::Vulkan) {
        report.not_evaluated =
            std::string("the '") + selection.selected + "' backend answered instead of Vulkan (" +
            selection.reason +
            "), and a swapchain needs the Vulkan instance the platform creates a surface against";
        rhi::destroy_device(allocator, &gpu);
        display->destroy_window(*window);
        servers.stop();
        return report;
    }

#ifdef CY_SHIP_HAS_VULKAN
    SurfaceDescription surface_description;
    surface_description.api = GraphicsApi::Vulkan;
    // The one thing a host has to know about its backend, and M11.d added the accessor that makes
    // it reachable — see vulkan_backend.h, which argues why it is on the backend and not on Device.
    surface_description.api_instance = rhi::vulkan::vulkan_instance_handle(gpu);
    const Expected<NativeSurface, Error> surface =
        display->create_surface(*window, surface_description);
    if (!surface) {
        report.not_evaluated = std::string("no surface: ") + surface.error().message;
        rhi::destroy_device(allocator, &gpu);
        display->destroy_window(*window);
        servers.stop();
        return report;
    }
    report.surface_created = true;

    rhi::SwapchainDescription swapchain_description;
    swapchain_description.name = "11-ship";
    swapchain_description.native_surface = surface->handle;
    swapchain_description.extent = rhi::Extent2D{image.width, image.height};
    const Expected<rhi::SwapchainHandle, Error> swapchain =
        gpu.create_swapchain(swapchain_description);
    if (!swapchain) {
        report.not_evaluated = std::string("no swapchain: ") + swapchain.error().message;
        display->destroy_surface(*surface);
        rhi::destroy_device(allocator, &gpu);
        display->destroy_window(*window);
        servers.stop();
        return report;
    }
    report.swapchain_created = true;

    const rhi::SwapchainInfo info = gpu.swapchain_info(*swapchain);
    report.swapchain_format = swapchain_format_name(info.format);
    report.swapchain_width = info.extent.width;
    report.swapchain_height = info.extent.height;
    const bool swizzle = is_bgra(info.format);
    const u32 copy_width = info.extent.width < image.width ? info.extent.width : image.width;
    const u32 copy_height = info.extent.height < image.height ? info.extent.height : image.height;

    // One staging buffer, filled once: the card does not change between frames, so re-uploading it
    // every frame would measure the upload path rather than the presentation path.
    rhi::BufferDescription staging_description;
    staging_description.name = "11-ship.card";
    staging_description.size = static_cast<u64>(image.width) * image.height * 4U;
    staging_description.usage = rhi::BufferUsage::TransferSource;
    staging_description.memory = rhi::MemoryUse::Upload;
    const Expected<rhi::BufferHandle, Error> staging = gpu.create_buffer(staging_description);

    rhi::BufferDescription readback_description;
    readback_description.name = "11-ship.photograph";
    readback_description.size = staging_description.size;
    readback_description.usage = rhi::BufferUsage::TransferDestination;
    readback_description.memory = rhi::MemoryUse::Readback;
    const Expected<rhi::BufferHandle, Error> readback =
        options.capture ? gpu.create_buffer(readback_description)
                        : Expected<rhi::BufferHandle, Error>{rhi::BufferHandle{}};

    if (!staging || (options.capture && !readback)) {
        report.not_evaluated = "the staging or readback buffer could not be created";
        gpu.destroy_swapchain(*swapchain);
        display->destroy_surface(*surface);
        rhi::destroy_device(allocator, &gpu);
        display->destroy_window(*window);
        servers.stop();
        return report;
    }
    auto* mapped = static_cast<u8*>(gpu.buffer_mapped_pointer(*staging));
    const Expected<rhi::SemaphoreHandle, Error> acquired = gpu.create_semaphore();
    const Expected<rhi::SemaphoreHandle, Error> presented = gpu.create_semaphore();
    if (mapped == nullptr || !acquired || !presented) {
        // Reported rather than skipped. An unmapped staging buffer presents a BLACK window, which
        // looks like a frame and is not one — the class of outcome this whole file is written to
        // make impossible to confuse with a pass.
        report.not_evaluated =
            mapped == nullptr ? "the staging buffer did not map, so there was nothing to present"
                              : "presentation's binary semaphores could not be created";
        if (options.capture) {
            gpu.destroy_buffer(*readback);
        }
        gpu.destroy_buffer(*staging);
        gpu.destroy_swapchain(*swapchain);
        display->destroy_surface(*surface);
        rhi::destroy_device(allocator, &gpu);
        display->destroy_window(*window);
        servers.stop();
        return report;
    }
    fill_staging(mapped, image, swizzle);

    // THE EXECUTOR IS SCOPED, AND THAT IS A CRASH THIS SAMPLE ALREADY PAID FOR.
    //
    // `GraphExecutor` holds the device and releases its transient pool in its DESTRUCTOR. Declared
    // at function scope it is destroyed at `return`, which is after `destroy_device` below —
    // `device_->release_transient_resources()` on a freed device. It segfaulted on the native X11
    // leg and did not on the SDL3 one, which is what a dangling pointer does: the same bug, one
    // allocator's luck apart. The brace is the fix and the brace is why it is here.
    bool captured = false;
    {
        rendering::GraphExecutor executor(allocator, gpu);

        for (u32 frame = 0; frame < options.frames; ++frame) {
            // The window's own events. A close request ends the run the way samples/00-empty does:
            // the intent is recorded and the loop observes it, rather than the event handler
            // exiting.
            display->pump_events();
            WindowEvent event;
            bool closing = false;
            while (display->poll_event(event)) {
                closing = closing || event.type == WindowEventType::CloseRequested;
            }
            if (closing) {
                break;
            }

            if (!gpu.begin_frame()) {
                break;
            }
            const Expected<u32, Error> index =
                gpu.acquire_next_image(*swapchain, *acquired, 1'000'000'000ULL);
            if (!index) {
                // An out-of-date swapchain is the window changing size, which the interface's own
                // comment says "the caller answers by resizing rather than by failing the frame".
                // The window is asked what it is now rather than the swapchain being told what it
                // was.
                if (const Expected<Extent, Error> size = display->window_size(*window); size) {
                    (void)gpu.resize_swapchain(*swapchain,
                                               rhi::Extent2D{static_cast<u32>(size->width),
                                                             static_cast<u32>(size->height)});
                }
                (void)gpu.end_frame();
                continue;
            }

            rendering::RenderGraph graph(allocator);

            rendering::BufferRequest staging_request;
            staging_request.name = "card";
            staging_request.size = staging_description.size;
            staging_request.extra_usage = rhi::BufferUsage::TransferSource;
            const ResourceId staging_id = graph.import_buffer(staging_request, *staging);

            rendering::TextureRequest target_request;
            target_request.name = "swapchain";
            target_request.format = info.format;
            target_request.width = info.extent.width;
            target_request.height = info.extent.height;
            target_request.extra_usage =
                rhi::TextureUsage::TransferDestination | rhi::TextureUsage::TransferSource;
            // `Undefined` is the honest state after an acquire: the presentation engine promises
            // the image, not its contents, and this frame overwrites every texel of it. `ImageUse`
            // rather than a Vulkan image layout because M11.d section 1 made it one — gap 3 — and
            // this is the first caller outside the graph's own tests to say so.
            const ResourceId target_id =
                graph.import_texture(target_request, gpu.swapchain_texture(*swapchain, *index),
                                     rhi::ImageUse::Undefined);

            const bool capture_this_frame = options.capture && !captured;
            ResourceId readback_id = rendering::kInvalidResource;
            if (capture_this_frame) {
                rendering::BufferRequest readback_request;
                readback_request.name = "photograph";
                readback_request.size = readback_description.size;
                readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
                readback_id = graph.import_buffer(readback_request, *readback);
            }

            BlitState state;
            state.executor = &executor;
            state.staging = staging_id;
            state.target = target_id;
            state.readback = readback_id;
            state.width = copy_width;
            state.height = copy_height;
            state.row_length = image.width;

            graph.add_pass("card", QueueKind::Graphics)
                .read(staging_id, Access::TransferRead)
                .write(target_id, Access::TransferWrite)
                .record(&record_blit, &state);

            if (capture_this_frame) {
                // THE PHOTOGRAPH IS OF THE PRESENTED IMAGE, not of the CPU buffer it came from. A
                // screenshot composed on the host would prove the compositor nothing; this one is
                // the device reading back what it is about to hand to the presentation engine.
                graph.add_pass("photograph", QueueKind::Graphics)
                    .read(target_id, Access::TransferRead)
                    .write(readback_id, Access::TransferWrite)
                    .record(&record_capture, &state);
                graph.add_pass("host", QueueKind::Graphics)
                    .read(readback_id, Access::HostRead)
                    .side_effect();
            }

            // The pass that hands the image back. It records nothing: what it declares is the
            // LAYOUT the presentation engine needs, and the graph derives the transition like every
            // other one.
            graph.add_pass("present", QueueKind::Graphics)
                .read(target_id, Access::Present)
                .side_effect();

            // A graph that failed to declare something refuses to compile rather than compiling a
            // plan with a hole in it, and `status()` is where the accumulated failure surfaces.
            // Checking it is one line and the alternative is an error attributed to the executor.
            if (const Status declared = graph.status(); !declared) {
                std::fprintf(stderr, "11-ship: the frame's graph is not well formed: %s\n",
                             declared.error().message);
                (void)gpu.end_frame();
                break;
            }

            rendering::ExecuteOptions execute;
            execute.wait_acquire = *acquired;
            execute.signal_present = *presented;
            const Expected<rendering::ExecutionResult, Error> executed =
                executor.execute(graph, rendering::CompileOptions{}, execute);
            if (!executed) {
                std::fprintf(stderr, "11-ship: the frame did not execute: %s\n",
                             executed.error().message);
                (void)gpu.end_frame();
                break;
            }

            report.submits = executed->submits;
            report.passes_recorded = executed->passes_recorded;
            report.barriers = executed->barriers;
            report.plan_hash = executed->plan_hash;

            if (gpu.present(*swapchain, *index, *presented)) {
                ++report.frames_presented;
            }
            // One frame at a time. A sample that showed a still card has nothing to gain from
            // frames in flight, and waiting here is what makes the acquire semaphore safe to reuse
            // next frame without a per-image fence — stated because it is a simplification, not an
            // oversight.
            (void)gpu.wait_idle();
            (void)gpu.end_frame();

            if (capture_this_frame) {
                if (const auto* bytes =
                        static_cast<const u8*>(gpu.buffer_mapped_pointer(*readback));
                    bytes != nullptr) {
                    read_photograph(report.photograph, bytes, copy_width, copy_height, image.width,
                                    swizzle);
                    captured = true;
                }
            }
        }

        report.validation_errors = gpu.statistics().validation_errors;

        executor.release();
    }  // the executor is destroyed HERE, while the device it holds is still alive

    (void)gpu.wait_idle();
    if (options.capture) {
        gpu.destroy_buffer(*readback);
    }
    gpu.destroy_buffer(*staging);
    gpu.destroy_semaphore(*acquired);
    gpu.destroy_semaphore(*presented);
    gpu.destroy_swapchain(*swapchain);
    display->destroy_surface(*surface);
#else
    report.not_evaluated =
        "this binary was built with CY_RENDERER_VULKAN off, so it has no backend that can create a "
        "swapchain; the card was composed and written and nothing was presented";
#endif

    rhi::destroy_device(allocator, &gpu);
    display->destroy_window(*window);
    servers.stop();
    return report;
}

}  // namespace cy::sample::ship
