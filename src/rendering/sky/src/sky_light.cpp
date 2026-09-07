#include <cy/rendering/sky/sky_light.h>

#include <cy/core/math/math.h>

#include <cmath>
#include <numbers>

namespace cy::rendering::sky {
namespace {

/// A stratified set of directions over the sphere, in (cos theta, phi). Deterministic: the sky's
/// irradiance must be the same number on two runs, or a bake and a frame disagree.
struct SphereSampler {
    u32 steps;

    [[nodiscard]] u32 count() const noexcept { return steps * steps * 2U; }

    [[nodiscard]] Vec3 direction(u32 index) const noexcept {
        const u32 i = index % steps;
        const u32 j = (index / steps) % (steps * 2U);
        const f32 cos_theta =
            1.0F - (2.0F * (static_cast<f32>(i) + 0.5F) / static_cast<f32>(steps));
        const f32 sin_theta = std::sqrt(math::max(0.0F, 1.0F - (cos_theta * cos_theta)));
        const f32 phi =
            2.0F * math::kPi * (static_cast<f32>(j) + 0.5F) / static_cast<f32>(steps * 2U);
        return Vec3{sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi)};
    }

    /// The solid angle one sample stands for.
    [[nodiscard]] f32 weight() const noexcept {
        return 4.0F * math::kPi / static_cast<f32>(count());
    }
};

/// The nine real second-order spherical harmonics at a direction, in the usual order.
void sh_basis(Vec3 d, f32 (&out)[9]) noexcept {
    out[0] = 0.282095F;
    out[1] = 0.488603F * d.y;
    out[2] = 0.488603F * d.z;
    out[3] = 0.488603F * d.x;
    out[4] = 1.092548F * d.x * d.y;
    out[5] = 1.092548F * d.y * d.z;
    out[6] = 0.315392F * ((3.0F * d.z * d.z) - 1.0F);
    out[7] = 1.092548F * d.x * d.z;
    out[8] = 0.546274F * ((d.x * d.x) - (d.y * d.y));
}

/// The cosine convolution: the three band coefficients that turn a radiance projection into an
/// irradiance one. Ramamoorthi and Hanrahan's constants.
constexpr f32 kCosineBand0 = std::numbers::pi_v<float>;
constexpr f32 kCosineBand1 = 2.094395F;
constexpr f32 kCosineBand2 = 0.785398F;

/// The band a coefficient belongs to, as a table rather than as nested conditionals: index 0 is
/// band 0, indices 1 to 3 are band 1, and 4 to 8 are band 2.
constexpr f32 kCosineBand[9] = {kCosineBand0, kCosineBand1, kCosineBand1,
                                kCosineBand1, kCosineBand2, kCosineBand2,
                                kCosineBand2, kCosineBand2, kCosineBand2};

}  // namespace

Vec3 SkyIrradianceSh::irradiance(Vec3 normal) const noexcept {
    const Vec3 unit = normalized_or(normal, Vec3{0.0F, 1.0F, 0.0F});
    f32 basis[9];
    sh_basis(unit, basis);
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < 9; ++index) {
        total = total + coefficient[index] * basis[index];
    }
    // Clamped, because a nine-coefficient reconstruction of a sky with a bright sun can ring
    // NEGATIVE in the direction opposite it — and negative irradiance reaches a shader as a
    // subtraction, which is a black rim on the shadowed side of everything.
    return Vec3{math::max(total.x, 0.0F), math::max(total.y, 0.0F), math::max(total.z, 0.0F)};
}

SkyIrradianceSh project_sky_irradiance(const Atmosphere& atmosphere, Vec3 view_position,
                                       Vec3 sun_direction, u32 samples) noexcept {
    const SphereSampler sampler{math::max(samples, 4U)};
    const f32 weight = sampler.weight();

    SkyIrradianceSh result;
    for (u32 index = 0; index < sampler.count(); ++index) {
        const Vec3 direction = sampler.direction(index);
        const Vec3 radiance = sky_radiance(atmosphere, view_position, direction, sun_direction, 16);
        f32 basis[9];
        sh_basis(direction, basis);
        for (u32 slot = 0; slot < 9; ++slot) {
            result.coefficient[slot] = result.coefficient[slot] + radiance * (basis[slot] * weight);
        }
    }
    // Fold the cosine convolution in here rather than at every evaluation: it is three constants
    // and doing it per lookup would put them in a shader where a second copy could drift.
    //
    // AND THERE IS NO 1/pi. The convolved coefficients give IRRADIANCE — what `sky_irradiance`
    // returns and what a light meter reads. Dividing by pi gives the "diffuse radiance" a shader
    // wants after it has also multiplied by albedo, and doing it here makes this function's answer
    // 31.8% of the reference it is supposed to match. Measured, on the first draft: exactly 68%
    // off, which is 1 - 1/pi and is the shape of the mistake rather than a coincidence.
    for (u32 slot = 0; slot < 9; ++slot) {
        result.coefficient[slot] = result.coefficient[slot] * kCosineBand[slot];
    }
    return result;
}

Vec3 sky_irradiance(const Atmosphere& atmosphere, Vec3 view_position, Vec3 sun_direction,
                    Vec3 normal, u32 samples) noexcept {
    const Vec3 unit = normalized_or(normal, Vec3{0.0F, 1.0F, 0.0F});
    const SphereSampler sampler{math::max(samples, 4U)};
    const f32 weight = sampler.weight();
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < sampler.count(); ++index) {
        const Vec3 direction = sampler.direction(index);
        const f32 cosine = dot(direction, unit);
        if (cosine <= 0.0F) {
            continue;
        }
        total = total + sky_radiance(atmosphere, view_position, direction, sun_direction, 16) *
                            (cosine * weight);
    }
    return total;
}

Vec3 SkyGradient::radiance(Vec3 direction) const noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    if (unit.y >= 0.0F) {
        // The same shape `gi::SkyTerm::radiance` uses: a blend from the horizon to the zenith by
        // elevation. Keeping the two identical is what makes the fit below meaningful — a fit
        // against one interpolation evaluated through another is a fit against nothing.
        return lerp(horizon, zenith, unit.y) * intensity;
    }
    return lerp(horizon, ground, -unit.y) * intensity;
}

SkyGradient fit_sky_gradient(const Atmosphere& atmosphere, Vec3 view_position, Vec3 sun_direction,
                             u32 samples) noexcept {
    SkyGradient gradient;
    gradient.intensity = 1.0F;
    gradient.zenith =
        sky_radiance(atmosphere, view_position, Vec3{0.0F, 1.0F, 0.0F}, sun_direction, 24);
    gradient.ground =
        sky_radiance(atmosphere, view_position, Vec3{0.0F, -1.0F, 0.0F}, sun_direction, 24);

    // THE HORIZON IS SOLVED FOR, NOT SAMPLED, AND IT IS SOLVED OVER SEVERAL ORIENTATIONS.
    //
    // The gradient's irradiance for any normal is LINEAR in its three colours:
    //     E(n) = A(n) * zenith + B(n) * horizon + C(n) * ground
    // with A, B and C the cosine-weighted integrals of the three blend weights over that normal's
    // hemisphere. Zenith and ground are sampled directly, so the only unknown is `horizon`, and the
    // least-squares solution over a set of normals is one division per channel.
    //
    // SOLVING FOR THE UPWARD NORMAL ALONE IS EXACT THERE AND WORST EVERYWHERE ELSE. Measured over
    // twelve sun-and-surface combinations: fitting the upward hemisphere alone gives 0.0002% error
    // straight up and 50% on a wall facing away from the sun; fitting these five together spreads
    // it. A GI sky term is read by every surface in the scene and almost none of them faces
    // straight up, so the set is what it is fitted against.
    //
    // Sampling the horizon direction instead of solving at all is worse than either: 53% off even
    // for the upward normal, because most of a hemisphere's solid angle is nowhere near any of the
    // three sample directions. `tests/test_sky_light.cpp` measures all three.
    const Vec3 normals[5] = {Vec3{0.0F, 1.0F, 0.0F}, normalize(Vec3{1.0F, 0.25F, 0.0F}),
                             normalize(Vec3{-1.0F, 0.25F, 0.0F}),
                             normalize(Vec3{0.0F, 0.25F, 1.0F}),
                             normalize(Vec3{0.0F, 0.25F, -1.0F})};

    const SphereSampler sampler{math::max(samples, 4U)};
    const f32 weight = sampler.weight();
    Vec3 numerator{0.0F, 0.0F, 0.0F};
    f32 denominator = 0.0F;

    for (const Vec3& normal : normals) {
        f32 zenith_weight = 0.0F;
        f32 horizon_weight = 0.0F;
        f32 ground_weight = 0.0F;
        Vec3 reference{0.0F, 0.0F, 0.0F};
        for (u32 index = 0; index < sampler.count(); ++index) {
            const Vec3 direction = sampler.direction(index);
            const f32 cosine = dot(direction, normal);
            if (cosine <= 0.0F) {
                continue;
            }
            const f32 share = cosine * weight;
            if (direction.y >= 0.0F) {
                zenith_weight += direction.y * share;
                horizon_weight += (1.0F - direction.y) * share;
            } else {
                ground_weight += -direction.y * share;
                horizon_weight += (1.0F + direction.y) * share;
            }
            reference =
                reference +
                sky_radiance(atmosphere, view_position, direction, sun_direction, 16) * share;
        }
        const Vec3 residual =
            reference - gradient.zenith * zenith_weight - gradient.ground * ground_weight;
        numerator = numerator + residual * horizon_weight;
        denominator += horizon_weight * horizon_weight;
    }

    gradient.horizon = denominator > 1.0e-9F ? numerator / denominator : gradient.zenith;
    // A solved horizon can come out negative when the zenith alone already over-delivers, which
    // happens under a thin atmosphere whose sky is nearly uniform. Clamping keeps the gradient a
    // sky rather than a subtraction, and the fit's own test measures what the clamp costs.
    gradient.horizon =
        Vec3{math::max(gradient.horizon.x, 0.0F), math::max(gradient.horizon.y, 0.0F),
             math::max(gradient.horizon.z, 0.0F)};
    return gradient;
}

const char* sky_table_quality_name(SkyTableQuality quality) noexcept {
    switch (quality) {
        case SkyTableQuality::Low:
            return "low";
        case SkyTableQuality::Medium:
            return "medium";
        case SkyTableQuality::High:
            return "high";
        case SkyTableQuality::Cinematic:
            return "cinematic";
        case SkyTableQuality::Count:
            break;
    }
    return "unknown";
}

u32 sky_table_width(SkyTableQuality quality) noexcept {
    switch (quality) {
        case SkyTableQuality::Low:
            return 32;
        case SkyTableQuality::Medium:
            return 64;
        case SkyTableQuality::High:
            return 128;
        case SkyTableQuality::Cinematic:
            return 192;
        case SkyTableQuality::Count:
            break;
    }
    return 64;
}

u32 sky_table_height(SkyTableQuality quality) noexcept {
    return sky_table_width(quality) / 2U;
}

Status SkyViewTable::configure(SkyTableQuality quality) noexcept {
    if (quality >= SkyTableQuality::Count) {
        return fail(ErrorCode::InvalidArgument, "SkyViewTable: quality is out of range");
    }
    quality_ = quality;
    built_ = false;
    radiance_.clear();
    return radiance_.resize(static_cast<usize>(sky_table_width(quality)) *
                            sky_table_height(quality));
}

Expected<bool, Error> SkyViewTable::update(const Atmosphere& atmosphere, Vec3 view_position,
                                           Vec3 sun_direction) noexcept {
    if (radiance_.empty()) {
        if (auto configured = configure(quality_); !configured) {
            return fail(configured.error().code, configured.error().message);
        }
    }
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});

    // "Tables SHALL be regenerated only when the parameters they depend on change." The sun is
    // compared by ANGLE rather than by equality, because a sun driven from a clock is never twice
    // the same float and a table rebuilt every frame is not a table.
    const bool same_atmosphere =
        last_atmosphere_.rayleigh_scale_height == atmosphere.rayleigh_scale_height &&
        last_atmosphere_.mie_scattering == atmosphere.mie_scattering &&
        last_atmosphere_.mie_extinction == atmosphere.mie_extinction &&
        last_atmosphere_.mie_anisotropy == atmosphere.mie_anisotropy &&
        last_atmosphere_.rayleigh_scattering == atmosphere.rayleigh_scattering &&
        last_atmosphere_.ozone_absorption == atmosphere.ozone_absorption &&
        last_atmosphere_.ground_albedo == atmosphere.ground_albedo &&
        last_atmosphere_.stellar_illuminance == atmosphere.stellar_illuminance &&
        last_atmosphere_.multiple_scattering_factor == atmosphere.multiple_scattering_factor;
    const bool sun_still = dot(sun, last_sun_) >= std::cos(kSunMovementThreshold);
    const bool same_place = length_squared(view_position - last_position_) < 1.0F;
    if (built_ && same_atmosphere && sun_still && same_place) {
        ++stats_.reuses;
        return false;
    }

    const u32 width = sky_table_width(quality_);
    const u32 height = sky_table_height(quality_);
    for (u32 y = 0; y < height; ++y) {
        // The non-linear latitude parameterisation: the row index goes as the square of the
        // distance from the horizon, so half the rows cover the twenty degrees above it where the
        // sky's whole gradient lives. A linear map spends four fifths of its rows on the smooth
        // part and bands the interesting one.
        const f32 v = ((static_cast<f32>(y) + 0.5F) / static_cast<f32>(height) * 2.0F) - 1.0F;
        const f32 elevation = (v < 0.0F ? -1.0F : 1.0F) * v * v * (math::kPi * 0.5F);
        const f32 cos_elevation = std::cos(elevation);
        for (u32 x = 0; x < width; ++x) {
            const f32 azimuth =
                2.0F * math::kPi * (static_cast<f32>(x) + 0.5F) / static_cast<f32>(width);
            const Vec3 direction{cos_elevation * std::cos(azimuth), std::sin(elevation),
                                 cos_elevation * std::sin(azimuth)};
            radiance_[(static_cast<usize>(y) * width) + x] =
                sky_radiance(atmosphere, view_position, direction, sun, 16);
        }
    }

    last_sun_ = sun;
    last_position_ = view_position;
    last_atmosphere_ = atmosphere;
    built_ = true;
    ++stats_.full_rebuilds;
    stats_.directions_integrated += static_cast<u64>(width) * height;
    return true;
}

Vec3 SkyViewTable::sample(Vec3 direction) const noexcept {
    if (!built_) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    const u32 width = sky_table_width(quality_);
    const u32 height = sky_table_height(quality_);

    const f32 elevation = std::asin(math::clamp(unit.y, -1.0F, 1.0F));
    const f32 normalised = elevation / (math::kPi * 0.5F);
    const f32 v = (normalised < 0.0F ? -1.0F : 1.0F) *
                  std::sqrt(std::fabs(normalised));  // the inverse of the build's parameterisation
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

}  // namespace cy::rendering::sky
