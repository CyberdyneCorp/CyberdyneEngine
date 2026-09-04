#pragma once
// Backend selection: which RHI implementation a device comes from. Task 2.1.3.
//
// `engine-architecture` requires a backend to be "selectable by configuration, falling back to a
// documented default and finally to a null implementation that keeps handle bookkeeping valid", and
// cy::runtime::ServerRegistry applies exactly that rule one layer up. This file is the RHI's half
// of it: a small registry of device factories, keyed by name, with the null backend registered
// unconditionally so the fallback chain always terminates somewhere real.
//
// THE NULL BACKEND IS NOT A STUB IN THE FALLBACK POSITION. It implements the whole interface,
// validates handles, records commands and runs the render graph's derivation unchanged. That is
// what makes `create_device("null", ...)` a way to test a frame on a machine with no GPU rather
// than a way to make the process start.
//
// A backend registers itself; nothing here knows what a Vulkan is. cy_rhi_vulkan calls
// register_backend() from its own initialiser, and the Vulkan module is excluded from the link
// entirely when CY_RENDERER_VULKAN is off — so a dedicated server build has no Vulkan code in it at
// all, rather than Vulkan code that is not called.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy::rhi {

/// The name the null backend registers under, and the last link in the fallback chain.
inline constexpr const char* kNullBackendName = "null";

/// Constructs a device. Returns a device the caller owns and destroys with `destroy_device`.
using DeviceFactory = Expected<Device*, Error> (*)(Allocator& allocator,
                                                   const DeviceDescription& desc) noexcept;
using DeviceDestructor = void (*)(Allocator& allocator, Device* device) noexcept;

struct BackendRegistration {
    /// "vulkan", "null". Compared case-sensitively against what the configuration asked for.
    const char* name = "";
    BackendKind kind = BackendKind::Null;
    DeviceFactory create = nullptr;
    DeviceDestructor destroy = nullptr;
    /// Answered without creating a device, so selection can skip a backend whose loader is absent
    /// rather than reporting its creation failure as the reason nothing rendered. Null means
    /// "always available", which is the null backend's answer.
    bool (*is_available)() noexcept = nullptr;
};

/// Register a backend. Idempotent by name: registering the same name twice replaces the entry, so a
/// test can substitute a backend without unregistering first. Fails when the table is full.
Status register_backend(const BackendRegistration& registration) noexcept;

/// Every registered backend, in registration order. The null backend is always first because it is
/// registered by this module's own initialiser.
[[nodiscard]] Span<const BackendRegistration> registered_backends() noexcept;

[[nodiscard]] const BackendRegistration* find_backend(const char* name) noexcept;

/// How `create_device` chose. Reported rather than inferred: "asked for vulkan, ran null" is the
/// diagnostic a bug report needs, and it is not recoverable from the device alone.
struct BackendSelection {
    const char* requested = "";
    const char* selected = "";
    BackendKind kind = BackendKind::Null;
    bool fell_back = false;
    /// Why the requested backend was not used. Empty when it was.
    const char* reason = "";
};

/// Create a device from the named backend, falling back to the null backend.
///
/// `requested` may be null or empty, which means "the default": the first available registered
/// backend that is not the null one, and the null one when there is no such backend. That rule is
/// what makes a build with CY_RENDERER_VULKAN off run the same code with no configuration change.
Expected<Device*, Error> create_device(Allocator& allocator, const char* requested,
                                       const DeviceDescription& desc,
                                       BackendSelection& selection) noexcept;

/// Destroy a device created by `create_device`, through the factory that made it.
void destroy_device(Allocator& allocator, Device* device) noexcept;

}  // namespace cy::rhi
