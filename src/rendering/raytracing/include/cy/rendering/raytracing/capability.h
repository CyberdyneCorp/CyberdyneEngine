#pragma once
// WHERE A DEVICE CAPABILITY BECOMES THIS SERVICE'S CONFIGURATION. M11.c task 2.6.
//
// `ray-tracing-infrastructure` — "Capability gating and fallback". The module's own README has
// carried the closing act since M7, in these words: *"A device backend arrives as two edits and no
// interface change: the RHI reports the capability, and
// `ServiceConfig::device_supports_ray_tracing` is set from it."* The first edit is in
// `src/backends/rhi/vulkan/`; this file is the second, and its whole content is the sentence above
// compiled.
//
// ================================================================================================
// WHY THIS IS A FUNCTION IN A HEADER AND NOT A LINE AT EVERY CALL SITE
// ================================================================================================
//
// Because a line at every call site is how a renderer ends up with two predicates that disagree.
// `query.h` already argues that `is_active()` must be the only thing consumers branch on; the same
// argument one level up says the capability must reach the service through one expression, so that
// "ray tracing is on" has one derivation and a bug report has one place to look.
//
// ================================================================================================
// WHAT THIS CLAIM IS NARROW ABOUT, SAID HERE RATHER THAN IN A REPORT
// ================================================================================================
//
// `Capability::RayTracing` is filled in by the VULKAN backend and by no other. Metal and D3D12 are
// `rhi-and-render-graph`'s, which is M11.d's row, so on any other backend this function returns a
// configuration with `device_supports_ray_tracing` false and the service runs its software tier —
// which is correct behaviour and an incomplete claim, and the two are different sentences. The
// capability model is what makes the narrowness expressible at all: this file asks a device what it
// can do and never asks which backend it is.

#include <cy/backends/rhi/capabilities.h>
#include <cy/rendering/raytracing/acceleration.h>

namespace cy::rendering::rt {

/// The service configuration a device implies.
///
/// `enabled_by_profile` is the renderer profile's answer and is deliberately a separate parameter:
/// the specification requires a profile that disables ray tracing on capable hardware to behave
/// exactly like hardware without it, so the two inputs must stay two inputs all the way down to
/// `Availability`. A single boolean here would collapse the case the requirement is about.
/// It reads the capability AND the observation behind it. `DeviceCapabilities::set()` is public, so
/// `has(RayTracing)` can be true on a device the engine created without asking for the features —
/// a capability a ray query cannot use — and this is where that stops.
[[nodiscard]] AccelerationService::ServiceConfig service_config_for(
    const rhi::DeviceCapabilities& capabilities, bool enabled_by_profile,
    const BuildBudget& budget = BuildBudget{}) noexcept;

/// Why a device is not tracing, in the device's own answers, for a log line and a bug report.
///
/// Returns an empty string when the device does report ray tracing. Never null: a diagnostic that
/// can be null is a diagnostic somebody prints with `%s` and crashes on.
[[nodiscard]] const char* ray_tracing_refusal(const rhi::DeviceCapabilities& capabilities) noexcept;

}  // namespace cy::rendering::rt
