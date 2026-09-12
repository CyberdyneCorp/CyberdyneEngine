// The four tables the requirement names, and the incremental regeneration M7's Seed did not do.

#include <cy/rendering/sky/tables.h>

#include <cy/core/math/math.h>

#include "internal.h"

#include <cmath>

namespace cy::rendering::sky {

using detail::density_at;
using detail::entry_distance;
using detail::exit_distance;
using detail::exp3;
using detail::same_atmosphere;
using detail::scattering_at;

namespace {

/// The multiple-scattering table is SQUARE and coarser than the sky view, and both are consequences
/// of what it stores: a quantity that has forgotten its direction varies slowly in both of its
/// arguments, so a 32x32 table at `High` resolves it to well under the sky model's own error while
/// a 128x64 one would cost sixteen times as much for no visible difference. Measured in
/// `test_sky_composition.cpp` against a table at twice the resolution.
[[nodiscard]] u32 multiple_scattering_resolution(SkyTableQuality quality) noexcept {
    switch (quality) {
        case SkyTableQuality::Low:
            return 16;
        case SkyTableQuality::Medium:
            return 24;
        case SkyTableQuality::High:
            return 32;
        case SkyTableQuality::Cinematic:
            return 48;
        case SkyTableQuality::Count:
            break;
    }
    return 24;
}

/// Directions sampled per entry when integrating multiple scattering over the sphere. Low for the
/// same reason the resolution is: the integrand has no phase function in it, so it is smooth.
[[nodiscard]] u32 multiple_scattering_directions(SkyTableQuality quality) noexcept {
    return quality >= SkyTableQuality::High ? 32U : 16U;
}

/// The altitude axis is SQUARE-ROOTED: the density it stands for is exponential, so a linear axis
/// spends most of its rows on air that is not there. `altitude_from_axis` and `axis_from_altitude`
/// are each other's inverse and are written next to each other for that reason.
[[nodiscard]] f32 altitude_from_axis(const Atmosphere& atmosphere, f32 v) noexcept {
    const f32 thickness = math::max(atmosphere.atmosphere_radius - atmosphere.planet_radius, 1.0F);
    const f32 clamped = math::saturate(v);
    return thickness * clamped * clamped;
}

[[nodiscard]] f32 axis_from_altitude(const Atmosphere& atmosphere, f32 altitude) noexcept {
    const f32 thickness = math::max(atmosphere.atmosphere_radius - atmosphere.planet_radius, 1.0F);
    return std::sqrt(math::saturate(math::max(altitude, 0.0F) / thickness));
}

/// A direction at `cos_zenith` from the local zenith, in the frame where "up" is +Y. The azimuth is
/// arbitrary in a spherically symmetric atmosphere, which is exactly why the table has no third
/// axis.
[[nodiscard]] Vec3 zenith_direction(f32 cos_zenith) noexcept {
    const f32 clamped = math::clamp(cos_zenith, -1.0F, 1.0F);
    const f32 sine = std::sqrt(math::max(1.0F - (clamped * clamped), 0.0F));
    return Vec3{sine, clamped, 0.0F};
}

/// Bilinear fetch from a table stored row-major as `width` columns by `height` rows, with both axes
/// CLAMPED. Neither axis wraps: a cosine of 1 and a cosine of -1 are opposite directions, not
/// neighbours, and an altitude above the atmosphere has no row beyond the last.
[[nodiscard]] Vec3 bilinear_clamped(Span<const Vec3> values, u32 width, u32 height, f32 fx,
                                    f32 fy) noexcept {
    if (values.empty() || width == 0 || height == 0) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const f32 x =
        math::clamp((fx * static_cast<f32>(width)) - 0.5F, 0.0F, static_cast<f32>(width - 1U));
    const f32 y =
        math::clamp((fy * static_cast<f32>(height)) - 0.5F, 0.0F, static_cast<f32>(height - 1U));
    const auto x0 = static_cast<u32>(x);
    const auto y0 = static_cast<u32>(y);
    const u32 x1 = math::min(x0 + 1U, width - 1U);
    const u32 y1 = math::min(y0 + 1U, height - 1U);
    const f32 tx = x - static_cast<f32>(x0);
    const f32 ty = y - static_cast<f32>(y0);
    const Vec3 a = values[(static_cast<usize>(y0) * width) + x0];
    const Vec3 b = values[(static_cast<usize>(y0) * width) + x1];
    const Vec3 c = values[(static_cast<usize>(y1) * width) + x0];
    const Vec3 d = values[(static_cast<usize>(y1) * width) + x1];
    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

/// Transmittance between two points on ONE ray, from a table that only stores transmittance to the
/// top of the atmosphere. `T(a->b) = T(a->top) / T(b->top)`, which holds because both paths share
/// their tail. The division is what makes one table serve every segment, and the guard is what
/// stops a path that is already opaque producing an infinity.
[[nodiscard]] Vec3 segment_transmittance(Vec3 to_top_near, Vec3 to_top_far) noexcept {
    const auto ratio = [](f32 near_value, f32 far_value) {
        return far_value > 1.0e-8F ? math::min(near_value / far_value, 1.0F) : 0.0F;
    };
    return Vec3{ratio(to_top_near.x, to_top_far.x), ratio(to_top_near.y, to_top_far.y),
                ratio(to_top_near.z, to_top_far.z)};
}

}  // namespace

// ================================================================================================
// TransmittanceTable
// ================================================================================================

u32 TransmittanceTable::width() const noexcept {
    return sky_table_width(quality_);
}

u32 TransmittanceTable::height() const noexcept {
    return sky_table_height(quality_);
}

Status TransmittanceTable::configure(SkyTableQuality quality) noexcept {
    if (quality >= SkyTableQuality::Count) {
        return fail(ErrorCode::InvalidArgument, "TransmittanceTable: quality is out of range");
    }
    quality_ = quality;
    built_ = false;
    values_.clear();
    return values_.resize(static_cast<usize>(sky_table_width(quality)) * sky_table_height(quality));
}

Expected<bool, Error> TransmittanceTable::build(const Atmosphere& atmosphere) noexcept {
    if (values_.empty()) {
        if (auto configured = configure(quality_); !configured) {
            return fail(configured.error().code, configured.error().message);
        }
    }
    if (built_ && same_atmosphere(last_atmosphere_, atmosphere)) {
        ++stats_.reuses;
        return false;
    }

    const u32 w = width();
    const u32 h = height();
    for (u32 y = 0; y < h; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(h);
        const f32 altitude = altitude_from_axis(atmosphere, v);
        const Vec3 position = ground_position(atmosphere, altitude);
        for (u32 x = 0; x < w; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(w);
            const f32 cos_zenith = (u * 2.0F) - 1.0F;
            values_[(static_cast<usize>(y) * w) + x] =
                transmittance(atmosphere, position, zenith_direction(cos_zenith), 40);
        }
    }

    last_atmosphere_ = atmosphere;
    built_ = true;
    ++stats_.full_rebuilds;
    stats_.directions_integrated += static_cast<u64>(w) * h;
    return true;
}

Vec3 TransmittanceTable::sample(f32 altitude_metres, f32 cos_zenith) const noexcept {
    if (!built_) {
        return Vec3{1.0F, 1.0F, 1.0F};
    }
    const f32 u = (math::clamp(cos_zenith, -1.0F, 1.0F) + 1.0F) * 0.5F;
    const f32 v = axis_from_altitude(last_atmosphere_, altitude_metres);
    return bilinear_clamped(values_.span(), width(), height(), u, v);
}

// ================================================================================================
// MultipleScatteringTable
// ================================================================================================

u32 MultipleScatteringTable::resolution() const noexcept {
    return multiple_scattering_resolution(quality_);
}

Status MultipleScatteringTable::configure(SkyTableQuality quality) noexcept {
    if (quality >= SkyTableQuality::Count) {
        return fail(ErrorCode::InvalidArgument, "MultipleScatteringTable: quality is out of range");
    }
    quality_ = quality;
    built_ = false;
    values_.clear();
    const u32 size = multiple_scattering_resolution(quality);
    return values_.resize(static_cast<usize>(size) * size);
}

namespace {

/// One entry of the multiple-scattering table: the second-order radiance and the return fraction,
/// integrated over the sphere at one altitude for one sun elevation.
struct SphericalTransfer {
    Vec3 second_order{0.0F, 0.0F, 0.0F};
    Vec3 return_fraction{0.0F, 0.0F, 0.0F};
};

/// The value of a uniform phase function, per steradian. Light that has scattered twice has very
/// nearly forgotten which way it came from, so every scattering event inside this table uses it
/// instead of Rayleigh's or Mie's.
inline constexpr f32 kUniformPhase = 1.0F / (4.0F * math::kPi);

/// March one direction and accumulate its contribution to both integrals.
///
/// THE TWO INTEGRALS CARRY DIFFERENT FACTORS, and the difference is the one thing in this file
/// that is easy to get wrong by a factor of 4*pi.
///
///   `second_order` is a RADIANCE arriving at the point from direction `omega`: light that left the
///   star, scattered once on its way here, and is now travelling towards the point. That first
///   scattering event has a phase function, which is `kUniformPhase` here.
///
///   `return_fraction` is not a radiance. It is the dimensionless fraction of light that leaves the
///   point and comes back to it, and it carries no phase at all.
///
/// The caller averages both over the sampled directions: `4*pi/count` of solid angle each, times
/// the `1/(4*pi)` of the uniform phase applied when they are scattered INTO the view ray, cancels
/// to a division by `count`. Leaving the first factor out of `second_order` — which an earlier
/// version of this file did — makes multiple scattering about twelve times too strong, which reads
/// as a sky that is bright and washed out rather than as an obvious defect.
void accumulate_direction(const Atmosphere& atmosphere, const TransmittanceTable& transmittance,
                          Vec3 position, Vec3 direction, Vec3 sun, SphericalTransfer& transfer,
                          u32 steps) noexcept {
    const f32 ground = entry_distance(position, direction, atmosphere.planet_radius);
    const f32 top = exit_distance(position, direction, atmosphere.atmosphere_radius);
    const f32 span = ground >= 0.0F ? ground : top;
    if (span <= 0.0F) {
        return;
    }

    const f32 step = span / static_cast<f32>(steps);
    Vec3 optical_depth{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < steps; ++index) {
        const Vec3 sample = position + direction * (step * (static_cast<f32>(index) + 0.5F));
        const f32 altitude = altitude_of(atmosphere, sample);
        optical_depth = optical_depth + extinction_at(atmosphere, altitude) * step;
        const Vec3 along = exp3(optical_depth);
        const Vec3 sigma_s = scattering_at(atmosphere, altitude);

        // The sun's own shadow: a point on the far side of the planet receives nothing, and the
        // table is where that has to be honoured. A multiple-scattering table with no planet in it
        // lights the ground from below at midnight.
        const bool shadowed = entry_distance(sample, sun, atmosphere.planet_radius) >= 0.0F;
        const Vec3 sun_transmittance =
            shadowed ? Vec3{0.0F, 0.0F, 0.0F}
                     : transmittance.sample(altitude, dot(normalize(sample), sun));

        transfer.second_order =
            transfer.second_order +
            cwise_mul(along, cwise_mul(sigma_s, sun_transmittance)) * (step * kUniformPhase);
        transfer.return_fraction = transfer.return_fraction + cwise_mul(along, sigma_s) * step;
    }

    if (ground > 0.0F) {
        // The ground is part of the transfer, and it is the only route by which `ground_albedo`
        // brightens a sky here. A Lambertian surface reflects `albedo/pi` per steradian.
        const Vec3 hit = position + direction * ground;
        const Vec3 normal = normalize(hit);
        const f32 lambert = math::max(0.0F, dot(normal, sun));
        const Vec3 sun_at_ground = transmittance.sample(0.0F, dot(normal, sun));
        const Vec3 reflected =
            cwise_mul(atmosphere.ground_albedo, sun_at_ground) * (lambert / math::kPi);
        transfer.second_order = transfer.second_order + cwise_mul(exp3(optical_depth), reflected);
    }
}

}  // namespace

Expected<bool, Error> MultipleScatteringTable::build(
    const Atmosphere& atmosphere, const TransmittanceTable& transmittance) noexcept {
    if (!transmittance.built()) {
        return fail(ErrorCode::Unavailable,
                    "MultipleScatteringTable::build: the transmittance table it is derived from "
                    "has not been built, and a table derived from an unbuilt one is zero "
                    "everywhere — which looks like a plausible answer");
    }
    if (values_.empty()) {
        if (auto configured = configure(quality_); !configured) {
            return fail(configured.error().code, configured.error().message);
        }
    }
    if (built_ && same_atmosphere(last_atmosphere_, atmosphere)) {
        ++stats_.reuses;
        return false;
    }

    const u32 size = multiple_scattering_resolution(quality_);
    const u32 directions = multiple_scattering_directions(quality_);
    const u32 steps = 20;

    for (u32 y = 0; y < size; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(size);
        const f32 altitude = altitude_from_axis(atmosphere, v);
        const Vec3 position = ground_position(atmosphere, altitude);
        for (u32 x = 0; x < size; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(size);
            const Vec3 sun = zenith_direction((u * 2.0F) - 1.0F);

            SphericalTransfer transfer;
            for (u32 index = 0; index < directions; ++index) {
                // A Fibonacci sphere: uniform in solid angle, deterministic, and with no clustering
                // at the poles that a latitude-longitude sweep would have. The quantity being
                // integrated is smooth, so what matters is that the directions are unbiased rather
                // than that there are many of them.
                const f32 fraction =
                    (static_cast<f32>(index) + 0.5F) / static_cast<f32>(directions);
                const f32 cos_theta = 1.0F - (2.0F * fraction);
                const f32 sin_theta = std::sqrt(math::max(1.0F - (cos_theta * cos_theta), 0.0F));
                const f32 phi = 2.39996323F * static_cast<f32>(index);  // the golden angle
                const Vec3 direction{sin_theta * std::cos(phi), cos_theta,
                                     sin_theta * std::sin(phi)};
                accumulate_direction(atmosphere, transmittance, position, direction, sun, transfer,
                                     steps);
            }

            const f32 inverse = 1.0F / static_cast<f32>(directions);
            const Vec3 second = transfer.second_order * inverse;
            const Vec3 fms = transfer.return_fraction * inverse;

            // Orders three and up are the same transfer applied again, so their sum is a geometric
            // series and `L2 / (1 - fms)` is the whole of it. The clamp on `fms` is not cosmetic: a
            // return fraction of one is an atmosphere that scatters light for ever, which no
            // physical one does but a coarse quadrature can produce.
            const auto series = [](f32 l2, f32 f) {
                return l2 / math::max(1.0F - math::min(f, 0.95F), 0.05F);
            };
            values_[(static_cast<usize>(y) * size) + x] =
                Vec3{series(second.x, fms.x), series(second.y, fms.y), series(second.z, fms.z)};
        }
    }

    last_atmosphere_ = atmosphere;
    built_ = true;
    ++stats_.full_rebuilds;
    stats_.directions_integrated += static_cast<u64>(size) * size * directions;
    return true;
}

Vec3 MultipleScatteringTable::sample(f32 altitude_metres, f32 cos_sun) const noexcept {
    if (!built_) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const u32 size = multiple_scattering_resolution(quality_);
    const f32 u = (math::clamp(cos_sun, -1.0F, 1.0F) + 1.0F) * 0.5F;
    const f32 v = axis_from_altitude(last_atmosphere_, altitude_metres);
    return bilinear_clamped(values_.span(), size, size, u, v);
}

// ================================================================================================
// AtmosphereTables
// ================================================================================================

Status AtmosphereTables::configure(SkyTableQuality quality) noexcept {
    if (auto status = transmittance.configure(quality); !status) {
        return status;
    }
    return multiple_scattering.configure(quality);
}

Expected<bool, Error> AtmosphereTables::build(const Atmosphere& atmosphere) noexcept {
    auto first = transmittance.build(atmosphere);
    if (!first) {
        return fail(first.error().code, first.error().message);
    }
    auto second = multiple_scattering.build(atmosphere, transmittance);
    if (!second) {
        return fail(second.error().code, second.error().message);
    }
    return first.value() || second.value();
}

// ================================================================================================
// The sky, through the tables
// ================================================================================================

Vec3 sky_radiance_tabulated(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                            Vec3 view_position, Vec3 view_direction, Vec3 sun_direction,
                            u32 steps) noexcept {
    const Vec3 view = normalized_or(view_direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});

    const f32 ground = entry_distance(view_position, view, atmosphere.planet_radius);
    const f32 top = exit_distance(view_position, view, atmosphere.atmosphere_radius);
    const f32 span = ground >= 0.0F ? ground : top;
    if (span <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }

    const f32 cos_theta = dot(view, sun);
    const f32 rayleigh_p = rayleigh_phase(cos_theta);
    const f32 mie_p = mie_phase(cos_theta, atmosphere.mie_anisotropy);

    const u32 count = math::max(steps, 4U);
    const f32 step = span / static_cast<f32>(count);
    Vec3 optical_depth{0.0F, 0.0F, 0.0F};
    Vec3 scattered{0.0F, 0.0F, 0.0F};

    for (u32 index = 0; index < count; ++index) {
        const Vec3 sample = view_position + view * (step * (static_cast<f32>(index) + 0.5F));
        const f32 altitude = altitude_of(atmosphere, sample);
        const f32 cos_sun = dot(normalize(sample), sun);
        optical_depth = optical_depth + extinction_at(atmosphere, altitude) * step;
        const Vec3 view_transmittance = exp3(optical_depth);

        const bool shadowed = entry_distance(sample, sun, atmosphere.planet_radius) >= 0.0F;
        const Vec3 sun_transmittance =
            shadowed ? Vec3{0.0F, 0.0F, 0.0F} : tables.transmittance.sample(altitude, cos_sun);

        const f32 rayleigh_density = density_at(altitude, atmosphere.rayleigh_scale_height);
        const f32 mie_density = density_at(altitude, atmosphere.mie_scale_height);
        const Vec3 rayleigh_term = atmosphere.rayleigh_scattering * (rayleigh_density * rayleigh_p);
        const f32 mie_value = atmosphere.mie_scattering * mie_density * mie_p;
        const Vec3 single =
            cwise_mul(sun_transmittance, rayleigh_term + Vec3{mie_value, mie_value, mie_value});

        // THE DIFFERENCE FROM `sky_radiance()`, in one line: the multiply-scattered term is read
        // from a table indexed by where this sample IS and where the sun IS, rather than being a
        // constant fraction of the single-scattered result. It therefore survives where the single
        // scattering does not — a shadowed slope, and a horizon at sunset.
        const Vec3 multiple = cwise_mul(scattering_at(atmosphere, altitude),
                                        tables.multiple_scattering.sample(altitude, cos_sun));

        scattered = scattered + cwise_mul(view_transmittance, single + multiple) * step;
    }

    Vec3 radiance = scattered * atmosphere.stellar_illuminance;

    if (ground > 0.0F) {
        const Vec3 hit = view_position + view * ground;
        const Vec3 normal = normalize(hit);
        const f32 lambert = math::max(0.0F, dot(normal, sun));
        const Vec3 sun_at_ground = tables.transmittance.sample(0.0F, dot(normal, sun));
        const Vec3 ground_radiance = cwise_mul(atmosphere.ground_albedo, sun_at_ground) *
                                     (lambert * atmosphere.stellar_illuminance / math::kPi);
        radiance = radiance + cwise_mul(exp3(optical_depth), ground_radiance);
    }
    return radiance;
}

Vec3 segment_transmittance_tabulated(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                                     Vec3 from, Vec3 to) noexcept {
    const Vec3 offset = to - from;
    const f32 distance = length(offset);
    if (distance <= 1.0e-3F) {
        return Vec3{1.0F, 1.0F, 1.0F};
    }
    const Vec3 direction = offset / distance;
    const Vec3 near_value =
        tables.transmittance.sample(altitude_of(atmosphere, from), dot(normalize(from), direction));
    const Vec3 far_value =
        tables.transmittance.sample(altitude_of(atmosphere, to), dot(normalize(to), direction));
    return segment_transmittance(near_value, far_value);
}

Vec3 sun_illuminance_tabulated(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                               Vec3 view_position, Vec3 sun_direction) noexcept {
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    if (entry_distance(view_position, sun, atmosphere.planet_radius) >= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const f32 altitude = altitude_of(atmosphere, view_position);
    const f32 cos_sun = dot(normalize(view_position), sun);
    return tables.transmittance.sample(altitude, cos_sun) * atmosphere.stellar_illuminance;
}

// ================================================================================================
// IncrementalSkyView
// ================================================================================================

u32 IncrementalSkyView::rows() const noexcept {
    return sky_table_height(quality_);
}

Status IncrementalSkyView::configure(SkyTableQuality quality) noexcept {
    if (quality >= SkyTableQuality::Count) {
        return fail(ErrorCode::InvalidArgument, "IncrementalSkyView: quality is out of range");
    }
    quality_ = quality;
    built_ = false;
    radiance_.clear();
    row_sun_.clear();
    if (auto status = radiance_.resize(static_cast<usize>(sky_table_width(quality)) *
                                       sky_table_height(quality));
        !status) {
        return status;
    }
    return row_sun_.resize(sky_table_height(quality));
}

void IncrementalSkyView::integrate_row(const Atmosphere& atmosphere, const AtmosphereTables* tables,
                                       Vec3 view_position, Vec3 sun, u32 row) noexcept {
    const u32 width = sky_table_width(quality_);
    const u32 height = sky_table_height(quality_);

    // `SkyViewTable::update()`'s parameterisation, deliberately identical: the row index goes as
    // the square of the angle from the horizon, so half the rows cover the twenty degrees above it
    // where the sky's whole gradient lives. The two tables have to be comparable entry for entry
    // for the oracle test to mean anything.
    const f32 v = ((static_cast<f32>(row) + 0.5F) / static_cast<f32>(height) * 2.0F) - 1.0F;
    const f32 elevation = (v < 0.0F ? -1.0F : 1.0F) * v * v * (math::kPi * 0.5F);
    const f32 cos_elevation = std::cos(elevation);

    for (u32 x = 0; x < width; ++x) {
        const f32 azimuth =
            2.0F * math::kPi * (static_cast<f32>(x) + 0.5F) / static_cast<f32>(width);
        const Vec3 direction{cos_elevation * std::cos(azimuth), std::sin(elevation),
                             cos_elevation * std::sin(azimuth)};
        radiance_[(static_cast<usize>(row) * width) + x] =
            tables != nullptr
                ? sky_radiance_tabulated(atmosphere, *tables, view_position, direction, sun, 16)
                : sky_radiance(atmosphere, view_position, direction, sun, 16);
    }
    row_sun_[row] = sun;
}

IncrementalSkyView::UpdatePlan IncrementalSkyView::plan_update(const Atmosphere& atmosphere,
                                                               Vec3 view_position,
                                                               Vec3 sun) noexcept {
    UpdatePlan plan;
    if (!built_ || !same_atmosphere(last_atmosphere_, atmosphere) ||
        length_squared(view_position - last_position_) >= 1.0F) {
        plan.full = true;
        return plan;
    }
    // "Tables SHALL be regenerated only when the parameters they depend on change." The sun is
    // compared by ANGLE and not by equality, for the reason `SkyViewTable` gives: a sun driven from
    // a clock is never twice the same float.
    if (dot(sun, last_sun_) >= std::cos(kSunMovementThreshold)) {
        plan.skip = true;
    }
    return plan;
}

Expected<SkyViewUpdate, Error> IncrementalSkyView::update_common(const Atmosphere& atmosphere,
                                                                 const AtmosphereTables* tables,
                                                                 Vec3 view_position, Vec3 sun_in,
                                                                 u32 row_budget) noexcept {
    if (radiance_.empty()) {
        if (auto configured = configure(quality_); !configured) {
            return fail(configured.error().code, configured.error().message);
        }
    }
    const Vec3 sun = normalized_or(sun_in, Vec3{0.0F, 1.0F, 0.0F});
    const u32 height = sky_table_height(quality_);
    const u32 width = sky_table_width(quality_);

    const UpdatePlan plan = plan_update(atmosphere, view_position, sun);
    SkyViewUpdate update;

    if (plan.skip) {
        ++stats_.reuses;
        return update;
    }

    if (plan.full || row_budget == 0 || row_budget >= height) {
        for (u32 row = 0; row < height; ++row) {
            integrate_row(atmosphere, tables, view_position, sun, row);
        }
        update.rows_rebuilt = height;
        update.directions_integrated = static_cast<u64>(width) * height;
        update.full_rebuild = plan.full;
        last_sun_ = sun;
        last_position_ = view_position;
        last_atmosphere_ = atmosphere;
        built_ = true;
        if (plan.full) {
            ++stats_.full_rebuilds;
        } else {
            ++stats_.incremental_updates;
        }
        stats_.rows_rebuilt += height;
        stats_.directions_integrated += update.directions_integrated;
        return update;
    }

    // THE INCREMENTAL PATH. Each row's urgency is how far the sun has moved since that row was last
    // integrated, weighted by how sensitive the row is. Staleness ACCUMULATES per row — a row
    // nobody chooses keeps getting more urgent — which is what stops the rows far from the sun
    // being starved by a scheme that always picks the ones near it.
    for (u32 chosen = 0; chosen < row_budget; ++chosen) {
        u32 worst_row = 0;
        f32 worst = -1.0F;
        for (u32 row = 0; row < height; ++row) {
            const f32 moved = std::acos(math::clamp(dot(sun, row_sun_[row]), -1.0F, 1.0F));
            // Rows near the horizon carry the sky's whole gradient (that is why the latitude axis
            // is non-linear in the first place), so a radian of sun movement changes them more than
            // it changes the zenith. `elevation_weight` is that sensitivity, and it is the only
            // place in this scheme where one row is preferred to another for a reason other than
            // how long it has waited.
            //
            // FOUR, AND THE QUADRATIC, ARE MEASURED RATHER THAN CHOSEN. Over a quarter-day at a
            // three-row budget, the worst visible error against a full rebuild is 24% of the
            // brightest sky at a linear 3:1 weight, 19% at this one, and 19% again at 8:1 — which
            // buys nothing and costs the zenith rows a staleness bound that grows from 18 to 23
            // degrees of sun movement. The trade-off is the whole design, so the numbers that
            // settled it belong here rather than in a commit message.
            const f32 v = ((static_cast<f32>(row) + 0.5F) / static_cast<f32>(height) * 2.0F) - 1.0F;
            const f32 horizon = 1.0F - std::fabs(v);
            const f32 elevation_weight = 1.0F + (4.0F * horizon * horizon);
            const f32 urgency = moved * elevation_weight;
            if (urgency > worst) {
                worst = urgency;
                worst_row = row;
            }
        }
        if (worst <= 0.0F) {
            break;  // every row already carries this sun: nothing to do with the rest of the budget
        }
        stats_.worst_row_staleness = math::max(stats_.worst_row_staleness, worst);
        integrate_row(atmosphere, tables, view_position, sun, worst_row);
        ++update.rows_rebuilt;
        update.directions_integrated += width;
    }

    last_sun_ = sun;
    last_position_ = view_position;
    last_atmosphere_ = atmosphere;
    ++stats_.incremental_updates;
    stats_.rows_rebuilt += update.rows_rebuilt;
    stats_.directions_integrated += update.directions_integrated;
    return update;
}

Expected<SkyViewUpdate, Error> IncrementalSkyView::update(const Atmosphere& atmosphere,
                                                          Vec3 view_position, Vec3 sun_direction,
                                                          u32 row_budget) noexcept {
    return update_common(atmosphere, nullptr, view_position, sun_direction, row_budget);
}

Expected<SkyViewUpdate, Error> IncrementalSkyView::update_tabulated(const Atmosphere& atmosphere,
                                                                    const AtmosphereTables& tables,
                                                                    Vec3 view_position,
                                                                    Vec3 sun_direction,
                                                                    u32 row_budget) noexcept {
    return update_common(atmosphere, &tables, view_position, sun_direction, row_budget);
}

Vec3 IncrementalSkyView::sample(Vec3 direction) const noexcept {
    if (!built_) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    const u32 width = sky_table_width(quality_);
    const u32 height = sky_table_height(quality_);

    const f32 elevation = std::asin(math::clamp(unit.y, -1.0F, 1.0F));
    const f32 normalised = elevation / (math::kPi * 0.5F);
    const f32 v = (normalised < 0.0F ? -1.0F : 1.0F) * std::sqrt(std::fabs(normalised));
    const f32 row = (((v * 0.5F) + 0.5F) * static_cast<f32>(height)) - 0.5F;

    f32 azimuth = std::atan2(unit.z, unit.x);
    if (azimuth < 0.0F) {
        azimuth += 2.0F * math::kPi;
    }
    const f32 column = (azimuth / (2.0F * math::kPi) * static_cast<f32>(width)) - 0.5F;

    const f32 clamped_row = math::clamp(row, 0.0F, static_cast<f32>(height - 1U));
    const auto y0 = static_cast<u32>(clamped_row);
    const u32 y1 = math::min(y0 + 1U, height - 1U);
    const f32 fy = clamped_row - static_cast<f32>(y0);

    // The azimuth WRAPS rather than clamps: a table whose last column does not blend into its first
    // shows a seam straight up the sky, and it is invisible until somebody turns around.
    const f32 wrapped = column < 0.0F ? column + static_cast<f32>(width) : column;
    const auto x0 = static_cast<u32>(wrapped) % width;
    const u32 x1 = (x0 + 1U) % width;
    const f32 fx = wrapped - std::floor(wrapped);

    const Vec3 a = radiance_[(static_cast<usize>(y0) * width) + x0];
    const Vec3 b = radiance_[(static_cast<usize>(y0) * width) + x1];
    const Vec3 c = radiance_[(static_cast<usize>(y1) * width) + x0];
    const Vec3 d = radiance_[(static_cast<usize>(y1) * width) + x1];
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

// ================================================================================================
// AerialPerspectiveTable
// ================================================================================================

u64 AerialPerspectiveTable::froxels() const noexcept {
    return static_cast<u64>(volume_.width) * volume_.height * volume_.depth;
}

Status AerialPerspectiveTable::configure(const FroxelVolume& volume) noexcept {
    if (volume.width == 0 || volume.height == 0 || volume.depth == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "AerialPerspectiveTable: a froxel volume needs a non-zero extent on all three "
                    "axes");
    }
    volume_ = volume;
    built_ = false;
    transmittance_.clear();
    in_scattering_.clear();
    const usize count = static_cast<usize>(volume.width) * volume.height * volume.depth;
    if (auto status = transmittance_.resize(count); !status) {
        return status;
    }
    return in_scattering_.resize(count);
}

Status AerialPerspectiveTable::update(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                                      Vec3 view_position, const View& view,
                                      Vec3 sun_direction) noexcept {
    if (transmittance_.empty()) {
        if (auto status = configure(volume_); !status) {
            return status;
        }
    }
    if (!tables.built()) {
        return fail(ErrorCode::Unavailable,
                    "AerialPerspectiveTable::update: the atmosphere tables have not been built");
    }
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});

    for (u32 y = 0; y < volume_.height; ++y) {
        const f32 ndc_y =
            (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(volume_.height)) * 2.0F) - 1.0F;
        for (u32 x = 0; x < volume_.width; ++x) {
            const f32 ndc_x =
                (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(volume_.width)) * 2.0F) - 1.0F;
            // CAMERA-RELATIVE: the ray is built in the view's own basis and only the ALTITUDE of
            // the camera reaches the atmosphere. That is the whole of the planetary-scale precision
            // argument — see `composition.h` — and it is why this loop never adds a world position
            // to a distance.
            const Vec3 direction =
                normalize(view.forward + (view.right * (ndc_x * view.tan_half_fov_x)) +
                          (view.up * (ndc_y * view.tan_half_fov_y)));
            const f32 forward_cosine = math::max(dot(direction, view.forward), 1.0e-3F);

            Vec3 optical_depth{0.0F, 0.0F, 0.0F};
            Vec3 scattered{0.0F, 0.0F, 0.0F};
            f32 previous_distance = 0.0F;
            const f32 cos_theta = dot(direction, sun);
            const f32 rayleigh_p = rayleigh_phase(cos_theta);
            const f32 mie_p = mie_phase(cos_theta, atmosphere.mie_anisotropy);

            for (u32 slice = 0; slice < volume_.depth; ++slice) {
                // `froxel_slice_depth()` is called rather than re-derived: the requirement's
                // "share the engine's volumetric infrastructure" is a build fact only if the slice
                // distribution is literally the one `rendering-post-processing` defines.
                const f32 slice_depth = froxel_slice_depth(volume_, slice);
                const f32 distance = slice_depth / forward_cosine;
                const f32 step = math::max(distance - previous_distance, 0.0F);
                const Vec3 sample = view_position + direction * (previous_distance + (step * 0.5F));
                previous_distance = distance;

                const f32 altitude = altitude_of(atmosphere, sample);
                optical_depth = optical_depth + extinction_at(atmosphere, altitude) * step;
                const Vec3 along = exp3(optical_depth);
                const f32 cos_sun = dot(normalize(sample), sun);
                const bool shadowed = entry_distance(sample, sun, atmosphere.planet_radius) >= 0.0F;
                const Vec3 sun_transmittance = shadowed
                                                   ? Vec3{0.0F, 0.0F, 0.0F}
                                                   : tables.transmittance.sample(altitude, cos_sun);

                const f32 rayleigh_density = density_at(altitude, atmosphere.rayleigh_scale_height);
                const f32 mie_density = density_at(altitude, atmosphere.mie_scale_height);
                const Vec3 rayleigh_term =
                    atmosphere.rayleigh_scattering * (rayleigh_density * rayleigh_p);
                const f32 mie_value = atmosphere.mie_scattering * mie_density * mie_p;
                const Vec3 single = cwise_mul(
                    sun_transmittance, rayleigh_term + Vec3{mie_value, mie_value, mie_value});
                const Vec3 multiple =
                    cwise_mul(scattering_at(atmosphere, altitude),
                              tables.multiple_scattering.sample(altitude, cos_sun));
                scattered = scattered + cwise_mul(along, single + multiple) * step;

                const usize index = (static_cast<usize>(slice) * volume_.height * volume_.width) +
                                    (static_cast<usize>(y) * volume_.width) + x;
                transmittance_[index] = along;
                in_scattering_[index] = scattered * atmosphere.stellar_illuminance;
            }
        }
    }
    built_ = true;
    return {};
}

AerialPerspective AerialPerspectiveTable::sample(Vec2 uv, f32 view_depth) const noexcept {
    AerialPerspective result;
    if (!built_) {
        return result;
    }
    const f32 x = math::clamp((uv.x * static_cast<f32>(volume_.width)) - 0.5F, 0.0F,
                              static_cast<f32>(volume_.width - 1U));
    const f32 y = math::clamp((uv.y * static_cast<f32>(volume_.height)) - 0.5F, 0.0F,
                              static_cast<f32>(volume_.height - 1U));
    const auto x0 = static_cast<u32>(x);
    const auto y0 = static_cast<u32>(y);
    const u32 x1 = math::min(x0 + 1U, volume_.width - 1U);
    const u32 y1 = math::min(y0 + 1U, volume_.height - 1U);
    const f32 tx = x - static_cast<f32>(x0);
    const f32 ty = y - static_cast<f32>(y0);

    // `froxel_slice_of()` is the inverse of the distribution the build used, and it is the engine's
    // function rather than a local one for the same reason the build called `froxel_slice_depth()`.
    const u32 slice = math::min(froxel_slice_of(volume_, view_depth), volume_.depth - 1U);
    const usize plane = static_cast<usize>(slice) * volume_.height * volume_.width;

    const auto fetch = [&](Span<const Vec3> values) {
        const Vec3 a = values[plane + (static_cast<usize>(y0) * volume_.width) + x0];
        const Vec3 b = values[plane + (static_cast<usize>(y0) * volume_.width) + x1];
        const Vec3 c = values[plane + (static_cast<usize>(y1) * volume_.width) + x0];
        const Vec3 d = values[plane + (static_cast<usize>(y1) * volume_.width) + x1];
        return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
    };

    result.transmittance = fetch(transmittance_.span());
    result.in_scattering = fetch(in_scattering_.span());
    return result;
}

}  // namespace cy::rendering::sky
