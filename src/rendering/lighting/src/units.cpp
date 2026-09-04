#include <cy/rendering/lighting/units.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The plausible ranges, in each kind's shading unit. Sourced from the physical world rather than
/// invented: moonlight is about 0.25 lux and direct midday sun about 120 000; a candle is roughly
/// 1 cd and a car headlight's hot spot tens of thousands.
constexpr f32 kDirectionalLow = 0.1F;       // deep twilight, in lux
constexpr f32 kDirectionalHigh = 150000.F;  // brighter than any real daylight, in lux
constexpr f32 kPunctualLow = 0.1F;          // a dim indicator, in candela
constexpr f32 kPunctualHigh = 100000.F;     // a searchlight, in candela

/// The geometric middle of a range. Used by the arbitrary-to-physical conversion, because a range
/// spanning six orders of magnitude has no useful arithmetic middle.
[[nodiscard]] f32 geometric_middle(const IntensityRange& range) noexcept {
    return std::sqrt(range.low * range.high);
}

/// Kim et al.'s approximation of the Planckian locus in CIE 1931 xy, valid 1667–25000 K.
void planckian_locus(f32 kelvin, f32& chroma_x, f32& chroma_y) noexcept {
    const f32 temperature = math::clamp(kelvin, 1667.0F, 25000.0F);
    const f32 inverse = 1.0F / temperature;
    const f32 inverse2 = inverse * inverse;
    const f32 inverse3 = inverse2 * inverse;

    if (temperature <= 4000.0F) {
        chroma_x = (-0.2661239e9F * inverse3) - (0.2343589e6F * inverse2) +
                   (0.8776956e3F * inverse) + 0.179910F;
    } else {
        chroma_x = (-3.0258469e9F * inverse3) + (2.1070379e6F * inverse2) +
                   (0.2226347e3F * inverse) + 0.240390F;
    }

    const f32 x2 = chroma_x * chroma_x;
    const f32 x3 = x2 * chroma_x;
    if (temperature <= 2222.0F) {
        chroma_y = (-1.1063814F * x3) - (1.34811020F * x2) + (2.18555832F * chroma_x) - 0.20219683F;
    } else if (temperature <= 4000.0F) {
        chroma_y = (-0.9549476F * x3) - (1.37418593F * x2) + (2.09137015F * chroma_x) - 0.16748867F;
    } else {
        chroma_y = (3.0817580F * x3) - (5.87338670F * x2) + (3.75112997F * chroma_x) - 0.37001483F;
    }
}

}  // namespace

const char* photometric_unit_name(PhotometricUnit unit) noexcept {
    switch (unit) {
        case PhotometricUnit::Lux:
            return "lux";
        case PhotometricUnit::Lumen:
            return "lumen";
        case PhotometricUnit::Candela:
            return "candela";
        case PhotometricUnit::Nit:
            return "nit";
        case PhotometricUnit::Arbitrary:
            return "arbitrary";
        case PhotometricUnit::Count:
            break;
    }
    return "unknown";
}

PhotometricUnit default_unit_for(render::LightKind kind) noexcept {
    switch (kind) {
        case render::LightKind::Directional:
            return PhotometricUnit::Lux;
        case render::LightKind::Point:
        case render::LightKind::Spot:
            return PhotometricUnit::Lumen;
        case render::LightKind::Count:
            break;
    }
    return PhotometricUnit::Arbitrary;
}

// --- Conversions -------------------------------------------------------------------------------

f32 point_candela_from_lumens(f32 lumens) noexcept {
    return lumens / (4.0F * kPi);
}

f32 spot_candela_from_lumens(f32 lumens) noexcept {
    // The photometric convention: independent of the cone angle, so narrowing a beam shrinks the
    // lit region rather than brightening it. See the header comment.
    return lumens / kPi;
}

f32 spot_candela_physical(f32 lumens, f32 outer_cone_radians) noexcept {
    const f32 cone = math::clamp(outer_cone_radians, 0.0F, kPi);
    const f32 solid_angle = 2.0F * kPi * (1.0F - std::cos(cone));
    return solid_angle > 1e-6F ? lumens / solid_angle : lumens;
}

f32 area_nits_from_lumens(f32 lumens, f32 area_square_metres) noexcept {
    // A Lambertian emitter's flux spreads over π steradians of PROJECTED solid angle. Using 2π —
    // the geometric hemisphere — is the classic factor-of-two error and it is why this is a
    // function rather than an expression at a call site.
    if (area_square_metres <= 0.0F) {
        return 0.0F;
    }
    return lumens / (area_square_metres * kPi);
}

f32 to_shading_intensity(render::LightKind kind, PhotometricUnit unit, f32 value,
                         f32 outer_cone_radians, f32 area_square_metres) noexcept {
    switch (unit) {
        case PhotometricUnit::Arbitrary:
        case PhotometricUnit::Candela:
        case PhotometricUnit::Lux:
        case PhotometricUnit::Nit:
            // Already the quantity the shader integrates for the kinds that use it. A lux value on
            // a point light is a content error the validator reports; converting it silently here
            // would hide it.
            return value;
        case PhotometricUnit::Lumen:
            break;
        case PhotometricUnit::Count:
            return value;
    }

    switch (kind) {
        case render::LightKind::Point:
            return point_candela_from_lumens(value);
        case render::LightKind::Spot:
            return spot_candela_from_lumens(value);
        case render::LightKind::Directional:
            // Flux has no meaning for a light with no position; the value is taken as illuminance.
            return value;
        case render::LightKind::Count:
            break;
    }
    (void)outer_cone_radians;
    (void)area_square_metres;
    return value;
}

// --- Plausibility ------------------------------------------------------------------------------

IntensityRange plausible_range(render::LightKind kind) noexcept {
    if (kind == render::LightKind::Directional) {
        return IntensityRange{kDirectionalLow, kDirectionalHigh};
    }
    return IntensityRange{kPunctualLow, kPunctualHigh};
}

bool intensity_is_plausible(render::LightKind kind, f32 shading_intensity) noexcept {
    const IntensityRange range = plausible_range(kind);
    return shading_intensity >= range.low && shading_intensity <= range.high;
}

f32 arbitrary_to_physical(render::LightKind kind, f32 arbitrary) noexcept {
    if (arbitrary <= 0.0F) {
        return 0.0F;
    }
    return arbitrary * geometric_middle(plausible_range(kind));
}

f32 physical_to_arbitrary(render::LightKind kind, f32 shading_intensity) noexcept {
    const f32 middle = geometric_middle(plausible_range(kind));
    return middle > 0.0F ? shading_intensity / middle : shading_intensity;
}

// --- Colour temperature ------------------------------------------------------------------------

Vec3 blackbody_color(f32 kelvin) noexcept {
    f32 chroma_x = 0.0F;
    f32 chroma_y = 0.0F;
    planckian_locus(kelvin, chroma_x, chroma_y);
    if (chroma_y <= 1e-6F) {
        return Vec3{1.0F, 1.0F, 1.0F};
    }

    // xyY with Y = 1 into XYZ, then into linear Rec. 709.
    const f32 big_x = chroma_x / chroma_y;
    const f32 big_y = 1.0F;
    const f32 big_z = (1.0F - chroma_x - chroma_y) / chroma_y;

    Vec3 rgb{(3.2404542F * big_x) - (1.5371385F * big_y) - (0.4985314F * big_z),
             (-0.9692660F * big_x) + (1.8760108F * big_y) + (0.0415560F * big_z),
             (0.0556434F * big_x) - (0.2040259F * big_y) + (1.0572252F * big_z)};
    rgb.x = math::max(rgb.x, 0.0F);
    rgb.y = math::max(rgb.y, 0.0F);
    rgb.z = math::max(rgb.z, 0.0F);

    // Normalise to unit luminance, so the temperature is a tint and not a second intensity.
    const f32 luminance = (0.2126F * rgb.x) + (0.7152F * rgb.y) + (0.0722F * rgb.z);
    if (luminance <= 1e-6F) {
        return Vec3{1.0F, 1.0F, 1.0F};
    }
    return rgb * (1.0F / luminance);
}

// --- Exposure ----------------------------------------------------------------------------------

f32 exposure_value(const CameraExposure& camera) noexcept {
    const f32 aperture = math::max(camera.aperture, 1e-3F);
    const f32 shutter = math::max(camera.shutter_seconds, 1e-6F);
    const f32 sensitivity = math::max(camera.sensitivity, 1e-3F);
    return std::log2((aperture * aperture) / shutter) - std::log2(sensitivity / 100.0F) -
           camera.compensation;
}

f32 exposure_multiplier(f32 exposure_value) noexcept {
    return 1.0F / (1.2F * std::exp2(exposure_value));
}

f32 exposure_multiplier(const CameraExposure& camera) noexcept {
    return exposure_multiplier(exposure_value(camera));
}

f32 exposure_value_for_luminance(f32 average_luminance) noexcept {
    // The standard relation between an average scene luminance and the exposure value that places
    // it at mid grey: EV100 = log2(L · S / K) with S = 100 and the reflected-light meter constant
    // K = 12.5, which is what every hand-held meter is calibrated to.
    const f32 luminance = math::max(average_luminance, 1e-6F);
    return std::log2((luminance * 100.0F) / 12.5F);
}

}  // namespace cy::rendering
