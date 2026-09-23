// The spectrum, the cascades it fills, and the patch generated around the camera. M10 task 2.3.

#include <cy/water/ocean.h>

#include <cy/core/jobs/parallel.h>

#include <cmath>
#include <numbers>

namespace cy::water {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

/// Angular frequency of a deep-water wave of this wavelength. The dispersion relation
/// `displacement.h` synthesises with, spelled once here so the spectrum and the surface agree about
/// which frequency a wavelength is.
[[nodiscard]] f32 omega_of_wavelength(f32 wavelength) noexcept {
    const f32 k = (2.0F * kPi) / wavelength;
    return std::sqrt(kGravity * k);
}

/// The Pierson-Moskowitz peak, fetch-limited in the JONSWAP manner.
///
/// A fully developed sea peaks at omega_p = 0.855 g / U. A fetch-limited one peaks HIGHER — shorter
/// waves — and the JONSWAP fit gives f_p = 3.5 (g/U) (gX/U^2)^-0.33. The larger of the two is the
/// answer: a short fetch cannot produce swell longer than the fully developed sea, and a long fetch
/// cannot make it longer than fully developed either.
[[nodiscard]] f32 peak_omega(f32 wind_speed, f32 fetch_metres) noexcept {
    const f32 developed = 0.855F * kGravity / wind_speed;
    const f32 chi = (kGravity * fetch_metres) / (wind_speed * wind_speed);
    const f32 peak_hz = 3.5F * (kGravity / wind_speed) * std::pow(chi, -0.33F);
    const f32 limited = 2.0F * kPi * peak_hz;
    return (limited > developed) ? limited : developed;
}

/// The Pierson-Moskowitz variance density at one angular frequency, m^2 s.
[[nodiscard]] f32 spectrum_density(f32 omega, f32 omega_peak, f32 alpha) noexcept {
    if (omega <= 0.0F) {
        return 0.0F;
    }
    const f32 ratio = omega_peak / omega;
    const f32 ratio4 = ratio * ratio * ratio * ratio;
    const f32 omega5 = omega * omega * omega * omega * omega;
    return (alpha * kGravity * kGravity / omega5) * std::exp(-1.25F * ratio4);
}

/// Integrate the spectrum over [omega_low, omega_high] by the midpoint rule over `steps` slices.
///
/// Numerically and not in closed form because the closed form of PM's integral is an incomplete
/// gamma function, and a sixteen-slice midpoint rule over one cascade is within a fraction of a
/// percent of it — well inside the accuracy of the spectrum itself, which is a fit to a North
/// Atlantic data set and not a law.
[[nodiscard]] f32 band_variance(f32 omega_low, f32 omega_high, f32 omega_peak, f32 alpha) noexcept {
    constexpr u32 kSteps = 16;
    if (omega_high <= omega_low) {
        return 0.0F;
    }
    const f32 step = (omega_high - omega_low) / static_cast<f32>(kSteps);
    f32 total = 0.0F;
    for (u32 index = 0; index < kSteps; ++index) {
        const f32 omega = omega_low + (step * (static_cast<f32>(index) + 0.5F));
        total += spectrum_density(omega, omega_peak, alpha) * step;
    }
    return total;
}

[[nodiscard]] bool params_are_sane(const OceanParams& params) noexcept {
    return params.wind_speed_mps > 0.0F && params.fetch_km > 0.0F && params.cascades >= 1 &&
           params.cascades <= kMaxDisplacementBands && params.shortest_wavelength > 0.0F &&
           params.longest_wavelength > params.shortest_wavelength &&
           params.trains_per_cascade >= 1 && params.choppiness >= 0.0F && params.choppiness <= 1.0F;
}

}  // namespace

Expected<DisplacementModel, Error> build_ocean_model(const OceanParams& params, u64 seed,
                                                     OceanReport& report) noexcept {
    if (!params_are_sane(params)) {
        return fail(ErrorCode::InvalidArgument,
                    "water: an ocean needs a positive wind speed and fetch, between one and "
                    "kMaxDisplacementBands cascades, an increasing wavelength range, at least one "
                    "train per cascade and a choppiness in [0, 1]");
    }

    const f32 fetch_metres = params.fetch_km * 1000.0F;
    const f32 omega_peak = peak_omega(params.wind_speed_mps, fetch_metres);
    // Fetch-dependent Phillips constant, JONSWAP's alpha = 0.076 chi^-0.22. A short fetch is
    // steeper for its height, which is why a bay chops rather than swells.
    const f32 chi = (kGravity * fetch_metres) / (params.wind_speed_mps * params.wind_speed_mps);
    const f32 alpha = 0.076F * std::pow(chi, -0.22F);

    DisplacementModel model;
    model.seed = seed;
    model.mean_level = params.sea_level + static_cast<f64>(params.tide_metres);
    model.felt_threshold_metres = params.felt_threshold_metres;

    const f32 ratio = std::pow(params.longest_wavelength / params.shortest_wavelength,
                               1.0F / static_cast<f32>(params.cascades));
    f32 total_variance = 0.0F;
    report = OceanReport{};

    for (u32 index = 0; index < params.cascades; ++index) {
        DisplacementBand band;
        band.wavelength_min = params.shortest_wavelength * std::pow(ratio, static_cast<f32>(index));
        band.wavelength_max = band.wavelength_min * ratio;
        band.direction_degrees = params.wind_direction_degrees;
        band.spread_degrees = params.spread_degrees;
        band.steepness = params.choppiness;
        band.wave_count = params.trains_per_cascade;

        // A long wavelength is a LOW frequency, so the band's frequency range is the reverse of its
        // wavelength range. Getting this backwards integrates the spectrum over the wrong slice and
        // produces a sea whose energy is in the ripples.
        const f32 omega_high = omega_of_wavelength(band.wavelength_min);
        const f32 omega_low = omega_of_wavelength(band.wavelength_max);
        const f32 variance = band_variance(omega_low, omega_high, omega_peak, alpha);
        total_variance += variance;
        // A narrow-band Gaussian surface of variance sigma^2 has the same energy as a sinusoid of
        // amplitude sqrt(2) sigma, and that amplitude is what the trains carry.
        band.amplitude = std::sqrt(2.0F * variance);

        // A cascade is visual when the waves it CARRIES are mostly shorter than the threshold, and
        // the wavelength that stands for a geometrically-spaced band is its geometric centre — not
        // its upper edge, which would make a cascade spanning a decade visual only if its very
        // longest wave was capillary, and no cascade ever is.
        const f32 centre_wavelength = std::sqrt(band.wavelength_min * band.wavelength_max);
        band.authority = (centre_wavelength <= params.visual_wavelength_metres)
                             ? BandAuthority::Visual
                             : BandAuthority::Authoritative;
        if (band.authority == BandAuthority::Visual &&
            band.amplitude > params.felt_threshold_metres) {
            // See the header note: promoted rather than clamped, and reported.
            band.authority = BandAuthority::Authoritative;
            ++report.promoted_cascades;
        }
        if (!model.add(band)) {
            return fail(ErrorCode::OutOfRange,
                        "water: an ocean declared more cascades than a displacement model carries");
        }
    }

    report.cascades = model.band_count;
    report.significant_height_metres = 4.0F * std::sqrt(total_variance);
    report.peak_period_seconds = (2.0F * kPi) / omega_peak;
    // The deep-water wavelength of the peak period: lambda = g T^2 / 2pi.
    report.peak_wavelength_metres =
        (kGravity * report.peak_period_seconds * report.peak_period_seconds) / (2.0F * kPi);
    report.split = amplitude_split(model);
    return model;
}

Expected<DisplacementModel, Error> build_ocean_model(const OceanParams& params, u64 seed) noexcept {
    OceanReport discarded;
    return build_ocean_model(params, seed, discarded);
}

// --- The camera-relative surface ------------------------------------------------------------

OceanSurface::OceanSurface(Allocator& allocator) noexcept
    : positions_(allocator),
      normals_(allocator),
      breaking_(allocator),
      indices_(allocator),
      trains_(allocator) {}

Status OceanSurface::configure(const OceanSurfaceParams& params) noexcept {
    if (params.near_cell_metres <= 0.0F || params.rings == 0 || params.ring_quads < 2 ||
        (params.ring_quads % 2) != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "water: an ocean patch needs a positive cell size, at least one ring and an "
                    "even, non-zero quad count per ring");
    }
    params_ = params;
    configured_ = true;
    // The coarsest ring's half-extent is how far the patch reaches.
    const f32 coarsest_cell = params.near_cell_metres * static_cast<f32>(1U << (params.rings - 1U));
    extent_ = coarsest_cell * static_cast<f32>(params.ring_quads) * 0.5F;
    return build_indices();
}

Status OceanSurface::build_indices() noexcept {
    indices_.clear();
    const u32 side = params_.ring_quads + 1U;
    const u32 per_ring = side * side;
    const u32 quarter = params_.ring_quads / 4U;

    for (u32 ring = 0; ring < params_.rings; ++ring) {
        const u32 base = ring * per_ring;
        for (u32 z = 0; z < params_.ring_quads; ++z) {
            for (u32 x = 0; x < params_.ring_quads; ++x) {
                // A ring other than the innermost leaves its middle quarter to the ring inside it,
                // whose cells are half the size and cover exactly that square. Without the hole the
                // patch would draw the same water twice, at two resolutions, and z-fight.
                if (ring > 0 && x >= quarter && x < (params_.ring_quads - quarter) &&
                    z >= quarter && z < (params_.ring_quads - quarter)) {
                    continue;
                }
                const u32 a = base + (z * side) + x;
                const u32 b = a + 1U;
                const u32 c = a + side;
                const u32 d = c + 1U;
                const u32 triangle[6] = {a, c, b, b, c, d};
                for (const u32 vertex : triangle) {
                    if (Status pushed = indices_.push_back(vertex); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    return ok();
}

namespace {

/// Everything one row of the patch needs, shared read-only by every partition of a parallel build.
/// The three outputs are written at disjoint indices, one row per call, so partitions never touch
/// the same element and the result does not depend on which worker ran which row.
struct PatchRows {
    const OceanSurfaceParams* params = nullptr;
    Span<const WaveTrain> trains;
    f64 mean_level = 0.0;
    world::WorldVec3d origin;
    f64 time = 0.0;
    Vec3* positions = nullptr;
    Vec3* normals = nullptr;
    f32* breaking = nullptr;
};

[[nodiscard]] f32 ring_cell(const OceanSurfaceParams& params, u32 ring) noexcept {
    return params.near_cell_metres * static_cast<f32>(1U << ring);
}

/// Local coordinate of lattice line `index` of `ring`. The one expression every vertex uses.
[[nodiscard]] f32 ring_local(const OceanSurfaceParams& params, u32 ring, u32 index) noexcept {
    const f32 cell = ring_cell(params, ring);
    const f32 half = cell * static_cast<f32>(params.ring_quads) * 0.5F;
    return (static_cast<f32>(index) * cell) - half;
}

/// THE RINGS OVERLAP, AND WHERE THEY DO THE VERTICES ARE THE SAME POINT. Ring `r`'s cells are
/// twice ring `r - 1`'s and its half-extent is twice as far, so its line `i` lies on ring
/// `r - 1`'s line `2i - quads/2` whenever that line exists — the middle half of every ring beyond
/// the first, which is the hole `build_indices()` leaves for the ring inside it. Where the two f32
/// local coordinates are BIT-EQUAL the evaluation is the same call with the same arguments, so the
/// vertex is copied rather than evaluated a second time. Where rounding makes them differ (a cell
/// size that is not exact in binary) nothing is shared and the vertex is evaluated as before.
[[nodiscard]] bool line_is_inner(const OceanSurfaceParams& params, u32 ring, u32 index,
                                 u32& inner_index) noexcept {
    if (ring == 0) {
        return false;
    }
    const u32 twice = 2U * index;
    const u32 offset = params.ring_quads / 2U;
    if (twice < offset || twice - offset > params.ring_quads) {
        return false;
    }
    inner_index = twice - offset;
    return ring_local(params, ring, index) == ring_local(params, ring - 1U, inner_index);
}

void evaluate_rows(const PatchRows& rows, u64 begin, u64 end) noexcept {
    const OceanSurfaceParams& params = *rows.params;
    const u32 side = params.ring_quads + 1U;
    for (u64 row = begin; row < end; ++row) {
        const auto ring = static_cast<u32>(row / side);
        const auto z = static_cast<u32>(row % side);
        u32 unused = 0;
        const bool inner_row = line_is_inner(params, ring, z, unused);
        const f32 local_z = ring_local(params, ring, z);
        for (u32 x = 0; x < side; ++x) {
            if (inner_row && line_is_inner(params, ring, x, unused)) {
                continue;  // Copied from the ring inside, after every ring is evaluated.
            }
            const f32 local_x = ring_local(params, ring, x);
            const f64 world_x = rows.origin.x + static_cast<f64>(local_x);
            const f64 world_z = rows.origin.z + static_cast<f64>(local_z);
            // THE RENDERING PATH: every band, including the declared visual-only ones. What a
            // physics query gets differs from this by exactly those bands.
            const Displacement displacement =
                evaluate_trains(rows.trains, rows.mean_level, world_x, world_z, rows.time);
            const usize index = (static_cast<usize>(row) * side) + x;
            rows.positions[index] = Vec3{local_x + displacement.offset.x,
                                         static_cast<f32>(displacement.height - rows.origin.y),
                                         local_z + displacement.offset.z};
            rows.normals[index] = displacement.normal;
            rows.breaking[index] = displacement.breaking;
        }
    }
}

/// Fill every shared vertex from the ring inside it, innermost ring first so that a vertex shared
/// by three rings is copied from one that already holds its value.
void copy_inner_vertices(const PatchRows& rows) noexcept {
    const OceanSurfaceParams& params = *rows.params;
    const u32 side = params.ring_quads + 1U;
    const usize per_ring = static_cast<usize>(side) * side;
    for (u32 ring = 1; ring < params.rings; ++ring) {
        for (u32 z = 0; z < side; ++z) {
            u32 inner_z = 0;
            if (!line_is_inner(params, ring, z, inner_z)) {
                continue;
            }
            for (u32 x = 0; x < side; ++x) {
                u32 inner_x = 0;
                if (!line_is_inner(params, ring, x, inner_x)) {
                    continue;
                }
                const usize to = (ring * per_ring) + (static_cast<usize>(z) * side) + x;
                const usize from =
                    ((ring - 1U) * per_ring) + (static_cast<usize>(inner_z) * side) + inner_x;
                rows.positions[to] = rows.positions[from];
                rows.normals[to] = rows.normals[from];
                rows.breaking[to] = rows.breaking[from];
            }
        }
    }
}

}  // namespace

Status OceanSurface::build(const DisplacementModel& model, const world::WorldVec3d& camera,
                           f64 time) noexcept {
    return build(model, camera, time, nullptr);
}

Status OceanSurface::build(const DisplacementModel& model, const world::WorldVec3d& camera,
                           f64 time, jobs::JobSystem* jobs) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable,
                    "water: an ocean patch must be configured before it is built");
    }
    const u32 side = params_.ring_quads + 1U;
    const u32 total = side * side * params_.rings;
    if (Status sized = positions_.resize(total); !sized) {
        return sized;
    }
    if (Status sized = normals_.resize(total); !sized) {
        return sized;
    }
    if (Status sized = breaking_.resize(total); !sized) {
        return sized;
    }
    // THE TRAINS ARE RESOLVED ONCE PER BUILD, not once per vertex. See `WaveTrain`: resolving one
    // is two hashed-stream draws, a `pow` and a `cos`/`sin` pair, and doing it for every train at
    // every one of the patch's vertices was most of the ocean's frame cost. The table is the same
    // trains in the same order, so every vertex is bit-identical to `evaluate_displacement()`.
    if (Status sized = trains_.resize(count_trains(model, BandSelection::All)); !sized) {
        return sized;
    }
    const Span<const WaveTrain> trains(trains_.data(),
                                       resolve_trains(model, BandSelection::All, trains_.span()));

    // Snap the origin to the COARSEST ring's cell. A lattice snapped per ring would shear where two
    // rings meet; snapped to the coarsest, every ring's vertices land on their own multiples and
    // the patch slides under the camera in whole cells rather than swimming.
    const f32 coarsest_cell = ring_cell(params_, params_.rings - 1U);
    const auto snap = [coarsest_cell](f64 value) noexcept {
        return std::floor(value / static_cast<f64>(coarsest_cell)) *
               static_cast<f64>(coarsest_cell);
    };
    origin_ = world::WorldVec3d{snap(camera.x), model.mean_level, snap(camera.z)};

    PatchRows rows;
    rows.params = &params_;
    rows.trains = trains;
    rows.mean_level = model.mean_level;
    rows.origin = origin_;
    rows.time = time;
    rows.positions = positions_.data();
    rows.normals = normals_.data();
    rows.breaking = breaking_.data();
    const u64 row_count = static_cast<u64>(side) * params_.rings;
    if (jobs == nullptr) {
        evaluate_rows(rows, 0, row_count);
    } else {
        // Rows are independent — every vertex is a pure function of the model, its position and
        // the time — so a parallel build writes exactly what a serial one does, in any schedule.
        auto body = [&rows](const jobs::TaskContext& /*task*/, u64 begin, u64 end) noexcept {
            evaluate_rows(rows, begin, end);
        };
        if (Status ran =
                jobs::parallel_for(*jobs, row_count, kOceanRowsPerJob, body, "water.ocean_rows");
            !ran) {
            return ran;
        }
    }
    copy_inner_vertices(rows);
    return ok();
}

u64 OceanSurface::bytes() const noexcept {
    return static_cast<u64>((positions_.size() * sizeof(Vec3)) + (normals_.size() * sizeof(Vec3)) +
                            (breaking_.size() * sizeof(f32)) + (indices_.size() * sizeof(u32)));
}

}  // namespace cy::water
