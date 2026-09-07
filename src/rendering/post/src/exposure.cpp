#include <cy/rendering/post/exposure.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The constant relating EV100 to scene luminance. 12.5 is the reflected-light calibration constant
/// most meters use, and stating it here is what makes the two conversions below inverses.
constexpr f32 kCalibration = 12.5F;

/// The saturation-based constant relating EV100 to the maximum luminance an exposure admits. 1.2 is
/// the conventional value and it is what puts middle grey where a photographer expects it.
constexpr f32 kSaturation = 1.2F;

}  // namespace

const char* exposure_mode_name(ExposureMode mode) noexcept {
    switch (mode) {
        case ExposureMode::Manual:
            return "Manual";
        case ExposureMode::Camera:
            return "Camera";
        case ExposureMode::Automatic:
            return "Automatic";
        case ExposureMode::Count:
            break;
    }
    return "Unknown";
}

f32 ev100_from_camera(const CameraExposure& camera) noexcept {
    const f32 aperture = math::max(camera.aperture, 0.5F);
    const f32 shutter = math::max(camera.shutter_seconds, 1e-6F);
    const f32 iso = math::max(camera.iso, 1.0F);
    return std::log2((aperture * aperture) / shutter * 100.0F / iso);
}

f32 exposure_multiplier(f32 ev100) noexcept {
    // The reciprocal of the maximum luminance the exposure admits. A project that publishes this to
    // shaders gets emissive values in physical units for free, which is the second half of the
    // requirement and the reason this is a function rather than a line inside the tonemap pass.
    return 1.0F / math::max(kSaturation * std::exp2(ev100), 1e-6F);
}

f32 ev100_from_luminance(f32 luminance) noexcept {
    return std::log2(math::max(luminance, 1e-6F) * 100.0F / kCalibration);
}

f32 luminance_for_ev100(f32 ev100) noexcept {
    return std::exp2(ev100) * kCalibration / 100.0F;
}

f32 ExposureCompensationCurve::at(f32 metered) const noexcept {
    if (metered <= metered_ev[0]) {
        return compensation[0];
    }
    for (u32 index = 1; index < kPoints; ++index) {
        if (metered <= metered_ev[index]) {
            const f32 span = metered_ev[index] - metered_ev[index - 1];
            const f32 t = span > 1e-6F ? (metered - metered_ev[index - 1]) / span : 0.0F;
            return math::lerp(compensation[index - 1], compensation[index], t);
        }
    }
    return compensation[kPoints - 1];
}

f32 metered_ev100(const LuminanceHistogram& histogram,
                  const AutoExposureSettings& settings) noexcept {
    if (histogram.bins == nullptr || histogram.bin_count == 0) {
        return settings.min_ev;
    }
    u64 total = 0;
    for (u32 index = 0; index < histogram.bin_count; ++index) {
        total += histogram.bins[index];
    }
    if (total == 0) {
        return settings.min_ev;
    }

    const f32 low_fraction = math::clamp(settings.low_percentile, 0.0F, 1.0F);
    const f32 high_fraction = math::clamp(settings.high_percentile, low_fraction, 1.0F);
    const f64 low_count = static_cast<f64>(total) * static_cast<f64>(low_fraction);
    const f64 high_count = static_cast<f64>(total) * static_cast<f64>(high_fraction);

    // Walk the histogram accumulating a weighted sum over the samples between the two percentiles.
    // Discarding whole bins would quantise the answer to the bin width, which at 256 bins over 20
    // EV is a twelfth of a stop of permanent error; this splits the bins the percentiles fall
    // inside.
    const f32 span = histogram.max_ev - histogram.min_ev;
    const f32 bin_width = span / static_cast<f32>(histogram.bin_count);
    f64 seen = 0.0;
    f64 weighted = 0.0;
    f64 weight = 0.0;
    for (u32 index = 0; index < histogram.bin_count; ++index) {
        const f64 count = static_cast<f64>(histogram.bins[index]);
        const f64 begin = seen;
        const f64 end = seen + count;
        seen = end;
        const f64 taken = math::min(end, high_count) - math::max(begin, low_count);
        if (taken <= 0.0) {
            continue;
        }
        const f32 centre = histogram.min_ev + (bin_width * (static_cast<f32>(index) + 0.5F));
        weighted += taken * static_cast<f64>(centre);
        weight += taken;
    }
    if (weight <= 0.0) {
        return math::clamp(histogram.min_ev, settings.min_ev, settings.max_ev);
    }
    const f32 metered = static_cast<f32>(weighted / weight);
    return math::clamp(metered + settings.compensation.at(metered), settings.min_ev,
                       settings.max_ev);
}

f32 adapt_ev100(f32 current_ev, f32 target_ev, f32 delta_seconds,
                const AutoExposureSettings& settings) noexcept {
    if (delta_seconds <= 0.0F) {
        return current_ev;
    }
    // Brightening means the scene got brighter, so the exposure value RISES. The two speeds are not
    // interchangeable and swapping them produces an adaptation that feels wrong without looking
    // wrong in a still.
    const f32 speed =
        target_ev > current_ev ? settings.speed_brightening : settings.speed_darkening;
    const f32 blend = 1.0F - std::exp(-math::max(speed, 0.0F) * delta_seconds);
    return math::lerp(current_ev, target_ev, math::clamp(blend, 0.0F, 1.0F));
}

void ExposureState::update(const LuminanceHistogram& histogram, f32 delta_seconds) noexcept {
    switch (mode) {
        case ExposureMode::Manual:
            current_ev100 = manual_ev100;
            return;
        case ExposureMode::Camera:
            current_ev100 = ev100_from_camera(camera);
            return;
        case ExposureMode::Automatic:
            current_ev100 = adapt_ev100(current_ev100, metered_ev100(histogram, automatic),
                                        delta_seconds, automatic);
            return;
        case ExposureMode::Count:
            break;
    }
}

}  // namespace cy::rendering
