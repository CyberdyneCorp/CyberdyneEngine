// Image-based lighting: the DFG table bake and octahedral environment maps. Task 4.2.3.
//
// `rendering-materials-and-shading` — "Image-based lighting": "Indirect specular SHALL use the
// split-sum approximation: a pre-filtered environment map indexed by roughness, multiplied by a
// precomputed DFG lookup table (a 2D RG16F texture indexed by NoV and roughness, generated with GGX
// importance sampling)", and "Environment maps SHALL be stored as octahedral maps rather than
// cubemaps", with border texels replicated so bilinear filtering across the seam is correct.
//
// WHY THIS SUITE IS `integration` AND NOT `unit`. A DFG bake is `size² · samples` importance
// samples with a square root and two transcendentals each; a 32² table at 128 samples is 131 072 of
// them, which is a hundred times a `unit` budget at `-O0` and would be a case shrunk until it
// stopped measuring the bake. `testing-and-quality`'s taxonomy has a suite for exactly this, and
// the determinism claim below — "two runs produce byte-identical tables" — is the one that would be
// lost first if the case were trimmed to fit.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/brdf.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using cy::u32;
using cy::Vec2;
using cy::Vec3;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

constexpr f32 kPi = cy::math::kPi;
constexpr u32 kTableSize = 32;
constexpr u32 kTableSamples = 128;

}  // namespace

CY_TEST_CASE("the DFG table is bounded, and a white f0 loses energy as roughness rises") {
    cy::Array<DfgEntry> table(allocator());
    CY_REQUIRE(generate_dfg_table(kTableSize, kTableSamples, table).has_value());
    CY_REQUIRE_EQ(table.size(), static_cast<cy::usize>(kTableSize) * kTableSize);

    for (const DfgEntry& entry : table) {
        // Every entry is a directional albedo: a fraction of the incoming energy, never negative
        // and never more than one. A table that violated it would make a material brighter than its
        // environment, which is the failure that shows up as bloom on a matte surface.
        CY_CHECK_GE(entry.scale, 0.0F);
        CY_CHECK_GE(entry.bias, 0.0F);
        CY_CHECK_LE(entry.scale + entry.bias, 1.05F);
        CY_CHECK(std::isfinite(entry.scale));
        CY_CHECK(std::isfinite(entry.bias));
    }

    // Ess = scale + bias for a white f0. The single-scatter lobe loses more energy the rougher the
    // surface, which is the loss `multiscatter_compensation` gives back.
    const auto ess = [&table](u32 row, u32 column) noexcept {
        const DfgEntry& entry = table[(static_cast<cy::usize>(row) * kTableSize) + column];
        return entry.scale + entry.bias;
    };
    const u32 head_on = kTableSize - 1;
    CY_CHECK_GT(ess(0, head_on), ess(kTableSize - 1, head_on));
    CY_CHECK_GT(ess(0, head_on), 0.9F);
}

CY_TEST_CASE("two bakes of the same table are byte-identical") {
    // The Hammersley sequence is a closed form in the sample index, which is what makes this true.
    // A random sequence would make every golden image that samples the table depend on a seed —
    // and the failure would be a one-pixel difference nobody could attribute.
    cy::Array<DfgEntry> first(allocator());
    cy::Array<DfgEntry> second(allocator());
    CY_REQUIRE(generate_dfg_table(16, 64, first).has_value());
    CY_REQUIRE(generate_dfg_table(16, 64, second).has_value());
    CY_REQUIRE_EQ(first.size(), second.size());
    for (cy::usize index = 0; index < first.size(); ++index) {
        CY_CHECK_EQ(first[index].scale, second[index].scale);
        CY_CHECK_EQ(first[index].bias, second[index].bias);
    }
}

CY_TEST_CASE("a table with no size or no samples is refused rather than divided by") {
    cy::Array<DfgEntry> table(allocator());
    CY_CHECK_FALSE(generate_dfg_table(0, 64, table).has_value());
    CY_CHECK_FALSE(generate_dfg_table(16, 0, table).has_value());
}

CY_TEST_CASE("sampling the table interpolates, and clamps at the edges rather than wrapping") {
    cy::Array<DfgEntry> table(allocator());
    CY_REQUIRE(generate_dfg_table(kTableSize, kTableSamples, table).has_value());
    const cy::Span<const DfgEntry> span = table.span();

    // A texel centre reads that texel back exactly, which is what says the bilinear coordinate is
    // the same convention the bake wrote with.
    const f32 centre = 0.5F / static_cast<f32>(kTableSize);
    const DfgEntry corner = sample_dfg(span, kTableSize, centre, centre);
    CY_CHECK_NEAR(corner.scale, table[0].scale, 1e-5F);
    CY_CHECK_NEAR(corner.bias, table[0].bias, 1e-5F);

    // Off the end in both directions is the edge texel, not a wrap into the other end of the
    // roughness range — which would put a smooth surface's response on a rough one.
    const DfgEntry beyond = sample_dfg(span, kTableSize, 2.0F, 2.0F);
    const DfgEntry last = table[table.size() - 1];
    CY_CHECK_NEAR(beyond.scale, last.scale, 1e-5F);
    const DfgEntry before = sample_dfg(span, kTableSize, -1.0F, -1.0F);
    CY_CHECK_NEAR(before.scale, table[0].scale, 1e-5F);

    // A malformed request is answered with zeroes rather than reading past the array.
    CY_CHECK_EQ(sample_dfg(span, 0, 0.5F, 0.5F).scale, 0.0F);
    CY_CHECK_EQ(sample_dfg(span, kTableSize * 2, 0.5F, 0.5F).scale, 0.0F);
}

CY_TEST_CASE("the octahedral mapping round-trips every direction in both hemispheres") {
    // Both hemispheres, because the fold is what an octahedral map is and getting its sign wrong
    // mirrors the lower half of every environment — which reads as "the sky is in the floor".
    u32 checked = 0;
    for (u32 iz = 0; iz <= 8; ++iz) {
        for (u32 iy = 0; iy <= 8; ++iy) {
            for (u32 ix = 0; ix <= 8; ++ix) {
                const Vec3 raw{(static_cast<f32>(ix) / 4.0F) - 1.0F,
                               (static_cast<f32>(iy) / 4.0F) - 1.0F,
                               (static_cast<f32>(iz) / 4.0F) - 1.0F};
                const f32 length = std::sqrt((raw.x * raw.x) + (raw.y * raw.y) + (raw.z * raw.z));
                if (length < 1e-3F) {
                    continue;
                }
                const Vec3 direction{raw.x / length, raw.y / length, raw.z / length};
                const Vec2 uv = octahedral_uv_from_direction(direction);
                CY_CHECK_GE(uv.x, -1e-5F);
                CY_CHECK_LE(uv.x, 1.0F + 1e-5F);
                CY_CHECK_GE(uv.y, -1e-5F);
                CY_CHECK_LE(uv.y, 1.0F + 1e-5F);

                const Vec3 back = direction_from_octahedral_uv(uv);
                CY_CHECK_NEAR(back.x, direction.x, 1e-4F);
                CY_CHECK_NEAR(back.y, direction.y, 1e-4F);
                CY_CHECK_NEAR(back.z, direction.z, 1e-4F);
                ++checked;
            }
        }
    }
    CY_CHECK_GT(checked, 700U);

    // A zero direction is answered with the centre of the map rather than a division by zero.
    const Vec2 degenerate = octahedral_uv_from_direction(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_NEAR(degenerate.x, 0.5F, 1e-6F);
    CY_CHECK_NEAR(degenerate.y, 0.5F, 1e-6F);
}

CY_TEST_CASE("a texel one step off the map holds the direction its mirror does") {
    // The seam scenario: "WHEN an octahedral map is filtered THEN border texels SHALL be replicated
    // so bilinear filtering across the octahedral seam is correct." The replication is a coordinate
    // mapping, and this is the claim that the mapping names the texel holding the same direction.
    constexpr u32 kSize = 16;
    const auto direction_at = [](u32 x, u32 y) noexcept {
        return direction_from_octahedral_uv(
            Vec2{(static_cast<f32>(x) + 0.5F) / static_cast<f32>(kSize),
                 (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kSize)});
    };

    for (u32 index = 0; index < kSize; ++index) {
        const auto coordinate = static_cast<cy::i32>(index);
        struct Step {
            cy::i32 x;
            cy::i32 y;
        };
        const Step steps[] = {{coordinate, -1},
                              {coordinate, static_cast<cy::i32>(kSize)},
                              {-1, coordinate},
                              {static_cast<cy::i32>(kSize), coordinate}};
        for (const Step& step : steps) {
            u32 source_x = 0;
            u32 source_y = 0;
            octahedral_border_source(step.x, step.y, kSize, source_x, source_y);
            CY_REQUIRE(source_x < kSize);
            CY_REQUIRE(source_y < kSize);

            // The replicated texel and the edge texel a bilinear tap would otherwise reach are on
            // the same side of the fold: their directions agree to within one texel of angle.
            const Vec3 replicated = direction_at(source_x, source_y);
            const Vec3 edge =
                direction_at(static_cast<u32>(cy::math::min(cy::math::max(step.x, 0),
                                                            static_cast<cy::i32>(kSize) - 1)),
                             static_cast<u32>(cy::math::min(cy::math::max(step.y, 0),
                                                            static_cast<cy::i32>(kSize) - 1)));
            const f32 alignment =
                (replicated.x * edge.x) + (replicated.y * edge.y) + (replicated.z * edge.z);
            CY_CHECK_GT(alignment, 0.5F);
        }
    }

    // A coordinate already inside the map is returned unchanged: the mapping is only for the
    // border, and one that moved interior texels would blur the whole image.
    u32 x = 0;
    u32 y = 0;
    octahedral_border_source(5, 7, kSize, x, y);
    CY_CHECK_EQ(x, 5U);
    CY_CHECK_EQ(y, 7U);
}

CY_TEST_CASE("the built-in directional-albedo table still agrees with a fresh bake") {
    // `directional_albedo` carries sixteen-by-sixteen constants baked from `generate_dfg_table`, so
    // that `evaluate_specular` can compensate for lost energy with nothing bound. Constants drift
    // from what they approximate the moment the integrator changes, and the only way that is caught
    // is by re-baking and comparing — which is this case.
    cy::Array<DfgEntry> table(allocator());
    CY_REQUIRE(generate_dfg_table(kTableSize, kTableSamples, table).has_value());

    f32 worst_interior = 0.0F;
    f32 worst_overall = 0.0F;
    for (u32 row = 0; row < kTableSize; ++row) {
        for (u32 column = 0; column < kTableSize; ++column) {
            const f32 roughness = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(kTableSize);
            const f32 n_dot_v = (static_cast<f32>(column) + 0.5F) / static_cast<f32>(kTableSize);
            const DfgEntry& entry = table[(static_cast<cy::usize>(row) * kTableSize) + column];
            const f32 baked = entry.scale + entry.bias;
            const f32 built_in = directional_albedo(n_dot_v, roughness);
            const f32 error = (baked > built_in) ? (baked - built_in) : (built_in - baked);
            worst_overall = (error > worst_overall) ? error : worst_overall;
            if (n_dot_v >= 0.05F) {
                worst_interior = (error > worst_interior) ? error : worst_interior;
            }
        }
    }
    // Measured, not guessed: 0.019 over the interior and 0.054 at the extreme grazing column, where
    // the run-time bake at this sample count is itself the noisier of the two. The margins are
    // twice the measurement, so a re-bake of the same integrator passes and a changed integrator
    // does not.
    CY_CHECK_LT(worst_interior, 0.04F);
    CY_CHECK_LT(worst_overall, 0.11F);

    // The values themselves, at the three roughnesses the shading model is usually discussed at.
    CY_CHECK_NEAR(directional_albedo(1.0F, 0.05F), 1.0F, 0.01F);
    CY_CHECK_LT(directional_albedo(1.0F, 0.9F), 0.6F);
    // Monotone in roughness head-on: more roughness, more energy lost to multiple scattering.
    f32 previous = 1.01F;
    for (u32 step = 0; step <= 10; ++step) {
        const f32 value = directional_albedo(1.0F, static_cast<f32>(step) * 0.1F);
        CY_CHECK_LE(value, previous + 1e-4F);
        previous = value;
    }
}

CY_TEST_CASE("the distribution is normalised, which is what makes the bake mean anything") {
    // ∫ D·NoH·dω = 1 over the hemisphere, for every roughness. A DEFECT FOUND BY THIS CASE AND
    // FIXED: the stable form was transcribed as `k = α / (1 − NoH² + α²)` rather than
    // `α / (1 − NoH² + (NoH·α)²)`, which integrates to 0.80 at α = 0.5 and 0.60 at α = 0.81. Every
    // rough highlight was losing energy that multi-scatter compensation could not distinguish from
    // the loss it exists to correct, and nothing said so — the surface just looked a little dull.
    constexpr u32 kSteps = 512;
    const f32 d_theta = cy::math::kHalfPi / static_cast<f32>(kSteps);
    for (const f32 perceptual : {0.1F, 0.3F, 0.5F, 0.7F, 0.9F, 1.0F}) {
        const f32 alpha = alpha_from_roughness(perceptual);
        f32 integral = 0.0F;
        for (u32 step = 0; step < kSteps; ++step) {
            const f32 theta = (static_cast<f32>(step) + 0.5F) * d_theta;
            const f32 n_dot_h = std::cos(theta);
            // dω = sinθ dθ dφ, integrated over φ analytically.
            integral += distribution_ggx(n_dot_h, alpha) * n_dot_h * std::sin(theta) * d_theta *
                        cy::math::kTwoPi;
        }
        CY_CHECK_NEAR(integral, 1.0F, 0.01F);
    }
}

// MOVED HERE FROM `unit.material`, AND THE MOVE IS THE POINT. The case integrates 4 096
// directions against a nine-term basis — 331 776 multiply-adds and 4 096 transcendentals — and
// at the Development profile's optimisation that measured between 0.54 ms and 1.24 ms of CPU
// against the unit suite's 1 ms budget, so it failed roughly one run in ten on an IDLE machine
// and turned `just roadmap-milestone m0` red through `just test-all`. Shrinking the sample
// count to fit would widen the tolerance until the orthonormality it checks stopped being
// visible, which is testing something else; `testing-and-quality`'s taxonomy has a suite for a
// case that integrates something, and this is it — the same argument the header above makes
// for the DFG bake.
CY_TEST_CASE("the L2 basis is orthonormal, which every irradiance property rests on") {
    // Monte Carlo over a deterministic spiral of directions: ∫ Yi·Yj = δij. Checked because if the
    // basis constants are wrong, irradiance is wrong by a per-band factor that looks like an
    // ambient intensity slider being off.
    constexpr u32 kSamples = 4096;
    f32 accumulated[9][9] = {};
    for (u32 index = 0; index < kSamples; ++index) {
        const f32 z = 1.0F - ((2.0F * (static_cast<f32>(index) + 0.5F)) / kSamples);
        const f32 radius = std::sqrt(cy::math::max(1.0F - (z * z), 0.0F));
        // The golden-angle spiral: deterministic, and equidistributed enough for four thousand
        // samples to settle an orthonormality check to two decimal places.
        const f32 phi = static_cast<f32>(index) * 2.39996323F;
        const Vec3 direction{radius * std::cos(phi), radius * std::sin(phi), z};

        f32 basis[9];
        sh_basis_l2(direction, basis);
        for (u32 i = 0; i < 9; ++i) {
            for (u32 j = 0; j < 9; ++j) {
                accumulated[i][j] += basis[i] * basis[j];
            }
        }
    }
    const f32 weight = (4.0F * kPi) / static_cast<f32>(kSamples);
    for (u32 i = 0; i < 9; ++i) {
        for (u32 j = 0; j < 9; ++j) {
            const f32 expected = (i == j) ? 1.0F : 0.0F;
            CY_CHECK_NEAR(accumulated[i][j] * weight, expected, 0.02F);
        }
    }
}

// MOVED HERE FROM `unit.material` FOR THE SAME REASON, and found one profile later: it is
// 2 048 `ShL2::accumulate()` calls, which cost 0.3 ms of CPU at the Development profile's -O2
// and 2.262 ms at Debug's -O0 — so it passed every sweep run in `dev` and failed `just test-all
// --profile debug` three times out of three. THE LESSON IS THE PROFILE, not the case: a
// per-case budget has to be swept in the SLOWEST configuration that runs it, and `four-profiles`
// is the criterion that runs it there.
CY_TEST_CASE("a uniform environment gives uniform irradiance, whatever the normal") {
    // The one closed-form case: a constant radiance L over the sphere irradiates every normal
    // equally. If the band constants were wrong, this would depend on the normal.
    ShL2 sh;
    constexpr u32 kSamples = 2048;
    const f32 solid_angle = (4.0F * kPi) / static_cast<f32>(kSamples);
    for (u32 index = 0; index < kSamples; ++index) {
        const f32 z = 1.0F - ((2.0F * (static_cast<f32>(index) + 0.5F)) / kSamples);
        const f32 radius = std::sqrt(cy::math::max(1.0F - (z * z), 0.0F));
        const f32 phi = static_cast<f32>(index) * 2.39996323F;
        sh.accumulate(Vec3{radius * std::cos(phi), radius * std::sin(phi), z},
                      Vec3{1.0F, 1.0F, 1.0F}, solid_angle);
    }

    const f32 up = sh.irradiance(Vec3{0.0F, 0.0F, 1.0F}).x;
    const f32 side = sh.irradiance(Vec3{1.0F, 0.0F, 0.0F}).x;
    const f32 down = sh.irradiance(Vec3{0.0F, -1.0F, 0.0F}).x;
    CY_CHECK_NEAR(side, up, 0.05F);
    CY_CHECK_NEAR(down, up, 0.05F);
    // And the value is π·L/π = 1 after the convolution constants are divided through, which is what
    // lets a consumer multiply by albedo/π and stop.
    CY_CHECK_NEAR(up, 1.0F, 0.05F);

    // A default-constructed set is a black environment, not nine uninitialised colours.
    const ShL2 black;
    CY_CHECK_EQ(black.irradiance(Vec3{0.0F, 1.0F, 0.0F}).x, 0.0F);
}
