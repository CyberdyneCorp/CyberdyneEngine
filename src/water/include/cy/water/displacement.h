#pragma once
// THE DISPLACEMENT CONTRACT. M10 task 2.3, and the requirement `water` says matters most.
//
// "The classic water bug is a boat floating on a flat plane beneath visible swell, and it happens
// because rendering displaces the surface on the GPU while physics samples something else. So a
// body declares which frequency bands are AUTHORITATIVE — evaluated identically for rendering,
// physics, and gameplay — and which are VISUAL ONLY. Large swell is authoritative; capillary detail
// is declared, not assumed."
//
// ================================================================================================
// ONE DEFINITION, TWO SELECTIONS — NOT TWO IMPLEMENTATIONS
// ================================================================================================
//
// `evaluate_displacement()` is the only function in this engine that says where the water surface
// is. Rendering calls it with `BandSelection::All`; a physics query, a buoyancy sample and a
// gameplay query call it with `BandSelection::Authoritative`. The difference between the renderer's
// answer and the physics answer is therefore exactly the visual-only bands and NOTHING ELSE —
// not a different integrator, not a different dispersion relation, not a different phase.
//
// That is why the selection is an argument rather than a second entry point. A `evaluate_visual()`
// beside an `evaluate_physics()` is the bug this file exists to make unwritable: two functions
// drift and one function cannot.
//
// ================================================================================================
// WHAT A VISUAL BAND IS ALLOWED TO BE
// ================================================================================================
//
// "A visual-only band whose amplitude is large enough to be FELT by gameplay SHALL be a
// specification violation, and the engine SHALL be able to report the amplitude split so it is
// checkable."
//
// So the violation is refused by `validate_model()` at configuration time, and the split is
// reported by `amplitude_split()` at any time — the diagnostic exists whether or not the refusal
// was taken, because the question a developer actually asks is "do physics and rendering disagree
// here, and by how much", and the answer to it is a number in metres.
//
// `DisplacementModel::felt_threshold_metres` is what "felt by gameplay" means, declared rather than
// assumed: a project whose boats are rafts sets it lower than one whose boats are frigates. Its
// default, 5 cm, is a quarter of the freeboard of a small dinghy and roughly the vertical error at
// which a floating object is seen to sit wrong.
//
// ================================================================================================
// THE SYNTHESIS
// ================================================================================================
//
// Each band is a sum of `wave_count` Gerstner trains whose wavelengths span the band geometrically,
// whose directions scatter about the band's mean by its declared spread, and whose phases are drawn
// from `cy::determinism::RandomStream` keyed by (seed, band, wave). Counter-based, so the sea at
// time t is a pure function of (model, position, t) with no accumulated state anywhere — which is
// what lets a client, a server and a replay evaluate the same swell without exchanging a wave.
//
// The dispersion is deep water's, omega = sqrt(g k). It is not a shallow-water solver and does not
// claim to be: `ShallowWater` is the specification's Planned backend, and shoaling near a beach is
// approximated by the shoreline's own breaking term rather than by pretending this relation holds
// there.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::water {

/// Standard gravity, m/s^2. Named here because the dispersion relation, the buoyancy force and the
/// wave celerity must use one number, and three files each writing 9.81 is three numbers.
inline constexpr f32 kGravity = 9.80665F;

/// Whether a band is part of the shared truth or part of the picture.
enum class BandAuthority : u8 {
    /// Evaluated identically for rendering, physics and gameplay.
    Authoritative = 0,
    /// Evaluated in rendering alone, and DECLARED so that the discrepancy is documented rather than
    /// discovered.
    Visual = 1,
};

[[nodiscard]] const char* band_authority_name(BandAuthority authority) noexcept;

/// Which bands an evaluation includes.
enum class BandSelection : u8 {
    /// The shared truth. What every physics and gameplay query uses, and what buoyancy floats on.
    Authoritative = 0,
    /// Everything, including the declared visual-only detail. What the renderer displaces by.
    All = 1,
};

[[nodiscard]] const char* band_selection_name(BandSelection selection) noexcept;

/// One frequency band of the surface. `water` — "Ocean simulation": "several frequency bands, each
/// covering a WAVELENGTH RANGE, driven by wind speed, direction, and fetch".
struct DisplacementBand {
    /// The band's wavelength range, in metres. `wavelength_min` must be positive and below
    /// `wavelength_max`; the trains inside it are spaced geometrically across the range.
    f32 wavelength_min = 4.0F;
    f32 wavelength_max = 16.0F;
    /// The band's total vertical amplitude, in metres, as the peak of the summed trains. The
    /// per-train amplitudes are this divided among them, so widening a band's train count changes
    /// its texture and not its height.
    f32 amplitude = 0.25F;
    /// The mean direction of travel, in degrees, measured from +X toward +Z.
    f32 direction_degrees = 0.0F;
    /// How far the trains scatter either side of the mean, in degrees. Zero is a single direction —
    /// a perfectly regular swell, which real water never is and a test often wants.
    f32 spread_degrees = 25.0F;
    /// Gerstner steepness in [0, 1]: how much the crests are pulled horizontally toward each other.
    /// Above about 1 the surface folds through itself, which is why the model's validation refuses
    /// it rather than rendering an inside-out wave.
    f32 steepness = 0.5F;
    /// Trains synthesised in the band. More is smoother and costs linearly.
    u32 wave_count = 4;
    BandAuthority authority = BandAuthority::Authoritative;
};

/// The most bands one body may declare. Eight, because the ocean's cascades are four and a project
/// adding swell from a second storm, a harbour's chop and a capillary detail band still fits —
/// while the array stays small enough that a `DisplacementModel` is copied by value into a job.
inline constexpr u32 kMaxDisplacementBands = 8;

/// A body's surface, as a set of declared bands. Copyable by value and free of pointers, because
/// every consumer of it — a physics query on a worker, a buoyancy solve, the renderer's own
/// evaluation — wants its own copy rather than a reference into a system that may be ticking.
struct DisplacementModel {
    /// The seed the trains' phases and directions are drawn from. Part of the model rather than
    /// ambient, so that two machines evaluating one sea agree without exchanging anything.
    u64 seed = 0;
    DisplacementBand bands[kMaxDisplacementBands];
    u32 band_count = 0;
    /// The vertical amplitude, in metres, at which a visual-only band is declared to be FELT by
    /// gameplay — and therefore a specification violation. See the header note.
    f32 felt_threshold_metres = 0.05F;
    /// The still-water level this model displaces about, in absolute metres.
    f64 mean_level = 0.0;

    [[nodiscard]] bool add(const DisplacementBand& band) noexcept {
        if (band_count >= kMaxDisplacementBands) {
            return false;
        }
        bands[band_count] = band;
        ++band_count;
        return true;
    }
};

/// One evaluation of the surface.
///
/// `offset` is the full Gerstner displacement of the lattice point: `y` is the vertical rise and
/// `x`/`z` are the horizontal pull that sharpens crests. A consumer wanting only the height reads
/// `height`, which is `mean_level + offset.y` and is the number a buoyancy sample uses.
struct Displacement {
    /// Absolute surface height at the sampled horizontal position, in metres.
    f64 height = 0.0;
    Vec3 offset{0.0F, 0.0F, 0.0F};
    /// The surface normal, from the analytic derivatives of the summed trains. Unit length.
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// The surface's own velocity, m/s — the time derivative of `offset`. This is what a query
    /// reports as the water's velocity for an open-water body, and what advects foam.
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    /// Breaking indicator in [0, 1], from the Jacobian of the horizontal displacement: where the
    /// Gerstner map folds, the surface is overturning and that is where foam is generated.
    /// `water` — "producing displacement, normals, and a FOAM OR BREAKING INDICATOR".
    f32 breaking = 0.0F;
    /// How many trains were summed. The query cost the diagnostics requirement asks for, counted
    /// rather than estimated.
    u32 trains = 0;
};

/// THE shared definition. See the header note: one function, two selections.
///
/// `horizontal` is the absolute world position in the XZ plane, in metres; `time` is the body's own
/// simulation time in seconds. Both are f64 because an ocean is world-scale and a phase computed
/// from an f32 coordinate a hundred kilometres out is quantised to something visibly periodic.
[[nodiscard]] Displacement evaluate_displacement(const DisplacementModel& model,
                                                 BandSelection selection, f64 horizontal_x,
                                                 f64 horizontal_z, f64 time) noexcept;

/// The amplitude split, reported so it is checkable. `water` — "the diagnostics SHALL report the
/// amplitude of authoritative and visual-only bands at that location".
struct AmplitudeSplit {
    f32 authoritative_metres = 0.0F;
    f32 visual_metres = 0.0F;
    u32 authoritative_bands = 0;
    u32 visual_bands = 0;
    /// The largest single visual-only band. This, not the sum, is what is compared against the
    /// felt threshold: two independent 3 cm ripples do not make a 6 cm error a boat can sit in.
    f32 largest_visual_band_metres = 0.0F;
};

[[nodiscard]] AmplitudeSplit amplitude_split(const DisplacementModel& model) noexcept;

/// Why a model was refused.
enum class DisplacementProblem : u8 {
    None = 0,
    NoBands,
    /// A band whose wavelength range is empty or non-positive.
    BadWavelengths,
    /// A band with no trains in it.
    NoTrains,
    /// Steepness outside [0, 1]: the Gerstner map folds through itself above one.
    BadSteepness,
    NegativeAmplitude,
    /// "Large swell SHALL be authoritative": the longest-wavelength band is declared visual.
    LongestBandIsVisual,
    /// "A visual-only band whose amplitude is large enough to be felt by gameplay SHALL be a
    /// specification violation." The refusal, at configuration time.
    VisualBandFeltByGameplay,
};

[[nodiscard]] const char* displacement_problem_name(DisplacementProblem problem) noexcept;

/// Validate a model against the displacement contract. See `DisplacementProblem`.
[[nodiscard]] DisplacementProblem validate_model(const DisplacementModel& model) noexcept;

/// The bands that contributed at a position, each with its own vertical contribution — the
/// diagnostic behind "WHICH BANDS CONTRIBUTED" in `water`'s diagnostics requirement.
struct BandContribution {
    u32 band = 0;
    BandAuthority authority = BandAuthority::Authoritative;
    f32 wavelength_min = 0.0F;
    f32 wavelength_max = 0.0F;
    /// This band's own vertical displacement at the sampled position, in metres. Signed: a band
    /// may be pulling the surface down where another pushes it up, and a report that showed only
    /// magnitudes would hide the cancellation that makes a trough.
    f32 vertical_metres = 0.0F;
};

/// Per-band contributions at one position, newest first in band order. Writes at most
/// `kMaxDisplacementBands` entries and returns how many it wrote.
[[nodiscard]] u32 band_contributions(const DisplacementModel& model, f64 horizontal_x,
                                     f64 horizontal_z, f64 time, BandContribution* out) noexcept;

}  // namespace cy::water
