#include <cy/rendering/lighting/area.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// GGX's normal distribution, with `alpha` the squared perceptual roughness.
[[nodiscard]] f32 ggx_d(f32 n_dot_h, f32 alpha) noexcept {
    const f32 a2 = alpha * alpha;
    const f32 denominator = (n_dot_h * n_dot_h * (a2 - 1.0F)) + 1.0F;
    return a2 / (math::kPi * denominator * denominator);
}

/// Smith's height-correlated visibility term for GGX, already divided by `4 n.l n.v`.
[[nodiscard]] f32 smith_v(f32 n_dot_l, f32 n_dot_v, f32 alpha) noexcept {
    const f32 a2 = alpha * alpha;
    const f32 lambda_v = n_dot_l * std::sqrt((n_dot_v * n_dot_v * (1.0F - a2)) + a2);
    const f32 lambda_l = n_dot_v * std::sqrt((n_dot_l * n_dot_l * (1.0F - a2)) + a2);
    const f32 sum = lambda_v + lambda_l;
    return sum > 0.0F ? 0.5F / sum : 0.0F;
}

/// The directions the fit is scored over, and the shape of the fit itself.
///
/// `M` is parameterised in the SHADING frame — +X the view's tangential part, +Z the normal — as
///
///     Minv = [ a  0  b ]
///            [ 0  c  0 ]
///            [ d  0  1 ]
///
/// which is the published parameterisation. The zeros are not an approximation: the GGX lobe is
/// symmetric about the plane of incidence, so the four remaining numbers are the whole of it. `b`
/// and `d` are the SHEAR, and they are what a pure scale about a rotated axis cannot express —
/// which is exactly what the moment-matched initialisation gets wrong at a grazing view, where it
/// over-reports a light the reflection misses by an order of magnitude (measured: x10.2 at
/// roughness 0.20 and cos(theta) 0.60, before this fit was added).
struct LtcFit {
    f32 a = 1.0F;
    f32 b = 0.0F;
    f32 c = 1.0F;
    f32 d = 0.0F;
};

[[nodiscard]] Mat3 matrix_of(const LtcFit& fit) noexcept {
    return Mat3::from_columns(Vec3{fit.a, 0.0F, fit.d}, Vec3{0.0F, fit.c, 0.0F},
                              Vec3{fit.b, 0.0F, 1.0F});
}

/// The linearly transformed cosine's density at `direction`, for `Minv`.
[[nodiscard]] f32 ltc_density(const LtcFit& fit, Vec3 direction) noexcept {
    const Vec3 transformed{(fit.a * direction.x) + (fit.b * direction.z), fit.c * direction.y,
                           (fit.d * direction.x) + direction.z};
    const f32 l = length(transformed);
    if (l <= 1.0e-8F || transformed.z <= 0.0F) {
        return 0.0F;
    }
    // det(Minv) for this sparse form, and the Jacobian of the projection back onto the sphere.
    const f32 determinant = std::fabs(fit.c * (fit.a - (fit.b * fit.d)));
    const f32 cosine = transformed.z / l;
    return (cosine / math::kPi) * determinant / (l * l * l);
}

/// The directions the fit is scored over. 20 x 10 on the hemisphere, with the azimuth folded by
/// the lobe's own mirror symmetry — 200 directions, which is what makes a per-entry optimisation
/// affordable at all.
constexpr u32 kFitTheta = 20;
constexpr u32 kFitPhi = 10;

struct FitTarget {
    Vec3 direction[kFitTheta * kFitPhi];
    /// The GGX BRDF times the cosine, normalised so it integrates to 1 — a density, so the fit is
    /// of the lobe's SHAPE and the energy is carried separately by `LtcEntry::magnitude`.
    f32 density[kFitTheta * kFitPhi]{};
    f32 energy = 1.0F;
    f32 fresnel = 0.0F;
};

/// The GGX directional albedo, and the split sum's Fresnel term with it, by importance sampling
/// the NDF over a stratified grid. `out_fresnel` receives the second term.
[[nodiscard]] f32 ggx_directional_albedo(f32 alpha, f32 cos_theta, f32& out_fresnel) noexcept {
    constexpr u32 kSteps = 48;
    const f32 sin_theta = std::sqrt(math::max(0.0F, 1.0F - (cos_theta * cos_theta)));
    const Vec3 view{sin_theta, 0.0F, cos_theta};

    f32 scale = 0.0F;
    f32 bias = 0.0F;
    for (u32 i = 0; i < kSteps; ++i) {
        const f32 u1 = (static_cast<f32>(i) + 0.5F) / static_cast<f32>(kSteps);
        // The GGX NDF's inverse cumulative distribution in cos(theta_h).
        const f32 cos_h = std::sqrt((1.0F - u1) / (1.0F + (((alpha * alpha) - 1.0F) * u1)));
        const f32 sin_h = std::sqrt(math::max(0.0F, 1.0F - (cos_h * cos_h)));
        for (u32 j = 0; j < kSteps; ++j) {
            const f32 phi =
                2.0F * math::kPi * (static_cast<f32>(j) + 0.5F) / static_cast<f32>(kSteps);
            const Vec3 half{sin_h * std::cos(phi), sin_h * std::sin(phi), cos_h};
            const f32 v_dot_h = dot(view, half);
            const Vec3 light = half * (2.0F * v_dot_h) - view;
            if (light.z <= 0.0F || v_dot_h <= 0.0F) {
                continue;
            }
            // Sampling h with pdf D(h) cos(theta_h) and changing variables to the light direction
            // leaves `4 V (v.h) cos(theta_l) / cos(theta_h)` as the estimator.
            const f32 weight = 4.0F * smith_v(light.z, cos_theta, alpha) * v_dot_h * light.z /
                               math::max(cos_h, 1.0e-6F);
            const f32 one_minus = 1.0F - v_dot_h;
            const f32 fc = one_minus * one_minus * one_minus * one_minus * one_minus;
            scale += weight * (1.0F - fc);
            bias += weight * fc;
        }
    }
    const f32 count = static_cast<f32>(kSteps * kSteps);
    out_fresnel = bias / count;
    return (scale + bias) / count;
}

[[nodiscard]] FitTarget build_target(f32 alpha, f32 cos_theta) noexcept {
    const f32 sin_theta = std::sqrt(math::max(0.0F, 1.0F - (cos_theta * cos_theta)));
    const Vec3 view{sin_theta, 0.0F, cos_theta};

    FitTarget target;
    // ENERGY IS IMPORTANCE SAMPLED, THE SHAPE IS NOT, AND THE TWO NEEDS ARE GENUINELY DIFFERENT.
    // A uniform grid over the hemisphere cannot resolve a near-mirror lobe: at roughness 0.20 the
    // directional albedo came out as 1.47 — an energy of 147% — because 288 uniform samples land
    // one or two inside a lobe two degrees wide. Sampling the NDF puts every sample inside the
    // lobe, which is what an albedo integral needs. The SHAPE fit wants the opposite: a uniform
    // set, so that the tail the fit is trying to match is represented at all.
    target.energy = ggx_directional_albedo(alpha, cos_theta, target.fresnel);
    for (u32 i = 0; i < kFitTheta; ++i) {
        const f32 cl = (static_cast<f32>(i) + 0.5F) / static_cast<f32>(kFitTheta);
        const f32 sl = std::sqrt(math::max(0.0F, 1.0F - (cl * cl)));
        for (u32 j = 0; j < kFitPhi; ++j) {
            // Half the azimuth: the lobe is symmetric about the plane of incidence, so the other
            // half carries no information the fit can use.
            const f32 phi = math::kPi * (static_cast<f32>(j) + 0.5F) / static_cast<f32>(kFitPhi);
            const Vec3 light{sl * std::cos(phi), sl * std::sin(phi), cl};
            const Vec3 half = normalize(light + view);
            const f32 v_dot_h = math::max(0.0F, dot(view, half));
            const f32 value =
                ggx_d(math::max(0.0F, half.z), alpha) * smith_v(cl, cos_theta, alpha) * cl;
            const u32 slot = (i * kFitPhi) + j;
            target.direction[slot] = light;
            target.density[slot] = value;
            (void)v_dot_h;
        }
    }

    // The shape fit is of a DENSITY, so the sampled target is normalised to integrate to one over
    // the same uniform grid it was taken on. Normalising by the importance-sampled albedo instead
    // would fold that integral's own quadrature error into every fitted matrix.
    f32 grid_energy = 0.0F;
    for (const f32 density : target.density) {
        grid_energy += density;
    }
    grid_energy *= 2.0F * math::kPi / static_cast<f32>(kFitTheta * kFitPhi);
    const f32 normalisation_source = grid_energy;
    const f32 normalisation = normalisation_source > 0.0F ? 1.0F / normalisation_source : 1.0F;
    for (f32& density : target.density) {
        density *= normalisation;
    }
    return target;
}

/// The error the fit minimises: the cube of the absolute difference, summed over the sampled
/// directions and weighted by the solid angle each stands for.
///
/// The CUBE rather than the square is the published choice and it matters: an L2 fit trades a large
/// error at the lobe's peak for a small one over the whole tail, and the peak is the part a viewer
/// sees as a highlight.
[[nodiscard]] f32 fit_error(const LtcFit& fit, const FitTarget& target) noexcept {
    f32 error = 0.0F;
    for (u32 slot = 0; slot < kFitTheta * kFitPhi; ++slot) {
        const f32 difference = ltc_density(fit, target.direction[slot]) - target.density[slot];
        const f32 magnitude = difference < 0.0F ? -difference : difference;
        error += magnitude * magnitude * magnitude;
    }
    return error;
}

/// Coordinate descent over the four parameters. Not L-BFGS: this is a four-parameter problem with
/// a smooth objective and — because the table is walked in an order that always provides one — a
/// good starting point, and a line search along each axis in turn reaches the same basin for a
/// fraction of the code.
///
/// `a` and `c` move MULTIPLICATIVELY and `b` and `d` additively in units of `a`. That is not a
/// stylistic choice: the four parameters differ in scale by two orders of magnitude across the
/// table (a near-mirror's `a` is 0.02 and a rough surface's is 1.0), and a step that is reasonable
/// for one is either useless or catastrophic for the other.
[[nodiscard]] LtcFit refine(LtcFit fit, const FitTarget& target, f32 floor) noexcept {
    f32 best = fit_error(fit, target);
    f32 step = 0.35F;
    for (u32 round = 0; round < 14; ++round) {
        bool improved = false;
        for (u32 parameter = 0; parameter < 4; ++parameter) {
            for (const f32 direction : {-1.0F, 1.0F}) {
                LtcFit candidate = fit;
                switch (parameter) {
                    case 0:
                        candidate.a *= 1.0F + (direction * step);
                        break;
                    case 1:
                        candidate.b += direction * step * fit.a;
                        break;
                    case 2:
                        candidate.c *= 1.0F + (direction * step);
                        break;
                    default:
                        candidate.d += direction * step * fit.a;
                        break;
                }
                // The floor is the physical one: a lobe cannot be narrower than the roughness it
                // came from, and without it the descent walks `a` to zero wherever the sampled
                // grid is too coarse to resolve the lobe — which is every near-mirror entry. The
                // matrix then has a determinant of 1e-8, the transformed polygon collapses below
                // the horizon, and the whole table answers zero. Measured, on the first draft.
                if (candidate.a < floor || candidate.c < floor) {
                    continue;
                }
                const f32 error = fit_error(candidate, target);
                if (error < best) {
                    best = error;
                    fit = candidate;
                    improved = true;
                }
            }
        }
        if (!improved) {
            step *= 0.6F;
        }
    }
    return fit;
}

/// The edge term of the spherical-polygon cosine integral: the angle between two unit vectors
/// times the z component of their normalised cross product.
[[nodiscard]] f32 edge_integral(Vec3 a, Vec3 b) noexcept {
    const f32 cosine = math::clamp(dot(a, b), -1.0F, 1.0F);
    const f32 theta = std::acos(cosine);
    const Vec3 axis = cross(a, b);
    const f32 sine = length(axis);
    // theta / sin(theta) is 1 in the limit; taking it as 1 below the threshold avoids a 0/0 on two
    // coincident corners, which a degenerate quad produces every time a light is edge on.
    const f32 ratio = sine > 1.0e-6F ? theta / sine : 1.0F;
    return axis.z * ratio;
}

/// Clip a polygon against the z > 0 half space, in place. Returns the new vertex count, 0 to 5.
///
/// The horizon clip is not an optimisation: without it the edge integral of a corner below the
/// surface contributes with the wrong sign, and a rect light sinking below a floor gets BRIGHTER
/// as it goes.
/// The most vertices a convex polygon of `kMaxPolygonCorners` can have after one half-space clip:
/// each edge can add at most one crossing, and at most one of the original vertices is replaced.
inline constexpr u32 kMaxClippedCorners = 5;
inline constexpr u32 kMaxPolygonCorners = 4;

[[nodiscard]] u32 clip_to_horizon(Vec3* corners, u32 count) noexcept {
    if (count < 3 || count > kMaxPolygonCorners) {
        // Bounded here rather than by the callers, so `clipped`'s size is a property this function
        // can be read to be correct about. Every caller already passes 3 or 4; the analyser cannot
        // see that, and neither can the next reader.
        return 0;
    }
    Vec3 clipped[kMaxClippedCorners];
    u32 written = 0;
    for (u32 index = 0; index < count && written + 2U <= kMaxClippedCorners; ++index) {
        const Vec3 current = corners[index];
        const Vec3 next = corners[(index + 1U) % count];
        const bool current_above = current.z > 0.0F;
        const bool next_above = next.z > 0.0F;
        if (current_above) {
            clipped[written++] = current;
        }
        if (current_above != next_above) {
            const f32 t = current.z / (current.z - next.z);
            clipped[written++] = current + (next - current) * t;
        }
    }
    for (u32 index = 0; index < written; ++index) {
        corners[index] = clipped[index];
    }
    return written;
}

/// The SIGNED spherical-polygon integral. The public `integrate_cosine_polygon` clamps it, which is
/// the clamped-cosine integral's own definition and what a caller stating corners directly means.
/// The evaluators want the signed form because the sign is the quad's WINDING, and a quad's winding
/// is a property of which way the emitter's tangent frame was authored rather than of whether it
/// lights anything. Single-sidedness is a separate test, on the emitter's normal, and it is made
/// before this is called.
[[nodiscard]] f32 signed_cosine_polygon(Vec3* working, u32 count) noexcept {
    if (count < 3 || count > 4) {
        return 0.0F;
    }
    for (u32 index = 0; index < count; ++index) {
        working[index] = normalized_or(working[index], Vec3{0.0F, 0.0F, 1.0F});
    }
    const u32 clipped = clip_to_horizon(working, count);
    if (clipped < 3) {
        return 0.0F;
    }
    for (u32 index = 0; index < clipped; ++index) {
        working[index] = normalized_or(working[index], Vec3{0.0F, 0.0F, 1.0F});
    }
    f32 sum = 0.0F;
    for (u32 index = 0; index < clipped; ++index) {
        sum += edge_integral(working[index], working[(index + 1U) % clipped]);
    }
    return sum / (2.0F * math::kPi);
}

}  // namespace

const char* area_light_shape_name(AreaLightShape shape) noexcept {
    switch (shape) {
        case AreaLightShape::Rect:
            return "rect";
        case AreaLightShape::Disc:
            return "disc";
        case AreaLightShape::Sphere:
            return "sphere";
        case AreaLightShape::Tube:
            return "tube";
        case AreaLightShape::Count:
            break;
    }
    return "unknown";
}

AreaQuad area_light_quad(const AreaLight& light) noexcept {
    // A disc of radius r has area pi r^2; the quad of half-extent r * sqrt(pi)/2 has the same area.
    // Substituting the equal-AREA quad rather than the inscribed one is what makes a disc light and
    // a rect light of the same emitting area read as the same brightness.
    constexpr f32 kDiscToQuad = 0.8862269255F;  // sqrt(pi) / 2

    f32 extent_x = light.half_x;
    f32 extent_y = light.half_y;
    switch (light.shape) {
        case AreaLightShape::Disc:
            extent_x = light.half_x * kDiscToQuad;
            extent_y = light.half_y * kDiscToQuad;
            break;
        case AreaLightShape::Sphere:
            // A sphere presents its projected disc, which is a disc of the same radius facing the
            // shading point whatever the sphere's own orientation.
            extent_x = light.half_x * kDiscToQuad;
            extent_y = light.half_x * kDiscToQuad;
            break;
        case AreaLightShape::Tube:
            extent_x = light.half_x * kDiscToQuad;
            extent_y = light.half_y;
            break;
        case AreaLightShape::Rect:
        case AreaLightShape::Count:
            break;
    }

    Vec3 axis_x = light.tangent_x;
    Vec3 axis_y = light.tangent_y;
    if (light.shape == AreaLightShape::Sphere) {
        // Face the shading point: the sphere's own tangent frame is not a property anybody set.
        const Vec3 forward = normalized_or(light.center, Vec3{0.0F, 0.0F, 1.0F});
        axis_x = normalized_or(cross(Vec3{0.0F, 0.0F, 1.0F}, forward), Vec3{1.0F, 0.0F, 0.0F});
        axis_y = cross(forward, axis_x);
    }

    AreaQuad quad;
    quad.corner[0] = light.center - axis_x * extent_x - axis_y * extent_y;
    quad.corner[1] = light.center + axis_x * extent_x - axis_y * extent_y;
    quad.corner[2] = light.center + axis_x * extent_x + axis_y * extent_y;
    quad.corner[3] = light.center - axis_x * extent_x + axis_y * extent_y;
    return quad;
}

void LtcTable::build() noexcept {
    // WARM STARTING IS WHAT MAKES THE FIT AFFORDABLE, AND THE DIRECTION OF THE WALK IS WHAT MAKES
    // IT CONVERGE AT ALL.
    //
    // The walk runs from the ROUGHEST, most normal-incidence entry towards the smoothest and most
    // grazing — the opposite of the table's storage order — because that corner is the one entry
    // whose answer is known in advance: a fully rough GGX lobe at normal incidence IS a clamped
    // cosine, so the fit starts from the identity and is already at the optimum.
    //
    // Starting from the smooth corner instead does not merely converge slowly, it does not
    // converge: a near-mirror lobe is two degrees wide, the 288-direction grid the shape is scored
    // on lands no samples inside it, and every candidate scores the same. The descent then wanders
    // and the floor in `refine` is what catches it. Each entry inherits the neighbour one step
    // less extreme, so the descent never has to cross that ill-conditioned region on its own.
    LtcFit column_start;  // the first entry of each cos(theta) row, inherited down the table
    for (u32 y = 0; y < kLtcTableSize; ++y) {
        // The published parameterisation: sqrt of both axes, so the table spends its resolution
        // where the lobe changes shape fastest.
        const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kLtcTableSize);
        const f32 cos_theta = math::max(1.0F - (v * v), 1.0e-3F);
        LtcFit fit = column_start;
        for (u32 step = 0; step < kLtcTableSize; ++step) {
            const u32 x = kLtcTableSize - 1U - step;  // roughest first
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kLtcTableSize);
            const f32 roughness = u * u;
            const f32 alpha = math::max(roughness * roughness, 1.0e-4F);
            const FitTarget target = build_target(alpha, cos_theta);
            fit = refine(fit, target, math::max(alpha * 0.5F, 1.0e-3F));
            if (step == 0) {
                column_start = fit;
            }
            LtcEntry& entry = entries_[(y * kLtcTableSize) + x];
            entry.inverse_transform = matrix_of(fit);
            entry.magnitude = target.energy;
            entry.fresnel = target.fresnel;
        }
    }
    built_ = true;
}

LtcEntry LtcTable::sample(f32 roughness, f32 cos_theta) const noexcept {
    if (!built_) {
        return LtcEntry{};
    }
    const f32 u =
        (std::sqrt(math::clamp(roughness, 0.0F, 1.0F)) * static_cast<f32>(kLtcTableSize)) - 0.5F;
    const f32 v =
        (std::sqrt(math::clamp(1.0F - cos_theta, 0.0F, 1.0F)) * static_cast<f32>(kLtcTableSize)) -
        0.5F;
    const f32 uf = math::clamp(u, 0.0F, static_cast<f32>(kLtcTableSize - 1U));
    const f32 vf = math::clamp(v, 0.0F, static_cast<f32>(kLtcTableSize - 1U));
    const auto x0 = static_cast<u32>(uf);
    const auto y0 = static_cast<u32>(vf);
    const u32 x1 = math::min(x0 + 1U, kLtcTableSize - 1U);
    const u32 y1 = math::min(y0 + 1U, kLtcTableSize - 1U);
    const f32 fx = uf - static_cast<f32>(x0);
    const f32 fy = vf - static_cast<f32>(y0);

    const LtcEntry& a = entries_[(y0 * kLtcTableSize) + x0];
    const LtcEntry& b = entries_[(y0 * kLtcTableSize) + x1];
    const LtcEntry& c = entries_[(y1 * kLtcTableSize) + x0];
    const LtcEntry& d = entries_[(y1 * kLtcTableSize) + x1];

    LtcEntry result;
    for (usize column = 0; column < 3; ++column) {
        const Vec3 top =
            lerp(a.inverse_transform.columns[column], b.inverse_transform.columns[column], fx);
        const Vec3 bottom =
            lerp(c.inverse_transform.columns[column], d.inverse_transform.columns[column], fx);
        result.inverse_transform.columns[column] = lerp(top, bottom, fy);
    }
    result.magnitude = math::lerp(math::lerp(a.magnitude, b.magnitude, fx),
                                  math::lerp(c.magnitude, d.magnitude, fx), fy);
    result.fresnel =
        math::lerp(math::lerp(a.fresnel, b.fresnel, fx), math::lerp(c.fresnel, d.fresnel, fx), fy);
    return result;
}

f32 integrate_cosine_polygon(const Vec3* corners, u32 count) noexcept {
    if (corners == nullptr || count < 3 || count > 4) {
        return 0.0F;
    }
    Vec3 working[5];
    for (u32 index = 0; index < count; ++index) {
        working[index] = corners[index];
    }
    // The form integrates to 2*pi over a hemisphere and the clamped cosine to pi, so the
    // normalisation is 1/(2*pi). A quad subtending the whole hemisphere therefore answers 1, which
    // is the case `tests/test_area.cpp` pins the sign and the scale against.
    return math::max(0.0F, signed_cosine_polygon(working, count));
}

f32 ltc_evaluate_diffuse(const AreaLight& light, Vec3 normal) noexcept {
    const AreaQuad quad = area_light_quad(light);
    // Into the shading frame: +Z is the normal.
    const Vec3 up = std::fabs(normal.z) < 0.999F ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 tangent = normalized_or(cross(up, normal), Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(normal, tangent);
    Vec3 local[4];
    for (u32 index = 0; index < 4; ++index) {
        const Vec3 corner = quad.corner[index];
        local[index] = Vec3{dot(corner, tangent), dot(corner, bitangent), dot(corner, normal)};
    }
    if (light.single_sided) {
        // A single-sided emitter facing away contributes nothing, and the test is on the emitter's
        // normal against the direction to the shading point — not on the surface normal, which is a
        // different question and the one a naive implementation asks.
        const Vec3 emitter_normal = cross(light.tangent_x, light.tangent_y);
        if (dot(emitter_normal, light.center) > 0.0F && light.shape != AreaLightShape::Sphere &&
            light.shape != AreaLightShape::Tube) {
            return 0.0F;
        }
    }
    return std::fabs(signed_cosine_polygon(local, 4));
}

f32 ltc_evaluate(const LtcTable& table, const AreaLight& light, Vec3 normal, Vec3 view,
                 f32 roughness) noexcept {
    const Vec3 unit_normal = normalized_or(normal, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 unit_view = normalized_or(view, Vec3{0.0F, 0.0F, 1.0F});
    const f32 cos_theta = math::clamp(dot(unit_normal, unit_view), 1.0e-3F, 1.0F);

    // The shading frame: +Z the normal, +X the view's tangential part. The LTC's anisotropy is
    // expressed in the plane of incidence, so the frame has to be built from the view and not from
    // an arbitrary tangent.
    const Vec3 tangent = normalized_or(unit_view - unit_normal * cos_theta, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(unit_normal, tangent);

    const AreaQuad quad = area_light_quad(light);
    const LtcEntry entry = table.sample(roughness, cos_theta);

    Vec3 local[4];
    for (u32 index = 0; index < 4; ++index) {
        const Vec3 corner = quad.corner[index];
        const Vec3 shading{dot(corner, tangent), dot(corner, bitangent), dot(corner, unit_normal)};
        local[index] = entry.inverse_transform * shading;
    }
    if (light.single_sided) {
        const Vec3 emitter_normal = cross(light.tangent_x, light.tangent_y);
        if (dot(emitter_normal, light.center) > 0.0F && light.shape != AreaLightShape::Sphere &&
            light.shape != AreaLightShape::Tube) {
            return 0.0F;
        }
    }
    return std::fabs(signed_cosine_polygon(local, 4)) * entry.magnitude;
}

f32 area_light_solid_angle(const AreaLight& light) noexcept {
    const f32 distance_squared = length_squared(light.center);
    if (distance_squared <= 1.0e-8F) {
        return 2.0F * math::kPi;
    }
    f32 area = 4.0F * light.half_x * light.half_y;
    switch (light.shape) {
        case AreaLightShape::Disc:
            area = math::kPi * light.half_x * light.half_y;
            break;
        case AreaLightShape::Sphere:
            // The projected disc, which is what a shading point sees however the sphere is turned.
            area = math::kPi * light.half_x * light.half_x;
            break;
        case AreaLightShape::Tube:
            area = 2.0F * light.half_x * (2.0F * light.half_y);
            break;
        case AreaLightShape::Rect:
        case AreaLightShape::Count:
            break;
    }
    return math::min(area / distance_squared, 2.0F * math::kPi);
}

RepresentativePoint representative_point(const AreaLight& light, Vec3 normal, Vec3 view,
                                         f32 roughness) noexcept {
    const Vec3 unit_normal = normalized_or(normal, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 unit_view = normalized_or(view, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 reflection =
        normalized_or(unit_normal * (2.0F * dot(unit_normal, unit_view)) - unit_view, unit_normal);

    // The point on the emitter's plane closest to the reflection ray, clamped to the emitter's
    // extent. For a sphere and a tube the closest point on the surface is the same construction
    // with the radius subtracted afterwards.
    const Vec3 emitter_normal =
        normalized_or(cross(light.tangent_x, light.tangent_y), Vec3{0.0F, 0.0F, -1.0F});
    const f32 denominator = dot(reflection, emitter_normal);
    Vec3 on_plane = light.center;
    if (std::fabs(denominator) > 1.0e-4F) {
        const f32 t = dot(light.center, emitter_normal) / denominator;
        if (t > 0.0F) {
            on_plane = reflection * t;
        }
    }
    const Vec3 offset = on_plane - light.center;
    const f32 u = math::clamp(dot(offset, light.tangent_x), -light.half_x, light.half_x);
    const f32 v = math::clamp(dot(offset, light.tangent_y), -light.half_y, light.half_y);

    RepresentativePoint result;
    result.position = light.center + light.tangent_x * u + light.tangent_y * v;
    if (light.shape == AreaLightShape::Sphere) {
        const Vec3 towards = normalized_or(light.center, Vec3{0.0F, 0.0F, 1.0F});
        result.position = light.center - towards * light.half_x;
    }

    // The widening, and the energy normalisation that goes with it. The solid angle is turned into
    // an angular radius and added to the lobe's own width; without the second half the highlight
    // spreads AND brightens, which is the failure mode this approximation is known for.
    const f32 solid_angle = area_light_solid_angle(light);
    const f32 angular_radius = std::sqrt(solid_angle / math::kPi);
    const f32 alpha = math::max(roughness * roughness, 1.0e-4F);
    const f32 widened = math::clamp(alpha + (angular_radius * 0.5F), alpha, 1.0F);
    result.effective_roughness = std::sqrt(widened);
    result.energy_scale = (alpha * alpha) / (widened * widened);
    return result;
}

f32 emitter_filter_level(f32 roughness, f32 solid_angle_steradians, u32 mip_count) noexcept {
    if (mip_count == 0) {
        return 0.0F;
    }
    const f32 top = static_cast<f32>(mip_count - 1U);
    // Two things widen the footprint on the emitter: the lobe (roughness) and how much of the view
    // the emitter fills (solid angle). A mirror looking at a small light reads texels; a rough
    // surface, or a light filling the sky, reads the average.
    const f32 lobe = math::clamp(roughness, 0.0F, 1.0F);
    const f32 coverage = math::clamp(solid_angle_steradians / (2.0F * math::kPi), 0.0F, 1.0F);
    const f32 spread = math::clamp(lobe + (coverage * (1.0F - lobe)), 0.0F, 1.0F);
    return spread * top;
}

}  // namespace cy::rendering
