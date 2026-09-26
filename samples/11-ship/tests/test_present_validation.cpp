// SPDX-License-Identifier: MIT
// A presented frame loop with synchronisation validation on, through each desktop display server
// this binary was built with, and ZERO validation errors required. M11.d.
//
// WHY THIS EXISTS. The first frame this repository ever presented to a window tripped two
// synchronisation hazards per frame, on both platform legs, and the only thing that saw them was
// `just run-ship`, which a person runs:
//
//   SYNC-HAZARD-WRITE-AFTER-READ against PRESENT_ACQUIRE_READ   the acquired image's first
//       transition had a source stage of NONE, so it did not chain to the acquire semaphore's wait
//   SYNC-HAZARD-PRESENT-AFTER-WRITE                             the transition to the presentable
//       state had a destination stage of NONE, so the submit's signal did not cover it
//
// `test_barriers.cpp` pins the derived barriers without a device. This case is the one that runs
// them against the layer that found them: `present_card` is the sample's own presentation path,
// unchanged — display server, surface, swapchain, acquire, render graph, submit, present — for a
// handful of frames, and every error the layer reports is counted.
//
// IT SKIPS A LEG, LOUDLY, WHEN THE LEG CANNOT RUN: no display, no Vulkan device, no surface. The
// reason `present_card` gives is printed, so "the leg passed" and "the leg found no display" are
// never confusable. A host with a display and a GPU runs both legs; this one does.

#include <cy/test/test.h>

#include <cy/core/platform/platform.h>
#include <cy/platform/sdl3_platform.h>
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
#    include <cy/platform/linux_platform.h>
#endif

#include <string>

#include "card.h"
#include "present.h"

namespace {

using cy::sample::ship::Image;
using cy::sample::ship::PlatformChoice;
using cy::sample::ship::PresentOptions;
using cy::sample::ship::PresentReport;

/// Several frames, so a hazard that needs a previous frame's present to exist — the acquire side's
/// is exactly that — has one to trip over. Each frame is two derived barriers and a copy.
constexpr cy::u32 kFrames = 6;

[[nodiscard]] Image flat_card() {
    Image image;
    image.width = 256;
    image.height = 160;
    image.pixels.assign(image.byte_size(), 0);
    for (cy::usize texel = 0; texel < image.pixels.size(); texel += 4) {
        image.pixels[texel + 0] = 32;
        image.pixels[texel + 1] = 48;
        image.pixels[texel + 2] = 72;
        image.pixels[texel + 3] = 255;
    }
    return image;
}

[[nodiscard]] PresentReport present_frames(cy::Platform& platform, PlatformChoice choice) {
    Image image = flat_card();
    PresentOptions options;
    options.platform = choice;
    options.frames = kFrames;
    options.capture = false;
    options.validation = true;
    return cy::sample::ship::present_card(platform, image, options);
}

void check_clean(const PresentReport& report, const char* leg) {
    if (!report.not_evaluated.empty()) {
        const std::string message =
            std::string("the '") + leg +
            "' leg did not run, so nothing was checked: " + report.not_evaluated;
        CY_TEST_MESSAGE(message.c_str());
        return;
    }
    CY_CHECK_EQ(report.frames_presented, kFrames);
    CY_CHECK_EQ(report.validation_errors, 0U);
}

}  // namespace

CY_TEST_CASE("the sdl3 leg presents with zero synchronisation-validation errors") {
    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(0, nullptr); !started) {
        CY_TEST_MESSAGE("the SDL3 platform did not initialise; skipping");
        return;
    }
    check_clean(present_frames(platform, PlatformChoice::Sdl3), "sdl3");
    platform.shutdown();
}

#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
CY_TEST_CASE("the native leg presents with zero synchronisation-validation errors") {
    cy::LinuxPlatform platform;
    if (const cy::Status started = platform.initialise(0, nullptr); !started) {
        CY_TEST_MESSAGE("the native platform did not initialise; skipping");
        return;
    }
    check_clean(present_frames(platform, PlatformChoice::Native), "native");
    platform.shutdown();
}
#endif
