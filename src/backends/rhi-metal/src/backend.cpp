#include <cy/backends/rhi-metal/backend.h>

#include <cy/backends/rhi-metal/mapping.h>

namespace cy::rhi::metal {

#if defined(CY_METAL_DEVICE_PRESENT)
// Defined by src/device.mm, which is compiled only on Apple. The declaration is here rather than in
// a header because nothing but this file may name it: a caller that could reach the factory
// directly could create a device without the registry knowing, and the registry is what
// `BackendSelection` reports from.
Expected<Device*, Error> create_metal_device(Allocator& allocator,
                                             const DeviceDescription& desc) noexcept;
void destroy_metal_device(Allocator& allocator, Device* device) noexcept;
bool metal_device_present() noexcept;
#endif

bool metal_backend_available() noexcept {
#if defined(CY_METAL_DEVICE_PRESENT)
    return metal_device_present();
#else
    return false;
#endif
}

Status register_metal_backend() noexcept {
#if defined(CY_METAL_DEVICE_PRESENT)
    BackendRegistration registration;
    registration.name = kMetalBackendName;
    registration.kind = BackendKind::Metal;
    registration.create = &create_metal_device;
    registration.destroy = &destroy_metal_device;
    registration.is_available = &metal_device_present;
    return register_backend(registration);
#else
    // Refused rather than registered-and-broken. A registration that exists and cannot work makes
    // "asked for metal, ran null" a runtime surprise; refusing here makes it a configuration
    // answer, which is what `BackendSelection` is for.
    return fail(ErrorCode::Unsupported,
                "the Metal backend is not in this build: it is compiled only on Apple platforms "
                "and only when CY_RENDERER_METAL is on");
#endif
}

MetalSeedStatus metal_seed_status() noexcept {
    MetalSeedStatus status;
    status.compiled_with_metal = metal_backend_available();
    status.gaps = kMetalGapCount;
    status.blocking_gaps = metal_blocking_gap_count();
    // WHAT THE SEED RENDERS, SAID PLAINLY AND IN ONE PLACE. M7 task 10.5 is "the Metal seed, which
    // renders the M3 golden scene"; this string is what a reader gets instead of having to infer
    // it from what compiles.
    status.renders =
        status.compiled_with_metal
            ? "the M3 golden scene's clear and its single triangle: a device, a command queue, a "
              "CAMetalLayer swapchain, and a render pass with a load and store action. Neither "
              "the material path nor the render graph's transient aliasing is here, and gap 1 "
              "(shaders are SPIR-V in the interface) is why."
            : "nothing on this platform. The seed's deliverable here is `mapping.h` and the eight "
              "gaps it names, which are built and tested wherever the engine is.";
    return status;
}

}  // namespace cy::rhi::metal
