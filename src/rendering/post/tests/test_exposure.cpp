// Exposure and tonemapping. The two cases worth reading are the percentile that stops a window
// blowing out a room, and the measurement of the reason AgX is the default.

#include <cy/test/test.h>

#include <cy/rendering/post/exposure.h>
#include <cy/rendering/post/tonemap.h>

#include <cmath>

namespace {

using cy::rendering::AutoExposureSettings;
using cy::rendering::CameraExposure;
using cy::rendering::ev100_from_luminance;
using cy::rendering::exposure_multiplier;
using cy::rendering::ExposureMode;
using cy::rendering::ExposureState;
using cy::rendering::luminance_for_ev100;
using cy::rendering::LuminanceHistogram;
using cy::rendering::OutputTarget;
using cy::rendering::TonemapOperator;
using cy::rendering::TonemapSettings;
using cy::rendering::TransferFunction;

/// A histogram of a dim interior with a small very bright window in it.
struct Room {
    static constexpr cy::u32 kBins = 64;
    cy::u32 bins[kBins] = {};
};

Room room_with_window(cy::u32 window_pixels) noexcept {
    Room room;
    // The room: everything between bins 12 and 20, which is a few stops around EV 0.
    for (cy::u32 bin = 12; bin <= 20; ++bin) {
        room.bins[bin] = 1000;
    }
    // The window: a handful of pixels at the very top.
    room.bins[Room::kBins - 1] = window_pixels;
    return room;
}

LuminanceHistogram histogram_of(const Room& room) noexcept {
    LuminanceHistogram histogram;
    histogram.bins = room.bins;
    histogram.bin_count = Room::kBins;
    histogram.min_ev = -4.0F;
    histogram.max_ev = 16.0F;
    return histogram;
}

}  // namespace

CY_TEST_CASE("a small bright window does not blow out the room") {
    AutoExposureSettings settings;

    const Room without = room_with_window(0);
    const Room with = room_with_window(400);
    const cy::f32 metered_without = metered_ev100(histogram_of(without), settings);
    const cy::f32 metered_with = metered_ev100(histogram_of(with), settings);

    // The window is four hundred pixels against nine thousand and it is twelve stops brighter. A
    // mean luminance would be dragged most of the way to it; the percentile is not.
    CY_CHECK_NEAR(metered_with, metered_without, 0.35F);

    // And the check that the test is measuring something: a window big enough to BE the scene does
    // move the exposure, so the percentile is rejecting an outlier rather than ignoring the data.
    const Room dominant = room_with_window(40000);
    CY_CHECK_GT(metered_ev100(histogram_of(dominant), settings), metered_without + 2.0F);
}

CY_TEST_CASE("metering is clamped, compensated, and honest about an empty histogram") {
    AutoExposureSettings settings;
    settings.min_ev = 2.0F;
    settings.max_ev = 4.0F;
    const Room room = room_with_window(0);
    CY_CHECK_LE(metered_ev100(histogram_of(room), settings), 4.0F);
    CY_CHECK_GE(metered_ev100(histogram_of(room), settings), 2.0F);

    LuminanceHistogram empty;
    CY_CHECK_NEAR(metered_ev100(empty, settings), settings.min_ev, 1e-6F);

    // The compensation curve is keyed on the metered value: darker scenes get lifted, bright ones
    // are left alone.
    AutoExposureSettings compensated;
    compensated.compensation.metered_ev[0] = 0.0F;
    compensated.compensation.metered_ev[1] = 4.0F;
    compensated.compensation.metered_ev[2] = 8.0F;
    compensated.compensation.metered_ev[3] = 12.0F;
    compensated.compensation.compensation[0] = 2.0F;
    compensated.compensation.compensation[1] = 1.0F;
    CY_CHECK_NEAR(compensated.compensation.at(-5.0F), 2.0F, 1e-6F);
    CY_CHECK_NEAR(compensated.compensation.at(2.0F), 1.5F, 1e-6F);
    CY_CHECK_NEAR(compensated.compensation.at(99.0F), 0.0F, 1e-6F);
}

CY_TEST_CASE("the camera's three controls agree with EV, and adaptation has two speeds") {
    CameraExposure camera;
    camera.aperture = 4.0F;
    camera.shutter_seconds = 1.0F / 100.0F;
    camera.iso = 100.0F;
    // f/4 at 1/100 s and ISO 100 is a well-known EV.
    CY_CHECK_NEAR(ev100_from_camera(camera), std::log2(16.0F * 100.0F), 1e-4F);

    // Stopping down one stop raises EV by exactly one, which is the property a photographer's
    // intuition rests on.
    CameraExposure stopped = camera;
    stopped.aperture = 5.6568542F;
    CY_CHECK_NEAR(ev100_from_camera(stopped), ev100_from_camera(camera) + 1.0F, 1e-3F);

    // Brighter scene means a bigger EV means a smaller multiplier.
    CY_CHECK_LT(exposure_multiplier(14.0F), exposure_multiplier(10.0F));
    // The two luminance conversions are inverses.
    CY_CHECK_NEAR(ev100_from_luminance(luminance_for_ev100(7.5F)), 7.5F, 1e-4F);

    AutoExposureSettings settings;
    settings.speed_brightening = 4.0F;
    settings.speed_darkening = 1.0F;
    const cy::f32 up = adapt_ev100(0.0F, 4.0F, 0.1F, settings);
    const cy::f32 down = adapt_ev100(0.0F, -4.0F, 0.1F, settings);
    // The eye adapts to darkness far more slowly than to light; matching that is the difference
    // between adapting and flickering.
    CY_CHECK_GT(up, 0.0F);
    CY_CHECK_LT(down, 0.0F);
    CY_CHECK_GT(up, -down);
    CY_CHECK_EQ(adapt_ev100(3.0F, 9.0F, 0.0F, settings), 3.0F);
}

CY_TEST_CASE("the exposure state does what its mode says and nothing else") {
    LuminanceHistogram nothing;

    ExposureState manual;
    manual.mode = ExposureMode::Manual;
    manual.manual_ev100 = 9.5F;
    manual.update(nothing, 1.0F / 60.0F);
    CY_CHECK_NEAR(manual.current_ev100, 9.5F, 1e-6F);
    CY_CHECK_NEAR(manual.multiplier(), exposure_multiplier(9.5F), 1e-9F);

    ExposureState camera;
    camera.mode = ExposureMode::Camera;
    camera.camera.aperture = 1.4F;
    camera.update(nothing, 1.0F / 60.0F);
    CY_CHECK_NEAR(camera.current_ev100, ev100_from_camera(camera.camera), 1e-5F);

    // Automatic converges on the metered value rather than jumping to it.
    const Room room = room_with_window(0);
    ExposureState automatic;
    automatic.mode = ExposureMode::Automatic;
    automatic.current_ev100 = 0.0F;
    const cy::f32 target = metered_ev100(histogram_of(room), automatic.automatic);
    automatic.update(histogram_of(room), 1.0F / 60.0F);
    CY_CHECK_GT(automatic.current_ev100, 0.0F);
    CY_CHECK_LT(automatic.current_ev100, target);
    for (cy::u32 frame = 0; frame < 600; ++frame) {
        automatic.update(histogram_of(room), 1.0F / 60.0F);
    }
    CY_CHECK_NEAR(automatic.current_ev100, target, 1e-2F);
}

CY_TEST_CASE("AgX is the default, and it desaturates a bright light instead of shifting its hue") {
    CY_CHECK_EQ(cy::rendering::default_tonemap_operator(), TonemapOperator::AgX);
    CY_CHECK_EQ(TonemapSettings{}.op, TonemapOperator::AgX);

    TonemapSettings agx;
    const cy::Vec3 red{1.0F, 0.05F, 0.05F};
    const cy::Vec3 dim = tonemap(red, agx);
    const cy::Vec3 bright = tonemap(cy::Vec3{red.x * 40.0F, red.y * 40.0F, red.z * 40.0F}, agx);

    // Rolls off toward white: less saturated as it gets brighter.
    CY_CHECK_LT(cy::rendering::colour_saturation(bright), cy::rendering::colour_saturation(dim));
    // And keeps its hue while doing so.
    CY_CHECK_NEAR(cy::rendering::colour_hue(bright), cy::rendering::colour_hue(dim), 0.15F);
    // Inside the display's range.
    CY_CHECK_LE(bright.x, 1.0001F);
    CY_CHECK_GE(bright.x, 0.0F);

    // Monotone: a brighter input is never a darker output. A non-monotone curve inverts a gradient
    // somewhere and the artefact looks like banding.
    cy::f32 previous = -1.0F;
    for (cy::u32 step = 0; step < 64; ++step) {
        const cy::f32 value = static_cast<cy::f32>(step) * 0.5F;
        const cy::Vec3 grey = tonemap(cy::Vec3{value, value, value}, agx);
        CY_CHECK_GE(grey.x, previous - 1e-5F);
        previous = grey.x;
    }
}

CY_TEST_CASE("every operator is bounded, and an HDR display gets its own range") {
    const cy::Vec3 blazing{60.0F, 40.0F, 12.0F};
    for (cy::u32 index = 0; index < static_cast<cy::u32>(TonemapOperator::Count); ++index) {
        TonemapSettings settings;
        settings.op = static_cast<TonemapOperator>(index);
        if (settings.op == TonemapOperator::Custom) {
            continue;  // the caller supplies the curve; passing through is deliberate
        }
        const cy::Vec3 mapped = tonemap(blazing, settings);
        CY_CHECK_LE(mapped.x, 1.0001F);
        CY_CHECK_LE(mapped.y, 1.0001F);
        CY_CHECK_GE(mapped.z, 0.0F);
        CY_CHECK_NE(cy::rendering::tonemap_operator_name(settings.op)[0], '\0');
    }

    // An HDR display is mapped to ITS range and not to SDR white: the same input comes back
    // brighter, rather than being clipped at one and then stretched.
    TonemapSettings hdr;
    hdr.output.transfer = TransferFunction::Pq;
    hdr.output.peak_nits = 1000.0F;
    CY_CHECK_GT(tonemap(blazing, hdr).x, 1.0F);

    // The encodings: sRGB is a curve into [0,1], PQ is a curve into [0,1] against 10000 nits, and
    // scRGB passes linear values through.
    const cy::Vec3 half{0.5F, 0.5F, 0.5F};
    CY_CHECK_GT(encode_output(half, OutputTarget{}).x, 0.5F);
    OutputTarget pq;
    pq.transfer = TransferFunction::Pq;
    pq.peak_nits = 1000.0F;
    CY_CHECK_LT(encode_output(half, pq).x, 1.0F);
    OutputTarget scrgb;
    scrgb.transfer = TransferFunction::ScRgb;
    CY_CHECK_NEAR(encode_output(half, scrgb).x, 0.5F, 1e-6F);
}
