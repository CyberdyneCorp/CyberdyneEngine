// Stars, the composition, the filtered radiance map, and the state weather publishes into fog.

#include <cy/rendering/sky/composition.h>

#include <cy/core/determinism/random.h>
#include <cy/core/math/math.h>

#include "internal.h"

#include <cmath>

namespace cy::rendering::sky {
namespace {

/// The stream stars are drawn from. Named, hierarchical and PRESENTATION: a star's position is not
/// part of authoritative state, and `simulation-and-determinism` requires that to be declared on
/// the stream rather than decided at the draw.
[[nodiscard]] determinism::RandomStream star_stream(u64 seed) noexcept {
    return determinism::RandomStream{seed, determinism::stream_id("sky.stars"),
                                     determinism::StreamPurpose::Presentation};
}

/// Octahedral encode, with +Y as the pole because that is the engine's up axis.
///
/// Octahedral rather than latitude-longitude: no seam a filter has to special-case, no pole where a
/// third of the texels describe a hundredth of the sphere, and a square texture whose mip chain is
/// the roughness chain.
[[nodiscard]] Vec2 octahedral_encode(Vec3 direction) noexcept {
    const f32 norm = std::fabs(direction.x) + std::fabs(direction.y) + std::fabs(direction.z);
    const Vec3 p = direction / math::max(norm, 1.0e-8F);
    f32 u = p.x;
    f32 v = p.z;
    if (p.y < 0.0F) {
        u = (1.0F - std::fabs(p.z)) * (p.x >= 0.0F ? 1.0F : -1.0F);
        v = (1.0F - std::fabs(p.x)) * (p.z >= 0.0F ? 1.0F : -1.0F);
    }
    return Vec2{(u * 0.5F) + 0.5F, (v * 0.5F) + 0.5F};
}

[[nodiscard]] Vec3 octahedral_decode(Vec2 uv) noexcept {
    const f32 u = (uv.x * 2.0F) - 1.0F;
    const f32 v = (uv.y * 2.0F) - 1.0F;
    Vec3 direction{u, 1.0F - std::fabs(u) - std::fabs(v), v};
    if (direction.y < 0.0F) {
        direction.x = (1.0F - std::fabs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
        direction.z = (1.0F - std::fabs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
    }
    return normalize(direction);
}

/// A tangent frame around `normal`. Branchless, and it is the standard one rather than a new one:
/// a frame that flipped at a pole would put a seam in the prefiltered map exactly where the
/// octahedral projection was chosen to avoid one.
void tangent_frame(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept {
    const f32 sign = normal.z >= 0.0F ? 1.0F : -1.0F;
    const f32 a = -1.0F / (sign + normal.z);
    const f32 b = normal.x * normal.y * a;
    tangent = Vec3{1.0F + (sign * normal.x * normal.x * a), sign * b, -sign * normal.x};
    bitangent = Vec3{b, sign + (normal.y * normal.y * a), -normal.y};
}

/// A cosine-power lobe's exponent for a roughness. The standard mapping; written once because two
/// call sites that disagreed would make `sample()` fetch from a level `build()` did not create.
[[nodiscard]] f32 lobe_exponent(f32 roughness) noexcept {
    const f32 alpha = math::max(roughness * roughness, 1.0e-3F);
    return math::clamp((2.0F / (alpha * alpha)) - 2.0F, 1.0F, 8192.0F);
}

}  // namespace

// ================================================================================================
// Planetary scale
// ================================================================================================

PlanetaryView planetary_view(const Atmosphere& atmosphere,
                             const world::WorldVec3d& camera) noexcept {
    PlanetaryView view;
    // THE ONE NARROWING, and it happens here rather than at every call site. The altitude is small
    // — metres to hundreds of kilometres — so it survives the trip to `f32` with metres to spare,
    // while the horizontal coordinates, which do not, never make the trip at all.
    view.altitude_metres = static_cast<f32>(camera.y);
    view.planet_relative = ground_position(atmosphere, view.altitude_metres);
    view.world_position =
        Vec3{static_cast<f32>(camera.x), view.altitude_metres, static_cast<f32>(camera.z)};
    return view;
}

f32 horizon_dip(const Atmosphere& atmosphere, f32 altitude_metres) noexcept {
    const f32 radius = math::max(atmosphere.planet_radius, 1.0F);
    const f32 eye = radius + math::max(altitude_metres, 0.0F);
    // The tangent from the eye to the sphere: one arccosine, at every altitude, with no band. That
    // is the whole of "the horizon SHALL follow from the model rather than from separate
    // implementations per altitude band".
    return std::acos(math::clamp(radius / eye, -1.0F, 1.0F));
}

// ================================================================================================
// Stars
// ================================================================================================

const char* star_source_name(StarSource source) noexcept {
    switch (source) {
        case StarSource::None:
            return "none";
        case StarSource::Procedural:
            return "procedural";
        case StarSource::Catalogue:
            return "catalogue";
        case StarSource::Imagery:
            return "imagery";
        case StarSource::Count:
            break;
    }
    return "unknown";
}

Status generate_stars(StarField& field, u64 seed, u32 count) noexcept {
    field.stars.clear();
    if (auto status = field.stars.reserve(count); !status) {
        return status;
    }
    const determinism::RandomStream stream = star_stream(seed);
    const determinism::SimulationPoint point{};

    for (u32 index = 0; index < count; ++index) {
        // A uniform point on the sphere: the cosine of the polar angle is uniform, NOT the angle
        // itself. Drawing the angle uniformly clusters stars at the poles, and the result looks
        // like a bug in a way nobody can name.
        const f32 cos_theta = (stream.unit_float(point, index, 0) * 2.0F) - 1.0F;
        const f32 sin_theta = std::sqrt(math::max(1.0F - (cos_theta * cos_theta), 0.0F));
        const f32 phi = stream.unit_float(point, index, 1) * 2.0F * math::kPi;

        // THE MAGNITUDE DISTRIBUTION IS THE REAL ONE. Star counts rise by about a factor of three
        // per magnitude, so brightness is drawn as a power of a uniform: the fifth power puts most
        // of the draws near zero and a handful near one. A uniform draw over illuminance gives a
        // sky of equally bright dots, which is the single most recognisable way a procedural night
        // sky goes wrong.
        const f32 uniform = stream.unit_float(point, index, 2);
        const f32 brightness = uniform * uniform * uniform * uniform * uniform;

        // Colour by temperature: most stars are cool and red, a few are hot and blue. Drawn on the
        // same principle as the magnitude and for the same reason.
        const f32 temperature = stream.unit_float(point, index, 3);
        const Vec3 tint =
            lerp(Vec3{1.0F, 0.78F, 0.62F}, Vec3{0.72F, 0.82F, 1.0F}, temperature * temperature);

        Star star;
        star.direction = Vec3{sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi)};
        star.illuminance = 1.0e-8F + (brightness * 2.0e-5F);
        star.tint = tint;
        if (auto status = field.stars.push_back(star); !status) {
            return status;
        }
    }
    field.source = StarSource::Procedural;
    return {};
}

Vec3 star_radiance(const StarField& field, Vec3 direction, f32 visibility,
                   f32 angular_radius) noexcept {
    if (field.source == StarSource::None || visibility <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    if (field.source == StarSource::Imagery) {
        // The imagery source resolves an asset this module does not own, so there is nothing to
        // sample here. Returning zero rather than pretending is the honest answer, and the
        // composition's caller binds the image — see the header: the SOURCE is a content decision
        // and every source composes through this one function.
        return Vec3{0.0F, 0.0F, 0.0F};
    }

    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    const f32 radius = math::max(angular_radius, 1.0e-4F);
    const f32 cos_limit = std::cos(radius);
    // A star is a point source, so its RADIANCE is its illuminance divided by whatever solid angle
    // it is spread over. Spreading it over the sampling disc rather than over its true angular size
    // is what makes a star visible at a finite resolution at all; the alternative is a sky whose
    // stars appear and disappear with the sampling rate.
    const f32 solid_angle = 2.0F * math::kPi * (1.0F - cos_limit);

    Vec3 total{0.0F, 0.0F, 0.0F};
    for (const Star& star : field.stars.span()) {
        const f32 cosine = dot(unit, star.direction);
        if (cosine < cos_limit) {
            continue;
        }
        total = total + (star.tint * (star.illuminance / solid_angle));
    }
    return total * (visibility * field.intensity);
}

// ================================================================================================
// SkyRadianceMap
// ================================================================================================

u32 SkyRadianceMap::level_resolution(u32 level) const noexcept {
    const u32 shifted = base_ >> level;
    return math::max(shifted, 4U);
}

usize SkyRadianceMap::level_offset(u32 level) const noexcept {
    usize offset = 0;
    for (u32 index = 0; index < level; ++index) {
        const u32 size = level_resolution(index);
        offset += static_cast<usize>(size) * size;
    }
    return offset;
}

u64 SkyRadianceMap::texels() const noexcept {
    return static_cast<u64>(level_offset(kLevels));
}

Status SkyRadianceMap::configure(u32 base_resolution) noexcept {
    if (base_resolution < 8) {
        return fail(ErrorCode::InvalidArgument,
                    "SkyRadianceMap: a base below 8 texels cannot carry a sky's gradient");
    }
    base_ = base_resolution;
    built_ = false;
    texels_.clear();
    return texels_.resize(level_offset(kLevels));
}

Status SkyRadianceMap::build(const Sampler& radiance) noexcept {
    if (texels_.empty()) {
        if (auto status = configure(base_); !status) {
            return status;
        }
    }
    if (radiance.function == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "SkyRadianceMap::build: no radiance function to filter");
    }

    // Level 0 is the sky itself. Every other level is a filter OF LEVEL 0 rather than of the level
    // below it: progressive filtering is cheaper and is what a mip chain does, but it compounds the
    // octahedral projection's own distortion five times over, and the artefact it produces — a
    // rough reflection that is subtly square — is very hard to attribute afterwards.
    const u32 size0 = level_resolution(0);
    for (u32 y = 0; y < size0; ++y) {
        for (u32 x = 0; x < size0; ++x) {
            const Vec2 uv{(static_cast<f32>(x) + 0.5F) / static_cast<f32>(size0),
                          (static_cast<f32>(y) + 0.5F) / static_cast<f32>(size0)};
            texels_[(static_cast<usize>(y) * size0) + x] = radiance(octahedral_decode(uv));
        }
    }

    constexpr u32 kFilterSamples = 48;
    for (u32 level = 1; level < kLevels; ++level) {
        const f32 roughness = static_cast<f32>(level) / static_cast<f32>(kLevels - 1U);
        const f32 exponent = lobe_exponent(roughness);
        const u32 size = level_resolution(level);
        const usize offset = level_offset(level);

        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                const Vec2 uv{(static_cast<f32>(x) + 0.5F) / static_cast<f32>(size),
                              (static_cast<f32>(y) + 0.5F) / static_cast<f32>(size)};
                const Vec3 centre = octahedral_decode(uv);
                Vec3 tangent;
                Vec3 bitangent;
                tangent_frame(centre, tangent, bitangent);

                Vec3 sum{0.0F, 0.0F, 0.0F};
                f32 weight = 0.0F;
                for (u32 index = 0; index < kFilterSamples; ++index) {
                    const f32 u1 =
                        (static_cast<f32>(index) + 0.5F) / static_cast<f32>(kFilterSamples);
                    const f32 u2 = std::fmod(static_cast<f32>(index) * 0.618033988F, 1.0F);
                    const f32 cos_alpha = std::pow(u1, 1.0F / (exponent + 1.0F));
                    const f32 sin_alpha =
                        std::sqrt(math::max(1.0F - (cos_alpha * cos_alpha), 0.0F));
                    const f32 phi = u2 * 2.0F * math::kPi;
                    const Vec3 direction = (tangent * (sin_alpha * std::cos(phi))) +
                                           (bitangent * (sin_alpha * std::sin(phi))) +
                                           (centre * cos_alpha);
                    // Level 0 is fetched by nearest rather than bilinearly: the lobe already
                    // averages dozens of texels, and a bilinear fetch inside it would cost four
                    // times as much to blur something that is about to be blurred.
                    const Vec2 fetch = octahedral_encode(direction);
                    const auto fx =
                        math::min(static_cast<u32>(fetch.x * static_cast<f32>(size0)), size0 - 1U);
                    const auto fy =
                        math::min(static_cast<u32>(fetch.y * static_cast<f32>(size0)), size0 - 1U);
                    sum = sum + texels_[(static_cast<usize>(fy) * size0) + fx];
                    weight += 1.0F;
                }
                texels_[offset + (static_cast<usize>(y) * size) + x] =
                    sum / math::max(weight, 1.0F);
            }
        }
    }
    built_ = true;
    return {};
}

Vec3 SkyRadianceMap::sample(Vec3 direction, f32 roughness) const noexcept {
    if (!built_) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const Vec2 uv = octahedral_encode(normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F}));
    const f32 level = math::saturate(roughness) * static_cast<f32>(kLevels - 1U);
    const auto low = static_cast<u32>(level);
    const u32 high = math::min(low + 1U, kLevels - 1U);
    const f32 blend = level - static_cast<f32>(low);

    const auto fetch = [&](u32 which) {
        const u32 size = level_resolution(which);
        const usize offset = level_offset(which);
        const auto x = math::min(static_cast<u32>(uv.x * static_cast<f32>(size)), size - 1U);
        const auto y = math::min(static_cast<u32>(uv.y * static_cast<f32>(size)), size - 1U);
        return texels_[offset + (static_cast<usize>(y) * size) + x];
    };
    return lerp(fetch(low), fetch(high), blend);
}

// ================================================================================================
// The composition
// ================================================================================================

namespace {

/// The aurora's contribution in a direction.
///
/// A BAND, placed from the oval's latitude and width and nothing else. It is deliberately the
/// simplest element in the composition: the requirement calls aurorae "optional phenomena", and
/// what matters structurally is that they are composed WITH the sky — occluded by cloud, attenuated
/// by air, invisible in daylight — rather than drawn beside it. A project wanting curtains and
/// rays replaces this function; a project wanting them drawn outside the composition gets an aurora
/// in front of its own clouds, which is the failure this arrangement prevents.
[[nodiscard]] Vec3 aurora_radiance(const Aurora& aurora, Vec3 direction, f32 visibility) noexcept {
    if (!aurora.enabled || visibility <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const f32 elevation = std::asin(math::clamp(direction.y, -1.0F, 1.0F)) * math::kRadToDeg;
    const f32 centre = 90.0F - aurora.oval_latitude;
    const f32 offset = std::fabs(elevation - centre) / math::max(aurora.oval_width, 0.1F);
    const f32 band = math::max(0.0F, 1.0F - (offset * offset));
    return aurora.colour * (band * aurora.intensity * visibility);
}

}  // namespace

SkyCompositionSample compose_sky(const SkyCompositionInputs& inputs, Vec3 direction) noexcept {
    SkyCompositionSample sample;
    if (inputs.atmosphere == nullptr) {
        return sample;
    }
    const Atmosphere& atmosphere = *inputs.atmosphere;
    const Vec3 view = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 position = inputs.view.planet_relative;
    const Vec3 sun =
        inputs.celestial != nullptr ? inputs.celestial->sun.direction : Vec3{0.0F, 1.0F, 0.0F};
    const f32 star_visibility =
        inputs.celestial != nullptr ? inputs.celestial->star_visibility : 0.0F;

    // THE ATMOSPHERE, from the table where there is one. "WHEN the sky is rendered THEN it SHALL
    // sample tables rather than integrating the atmosphere per pixel" — so the table is preferred,
    // the march is the fallback for a cook or a screenshot, and `from_table` is what lets a test
    // count which happened rather than trust this comment.
    if (inputs.sky_view != nullptr && inputs.sky_view->built()) {
        sample.atmosphere_radiance = inputs.sky_view->sample(view);
        sample.from_table = true;
    } else if (inputs.tables != nullptr && inputs.tables->built()) {
        sample.atmosphere_radiance =
            sky_radiance_tabulated(atmosphere, *inputs.tables, position, view, sun);
    } else {
        sample.atmosphere_radiance = sky_radiance(atmosphere, position, view, sun);
    }

    sample.stellar = stellar_radiance(atmosphere, position, view, sun);

    if (inputs.stars != nullptr) {
        // Starlight goes through the same air the sun does: a star near the horizon is reddened and
        // dimmed by the same transmittance, which is why this multiplies rather than adds.
        const Vec3 through_air =
            inputs.tables != nullptr && inputs.tables->built()
                ? inputs.tables->transmittance.sample(inputs.view.altitude_metres,
                                                      dot(normalize(position), view))
                : transmittance(atmosphere, position, view, 16);
        sample.stars = cwise_mul(star_radiance(*inputs.stars, view, star_visibility,
                                               atmosphere.stellar_angular_radius * 2.0F),
                                 through_air);
    }

    if (inputs.aurora != nullptr) {
        sample.aurora = aurora_radiance(*inputs.aurora, view, star_visibility);
    }

    Vec3 background = sample.atmosphere_radiance + sample.stellar + sample.stars + sample.aurora;

    if (inputs.clouds != nullptr && inputs.tables != nullptr && inputs.tables->built()) {
        const Vec3 sun_light = sun_illuminance_tabulated(atmosphere, *inputs.tables, position, sun);
        // The ambient a cloud sits in is the sky above it. Taking it from the zenith of the same
        // atmosphere rather than from a constant is what makes a cloud under an overcast dusk read
        // as grey instead of as a lit object in a dark scene.
        const Vec3 ambient = (inputs.sky_view != nullptr && inputs.sky_view->built())
                                 ? inputs.sky_view->sample(Vec3{0.0F, 1.0F, 0.0F}) * 0.5F
                                 : sky_radiance_tabulated(atmosphere, *inputs.tables, position,
                                                          Vec3{0.0F, 1.0F, 0.0F}, sun, 8) *
                                       0.5F;

        const CloudMarchResult clouds =
            march_clouds(atmosphere, *inputs.tables, *inputs.clouds, inputs.view.world_position,
                         view, sun, sun_light, ambient, inputs.time_seconds, inputs.cloud_quality);
        sample.clouds = clouds.scattering;
        sample.cloud_transmittance = clouds.transmittance;
        sample.cloud_depth_metres = clouds.half_transmittance_depth;
        sample.dominant_cloud_layer = clouds.dominant_layer;
        sample.cloud_stats = clouds.stats;
        background = (background * clouds.transmittance) + clouds.scattering;
    }

    sample.radiance = background;
    return sample;
}

SkyLighting compose_sky_lighting(const SkyCompositionInputs& inputs, u32 samples) noexcept {
    SkyLighting lighting;
    if (inputs.atmosphere == nullptr) {
        return lighting;
    }
    const Atmosphere& atmosphere = *inputs.atmosphere;
    const Vec3 position = inputs.view.planet_relative;
    const Vec3 sun =
        inputs.celestial != nullptr ? inputs.celestial->sun.direction : Vec3{0.0F, 1.0F, 0.0F};

    // The CLEAR-SKY halves come from M7's own measured functions rather than from a second
    // integral: `project_sky_irradiance()` and `fit_sky_gradient()` were built for exactly this and
    // their error against the full atmosphere is a number in `test_sky_light.cpp`.
    lighting.irradiance = project_sky_irradiance(atmosphere, position, sun, math::max(samples, 8U));
    lighting.gradient = fit_sky_gradient(atmosphere, position, sun, math::max(samples, 8U));

    Vec3 clear_sun = inputs.tables != nullptr && inputs.tables->built()
                         ? sun_illuminance_tabulated(atmosphere, *inputs.tables, position, sun)
                         : sun_illuminance(atmosphere, position, sun);

    if (inputs.clouds == nullptr || inputs.tables == nullptr || !inputs.tables->built()) {
        lighting.sun_illuminance = clear_sun;
        lighting.mean_sky_radiance =
            lighting.irradiance.irradiance(Vec3{0.0F, 1.0F, 0.0F}) * (1.0F / math::kPi);
        return lighting;
    }

    // THE CLOUD TERM IS A HEMISPHERICAL MEAN, AND THAT IS A NAMED APPROXIMATION.
    //
    // The clouds' effect on the sky's irradiance is measured over a small set of directions and
    // applied as one attenuation plus one addition, rather than by integrating the composed sky
    // over the hemisphere. It is right in magnitude — thicker cover gives less irradiance, which is
    // the scenario the requirement states — and it is wrong in DIRECTION: a cloud bank on one
    // horizon tilts the real irradiance and this does not. Integrating the composition properly
    // costs one cloud ray march per direction per frame, which is the sort of cost that quietly
    // ends up on the critical path. The gap is stated in the module's README.
    constexpr u32 kCloudProbes = 12;
    Vec3 cloud_radiance{0.0F, 0.0F, 0.0F};
    f32 transmittance_sum = 0.0F;
    Vec3 sky_sum{0.0F, 0.0F, 0.0F};

    SkyCompositionInputs probe = inputs;
    for (u32 index = 0; index < kCloudProbes; ++index) {
        // A hemisphere of probes, cosine-distributed: the irradiance integral weights a direction
        // by its cosine, so sampling that way puts the probes where they matter.
        const f32 u1 = (static_cast<f32>(index) + 0.5F) / static_cast<f32>(kCloudProbes);
        const f32 u2 = std::fmod(static_cast<f32>(index) * 0.618033988F, 1.0F);
        const f32 cos_theta = std::sqrt(1.0F - u1);
        const f32 sin_theta = std::sqrt(u1);
        const f32 phi = u2 * 2.0F * math::kPi;
        const Vec3 direction{sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi)};

        const SkyCompositionSample composed = compose_sky(probe, direction);
        cloud_radiance = cloud_radiance + composed.clouds;
        transmittance_sum += composed.cloud_transmittance;
        sky_sum = sky_sum + composed.radiance;
    }

    const f32 mean_transmittance = transmittance_sum / static_cast<f32>(kCloudProbes);
    const Vec3 mean_cloud = cloud_radiance / static_cast<f32>(kCloudProbes);
    lighting.cloud_transmittance = mean_transmittance;
    lighting.mean_sky_radiance = sky_sum / static_cast<f32>(kCloudProbes);

    for (Vec3& coefficient : lighting.irradiance.coefficient) {
        coefficient = coefficient * mean_transmittance;
    }
    // The clouds add their own lit radiance to the hemisphere. `pi` is the integral of the cosine
    // over it, so a uniform radiance of `L` delivers `pi * L` of irradiance — the constant that
    // turns the mean radiance into the irradiance it contributes.
    lighting.irradiance.coefficient[0] =
        lighting.irradiance.coefficient[0] + (mean_cloud * math::kPi * 0.282095F * 2.0F);
    lighting.gradient.zenith = (lighting.gradient.zenith * mean_transmittance) + mean_cloud;
    lighting.gradient.horizon = (lighting.gradient.horizon * mean_transmittance) + mean_cloud;

    // The sun through the clouds: one march along the sun's own ray, at the same quality the frame
    // is drawing with. This is the term a player sees — the moment a cloud crosses the sun, the
    // scene's key light changes — and it is a march rather than the hemispherical mean because a
    // mean cannot tell you whether THIS cloud is in front of the sun.
    const CloudMarchResult occlusion = march_clouds(
        atmosphere, *inputs.tables, *inputs.clouds, inputs.view.world_position, sun, sun,
        Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F}, inputs.time_seconds, inputs.cloud_quality);
    lighting.sun_illuminance = clear_sun * occlusion.transmittance;
    return lighting;
}

// ================================================================================================
// Volumetric integration
// ================================================================================================

AtmosphericMedium medium_from_weather(const Atmosphere& atmosphere,
                                      const CloudWeatherState& weather) noexcept {
    AtmosphericMedium medium;
    medium.humidity = math::saturate(weather.humidity);

    // Aerosol growth is strongly non-linear in humidity: hygroscopic particles take on water and
    // swell, and the visibility a meteorologist reports falls off a cliff above about 90% rather
    // than declining steadily. The fourth power is that shape, and it is why `humidity` is the
    // input rather than a pre-cooked "haziness" a project would have to tune.
    const f32 growth = medium.humidity * medium.humidity * medium.humidity * medium.humidity;
    medium.aerosol_density =
        1.0F + (28.0F * growth) + (0.9F * math::max(weather.precipitation, 0.0F));

    // Koschmieder: visibility is the distance at which contrast falls to 2%, which is
    // `ln(0.02) = -3.912` optical depths. The atmosphere's own Mie extinction at the ground is the
    // clear-air term, so a thin planet has a longer clear-air visibility for free.
    const f32 extinction = math::max(atmosphere.mie_extinction * medium.aerosol_density, 1.0e-9F);
    medium.visibility_metres = math::min(3.912F / extinction, 200000.0F);
    return medium;
}

FogParameters derive_fog_parameters(const Atmosphere& atmosphere,
                                    const AtmosphericMedium& medium) noexcept {
    FogParameters fog;
    // The inverse of the relation above, so that a project that publishes a visibility directly —
    // a scripted fog bank, a weather preset — and one that publishes a humidity arrive at the same
    // parameters. The round trip is a test.
    const f32 extinction = 3.912F / math::max(medium.visibility_metres, 1.0F);
    const f32 total = extinction + math::max(medium.injected_density, 0.0F);

    // Haze is very nearly conservative — droplets scatter far more than they absorb — so the single
    // scattering albedo is high and grey. The Rayleigh component at the ground is added on top,
    // which is what keeps a clear distance blue rather than white.
    constexpr f32 kAlbedo = 0.94F;
    const Vec3 rayleigh = atmosphere.rayleigh_scattering;
    fog.extinction = Vec3{total, total, total} + rayleigh;
    fog.scattering = Vec3{total * kAlbedo, total * kAlbedo, total * kAlbedo} + rayleigh;
    fog.emission = medium.injected_emission;
    // Droplets grow with humidity and larger droplets scatter more sharply forward, which is why
    // fog glows around a light and clear air does not.
    fog.anisotropy = math::clamp(0.55F + (0.35F * medium.humidity), 0.0F, 0.95F);
    return fog;
}

}  // namespace cy::rendering::sky
