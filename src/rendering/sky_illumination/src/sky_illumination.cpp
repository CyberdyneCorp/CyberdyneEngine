#include <cy/rendering/sky_illumination/sky_illumination.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::skylight {
namespace {

/// An orthonormal basis around `normal`. The branchless form, so a normal that happens to be +Y
/// does not take a different code path from every other normal and produce a different integral
/// there — which is the shape of bug a hemisphere integrator hides for years.
void basis_around(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept {
    const f32 sign = normal.z >= 0.0F ? 1.0F : -1.0F;
    const f32 a = -1.0F / (sign + normal.z);
    const f32 b = normal.x * normal.y * a;
    tangent = Vec3{1.0F + (sign * normal.x * normal.x * a), sign * b, -sign * normal.x};
    bitangent = Vec3{b, sign + (normal.y * normal.y * a), -normal.y};
}

/// The magnitude a lux triple reduces to for a directional light's intensity. Not luminance: the
/// sun's colour is carried separately, so what is wanted here is the size of the vector and not a
/// perceptual weighting of it.
[[nodiscard]] f32 magnitude_of(Vec3 illuminance) noexcept {
    return length(illuminance);
}

}  // namespace

const char* sky_term_provenance_name(SkyTermProvenance provenance) noexcept {
    switch (provenance) {
        case SkyTermProvenance::AnalyticFallback:
            return "analytic-fallback";
        case SkyTermProvenance::PhysicalAtmosphere:
            return "physical-atmosphere";
    }
    return "unknown";
}

Vec3 term_irradiance(const gi::SkyTerm& term, Vec3 normal, u32 rings) noexcept {
    const Vec3 up = normalized_or(normal, Vec3{0.0F, 1.0F, 0.0F});
    Vec3 tangent;
    Vec3 bitangent;
    basis_around(up, tangent, bitangent);

    const u32 steps = math::max(rings, 2U);
    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 weight_total = 0.0F;
    // A COSINE-WEIGHTED STRATIFIED HEMISPHERE, evaluated at cell centres. Deterministic and
    // symmetric: the same directions in the same order on every machine, which is what makes
    // `irradiance_step` a number two runs can be compared on.
    for (u32 ring = 0; ring < steps; ++ring) {
        const f32 cosine = (static_cast<f32>(ring) + 0.5F) / static_cast<f32>(steps);
        const f32 sine = std::sqrt(math::max(0.0F, 1.0F - (cosine * cosine)));
        for (u32 step = 0; step < steps * 2; ++step) {
            const f32 phi =
                math::kTwoPi * (static_cast<f32>(step) + 0.5F) / static_cast<f32>(steps * 2);
            const Vec3 direction = (tangent * (sine * std::cos(phi))) +
                                   (bitangent * (sine * std::sin(phi))) + (up * cosine);
            total = total + (term.radiance(direction) * cosine);
            weight_total += cosine;
        }
    }
    return weight_total > 0.0F ? total * (math::kPi / weight_total) : Vec3{0.0F, 0.0F, 0.0F};
}

Status SkyIllumination::configure(const SkyIlluminationSettings& settings) noexcept {
    if (settings.refit_threshold_rad < 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "a negative refit threshold would refit the sky term on every frame, which is "
                    "the full recomputation this seam exists to avoid");
    }
    if (settings.exposure <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "a sky exposure of zero or less is a sky that is switched off, which is not an "
                    "exposure — use the analytic fallback and say so");
    }
    if (settings.fit_samples == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a gradient fitted over no directions is the analytic placeholder wearing the "
                    "atmosphere's name");
    }
    settings_ = settings;
    report_ = SkyIlluminationReport{};
    report_.provenance = provenance_;
    fitted_ = false;
    return ok();
}

Status SkyIllumination::set_atmosphere(const sky::Atmosphere& atmosphere) noexcept {
    if (Status valid = sky::validate_atmosphere(atmosphere); !valid) {
        return valid;
    }
    atmosphere_ = atmosphere;
    has_atmosphere_ = true;
    provenance_ = SkyTermProvenance::PhysicalAtmosphere;
    // A NEW ATMOSPHERE IS NOT A SUN THAT MOVED. The threshold is about continuity; a weather front
    // that changed the medium has none, so the next update fits whatever the sun is doing.
    fitted_ = false;
    return ok();
}

void SkyIllumination::use_analytic_fallback(const gi::SkyTerm& term) noexcept {
    term_ = term;
    has_atmosphere_ = false;
    provenance_ = SkyTermProvenance::AnalyticFallback;
    fitted_ = false;
}

void SkyIllumination::set_cloud_shadow_source(const environment::FieldStore* store) noexcept {
    clouds_ = store;
}

f32 SkyIllumination::sunlight_fraction(const world::WorldVec3d& at) const noexcept {
    if (clouds_ == nullptr || !settings_.consume_cloud_shadow) {
        return 1.0F;
    }
    // THROUGH THE FUNCTION EVERY OTHER CONSUMER CALLS. See the header: a consumer that reached into
    // `FieldStore::sample_at` with a residency of its own would be a consumer the field's own
    // declared defect could not break, which is how `m10:sky-field-round-trip` came to be green on
    // the symptom it names.
    return math::clamp(sky::CloudShadowField::sample(*clouds_, at), 0.0F, 1.0F);
}

void SkyIllumination::fit(Vec3 sun_direction) noexcept {
    const Vec3 view = sky::ground_position(atmosphere_, settings_.observer_altitude_metres);
    const sky::SkyGradient gradient =
        sky::fit_sky_gradient(atmosphere_, view, sun_direction, settings_.fit_samples);
    // THE THREE LINES `sky_light.h` SAID A COMPOSITION POINT WRITES, and `SkyGradient`'s fields are
    // named and ordered so that this is the obvious spelling rather than a translation.
    term_ = gi::SkyTerm{gradient.zenith, gradient.horizon, gradient.ground,
                        gradient.intensity * settings_.exposure};
    fitted_sun_ = sun_direction;
    fitted_ = true;
    report_.fits += 1;
    report_.directions_integrated +=
        static_cast<u64>(settings_.fit_samples) * settings_.fit_samples;
}

SkyIlluminationReport SkyIllumination::update(const SkyIlluminationFrame& frame,
                                              gi::IlluminationSystem& system) noexcept {
    const Vec3 sun = normalized_or(frame.sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    report_.provenance = provenance_;
    report_.exposure = settings_.exposure;
    report_.refitted = false;
    report_.invalidation_filed = false;
    report_.irradiance_step = 0.0F;
    report_.sun_from_atmosphere = false;

    // The angle the sun turned through since the term was fitted. `dot` of two unit vectors, so the
    // clamp is against the rounding that puts it fractionally outside [-1, 1] and nothing else.
    const f32 cosine = math::clamp(dot(sun, fitted_sun_), -1.0F, 1.0F);
    report_.sun_delta_rad = fitted_ ? std::acos(cosine) : math::kPi;

    const bool wants_fit = has_atmosphere_ &&
                           (!fitted_ || report_.sun_delta_rad >= settings_.refit_threshold_rad) &&
                           settings_.max_fits_per_update > 0;
    if (wants_fit) {
        // THE FIRST FIT IS AN INSTALLATION AND NOT A STEP. There is no previous term for it to
        // step from, and reporting 1.0 there would make "the sky never jumped" a claim about a
        // frame that had no sky a moment earlier. Every subsequent fit is a continuity event and is
        // measured as one.
        const bool continuing = fitted_;
        const Vec3 before =
            continuing ? term_irradiance(term_, Vec3{0.0F, 1.0F, 0.0F}) : Vec3{0.0F, 0.0F, 0.0F};
        fit(sun);
        const Vec3 after = term_irradiance(term_, Vec3{0.0F, 1.0F, 0.0F});
        const f32 reference = math::max(magnitude_of(before), magnitude_of(after));
        report_.irradiance_step =
            continuing && reference > 0.0F ? magnitude_of(after - before) / reference : 0.0F;

        system.set_sky_term(term_);
        // THE INVALIDATION, UNDER ITS OWN CAUSE. `SkyChanged` is what stops the sparse distance
        // field being rebuilt for a sun that moved no geometry — see system.cpp — which is the
        // whole of "invalidating only the illumination that depends on it".
        system.scene().invalidate(frame.lit_region, gi::InvalidationCause::SkyChanged, frame.sky_id,
                                  frame.frame);
        report_.refitted = true;
        report_.invalidation_filed = true;
        report_.sun_delta_rad = 0.0F;
    } else {
        report_.reuses += 1;
        if (!has_atmosphere_) {
            // The fallback still has to reach the illumination system, or "no atmosphere" would
            // mean "whatever term the system was configured with", which is not the same thing.
            system.set_sky_term(term_);
        }
    }

    report_.cloud_field_read = clouds_ != nullptr && settings_.consume_cloud_shadow;
    report_.cloud_transmittance = sunlight_fraction(frame.observer);

    if (has_atmosphere_) {
        const Vec3 view = sky::ground_position(atmosphere_, settings_.observer_altitude_metres);
        const Vec3 illuminance = sky::sun_illuminance(atmosphere_, view, sun);
        const f32 lux = magnitude_of(illuminance);
        report_.sun.direction = sun * -1.0F;
        report_.sun.colour = lux > 0.0F ? illuminance * (1.0F / lux) : Vec3{1.0F, 1.0F, 1.0F};
        // THE CLOUD FIELD ATTENUATES THE SUN AND NOT THE GRADIENT. Cloud cover scatters direct
        // sunlight into the sky rather than deleting it, so dimming both would take the same light
        // out of the frame twice — the header argues this where a reader meets it first.
        report_.sun.intensity = lux * report_.cloud_transmittance;
        report_.sun.range = 0.0F;
        report_.sun.directional = true;
        report_.sun.id = frame.sky_id;
        report_.sun_from_atmosphere = true;
    }
    return report_;
}

}  // namespace cy::rendering::skylight
