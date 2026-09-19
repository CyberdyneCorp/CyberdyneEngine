#pragma once
// The window half: a display server, a device, a surface, a swapchain, and a frame that is
// PRESENTED. M11.d task 8.2.
//
// --- WHAT IS NEW HERE, AND IT IS NOT THE PICTURE --------------------------------------------------
//
// This is the first thing in this repository that presents a frame to a window. Eleven milestones
// of rendering — the golden images, the beauty shot, the world, the character — every one of them
// drew into an offscreen image and read it back. `cy::rhi::Device::create_swapchain` has existed
// since M3 with a file of its own (`vulkan_swapchain.cpp`), `GraphExecutor`'s `wait_acquire` and
// `signal_present` have existed just as long, and until this file nothing had ever called any of
// them. The seam they were written for — "the RHI never talks to a window system; DisplayServer
// produces the surface" — is therefore first exercised here, which is exactly the seam a SECOND
// platform backend has to satisfy.
//
// That is why the artefact of a packaging rung draws at all: presenting is the one thing a platform
// backend does that a headless test cannot fake.
//
// --- WHAT IT DOES WHEN IT CANNOT ------------------------------------------------------------------
//
// Every step is allowed to be absent and each absence is reported rather than worked around:
//
//   no display server      `--platform headless`, or no DISPLAY/WAYLAND_DISPLAY: the card is
//                          composed and written, and presentation reports NOT EVALUATED
//   no Vulkan loader       `create_device` falls back to the null backend, which is a real
//                          implementation and cannot present; reported as ABSENT with the reason
//   no VulkanSurface       the display server says so through `has_feature` before anything is
//                          created, which is what that query is for
//
// "NOT EVALUATED is never a pass" is the rung's own rule and it is applied to this file's own
// claims. A run that drew nothing exits non-zero if it was asked to draw.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <vector>

#include "card.h"

namespace cy {
class DisplayServer;
class Platform;
}  // namespace cy

namespace cy::sample::ship {

/// Which windowing implementation the run asked for. `Native` is the second desktop backend
/// `core-platform-abstraction`'s Complete cell needs — M11.d section 4 — and this sample selects it
/// at RUN TIME through the same `cy::DisplayServer` interface SDL3 is reached through, so the only
/// thing that changes between the two legs is which object was constructed.
enum class PlatformChoice : u8 {
    Auto,
    Sdl3,
    Native,
    Headless,
};

[[nodiscard]] const char* platform_choice_name(PlatformChoice choice) noexcept;

/// What one presentation leg actually did. Every field is measured, and the ones that could not be
/// measured say so rather than defaulting to something that reads like success.
struct PresentReport {
    std::string display_server;    // what DisplayServer::name() answered
    std::string backend;           // the RHI backend that was SELECTED, not the one requested
    std::string device_name;       // the device that answered, which is what task 8.3 asks for
    std::string device_class;      // "hardware", "software", "null" — never inferred from a flag
    bool window_opened = false;
    bool surface_created = false;
    bool swapchain_created = false;
    u32 frames_presented = 0;
    u32 validation_errors = 0;
    std::string swapchain_format;
    u32 swapchain_width = 0;
    u32 swapchain_height = 0;
    /// Empty when the leg ran. Otherwise why it did not, in a sentence a reader can act on.
    std::string not_evaluated;
    /// The frame as the DEVICE had it, read back out of the presented image. Empty when nothing
    /// was presented.
    Image photograph;
};

struct PresentOptions {
    PlatformChoice platform = PlatformChoice::Auto;
    u32 frames = 120;
    bool capture = false;   // read the presented image back, for --shot
    bool validation = true;
};

/// Open a window through `options.platform`, put `image` on it `options.frames` times, and report
/// what happened. Never fails the process: an absence is a report, not an error.
[[nodiscard]] PresentReport present_card(Platform& platform, const Image& image,
                                         const PresentOptions& options) noexcept;

/// The display servers this binary was BUILT with, in preference order. `Native` appears only when
/// a native desktop backend was linked — see CMakeLists.txt, which says what happens when M11.d
/// section 4 has not landed.
[[nodiscard]] std::vector<PlatformChoice> available_platforms() noexcept;

}  // namespace cy::sample::ship
