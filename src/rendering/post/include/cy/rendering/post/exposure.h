#pragma once
// Exposure: manual, and auto-exposure from a luminance histogram. Task 8.4.
//
// `rendering-post-processing` — "Exposure". Manual exposure in EV or in aperture/shutter/ISO;
// auto-exposure with a metering mask, **histogram percentile-based target selection to reject
// outliers**, minimum and maximum EV clamps, separate adaptation speeds for brightening and
// darkening, and an exposure compensation curve keyed on measured luminance.
//
// THE PERCENTILE IS THE REQUIREMENT, AND IT IS WHY THIS IS A HISTOGRAM AND NOT AN AVERAGE. The
// scenario: "WHEN a small bright region is in view THEN percentile-based metering SHALL reject the
// outlier and expose for the room". An average luminance is dragged by a window, a light bulb or a
// specular highlight, and the symptom is an interior that darkens as the camera turns toward a
// window — which is a very familiar bug and is not fixable by tuning the adaptation speed.
//
// EXPOSURE IS ALSO PUBLISHED TO SHADERS. "Exposure SHALL be applied as a scalar multiply before
// tonemapping and SHALL also be published to shaders so emissive values can be expressed in
// physical units." That is what `multiplier_for_ev100()` is for: the same number reaches the
// multiply and the material constant buffer, from one function, so an emissive surface authored in
// nits is the same brightness whichever path evaluates it.

#include <cy/core/base/types.h>

namespace cy::rendering {

enum class ExposureMode : u8 {
    /// EV100 set directly.
    Manual = 0,
    /// EV100 derived from aperture, shutter and ISO.
    Camera,
    /// EV100 metered from the scene's luminance histogram.
    Automatic,
    Count,
};

[[nodiscard]] const char* exposure_mode_name(ExposureMode mode) noexcept;

/// A physical camera's three controls.
///
/// NOT `CameraExposure`, and neither is `multiplier_for_ev100` `exposure_multiplier`: those two
/// names are `lighting/units.h`'s, in this same namespace. While both modules defined them, any
/// binary linking this file's object and `units.cpp`'s failed with a duplicate
/// `exposure_multiplier(float)`, and any source including both headers failed on the struct — which
/// nothing did until the graded resolve's suite linked the pipeline scene beside the metering.
/// render.grading's `the post chain's exposure and the lighting module's link together and agree`
/// is the regression case.
struct CameraControls {
    /// f-number.
    f32 aperture = 4.0F;
    /// Seconds.
    f32 shutter_seconds = 1.0F / 100.0F;
    f32 iso = 100.0F;
};

/// EV100 from the three controls. The standard relation, written once so that a project that
/// changes one control gets the exposure change a photographer expects.
[[nodiscard]] f32 ev100_from_camera(const CameraControls& camera) noexcept;

/// The scalar the scene colour is multiplied by: `1 / (1.2 · 2^EV100)`, the value
/// `lighting/units.h`'s `exposure_multiplier` also computes. Published to shaders as well as
/// applied.
[[nodiscard]] f32 multiplier_for_ev100(f32 ev100) noexcept;

/// EV100 for an average scene luminance in cd/m². The inverse of `luminance_for_ev100`.
[[nodiscard]] f32 ev100_from_luminance(f32 luminance) noexcept;

[[nodiscard]] f32 luminance_for_ev100(f32 ev100) noexcept;

/// The stops `cy/fullscreen.slang`'s resolve multiplies the scene by — `exp2(stops)` — for an
/// EV100: the base-two logarithm of `multiplier_for_ev100`. The frame's globals carry stops, so
/// this is how a physical exposure reaches a resolve that predates it without the resolve changing.
[[nodiscard]] f32 exposure_stops_for_ev100(f32 ev100) noexcept;

/// The inverse of `exposure_stops_for_ev100`.
[[nodiscard]] f32 ev100_for_exposure_stops(f32 stops) noexcept;

/// Rec. 709 luminance of a linear colour: what the metering reads from each pixel.
[[nodiscard]] f32 metering_luminance(f32 red, f32 green, f32 blue) noexcept;

/// The bin of a `bin_count`-bin histogram, log-spaced in EV100 between `min_ev` and `max_ev`, that
/// a luminance lands in. Below the range is bin 0 and above it the last bin, so every pixel is
/// counted. The host twin of the metering shader's binning, written once so the two cannot drift.
[[nodiscard]] u32 luminance_histogram_bin(f32 luminance, u32 bin_count, f32 min_ev,
                                          f32 max_ev) noexcept;

/// The seconds `adapt_ev100` takes at `speed` EV per second to close all but `remaining` of a gap
/// (0.05 is "within five per cent"). The adaptation is exponential, so this is its stated time
/// constant turned into a deadline: `-ln(remaining) / speed`. Infinite for a zero speed.
[[nodiscard]] f32 adaptation_seconds(f32 speed, f32 remaining) noexcept;

/// A compensation curve keyed on measured luminance: EV added at a given metered EV. Four control
/// points is enough for the shape projects actually author — darker at night, flatter in daylight —
/// and a curve object here would duplicate `cy::math::Curve` for no gain.
struct ExposureCompensationCurve {
    static constexpr u32 kPoints = 4;
    /// Metered EV100 at each control point, ascending. Unsorted input is not corrected.
    f32 metered_ev[kPoints] = {-4.0F, 0.0F, 8.0F, 16.0F};
    /// EV added at each control point.
    f32 compensation[kPoints] = {0.0F, 0.0F, 0.0F, 0.0F};

    [[nodiscard]] f32 at(f32 metered) const noexcept;
};

struct AutoExposureSettings {
    /// Fraction of the histogram to discard at each end before taking the mean. The requirement's
    /// outlier rejection: 0.5 and 0.95 exposes for the upper middle of the image and ignores both
    /// a black doorway and a bright window.
    f32 low_percentile = 0.50F;
    f32 high_percentile = 0.95F;
    f32 min_ev = -4.0F;
    f32 max_ev = 16.0F;
    /// EV per second. Separate, because the eye adapts to darkness far more slowly than to light
    /// and matching that is the difference between "adapting" and "flickering".
    f32 speed_brightening = 3.0F;
    f32 speed_darkening = 1.0F;
    ExposureCompensationCurve compensation;
};

/// A luminance histogram, log-spaced between `min_ev` and `max_ev`. The bin count is the caller's:
/// a compute shader typically writes 256, and the metering below does not care.
struct LuminanceHistogram {
    const u32* bins = nullptr;
    u32 bin_count = 0;
    f32 min_ev = -4.0F;
    f32 max_ev = 16.0F;
};

/// The metered EV100: the mean of the bins between the two percentiles. Returns `min_ev` for an
/// empty histogram, which is the honest answer for a frame nothing was measured in.
[[nodiscard]] f32 metered_ev100(const LuminanceHistogram& histogram,
                                const AutoExposureSettings& settings) noexcept;

/// One frame of adaptation, exponential toward the target at the direction's own speed.
[[nodiscard]] f32 adapt_ev100(f32 current_ev, f32 target_ev, f32 delta_seconds,
                              const AutoExposureSettings& settings) noexcept;

/// The whole exposure state, stepped once a frame.
struct ExposureState {
    ExposureMode mode = ExposureMode::Automatic;
    f32 manual_ev100 = 12.0F;
    CameraControls camera;
    AutoExposureSettings automatic;
    /// The value in force. Adapts toward the metered target under `Automatic`.
    f32 current_ev100 = 12.0F;

    /// Advance one frame. `histogram` is ignored outside `Automatic`.
    void update(const LuminanceHistogram& histogram, f32 delta_seconds) noexcept;

    [[nodiscard]] f32 multiplier() const noexcept { return multiplier_for_ev100(current_ev100); }
};

}  // namespace cy::rendering
