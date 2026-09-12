// The displacement contract's one evaluation, its amplitude split, and its refusals. M10 task 2.3.
//
// Everything in this file is a pure function of its arguments: there is no accumulated wave state
// anywhere, which is what lets a client, a dedicated server and a replay evaluate one sea at one
// time and get one answer without exchanging anything. The phases come from
// `determinism::RandomStream`, which is counter-based for the same reason.

#include <cy/water/displacement.h>

#include <cy/core/determinism/random.h>

#include <cmath>

namespace cy::water {

namespace {

/// The stream the trains' directions and phases are drawn from. A substream per (band, wave), so a
/// band gaining a train does not reshuffle the one beside it — the property `random.h` calls "a new
/// call does not shift the world", applied to a sea whose look must not change when a cascade's
/// resolution does.
[[nodiscard]] determinism::RandomStream train_stream(u64 seed, u32 band, u32 wave) noexcept {
    const determinism::StreamId root = determinism::stream_id("water.surface.train");
    const determinism::StreamId per_band = determinism::substream(root, band);
    // Presentation, not authoritative: these draws shape a surface, and the surface's AUTHORITY
    // comes from the band's own declaration rather than from the stream's purpose. Declaring them
    // authoritative would put the sea's phases into the divergence validator's comparison set,
    // which is a claim about what a replay must reproduce that nothing here needs.
    return determinism::RandomStream{seed, determinism::substream(per_band, wave),
                                     determinism::StreamPurpose::Presentation};
}

/// One train's constants, derived once from the band it belongs to.
struct Train {
    f32 wavelength = 1.0F;
    f32 amplitude = 0.0F;
    f32 dir_x = 1.0F;
    f32 dir_z = 0.0F;
    f32 phase = 0.0F;
};

/// The i-th train of a band. A pure function of (band, index), so the surface at a position is a
/// function of the model and nothing else — no table built at load, nothing to keep in step.
[[nodiscard]] Train train_of(const DisplacementModel& model, u32 band_index, u32 wave) noexcept {
    const DisplacementBand& band = model.bands[band_index];
    const u32 count = (band.wave_count == 0) ? 1U : band.wave_count;

    Train train;
    // Wavelengths spaced geometrically across the band: a band spanning a decade with four trains
    // puts one every 10^(1/3), which is how a spectrum is sampled. Linear spacing would crowd the
    // long end, where the energy is.
    const f32 t = (count == 1U) ? 0.5F : (static_cast<f32>(wave) / static_cast<f32>(count - 1U));
    const f32 ratio = band.wavelength_max / band.wavelength_min;
    train.wavelength = band.wavelength_min * std::pow(ratio, t);

    // The band's amplitude is its PEAK, divided among the trains so that adding trains changes the
    // texture and not the height. Divided rather than scaled by 1/sqrt(n) because the declared
    // number is what a designer reads off a diagnostic and compares against a hull.
    train.amplitude = band.amplitude / static_cast<f32>(count);

    const determinism::RandomStream stream = train_stream(model.seed, band_index, wave);
    const determinism::SimulationPoint origin{};
    // A direction offset in [-spread, +spread] and a phase in [0, 2pi). Two draws at two sample
    // indices of one substream, so neither depends on how many the other took.
    const f32 offset = (stream.unit_float(origin, 0, 0) * 2.0F - 1.0F) * band.spread_degrees;
    const f32 radians = (band.direction_degrees + offset) * (3.14159265358979F / 180.0F);
    train.dir_x = std::cos(radians);
    train.dir_z = std::sin(radians);
    train.phase = stream.unit_float(origin, 0, 1) * (2.0F * 3.14159265358979F);
    return train;
}

/// Whether a selection includes a band.
[[nodiscard]] constexpr bool included(BandSelection selection, BandAuthority authority) noexcept {
    return selection == BandSelection::All || authority == BandAuthority::Authoritative;
}

/// One train's contribution, accumulated into a displacement. Split out because the arithmetic is
/// the part a reader compares against the Gerstner definition, and it should be readable without
/// the band loop around it.
struct TrainAccumulator {
    f32 offset_x = 0.0F;
    f32 offset_y = 0.0F;
    f32 offset_z = 0.0F;
    f32 velocity_x = 0.0F;
    f32 velocity_y = 0.0F;
    f32 velocity_z = 0.0F;
    /// d(offset_x)/dx and d(offset_z)/dz, which are what the Jacobian of the horizontal map needs.
    f32 dxdx = 0.0F;
    f32 dzdz = 0.0F;
    f32 dxdz = 0.0F;
    f32 dzdx = 0.0F;
    /// The surface gradient, for the normal.
    f32 dydx = 0.0F;
    f32 dydz = 0.0F;
    u32 trains = 0;
};

void accumulate(TrainAccumulator& into, const Train& train, f32 steepness, f64 x, f64 z,
                f64 time) noexcept {
    const f32 k = (2.0F * 3.14159265358979F) / train.wavelength;
    // Deep-water dispersion. See the header note on why this relation and not a shallow one.
    const f32 omega = std::sqrt(kGravity * k);

    // The phase is computed in f64 and only then reduced: an f32 product of a 10^5 m coordinate and
    // a 10 rad/m wavenumber has lost every bit that distinguishes one crest from the next.
    const f64 along = (x * static_cast<f64>(train.dir_x)) + (z * static_cast<f64>(train.dir_z));
    const f64 theta = (along * static_cast<f64>(k)) - (time * static_cast<f64>(omega)) +
                      static_cast<f64>(train.phase);
    const auto sin_theta = static_cast<f32>(std::sin(theta));
    const auto cos_theta = static_cast<f32>(std::cos(theta));

    const f32 a = train.amplitude;
    const f32 qa = steepness * a;

    into.offset_y += a * cos_theta;
    into.offset_x -= qa * train.dir_x * sin_theta;
    into.offset_z -= qa * train.dir_z * sin_theta;

    // d/dt of the above. The surface's own velocity, which is what a query reports and what advects
    // foam — and it is the derivative of the SAME expression rather than a second model of motion.
    into.velocity_y += a * omega * sin_theta;
    into.velocity_x += qa * train.dir_x * omega * cos_theta;
    into.velocity_z += qa * train.dir_z * omega * cos_theta;

    into.dydx -= a * k * train.dir_x * sin_theta;
    into.dydz -= a * k * train.dir_z * sin_theta;
    into.dxdx -= qa * k * train.dir_x * train.dir_x * cos_theta;
    into.dzdz -= qa * k * train.dir_z * train.dir_z * cos_theta;
    into.dxdz -= qa * k * train.dir_x * train.dir_z * cos_theta;
    into.dzdx -= qa * k * train.dir_z * train.dir_x * cos_theta;
    ++into.trains;
}

}  // namespace

const char* band_authority_name(BandAuthority authority) noexcept {
    return (authority == BandAuthority::Authoritative) ? "authoritative" : "visual";
}

const char* band_selection_name(BandSelection selection) noexcept {
    return (selection == BandSelection::Authoritative) ? "authoritative" : "all";
}

const char* displacement_problem_name(DisplacementProblem problem) noexcept {
    switch (problem) {
        case DisplacementProblem::None:
            return "none";
        case DisplacementProblem::NoBands:
            return "a displacement model must declare at least one band";
        case DisplacementProblem::BadWavelengths:
            return "a band's wavelength range must be positive and increasing";
        case DisplacementProblem::NoTrains:
            return "a band must synthesise at least one wave train";
        case DisplacementProblem::BadSteepness:
            return "a band's steepness must lie in [0, 1]";
        case DisplacementProblem::NegativeAmplitude:
            return "a band's amplitude must not be negative";
        case DisplacementProblem::LongestBandIsVisual:
            return "large swell must be authoritative: the longest-wavelength band is visual-only";
        case DisplacementProblem::VisualBandFeltByGameplay:
            return "a visual-only band's amplitude is large enough to be felt by gameplay";
    }
    return "unknown";
}

Displacement evaluate_displacement(const DisplacementModel& model, BandSelection selection, f64 x,
                                   f64 z, f64 time) noexcept {
    TrainAccumulator sum;
    for (u32 band_index = 0; band_index < model.band_count; ++band_index) {
        const DisplacementBand& band = model.bands[band_index];
        if (!included(selection, band.authority)) {
            continue;
        }
        const u32 count = (band.wave_count == 0) ? 1U : band.wave_count;
        for (u32 wave = 0; wave < count; ++wave) {
            accumulate(sum, train_of(model, band_index, wave), band.steepness, x, z, time);
        }
    }

    Displacement result;
    result.offset = Vec3{sum.offset_x, sum.offset_y, sum.offset_z};
    result.height = model.mean_level + static_cast<f64>(sum.offset_y);
    result.velocity = Vec3{sum.velocity_x, sum.velocity_y, sum.velocity_z};
    result.normal = normalize(Vec3{-sum.dydx, 1.0F, -sum.dydz});
    // The Jacobian of the horizontal map. Where it falls below one the crest is pinching; where it
    // would go negative the surface has folded through itself and is breaking. Reported in [0, 1]
    // so a consumer can weight foam by it rather than threshold it.
    const f32 jacobian = ((1.0F + sum.dxdx) * (1.0F + sum.dzdz)) - (sum.dxdz * sum.dzdx);
    result.breaking = (jacobian >= 1.0F) ? 0.0F : (1.0F - jacobian);
    if (result.breaking > 1.0F) {
        result.breaking = 1.0F;
    }
    result.trains = sum.trains;
    return result;
}

AmplitudeSplit amplitude_split(const DisplacementModel& model) noexcept {
    AmplitudeSplit split;
    for (u32 index = 0; index < model.band_count; ++index) {
        const DisplacementBand& band = model.bands[index];
        if (band.authority == BandAuthority::Authoritative) {
            split.authoritative_metres += band.amplitude;
            ++split.authoritative_bands;
            continue;
        }
        split.visual_metres += band.amplitude;
        ++split.visual_bands;
        if (band.amplitude > split.largest_visual_band_metres) {
            split.largest_visual_band_metres = band.amplitude;
        }
    }
    return split;
}

DisplacementProblem validate_model(const DisplacementModel& model) noexcept {
    if (model.band_count == 0) {
        return DisplacementProblem::NoBands;
    }
    u32 longest = 0;
    for (u32 index = 0; index < model.band_count; ++index) {
        const DisplacementBand& band = model.bands[index];
        if (band.wavelength_min <= 0.0F || band.wavelength_max < band.wavelength_min) {
            return DisplacementProblem::BadWavelengths;
        }
        if (band.wave_count == 0) {
            return DisplacementProblem::NoTrains;
        }
        if (band.steepness < 0.0F || band.steepness > 1.0F) {
            return DisplacementProblem::BadSteepness;
        }
        if (band.amplitude < 0.0F) {
            return DisplacementProblem::NegativeAmplitude;
        }
        if (band.wavelength_max > model.bands[longest].wavelength_max) {
            longest = index;
        }
    }
    // "Large swell SHALL be authoritative." The longest-wavelength band is the swell whatever else
    // the model declares, so a model whose longest band is visual is refused however small it is.
    if (model.bands[longest].authority != BandAuthority::Authoritative) {
        return DisplacementProblem::LongestBandIsVisual;
    }
    const AmplitudeSplit split = amplitude_split(model);
    if (split.largest_visual_band_metres > model.felt_threshold_metres) {
        return DisplacementProblem::VisualBandFeltByGameplay;
    }
    return DisplacementProblem::None;
}

u32 band_contributions(const DisplacementModel& model, f64 x, f64 z, f64 time,
                       BandContribution* out) noexcept {
    if (out == nullptr) {
        return 0;
    }
    u32 written = 0;
    for (u32 index = 0; index < model.band_count && index < kMaxDisplacementBands; ++index) {
        const DisplacementBand& band = model.bands[index];
        TrainAccumulator sum;
        const u32 count = (band.wave_count == 0) ? 1U : band.wave_count;
        for (u32 wave = 0; wave < count; ++wave) {
            accumulate(sum, train_of(model, index, wave), band.steepness, x, z, time);
        }
        BandContribution contribution;
        contribution.band = index;
        contribution.authority = band.authority;
        contribution.wavelength_min = band.wavelength_min;
        contribution.wavelength_max = band.wavelength_max;
        contribution.vertical_metres = sum.offset_y;
        out[written] = contribution;
        ++written;
    }
    return written;
}

}  // namespace cy::water
