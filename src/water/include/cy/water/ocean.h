#pragma once
// The ocean: cascaded spectral synthesis, and a surface generated around the camera rather than
// stored for the world. M10 task 2.3.
//
// `water` — "Ocean simulation": "Oceans SHALL be simulated by cascaded spectral synthesis: several
// frequency bands, each covering a wavelength range, driven by wind speed, direction, and fetch,
// producing displacement, normals, and a foam or breaking indicator. A single world-scale
// simulation SHALL NOT be used; cascades SHALL be evaluated at appropriate resolutions and
// combined. The ocean surface SHALL be CAMERA-RELATIVE for rendering — generated around the viewer
// rather than as a world-scale mesh — while its parameters (sea level, wind, tide, weather) remain
// global."
//
// ================================================================================================
// THE SPECTRUM IS A FUNCTION OF WIND AND FETCH, NOT A TABLE OF AMPLITUDES
// ================================================================================================
//
// `build_ocean_model()` integrates a Pierson-Moskowitz spectrum, fetch-limited in the JONSWAP
// manner, over each cascade's own frequency range and converts the energy in the band to the
// amplitude the band's trains carry. That is what makes the specification's "Wind changes the sea"
// scenario true by construction: raising the wind field's speed raises every band's amplitude and
// lowers the peak frequency, and nothing had to be re-authored for it to happen.
//
// **The bands it produces are `displacement.h`'s bands**, so the ocean is not a second surface
// model beside the displacement contract — it is a way of filling one in. Rendering and physics
// then differ by exactly the declared visual-only cascades, which is the contract's whole content.
//
// ================================================================================================
// A CASCADE THAT WOULD BE FELT IS PROMOTED, NOT SHIPPED
// ================================================================================================
//
// Short cascades are declared visual-only: capillary detail is what the contract's own example
// names. But amplitude is a function of wind, and at thirty metres a second the shortest cascade
// carries more than the declared felt threshold — at which point a visual-only band would be large
// enough to be felt by gameplay, which `water` calls a specification violation in as many words.
//
// So the builder PROMOTES such a cascade to authoritative and reports the promotion, rather than
// emitting a model that `validate_model()` would refuse. The alternative — clamping the amplitude —
// would make a storm look calm to keep a declaration true, which is the wrong of the two things to
// bend. `OceanReport::promoted_cascades` is how a developer finds out that physics is now paying
// for a band it usually does not.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/water/displacement.h>
#include <cy/world/coordinates.h>

namespace cy::water {

/// The global parameters of an ocean. `water`: "its parameters (sea level, wind, tide, weather)
/// remain global" — they are global because there is one sea, not because they are constants.
///
/// Wind arrives here from the wind FIELD (`environment-fields`, produced by `weather-and-wind`):
/// `WaterSystem::drive_ocean_from_wind()` samples it and fills these two members, which is the
/// specification's "Weather arrives later without rework" seam. A project with no weather sets
/// them directly and the sea works, which is its "Water works without weather" scenario.
struct OceanParams {
    /// Wind speed at the standard ten-metre reference height, m/s.
    f32 wind_speed_mps = 9.0F;
    f32 wind_direction_degrees = 0.0F;
    /// The distance over which the wind has blown, in kilometres. A sheltered bay with a two
    /// kilometre fetch under a gale is choppy and not swell-bearing, and this is the number that
    /// says so.
    f32 fetch_km = 200.0F;

    /// Still-water level, absolute metres, before the tide.
    f64 sea_level = 0.0;
    /// The tide's current offset from `sea_level`, in metres. Added to the model's mean level, so
    /// a rising tide moves the surface every consumer queries without touching the spectrum.
    f32 tide_metres = 0.0F;

    /// Gerstner steepness applied to every cascade, in [0, 1]. Sharpens crests and is what makes
    /// the breaking indicator non-zero at the top of a wave.
    f32 choppiness = 0.55F;

    /// How many cascades the spectrum is split into. Four is the shape the specification's own
    /// wording implies and what an ocean shader cascade set usually carries.
    u32 cascades = 4;
    /// The full wavelength range the cascades span, in metres.
    f32 shortest_wavelength = 0.4F;
    f32 longest_wavelength = 320.0F;
    /// A cascade whose GEOMETRIC-CENTRE wavelength is at or below this is declared visual-only.
    /// Two metres, because a two-metre wave is the shortest a hull of any size responds to. The
    /// centre and not the upper edge: a cascade spanning a decade would otherwise be visual only if
    /// its very longest wave were capillary, and none ever is.
    f32 visual_wavelength_metres = 2.0F;

    /// Trains synthesised per cascade, and how widely they scatter about the wind direction.
    u32 trains_per_cascade = 4;
    f32 spread_degrees = 28.0F;

    /// The amplitude at which a visual-only cascade is declared felt by gameplay. Carried into the
    /// model it builds; see `DisplacementModel::felt_threshold_metres`.
    f32 felt_threshold_metres = 0.05F;
};

/// What the spectrum came out as. The numbers a designer tunes against and the diagnostics
/// requirement's "displacement band amplitudes with the authoritative and visual split".
struct OceanReport {
    /// Significant wave height, four times the standard deviation of the surface — the number
    /// marine forecasts quote, so a sea state can be compared against one.
    f32 significant_height_metres = 0.0F;
    /// The wavelength and period at the spectrum's peak.
    f32 peak_wavelength_metres = 0.0F;
    f32 peak_period_seconds = 0.0F;
    u32 cascades = 0;
    /// Cascades that would have been visual-only but carry enough amplitude to be felt. See the
    /// header note.
    u32 promoted_cascades = 0;
    AmplitudeSplit split;
};

/// Build the cascaded model for a set of parameters. Refuses parameters that cannot describe a sea:
/// a non-positive wind speed, an empty or inverted wavelength range, no cascades, or more cascades
/// than `kMaxDisplacementBands` can carry.
[[nodiscard]] Expected<DisplacementModel, Error> build_ocean_model(const OceanParams& params,
                                                                   u64 seed) noexcept;

/// The same, with the spectrum's own summary. The model is identical; this is the reporting path.
[[nodiscard]] Expected<DisplacementModel, Error> build_ocean_model(const OceanParams& params,
                                                                   u64 seed,
                                                                   OceanReport& report) noexcept;

// --- The camera-relative surface ------------------------------------------------------------

/// How much surface is generated, and how finely. The vertex count is a function of THESE and of
/// nothing else — not of the ocean's bounds, not of where the camera is — which is what "no
/// world-scale ocean mesh SHALL be stored or streamed" means when it is measured rather than
/// asserted.
struct OceanSurfaceParams {
    /// Edge length of one quad in the innermost ring, metres.
    f32 near_cell_metres = 2.0F;
    /// Quads per side in every ring. Even, so a ring's inner half is exactly the previous ring.
    u32 ring_quads = 16;
    /// Rings, each with twice the cell size of the one inside it. Four rings at 2 m and 16 quads
    /// reach 128 m either way for 1 089 vertices.
    u32 rings = 4;
};

/// One generated patch of ocean surface: positions relative to a snapped origin near the camera,
/// their normals, their breaking indicator, and the triangle list over them.
///
/// **Nothing here is world-scale and nothing here is stored between frames except the buffers.**
/// `build()` overwrites them. `origin()` is the f64 world position the positions are relative to,
/// snapped to the coarsest ring's cell so that the lattice does not swim under a moving camera —
/// the artefact that makes a naive camera-relative ocean shimmer.
///
/// WHAT THIS DOES NOT DO: publish into the GPU scene. `water` requires the surface to be "produced
/// procedurally and published into the GPU scene, so it is culled and shaded like other geometry",
/// and publishing is `cy::rendering`'s call, made with a device this module deliberately cannot
/// name (see the module's CMakeLists). What is here is the geometry and its contract; the publish
/// is owned by whichever renderer-facing row binds it. That gap is recorded rather than hidden.
class OceanSurface {
public:
    explicit OceanSurface(Allocator& allocator) noexcept;

    OceanSurface(const OceanSurface&) = delete;
    OceanSurface& operator=(const OceanSurface&) = delete;

    /// Fix the shape of the patch and build its index list. Refuses an odd or zero quad count, no
    /// rings, or a non-positive cell size.
    [[nodiscard]] Status configure(const OceanSurfaceParams& params) noexcept;

    /// Regenerate the patch around a camera. `time` is the body's simulation time.
    ///
    /// The model is evaluated with `BandSelection::All`: this is the rendering path, and the
    /// difference from what a physics query would get is exactly the declared visual-only bands.
    [[nodiscard]] Status build(const DisplacementModel& model, const world::WorldVec3d& camera,
                               f64 time) noexcept;

    [[nodiscard]] Span<const Vec3> positions() const noexcept { return positions_.span(); }
    [[nodiscard]] Span<const Vec3> normals() const noexcept { return normals_.span(); }
    [[nodiscard]] Span<const f32> breaking() const noexcept { return breaking_.span(); }
    [[nodiscard]] Span<const u32> indices() const noexcept { return indices_.span(); }

    /// The world position `positions()` are relative to. Snapped; see the class note.
    [[nodiscard]] const world::WorldVec3d& origin() const noexcept { return origin_; }
    /// How far the generated surface reaches from its origin, in metres.
    [[nodiscard]] f32 extent_metres() const noexcept { return extent_; }
    [[nodiscard]] usize vertex_count() const noexcept { return positions_.size(); }
    [[nodiscard]] usize triangle_count() const noexcept { return indices_.size() / 3; }
    /// Bytes the patch occupies. The number a test compares between a camera at the origin and a
    /// camera a thousand kilometres out.
    [[nodiscard]] u64 bytes() const noexcept;

private:
    [[nodiscard]] Status build_indices() noexcept;

    Allocator* allocator_;
    OceanSurfaceParams params_;
    Array<Vec3> positions_;
    Array<Vec3> normals_;
    Array<f32> breaking_;
    Array<u32> indices_;
    world::WorldVec3d origin_;
    f32 extent_ = 0.0F;
    bool configured_ = false;
};

}  // namespace cy::water
