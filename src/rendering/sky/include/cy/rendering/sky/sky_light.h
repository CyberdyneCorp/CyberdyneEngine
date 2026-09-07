#pragma once
// THE SKY AS A LIGHT: the irradiance a surface receives from it, the gradient GI's sky term is,
// and the tables a frame samples instead of ray marching. M7 task 10.4.
//
// `atmosphere-sky-and-clouds` — "Sky composition": "The sky SHALL be rendered as a dedicated path
// producing radiance for the background, a filtered radiance map for specular image-based lighting,
// and irradiance for ambient diffuse — consumed by `rendering-global-illumination`."
//
// ================================================================================================
// "SUFFICIENT FOR GI'S SKY TERM" IS A MEASUREMENT, NOT A CLAIM
// ================================================================================================
//
// M7's task 10.4 is "an analytic sky sufficient for GI's sky term". `src/rendering/gi/` already
// declares what it wants — `gi::SkyTerm`, a zenith colour, a horizon colour, a ground colour and an
// intensity, with a comment saying the full model "is the seam it will replace, not a second sky".
//
// `fit_sky_gradient()` is that replacement, and `tests/test_sky_light.cpp` measures what it costs:
// the hemispherical irradiance the three-colour gradient delivers, against the irradiance the full
// atmosphere delivers, over four sun elevations and three surface orientations. A gradient that
// agreed with the sky only at the zenith would pass a spot check and be wrong for every surface
// that is not facing straight up, so the criterion is the INTEGRAL and not a sample.
//
// MEASURED: mean 12.4% relative difference, worst 17.4%. The full nine-coefficient spherical
// harmonic projection beside it — `project_sky_irradiance()`, for a consumer that can hold nine
// colours rather than three — is 1.9%.
//
// THE ADAPTER IS THREE LINES AND IT IS NOT WRITTEN HERE, deliberately: this module names nothing in
// `cy::rendering::gi` and does not link it, because a sky that depended on a global illumination
// system could not be tested without one. The composition point writes
//
//     const auto gradient = sky::fit_sky_gradient(atmosphere, sun);
//     gi::SkyTerm term{gradient.zenith, gradient.horizon, gradient.ground, gradient.intensity};
//
// and `SkyGradient`'s fields are named and ordered to make that line obvious.
//
// ================================================================================================
// TABLES, AND WHAT THIS TIER DOES AND DOES NOT DO ABOUT THEM
// ================================================================================================
//
// "Sky evaluation SHALL use precomputed lookup tables... rather than ray marching the atmosphere
// per pixel", "regenerated only when the parameters they depend on change", "regeneration SHALL be
// incremental where a parameter changes continuously, such as a moving sun", and "their generation
// cost SHALL be reported."
//
// `SkyViewTable` honours the first, the second and the fourth. It does NOT honour the third: a sun
// that has moved past a threshold triggers a FULL rebuild, not an incremental one, and
// `SkyTableStats::full_rebuilds` is what makes that visible rather than invisible. The gap is named
// here and in the module's README because a Seed tier that quietly claimed all four would be
// exactly the sort of record M7's own gate exists to catch.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/sky/atmosphere.h>

namespace cy::rendering::sky {

/// The nine coefficients of a second-order spherical-harmonic projection, per channel. The standard
/// representation for diffuse irradiance from an environment: nine numbers reconstruct a
/// cosine-convolved sky to within a couple of per cent, which is far below the error of the sky
/// model itself.
struct SkyIrradianceSh {
    Vec3 coefficient[9] = {};

    /// The irradiance arriving at a surface with this normal, in lux. Already cosine convolved, so
    /// a caller multiplies by albedo over pi and nothing else.
    [[nodiscard]] Vec3 irradiance(Vec3 normal) const noexcept;
};

/// Project the sky into spherical harmonics. `samples` is the number of directions per axis of a
/// stratified sphere sampling; 32 is enough for the diffuse term and is what the default gives.
///
/// `view_position` is planet-centred, in metres — `ground_position(atmosphere, 0)` for a surface
/// observer.
[[nodiscard]] SkyIrradianceSh project_sky_irradiance(const Atmosphere& atmosphere,
                                                     Vec3 view_position, Vec3 sun_direction,
                                                     u32 samples = 32) noexcept;

/// The three-colour gradient GI's sky term is: what `gi::SkyTerm` holds, in the order it holds it.
struct SkyGradient {
    Vec3 zenith{0.0F, 0.0F, 0.0F};
    Vec3 horizon{0.0F, 0.0F, 0.0F};
    Vec3 ground{0.0F, 0.0F, 0.0F};
    f32 intensity = 1.0F;

    /// The gradient's own radiance in a direction, matching `gi::SkyTerm::radiance`'s shape: a
    /// blend from horizon to zenith above and to ground below, by the direction's own elevation.
    [[nodiscard]] Vec3 radiance(Vec3 direction) const noexcept;
};

/// Fit the gradient to the atmosphere.
///
/// NOT three samples of the sky. The zenith and the ground are sampled; the HORIZON colour is
/// SOLVED for, by least squares, so that the gradient's cosine-weighted irradiance matches the
/// atmosphere's over FIVE surface orientations rather than one.
///
/// Three numbers measured over twelve sun-and-surface combinations, all in
/// `tests/test_sky_light.cpp`: sampling the horizon direction is 53% off even for an upward-facing
/// surface; solving for the upward hemisphere alone is exact there and 50% off on a wall facing
/// away from the sun; solving over the five is what ships. A GI sky term is read by every surface
/// in a scene and almost none of them faces straight up.
[[nodiscard]] SkyGradient fit_sky_gradient(const Atmosphere& atmosphere, Vec3 view_position,
                                           Vec3 sun_direction, u32 samples = 24) noexcept;

/// The irradiance an upward-facing surface receives from the atmosphere, in lux. The reference the
/// gradient fit is measured against, and the number a light meter pointed at the sky would read.
[[nodiscard]] Vec3 sky_irradiance(const Atmosphere& atmosphere, Vec3 view_position,
                                  Vec3 sun_direction, Vec3 normal, u32 samples = 32) noexcept;

// ================================================================================================
// THE SKY VIEW TABLE
// ================================================================================================

/// Resolution, as a quality setting. "Table resolution SHALL be a quality setting" — so it is an
/// enumeration with declared sizes rather than two integers a caller invents.
enum class SkyTableQuality : u8 {
    Low = 0,    // 32 x 16
    Medium,     // 64 x 32
    High,       // 128 x 64
    Cinematic,  // 192 x 96
    Count,
};

[[nodiscard]] const char* sky_table_quality_name(SkyTableQuality quality) noexcept;
[[nodiscard]] u32 sky_table_width(SkyTableQuality quality) noexcept;
[[nodiscard]] u32 sky_table_height(SkyTableQuality quality) noexcept;

/// What building the tables cost. "Their generation cost SHALL be reported."
struct SkyTableStats {
    u32 full_rebuilds = 0;
    /// Directions integrated across every rebuild. The unit of work, and a number that does not
    /// depend on how fast the machine is — which a millisecond count would.
    u64 directions_integrated = 0;
    /// Requests that were served from the existing table because nothing had moved enough.
    u32 reuses = 0;
};

/// How far the sun may move, in radians, before the table is rebuilt. About a quarter of a degree,
/// which is half the sun's own diameter — the point at which the sky's gradient visibly steps.
inline constexpr f32 kSunMovementThreshold = 0.004F;

/// The sky view table: radiance by direction, sampled instead of ray marched.
///
/// Storage is a latitude-longitude map with a NON-LINEAR latitude parameterisation — the row index
/// goes as the square root of the angle from the horizon — because the sky's gradient is almost all
/// in the twenty degrees above the horizon and a linear map spends four fifths of its rows on a
/// gradient nobody can see.
class SkyViewTable {
public:
    explicit SkyViewTable(Allocator& allocator = current_allocator()) noexcept
        : radiance_(allocator) {}

    /// Set the resolution. Discards the table, because a table at the old resolution answers
    /// nothing at the new one.
    [[nodiscard]] Status configure(SkyTableQuality quality) noexcept;

    /// Rebuild if the sun has moved past `kSunMovementThreshold` or the atmosphere changed.
    /// Returns whether it rebuilt.
    [[nodiscard]] Expected<bool, Error> update(const Atmosphere& atmosphere, Vec3 view_position,
                                               Vec3 sun_direction) noexcept;

    /// The radiance in a direction, bilinearly sampled. Zero before the first `update`.
    [[nodiscard]] Vec3 sample(Vec3 direction) const noexcept;

    [[nodiscard]] const SkyTableStats& stats() const noexcept { return stats_; }
    [[nodiscard]] SkyTableQuality quality() const noexcept { return quality_; }
    [[nodiscard]] bool built() const noexcept { return built_; }

private:
    Array<Vec3> radiance_;
    SkyTableQuality quality_ = SkyTableQuality::Medium;
    Vec3 last_sun_{0.0F, 1.0F, 0.0F};
    Vec3 last_position_{0.0F, 0.0F, 0.0F};
    Atmosphere last_atmosphere_;
    SkyTableStats stats_;
    bool built_ = false;
};

}  // namespace cy::rendering::sky
