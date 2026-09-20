#pragma once
// The Metal backend's registration seam. M7 task 10.5; `delivery-roadmap` delivers it at M11.
//
// THIS HEADER COMPILES EVERYWHERE, INCLUDING ON MACHINES WITH NO METAL, and that is deliberate:
// `backend.h`'s fallback chain is a runtime one, and a seam that only existed on Apple would be a
// seam nobody could test. On a non-Apple build `register_metal_backend()` registers nothing and
// says why; `metal_backend_available()` answers false; and `create_device("metal", ...)` falls
// through to whatever the configuration's fallback is, which is what
// `engine-architecture`'s "falling back to a documented default and finally to a null
// implementation" requires.
//
// WHAT IS AND IS NOT VERIFIED. Everything in `mapping.h` and everything in this file is built and
// tested on the Linux machine this milestone was developed on. `src/device.mm` — the actual Metal
// device — is NOT: it has never been compiled, because no Apple toolchain exists here. That
// division is the reason this module puts its findings in a table rather than in the device.

#include <cy/backends/rhi/backend.h>
#include <cy/core/base/expected.h>

namespace cy::rhi::metal {

/// The name the Metal backend registers under.
inline constexpr const char* kMetalBackendName = "metal";

/// Whether this build can create a Metal device at all. False on every platform but Apple's, and
/// false on Apple when the module was built without Metal.
[[nodiscard]] bool metal_backend_available() noexcept;

/// The native features observed on the device that answered. Kept separate from the portable
/// capability set because the tier is evidence a conformance run prints, not a renderer branch.
struct MetalRuntimeInfo {
    char device_name[128] = {};
    u32 argument_buffer_tier = 0;
    bool apple_gpu_family = false;
};

[[nodiscard]] MetalRuntimeInfo metal_runtime_info() noexcept;

/// Register the backend. On a build with no Metal this returns `ErrorCode::Unsupported` with a
/// message naming the platform, rather than registering a factory that would fail at the first
/// call — a registration that exists and cannot work makes "asked for metal, ran null" a runtime
/// surprise instead of a configuration answer.
Status register_metal_backend() noexcept;

/// What a `metal` device would report about this build. Answerable without a device, so a tool, a
/// test and `just env-doctor` can all print it on any platform.
struct MetalSeedStatus {
    bool compiled_with_metal = false;
    /// The gaps in `cy::rhi` this seed found that are STILL OPEN and have no workaround. See
    /// `mapping.h`.
    u32 blocking_gaps = 0;
    /// The gaps in total. This number does not move: rows are never deleted when they close,
    /// because the finding and what was done about it are one row a reviewer reads together.
    u32 gaps = 0;
    /// The gaps still open. THIS is the number that shrinks, and M11.d task 1.5 is the reason it
    /// exists: a gap closed in prose and not in the data is a gap that will be re-found at the
    /// first Metal compile.
    u32 open_gaps = 0;
    /// What the seed can render today, stated plainly.
    const char* renders = "";
};

[[nodiscard]] MetalSeedStatus metal_seed_status() noexcept;

}  // namespace cy::rhi::metal
