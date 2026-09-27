#pragma once
// THE PRECOMPUTED TABLES, AND THE TWO THINGS M7's SEED SAID IT DID NOT DO. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Precomputed atmospheric tables": "Sky evaluation SHALL use
// precomputed lookup tables — TRANSMITTANCE, MULTIPLE SCATTERING, SKY VIEW, and AERIAL PERSPECTIVE
// — rather than ray marching the atmosphere per pixel. Tables SHALL be regenerated only when the
// parameters they depend on change, and regeneration SHALL be INCREMENTAL where a parameter changes
// continuously, such as a moving sun. Table resolution SHALL be a quality setting, and their
// generation cost SHALL be reported."
//
// M7 built the sky view table and named the two halves it did not build, in `sky_light.h` and in
// the module's README. This file is where both are closed, and it is deliberately written so that
// the M7 code is the ORACLE rather than the thing replaced:
//
//   * MULTIPLE SCATTERING. `Atmosphere::multiple_scattering_factor` is an isotropic fudge; the
//     requirement's list names a table. `MultipleScatteringTable` is that table — the second-order
//     scattered radiance plus the geometric series that stands for orders three and up, which is
//     the standard decomposition and is the one place in this module where an infinite sum is
//     summed in closed form rather than truncated. `sky_radiance_tabulated()` is the sky that reads
//     it, and `tests/test_sky_composition.cpp` measures the consequence M7's README predicted: a
//     sunset horizon that was too dark, and a shadowed slope that was too blue.
//
//   * INCREMENTAL REGENERATION. `SkyViewTable::update()` rebuilds in full past
//     `kSunMovementThreshold`. `IncrementalSkyView` holds the SAME parameterisation — copied
//     deliberately, so that the two tables are comparable row for row — and re-integrates a BOUNDED
//     number of rows per update, chosen by how stale each row is. The full table is then the oracle
//     the incremental one is measured against over a simulated day, which is the only way to state
//     "incremental" as a number rather than as a design intention.
//
// ================================================================================================
// WHY A ROW BUDGET AND NOT A RADIUS AROUND THE SUN
// ================================================================================================
//
// The obvious incremental scheme re-integrates the rows near the sun, because that is where the sky
// changes fastest. It is also the scheme that starves: a row far from the sun is never the most
// urgent row, so it holds a value integrated at dawn until something forces a full rebuild, and the
// error is invisible in a test that watches one sunrise.
//
// So staleness here ACCUMULATES. Each row remembers the sun it was last integrated with, and its
// urgency is the angle between that sun and the current one, weighted by the row's own sensitivity.
// A row nobody has chosen keeps getting more urgent until it is chosen, so the worst error over a
// day is bounded by the budget rather than by where the sun happens to be — and that bound is what
// `tests/test_sky_composition.cpp` measures.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/effects.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/sky_light.h>

namespace cy::rendering::sky {

// ================================================================================================
// TRANSMITTANCE
// ================================================================================================

/// How much of the light along a path survives it, as a function of where you are and which way you
/// look. The first table every other one is built on: a sky evaluation that ray marched
/// transmittance inside its own ray march would be quadratic in step count.
///
/// Parameterised by ALTITUDE and the COSINE OF THE ANGLE TO THE ZENITH, which is the whole of what
/// transmittance to the top of the atmosphere depends on in a spherically symmetric model. The
/// altitude axis is square-rooted because the density it stands for is exponential and a linear
/// axis spends most of its rows on air that is not there.
class TransmittanceTable {
public:
    explicit TransmittanceTable(Allocator& allocator = current_allocator()) noexcept
        : values_(allocator) {}

    [[nodiscard]] Status configure(SkyTableQuality quality) noexcept;

    /// Build for one atmosphere. Returns whether it rebuilt: an atmosphere that has not changed is
    /// a table that is not regenerated, which is the requirement's second sentence.
    [[nodiscard]] Expected<bool, Error> build(const Atmosphere& atmosphere) noexcept;

    /// The survival fraction from `altitude_metres` looking along a direction whose cosine with the
    /// local zenith is `cos_zenith`. Bilinear; the identity `T(0, 1)` is the zenith path from the
    /// ground.
    [[nodiscard]] Vec3 sample(f32 altitude_metres, f32 cos_zenith) const noexcept;

    [[nodiscard]] const SkyTableStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] SkyTableQuality quality() const noexcept { return quality_; }
    [[nodiscard]] u32 width() const noexcept;
    [[nodiscard]] u32 height() const noexcept;

private:
    Array<Vec3> values_;
    Atmosphere last_atmosphere_;
    SkyTableStats stats_;
    SkyTableQuality quality_ = SkyTableQuality::Medium;
    bool built_ = false;
};

// ================================================================================================
// MULTIPLE SCATTERING
// ================================================================================================

/// The light that reached a point after bouncing more than once, tabulated the way the requirement
/// names rather than approximated by a constant.
///
/// WHAT IS STORED. For an altitude and a sun elevation, two quantities are integrated over the
/// sphere of directions: `L2`, the radiance that arrives having scattered exactly twice, and `fms`,
/// the fraction of light that leaves a point and comes back to it. Orders three and up are the same
/// transfer applied again, so their sum is the geometric series `L2 * fms / (1 - fms)` and the
/// stored value is `L2 / (1 - fms)` — the whole infinite sum, in closed form, which is why this
/// costs one table rather than one table per order.
///
/// WHY IT IS ISOTROPIC AND WHY THAT IS NOT THE SAME COMPROMISE M7 MADE. Light that has scattered
/// twice has very nearly forgotten which way it came from, so storing it without a direction is a
/// physical statement rather than a saving. `Atmosphere::multiple_scattering_factor` was a
/// CONSTANT FRACTION OF THE SINGLE-SCATTERED RESULT, which is a different thing: it scales with the
/// single scattering at the point being shaded, so it vanishes exactly where multiple scattering
/// matters most — a shadowed slope and a horizon at sunset, where the single-scattered term is
/// small and the multiply-scattered one is most of the light.
class MultipleScatteringTable {
public:
    explicit MultipleScatteringTable(Allocator& allocator = current_allocator()) noexcept
        : values_(allocator) {}

    [[nodiscard]] Status configure(SkyTableQuality quality) noexcept;

    /// Build against an already-built transmittance table. Refused if that table is not built: a
    /// multiple-scattering table derived from an unbuilt transmittance is zero everywhere, and zero
    /// is a plausible-looking answer.
    [[nodiscard]] Expected<bool, Error> build(const Atmosphere& atmosphere,
                                              const TransmittanceTable& transmittance) noexcept;

    /// The multiply-scattered radiance per unit of stellar illuminance, at an altitude, for a sun
    /// whose cosine with the local zenith is `cos_sun`.
    [[nodiscard]] Vec3 sample(f32 altitude_metres, f32 cos_sun) const noexcept;

    [[nodiscard]] const SkyTableStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] u32 resolution() const noexcept;

private:
    Array<Vec3> values_;
    Atmosphere last_atmosphere_;
    SkyTableStats stats_;
    SkyTableQuality quality_ = SkyTableQuality::Medium;
    bool built_ = false;
};

/// The two tables a sky evaluation needs, held together because nothing uses one without the other
/// and because building them in the wrong order is the one mistake this pair makes possible.
struct AtmosphereTables {
    explicit AtmosphereTables(Allocator& allocator = current_allocator()) noexcept
        : transmittance(allocator), multiple_scattering(allocator) {}

    TransmittanceTable transmittance;
    MultipleScatteringTable multiple_scattering;

    [[nodiscard]] Status configure(SkyTableQuality quality) noexcept;
    /// Build both, in the only order that works, and report whether anything was rebuilt.
    [[nodiscard]] Expected<bool, Error> build(const Atmosphere& atmosphere) noexcept;
    [[nodiscard]] bool built() const noexcept {
        return transmittance.built() && multiple_scattering.built();
    }
    /// Directions integrated across both tables. The generation cost the requirement asks to be
    /// reported, in a unit that does not depend on how fast the machine is.
    [[nodiscard]] u64 directions_integrated() const noexcept {
        return transmittance.stats().directions_integrated +
               multiple_scattering.stats().directions_integrated;
    }
};

/// The sky, evaluated through the tables: single scattering ray marched with TABULATED
/// transmittance, plus the TABULATED multiple scattering, plus the ground.
///
/// The counterpart of `sky_radiance()` and deliberately the same signature with the tables added,
/// so that a caller switching between them changes one argument and a test can difference the two.
[[nodiscard]] Vec3 sky_radiance_tabulated(const Atmosphere& atmosphere,
                                          const AtmosphereTables& tables, Vec3 view_position,
                                          Vec3 view_direction, Vec3 sun_direction,
                                          u32 steps = 32) noexcept;

/// The sky, integrated THE WAY THE AERIAL PERSPECTIVE TABLE INTEGRATES THE AIR: the same source
/// terms from the same tables, each step taken exactly for a source and an extinction that are
/// constant across it, on steps that grow as the square of the distance out.
///
/// WHY A SECOND QUADRATURE OF THE SAME INTEGRAL. `sky_radiance_tabulated()` takes uniform steps
/// and weights each by the transmittance at its far end. Over a horizon path of eight hundred
/// kilometres in sixteen steps, a step near the ground is optically thick in blue, and that
/// weighting drops most of the light the first fifty kilometres scatter: measured against this
/// function, its horizon is a third too dark in blue. `AerialPerspectiveTable` resolves the same
/// air with slices a few hundred metres thick near the eye, so the two disagreed at the horizon by
/// exactly that bias — distant terrain fading into a sky bluer than the one drawn behind it.
/// A frame that draws its sky beside aerial perspective draws it with THIS, and the two are then
/// one model evaluated with one step rule. `sky_radiance_tabulated()` is left as it is because the
/// lighting integral and every existing capture are built on it.
[[nodiscard]] Vec3 sky_radiance_aerial(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                                       Vec3 view_position, Vec3 view_direction, Vec3 sun_direction,
                                       u32 steps = 32) noexcept;

/// The sunlight reaching a point, read from the transmittance table instead of ray marched. What a
/// renderer calls once a frame for the directional light's colour.
[[nodiscard]] Vec3 sun_illuminance_tabulated(const Atmosphere& atmosphere,
                                             const AtmosphereTables& tables, Vec3 view_position,
                                             Vec3 sun_direction) noexcept;

/// The atmosphere's transmittance along the segment between two planet-centred points, from the
/// table alone.
///
/// The table stores transmittance to the TOP of the atmosphere, and `T(a->b) = T(a->top) /
/// T(b->top)` because both paths share their tail. One table therefore serves every segment, which
/// is what lets a cloud ray march attenuate each of its steps without marching the atmosphere as
/// well — the alternative is quadratic in step count and is the reason the requirement asks for
/// tables in the first place.
[[nodiscard]] Vec3 segment_transmittance_tabulated(const Atmosphere& atmosphere,
                                                   const AtmosphereTables& tables, Vec3 from,
                                                   Vec3 to) noexcept;

// ================================================================================================
// THE INCREMENTAL SKY VIEW
// ================================================================================================

/// What one `update()` did. Returned rather than logged, because "regeneration SHALL be
/// incremental" is a claim about these numbers and a caller that cannot see them cannot hold the
/// table to it.
struct SkyViewUpdate {
    /// Rows re-integrated by this call.
    u32 rows_rebuilt = 0;
    /// Directions integrated by this call. Zero when nothing had moved enough.
    u64 directions_integrated = 0;
    /// True when the whole table was rebuilt — the first build, a resolution change, or an
    /// atmosphere that changed. A moving sun must never produce one of these, and that is a test.
    bool full_rebuild = false;
};

/// Per-table statistics, extending `SkyTableStats` with the two counters an incremental table has
/// that a full-rebuild one does not.
struct IncrementalSkyStats {
    u32 full_rebuilds = 0;
    u32 incremental_updates = 0;
    u32 reuses = 0;
    u64 rows_rebuilt = 0;
    u64 directions_integrated = 0;
    /// The largest per-row staleness, in radians of sun movement, observed at the moment a row was
    /// chosen. The bound the budget buys, reported rather than assumed.
    f32 worst_row_staleness = 0.0F;
};

/// The sky view table, regenerated incrementally as the sun moves.
///
/// The parameterisation is `SkyViewTable`'s, character for character: a non-linear latitude whose
/// row index goes as the square of the angle from the horizon, and a wrapping azimuth. It is copied
/// rather than shared because the two tables must be comparable ENTRY BY ENTRY for the oracle test
/// to mean anything, and a shared helper that one of them stopped calling would make the comparison
/// silently vacuous.
class IncrementalSkyView {
public:
    explicit IncrementalSkyView(Allocator& allocator = current_allocator()) noexcept
        : radiance_(allocator), row_sun_(allocator) {}

    [[nodiscard]] Status configure(SkyTableQuality quality) noexcept;

    /// Bring the table up to date, spending at most `row_budget` rows. A budget of zero means "as
    /// many as it takes", which is the full rebuild and is what a cook or a screenshot wants.
    ///
    /// The sun is compared by ANGLE for the reason `SkyViewTable::update()` gives: a sun driven
    /// from a clock is never twice the same float.
    [[nodiscard]] Expected<SkyViewUpdate, Error> update(const Atmosphere& atmosphere,
                                                        Vec3 view_position, Vec3 sun_direction,
                                                        u32 row_budget) noexcept;

    /// The same, reading multiple scattering from the tables rather than from the atmosphere's
    /// isotropic factor.
    [[nodiscard]] Expected<SkyViewUpdate, Error> update_tabulated(const Atmosphere& atmosphere,
                                                                  const AtmosphereTables& tables,
                                                                  Vec3 view_position,
                                                                  Vec3 sun_direction,
                                                                  u32 row_budget) noexcept;

    /// The same tables, integrated by `sky_radiance_aerial()`: the clear sky a frame draws beside
    /// `AerialPerspectiveTable`. A full rebuild's rows are spread over `jobs` when one is given,
    /// each row written to its own slots, so the table is the serial one bit for bit.
    [[nodiscard]] Expected<SkyViewUpdate, Error> update_aerial(
        const Atmosphere& atmosphere, const AtmosphereTables& tables, Vec3 view_position,
        Vec3 sun_direction, u32 row_budget, jobs::JobSystem* jobs = nullptr) noexcept;

    [[nodiscard]] Vec3 sample(Vec3 direction) const noexcept;

    [[nodiscard]] const IncrementalSkyStats& stats() const noexcept { return stats_; }
    [[nodiscard]] SkyTableQuality quality() const noexcept { return quality_; }
    [[nodiscard]] bool built() const noexcept { return built_; }
    /// Rows in the table. A budget is compared against this to say what fraction of a rebuild an
    /// update cost.
    [[nodiscard]] u32 rows() const noexcept;

private:
    struct UpdatePlan {
        bool full = false;
        bool skip = false;
    };

    /// Which integral a row is filled with. A table is rebuilt in full when it changes, because a
    /// table half of one and half of another answers neither.
    enum class Integrand : u8 { Marched, Tabulated, Aerial };

    [[nodiscard]] UpdatePlan plan_update(const Atmosphere& atmosphere, Vec3 view_position, Vec3 sun,
                                         Integrand integrand) noexcept;
    void integrate_row(const Atmosphere& atmosphere, const AtmosphereTables* tables,
                       Integrand integrand, Vec3 view_position, Vec3 sun, u32 row) noexcept;
    [[nodiscard]] Expected<SkyViewUpdate, Error> update_common(
        const Atmosphere& atmosphere, const AtmosphereTables* tables, Integrand integrand,
        Vec3 view_position, Vec3 sun_in, u32 row_budget, jobs::JobSystem* jobs) noexcept;

    Array<Vec3> radiance_;
    /// The sun each row was last integrated with. This is the whole of the incremental mechanism:
    /// staleness accumulates per row, so a row nobody chooses gets more urgent rather than being
    /// forgotten.
    Array<Vec3> row_sun_;
    Vec3 last_sun_{0.0F, 1.0F, 0.0F};
    Vec3 last_position_{0.0F, 0.0F, 0.0F};
    Atmosphere last_atmosphere_;
    IncrementalSkyStats stats_;
    SkyTableQuality quality_ = SkyTableQuality::Medium;
    Integrand last_integrand_ = Integrand::Marched;
    bool built_ = false;
};

// ================================================================================================
// AERIAL PERSPECTIVE, IN THE ENGINE'S OWN FROXEL VOLUME
// ================================================================================================
//
// "Clouds, fog, mist, smoke, and atmospheric volumes SHALL share the engine's VOLUMETRIC
// INFRASTRUCTURE rather than each building its own pipeline: the froxel volume defined in
// `rendering-post-processing` for local media, with clouds using a specialised long-range
// representation because their scale differs by orders of magnitude."
//
// So the aerial perspective table is a `rendering::FroxelVolume` — the same struct, the same
// exponential slice distribution, `froxel_slice_depth()` and `froxel_slice_of()` called rather than
// re-derived. A second slice distribution here would put the atmosphere's attenuation and the local
// fog's density on two different depth axes, and the seam between them would move with the camera.

/// The aerial perspective volume: what the atmosphere does to a surface, tabulated over the view
/// frustum, so that shading a pixel is a lookup rather than a march.
///
/// `rendering-post-processing` owns the froxel geometry; this owns what is stored in it.
class AerialPerspectiveTable {
public:
    explicit AerialPerspectiveTable(Allocator& allocator = current_allocator()) noexcept
        : transmittance_(allocator), in_scattering_(allocator) {}

    /// The camera the volume is built for. Camera-relative by construction: every ray starts at the
    /// origin of this basis and `view_position` is only used to find the ALTITUDE, which is the one
    /// thing the atmosphere needs from an absolute position. See `planetary_scale()` in
    /// `composition.h` for why that split is the whole of the precision argument.
    struct View {
        Vec3 forward{0.0F, 0.0F, -1.0F};
        Vec3 right{1.0F, 0.0F, 0.0F};
        Vec3 up{0.0F, 1.0F, 0.0F};
        /// A 45-degree vertical field of view on a 16:9 frame: tan(22.5 degrees) and that times
        /// 16/9. Written as the tangents rather than as an angle because that is what a projection
        /// uses, and stated here because they are the only camera parameters this module has.
        f32 tan_half_fov_x = 0.7364F;
        f32 tan_half_fov_y = 0.4142F;
    };

    [[nodiscard]] Status configure(const FroxelVolume& volume) noexcept;

    /// Rebuild the volume for this camera and sun.
    ///
    /// With `jobs`, the froxel columns are integrated on its workers. Every column is a pure
    /// function of its own ray written to its own slots, so the table is bit-identical to the
    /// serial one; a job system that will not take the work leaves it on this thread.
    [[nodiscard]] Status update(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                                Vec3 view_position, const View& view, Vec3 sun_direction,
                                jobs::JobSystem* jobs = nullptr) noexcept;

    /// What survives and what is added, for a pixel at `uv` in [0,1]^2 and a surface at
    /// `view_depth` metres along the forward axis.
    [[nodiscard]] AerialPerspective sample(Vec2 uv, f32 view_depth) const noexcept;

    /// What survives and what is added for a surface at `offset` metres from the camera, in the
    /// world axes the table's `View` was given — THE DEVICE'S SAMPLER, on the processor.
    ///
    /// `cy/aerial_perspective.slang` performs this arithmetic on `pack_aerial_perspective()`'s
    /// words, and a test compares the two. It differs from `sample()` in DEPTH: `sample()` returns
    /// the froxel whose far edge contains the surface, which is right for a volume that is
    /// integrated front to back and wrong for a surface, because a surface a metre from the eye
    /// would receive the first slice's full attenuation. This interpolates linearly in depth
    /// between the slices' far edges, from an implicit slice at the eye where nothing has been
    /// attenuated yet, so a near surface is left almost exactly as it was lit.
    [[nodiscard]] AerialPerspective sample_at(Vec3 offset) const noexcept;

    [[nodiscard]] const FroxelVolume& volume() const noexcept { return volume_; }
    /// The camera the volume was last built for.
    [[nodiscard]] const View& view() const noexcept { return view_; }
    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] u64 froxels() const noexcept;
    /// The stored values, slice-major then row-major, as `pack_aerial_perspective()` lays them out.
    [[nodiscard]] Span<const Vec3> transmittances() const noexcept { return transmittance_.span(); }
    [[nodiscard]] Span<const Vec3> in_scatterings() const noexcept { return in_scattering_.span(); }

private:
    void integrate_column(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                          Vec3 view_position, Vec3 sun, u32 x, u32 y) noexcept;

    Array<Vec3> transmittance_;
    Array<Vec3> in_scattering_;
    FroxelVolume volume_;
    View view_;
    bool built_ = false;
};

// ================================================================================================
// AERIAL PERSPECTIVE ON A DEVICE
// ================================================================================================
//
// The froxel table leaves the processor as one array of float4 words, read by
// `src/rendering/shaders/cy/aerial_perspective.slang`. The layout is defined HERE, once, and the
// shader module restates it; `AerialPerspectiveTable::sample_at()` is the same sampler on the
// processor, so a frame's shader and this module cannot disagree about what a word means without a
// test noticing.
//
//   word 0   forward.xyz, enabled (1 or 0)
//   word 1   right.xyz,   tan of half the horizontal field of view
//   word 2   up.xyz,      tan of half the vertical field of view
//   word 3   width, height, depth (as floats), the slice distribution's exponent
//   word 4   near plane, far plane, radiance scale, 0
//   then     two words per froxel, slice-major then row-major: transmittance.rgb, 0 and
//            in-scattering.rgb multiplied by the radiance scale, 0
//
// THE RADIANCE SCALE is the frame's own: in-scattering is stored in nits, and a frame that divides
// its sky by an exposure before drawing it must divide the light the air adds to a surface by the
// same number, or distant terrain fades toward a sky that is not the one drawn above it.

/// Float4 words before the first froxel.
inline constexpr u32 kAerialPerspectiveHeaderWords = 5;

/// The words a device sampler reads for `table`, with in-scattering multiplied by
/// `radiance_scale`. An unbuilt table packs the header alone, with `enabled` zero — which is also
/// what a frame with aerial perspective off binds, so the device leaves every surface unchanged.
[[nodiscard]] Status pack_aerial_perspective(const AerialPerspectiveTable& table,
                                             f32 radiance_scale, Array<Vec4>& out) noexcept;

}  // namespace cy::rendering::sky
