#pragma once
// The tracing tiers: screen, software and hardware, behind one interface. Tasks 9.2 and 9.5.
//
// `rendering-global-illumination` — "Screen-space tracing", "Tracing tiers and selection",
// "Sample confidence".
//
// ================================================================================================
// ONE INTERFACE, AND THE CONSUMER CANNOT SEE WHICH TIER ANSWERED
// ================================================================================================
//
// "Consumers SHALL request radiance along a ray and SHALL NOT branch on which tier answered, nor on
// device capability." `TieredTracer::trace` is that request. It returns a `RadianceSample` whose
// `source` says what happened for the DIAGNOSTICS, and no consumer in this module reads it to
// decide anything — the resolve weights by confidence and the reflections choose a strategy from
// roughness, neither from a tier.
//
// ================================================================================================
// CHEAPEST SUFFICIENT, AND WHAT "SUFFICIENT" MEANS
// ================================================================================================
//
// A screen trace answers most rays almost free and cannot answer any ray that leaves the screen.
// So it goes first, it returns a confidence rather than a boolean, and the world tier is issued
// only when that confidence is below the escalation threshold AND the budget allows it. When both
// answer, the result is BLENDED by the screen confidence rather than switched to — which is what
// makes a ray approaching the screen edge fade into the world answer instead of popping.
//
// ================================================================================================
// THE FALLBACK IS THE PATH THAT ACTUALLY RUNS HERE — TASK 9.5
// ================================================================================================
//
// `HardwareTracer` reaches a device through `cy::rendering-raytracing`, whose service reports
// `Unsupported` on every device this engine can open today (see that module's README for the two
// checkable reasons). So `TieredTracer` selects the software tier on this machine, every time, and
// `tests/test_fallback.cpp` measures what that costs against the hardware tier driven from the same
// scene. That is M7's "ray tracing disabled falls back to software tracing with no visual
// discontinuity beyond tolerance", and the tolerance is a number in that file rather than an
// adjective here.
//
// The selection rule itself is one line and it is the important one: a tier that is not ACTIVE is
// not selected, and `AccelerationService::active()` is the only thing consulted. A profile that
// disables ray tracing on capable hardware therefore takes exactly the same path as a device
// without it, because there is no second predicate that could disagree.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/raytracing/acceleration.h>

namespace cy::rendering::gi {

/// What one view's screen-space buffers look like to the screen tier.
struct ScreenView {
    u32 width = 0;
    u32 height = 0;
    /// Linear view-space depth in metres, positive in front of the eye. Zero or negative is sky.
    Span<const f32> depth;
    Span<const Vec3> normal;
    /// The lit colour a screen hit resolves to. Last frame's, which is why a screen-traced sample
    /// is a frame stale and why its confidence is capped below one.
    Span<const Vec3> colour;
    /// World to view. The depth a marched point is at.
    Mat4 world_to_view = Mat4::identity();
    /// World to clip. Where it lands on the screen.
    Mat4 world_to_clip = Mat4::identity();
    Vec3 camera_position{0.0F, 0.0F, 0.0F};
    /// How far behind the depth buffer a marched point may be and still count as a hit, in metres.
    /// Larger accepts more hits and invents more of them.
    f32 thickness_metres = 0.25F;
    u32 max_steps = 32;
};

/// The first and cheapest tier.
class ScreenTracer {
public:
    /// Trace one ray through the depth buffer. The sample's confidence is derived from hit
    /// validity, screen-edge proximity and thickness agreement, exactly as the requirement lists.
    [[nodiscard]] static RadianceSample trace(const ScreenView& view, Vec3 origin, Vec3 direction,
                                              f32 max_distance) noexcept;
};

/// The software tier: sphere trace the distance field, resolve the hit through the surface cache.
class SoftwareTracer final : public SceneTracer, public Occluder {
public:
    SoftwareTracer() noexcept = default;
    SoftwareTracer(const DistanceField& field, const RadianceLookup* radiance) noexcept
        : field_(&field), radiance_(radiance) {}

    /// The two-phase form, for an owner that constructs its subsystems before it can wire them.
    /// An unbound tracer misses every ray rather than dereferencing nothing.
    void bind(const DistanceField& field, const RadianceLookup* radiance) noexcept {
        field_ = &field;
        radiance_ = radiance;
    }
    [[nodiscard]] bool bound() const noexcept { return field_ != nullptr; }

    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             SceneHit& hit) const noexcept override;
    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;

    /// The radiance a hit resolves to. A cache lookup, never a material evaluation.
    [[nodiscard]] RadianceSample radiance(const SceneHit& hit, const SkyTerm& sky,
                                          Vec3 direction) const noexcept;

private:
    const DistanceField* field_ = nullptr;
    const RadianceLookup* radiance_ = nullptr;
};

/// The hardware tier: ray queries through `ray-tracing-infrastructure`, resolved through the same
/// surface cache. Inactive when the service is, which is what makes the fallback the default here.
class HardwareTracer final : public SceneTracer, public Occluder {
public:
    HardwareTracer() noexcept = default;
    HardwareTracer(rt::AccelerationService& service, const RadianceLookup* radiance) noexcept
        : service_(&service), radiance_(radiance) {}

    void bind(rt::AccelerationService& service, const RadianceLookup* radiance) noexcept {
        service_ = &service;
        radiance_ = radiance;
    }

    /// The one predicate the tier selection consults. An unbound tracer is unavailable, which is
    /// the same answer a device without ray tracing gives.
    [[nodiscard]] bool available() const noexcept {
        return service_ != nullptr && service_->active();
    }

    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             SceneHit& hit) const noexcept override;
    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;

    [[nodiscard]] RadianceSample radiance(const SceneHit& hit, const SkyTerm& sky,
                                          Vec3 direction) const noexcept;

private:
    rt::AccelerationService* service_ = nullptr;
    const RadianceLookup* radiance_ = nullptr;
};

/// What a consumer may spend on one gather. Decremented in place.
struct TraceBudget {
    /// World-tier rays remaining. Screen rays are not counted: they are a texture read.
    u32 world_rays = 64;
    /// How many low-confidence screen results may escalate. Separate from the ray count so a
    /// budget can allow world rays for a probe gather and forbid escalation for a screen effect.
    u32 escalations = 64;
};

struct TracingDiagnostics {
    u32 screen_rays = 0;
    u32 software_rays = 0;
    u32 hardware_rays = 0;
    u32 escalations = 0;
    /// Rays that wanted a world tier and could not have one because the budget was spent. The
    /// number that explains a frame that suddenly looks flatter.
    u32 budget_refusals = 0;
    u32 sky_answers = 0;
    u32 cache_misses = 0;
};

/// The one interface.
class TieredTracer {
public:
    struct Config {
        /// Escalate when the screen tier's confidence is below this.
        f32 escalation_threshold = 0.75F;
        SkyTerm sky{};
        /// A world ray's reach, in metres.
        f32 max_distance_metres = 60.0F;
    };

    TieredTracer() noexcept = default;

    void set_config(const Config& config) noexcept { config_ = config; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

    /// The screen tier. Both may be null on a view that has no colour history — a probe gather, for
    /// instance, which has no screen at all.
    void set_screen(const ScreenView* view) noexcept { screen_ = view; }
    void set_software(const SoftwareTracer* tracer) noexcept { software_ = tracer; }
    void set_hardware(const HardwareTracer* tracer) noexcept { hardware_ = tracer; }

    /// Which world tier a ray would take. Exposed for the diagnostics and for the fallback test,
    /// and consulted by nothing that decides what to render.
    [[nodiscard]] RadianceSource selected_world_tier() const noexcept;

    /// Request radiance along a ray.
    [[nodiscard]] RadianceSample trace(Vec3 origin, Vec3 direction, f32 max_distance,
                                       TraceBudget& budget) noexcept;

    [[nodiscard]] const TracingDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    void reset_diagnostics() noexcept { diagnostics_ = TracingDiagnostics{}; }

private:
    Config config_{};
    const ScreenView* screen_ = nullptr;
    const SoftwareTracer* software_ = nullptr;
    const HardwareTracer* hardware_ = nullptr;
    TracingDiagnostics diagnostics_{};
};

/// A `SceneTracer` that dispatches to whichever world tier a `TieredTracer` would select. The
/// adapter the probe gather and the bake hold, so neither of them names a tier either.
class WorldTracer final : public SceneTracer, public Occluder {
public:
    WorldTracer() noexcept = default;
    WorldTracer(const SoftwareTracer* software, const HardwareTracer* hardware) noexcept
        : software_(software), hardware_(hardware) {}

    void bind(const SoftwareTracer* software, const HardwareTracer* hardware) noexcept {
        software_ = software;
        hardware_ = hardware;
    }

    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             SceneHit& hit) const noexcept override;
    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;
    [[nodiscard]] RadianceSource tier() const noexcept;

private:
    const SoftwareTracer* software_ = nullptr;
    const HardwareTracer* hardware_ = nullptr;
};

}  // namespace cy::rendering::gi
