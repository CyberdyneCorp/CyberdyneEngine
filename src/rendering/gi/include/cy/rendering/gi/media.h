// SPDX-License-Identifier: MIT
#pragma once
// Transparency and participating media. `rendering-global-illumination` — "Transparency and
// participating media".
//
// ================================================================================================
// THE STAGING TABLE IS CODE, SO A DEFERRED FEATURE IS A REFUSAL RATHER THAN A SILENCE
// ================================================================================================
//
// | Class              | Support                                                              |
// |--------------------|----------------------------------------------------------------------|
// | Opaque, masked     | full participation                                                   |
// | Thin transmission  | approximate: the back face's indirect light, tinted, comes through   |
// | Volumetric media   | low frequency: the radiance cache's mean over the sphere, on a grid  |
// | True refraction    | DEFERRED — `RefractionSolver` is the reserved seam                   |
// | Caustics           | DEFERRED — `CausticsSource` is the reserved seam                     |
//
// `transmission_support` is that table. The two deferred rows have their interfaces declared here
// and `IlluminationSystem::set_refraction_solver` / `set_caustics_source` refuse them by name
// (`ErrorCode::NotImplemented`), so a project that needs caustics is told, at the call, that they
// are deferred — the specification's third scenario.
//
// ================================================================================================
// THIN TRANSMISSION IS THE BACK FACE'S IRRADIANCE, NOT A REFRACTION SOLVE
// ================================================================================================
//
// A leaf is a card with two faces. What reaches the eye from its front is its front face's
// reflection AND what its back face received, attenuated by the leaf: `T * E_back / pi` for a
// diffuse transmitter, which in the mean-radiance units every resolve here returns is `T` times
// the radiance cache sampled about the reversed normal, a hair behind the surface so the front's
// probes are on the other side of it. `IlluminationSystem::indirect_diffuse` returns it in
// `ResolveResult::transmitted`, already tinted — a shader adds it — and only when the surface
// declares a transmission; with none the resolve is bit for bit what it was.
//
// ================================================================================================
// VOLUMETRIC MEDIA TAKE THE CACHE'S MEAN, ON A GRID THE FOG READS
// ================================================================================================
//
// An isotropic medium scatters `sigma_s` times the MEAN radiance over the sphere. The radiance
// cache answers a direction's cosine-weighted mean; the average over the six cardinal directions
// is the sphere's mean for the SH L1 encoding exactly (the first band cancels in opposite pairs)
// and a close approximation for the other two. `build_media_indirect` samples it on a regular grid
// — the "low-frequency indirect from the radiance cache" of the staging table — and
// `fog::FogIndirect` is how the volumetric fog of `rendering-post-processing` reads that grid
// instead of computing an indirect term of its own.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

enum class TransmissionClass : u8 {
    Opaque = 0,
    Masked,
    ThinTransmission,
    VolumetricMedia,
    Refraction,
    Caustics,
    Count,
};

enum class TransmissionSupport : u8 {
    /// Takes part in every cache, tier and resolve.
    Full = 0,
    /// Receives indirect light and transmits it approximately.
    Approximate,
    /// Receives the radiance cache's low-frequency indirect.
    LowFrequency,
    /// Not built; the interface is reserved and installing one is refused.
    Deferred,
};

[[nodiscard]] TransmissionSupport transmission_support(TransmissionClass value) noexcept;
[[nodiscard]] const char* transmission_class_name(TransmissionClass value) noexcept;
[[nodiscard]] const char* transmission_support_name(TransmissionSupport value) noexcept;

/// RESERVED: a solver for true refraction through a thick transparent body. Deferred — the
/// illumination system refuses one (`IlluminationSystem::set_refraction_solver`).
class RefractionSolver {
public:
    RefractionSolver() = default;
    RefractionSolver(const RefractionSolver&) = delete;
    RefractionSolver(RefractionSolver&&) = delete;
    RefractionSolver& operator=(const RefractionSolver&) = delete;
    RefractionSolver& operator=(RefractionSolver&&) = delete;
    virtual ~RefractionSolver() = default;

    /// Radiance arriving at `position` through the body, seen along `view_direction`.
    [[nodiscard]] virtual RadianceSample refracted(Vec3 position, Vec3 normal, Vec3 view_direction,
                                                   f32 index_of_refraction) const noexcept = 0;
};

/// RESERVED: focused light from specular transmitters and reflectors. Deferred — the illumination
/// system refuses one (`IlluminationSystem::set_caustics_source`).
class CausticsSource {
public:
    CausticsSource() = default;
    CausticsSource(const CausticsSource&) = delete;
    CausticsSource(CausticsSource&&) = delete;
    CausticsSource& operator=(const CausticsSource&) = delete;
    CausticsSource& operator=(CausticsSource&&) = delete;
    virtual ~CausticsSource() = default;

    /// Caustic irradiance arriving at a surface, as a mean radiance.
    [[nodiscard]] virtual Vec3 caustic(Vec3 position, Vec3 normal) const noexcept = 0;
};

/// How far behind a thin transmitter its back face is sampled, in metres.
inline constexpr f32 kThinTransmissionBackOffsetMetres = 0.05F;

/// The radiance cache's mean over the sphere at `position`: the average of its answers about the
/// six cardinal directions, each weighted by its confidence. `coverage` is the mean confidence —
/// zero where no probe reaches — and the answer is `fallback` blended in by what is missing.
[[nodiscard]] Vec3 media_indirect_radiance(const RadianceCache& cache, Vec3 position, Vec3 fallback,
                                           f32& coverage) noexcept;

/// The grid a participating medium reads: points at `origin + spacing * (x, y, z)`, x fastest.
struct MediaIndirectGrid {
    explicit MediaIndirectGrid(Allocator& allocator = current_allocator()) noexcept
        : radiance(allocator), coverage(allocator) {}

    Vec3 origin{0.0F, 0.0F, 0.0F};
    f32 spacing = 1.0F;
    u32 dimensions[3] = {0, 0, 0};
    Array<Vec3> radiance;
    Array<f32> coverage;
};

/// Sample `media_indirect_radiance` at every point of a grid covering `bounds` at `spacing`.
/// Refuses an empty box, a non-positive spacing, or more than `max_points` points.
[[nodiscard]] Status build_media_indirect(const RadianceCache& cache, const Aabb& bounds,
                                          f32 spacing, Vec3 fallback, MediaIndirectGrid& out,
                                          u32 max_points = 65536) noexcept;

}  // namespace cy::rendering::gi
