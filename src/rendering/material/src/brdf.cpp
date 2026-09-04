// The core BRDF, split-sum image-based lighting and L2 irradiance. See
// cy/rendering/material/brdf.h.

#include <cy/rendering/material/brdf.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

// `cy::math`'s, rather than a second set of constants and clamps in this file: two spellings of π
// in one engine is how two shading paths end up differing in the last bit.
using math::kInvPi;
using math::kPi;
using math::lerp;
using math::saturate;

[[nodiscard]] f32 pow5(f32 value) noexcept {
    const f32 squared = value * value;
    return squared * squared * value;
}

/// Van der Corput radical inverse, base 2 — the second dimension of the Hammersley sequence.
///
/// Closed form in the sample index, which is what makes the DFG bake deterministic: two runs, and
/// two machines, produce byte-identical tables. A random sequence would make every image that
/// depends on the table depend on a seed.
[[nodiscard]] f32 radical_inverse_base2(u32 bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<f32>(bits) * 2.3283064365386963e-10F;  // / 2^32
}

/// Smith visibility with the `4·NoL·NoV` divisor folded in, for the DFG bake. The analytic
/// Schlick-GGX form rather than Hammon's approximation: the table is baked once and its accuracy
/// propagates into every material, so the extra square roots are worth paying there and not on the
/// per-pixel path.
[[nodiscard]] f32 visibility_smith_ggx_correlated(f32 n_dot_l, f32 n_dot_v, f32 alpha) noexcept {
    const f32 alpha_squared = alpha * alpha;
    const f32 lambda_v =
        n_dot_l * std::sqrt(((n_dot_v - (n_dot_v * alpha_squared)) * n_dot_v) + alpha_squared);
    const f32 lambda_l =
        n_dot_v * std::sqrt(((n_dot_l - (n_dot_l * alpha_squared)) * n_dot_l) + alpha_squared);
    const f32 denominator = lambda_v + lambda_l;
    return (denominator > 0.0F) ? (0.5F / denominator) : 0.0F;
}

/// The texel coordinate a [0, 1] value maps to in a `size`-wide table, CLAMPED AS A COORDINATE.
///
/// Clamping the two taps while leaving the fraction alone is the classic off-by-half: a request
/// below the first texel CENTRE lands at a negative coordinate whose fraction is then positive, so
/// it blends toward texel 1 and a request for roughness 0 comes back rougher than the smoothest row
/// of the table. Clamping here makes the edge behaviour what `CLAMP_TO_EDGE` does, which is what
/// the shader sampling the same table will use.
[[nodiscard]] f32 bilinear_coordinate(f32 value, u32 size) noexcept {
    const f32 texel = (saturate(value) * static_cast<f32>(size)) - 0.5F;
    return math::min(math::max(texel, 0.0F), static_cast<f32>(size - 1));
}

/// `Ess` at sixteen roughnesses by sixteen `NoV`, rows indexed by roughness, both at texel centres.
///
/// BAKED FROM `generate_dfg_table(16, 16384, ...)` — this engine's own integrator, not a published
/// fit against a different visibility term. `material_ibl` re-bakes at run time and holds these
/// numbers to it, so regenerating them after a change to `visibility_smith_ggx_correlated` is a
/// step the suite asks for rather than one somebody has to remember.
// clang-format off
// Roughness down, NoV across, both at texel centres; TWO SOURCE LINES PER TABLE ROW, so a reader
// checking a value against a re-bake finds it at (row, column) rather than at a flat index. Left
// unformatted deliberately: reflowed into a paragraph these numbers are unreadable, and they are
// read exactly when somebody is comparing them against `material_ibl`'s fresh bake.
constexpr f32 kDirectionalAlbedo[kDirectionalAlbedoSize * kDirectionalAlbedoSize] = {
    0.9976F, 0.9998F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F,
    1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F,
    0.9564F, 0.9947F, 0.9983F, 0.9991F, 0.9995F, 0.9997F, 0.9998F, 0.9998F,
    0.9999F, 0.9999F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F, 1.0000F,
    0.8913F, 0.9613F, 0.9849F, 0.9923F, 0.9954F, 0.9970F, 0.9979F, 0.9983F,
    0.9987F, 0.9986F, 0.9988F, 0.9990F, 0.9992F, 0.9993F, 0.9993F, 0.9994F,
    0.9030F, 0.9105F, 0.9497F, 0.9711F, 0.9817F, 0.9868F, 0.9905F, 0.9927F,
    0.9939F, 0.9949F, 0.9956F, 0.9962F, 0.9966F, 0.9970F, 0.9972F, 0.9974F,
    0.9274F, 0.8879F, 0.9081F, 0.9324F, 0.9515F, 0.9643F, 0.9729F, 0.9789F,
    0.9827F, 0.9856F, 0.9877F, 0.9891F, 0.9904F, 0.9914F, 0.9922F, 0.9927F,
    0.9442F, 0.8876F, 0.8800F, 0.8951F, 0.9140F, 0.9304F, 0.9435F, 0.9533F,
    0.9607F, 0.9668F, 0.9711F, 0.9745F, 0.9772F, 0.9794F, 0.9813F, 0.9827F,
    0.9529F, 0.8907F, 0.8691F, 0.8693F, 0.8790F, 0.8919F, 0.9053F, 0.9173F,
    0.9274F, 0.9361F, 0.9433F, 0.9491F, 0.9541F, 0.9580F, 0.9615F, 0.9642F,
    0.9559F, 0.8946F, 0.8645F, 0.8518F, 0.8502F, 0.8560F, 0.8647F, 0.8750F,
    0.8850F, 0.8947F, 0.9035F, 0.9112F, 0.9181F, 0.9241F, 0.9293F, 0.9339F,
    0.9546F, 0.8936F, 0.8582F, 0.8366F, 0.8265F, 0.8235F, 0.8256F, 0.8308F,
    0.8377F, 0.8456F, 0.8536F, 0.8616F, 0.8692F, 0.8763F, 0.8829F, 0.8889F,
    0.9502F, 0.8877F, 0.8477F, 0.8205F, 0.8027F, 0.7923F, 0.7874F, 0.7863F,
    0.7881F, 0.7918F, 0.7969F, 0.8028F, 0.8091F, 0.8156F, 0.8220F, 0.8283F,
    0.9431F, 0.8772F, 0.8323F, 0.7999F, 0.7764F, 0.7598F, 0.7483F, 0.7411F,
    0.7371F, 0.7355F, 0.7360F, 0.7379F, 0.7410F, 0.7448F, 0.7493F, 0.7541F,
    // NOLINTNEXTLINE(modernize-use-std-numbers) 0.6940 is a baked value, not ln2 spelled out
    0.9344F, 0.8629F, 0.8126F, 0.7752F, 0.7464F, 0.7241F, 0.7071F, 0.6940F,
    0.6844F, 0.6774F, 0.6726F, 0.6697F, 0.6683F, 0.6681F, 0.6688F, 0.6704F,
    0.9263F, 0.8453F, 0.7889F, 0.7460F, 0.7122F, 0.6851F, 0.6629F, 0.6449F,
    0.6301F, 0.6180F, 0.6082F, 0.6003F, 0.5941F, 0.5892F, 0.5856F, 0.5830F,
    0.9169F, 0.8257F, 0.7624F, 0.7138F, 0.6749F, 0.6430F, 0.6164F, 0.5938F,
    0.5746F, 0.5581F, 0.5439F, 0.5316F, 0.5210F, 0.5118F, 0.5039F, 0.4971F,
    0.9067F, 0.8043F, 0.7332F, 0.6790F, 0.6353F, 0.5989F, 0.5682F, 0.5418F,
    0.5189F, 0.4988F, 0.4811F, 0.4653F, 0.4512F, 0.4386F, 0.4272F, 0.4169F,
    0.8959F, 0.7813F, 0.7030F, 0.6428F, 0.5942F, 0.5539F, 0.5197F, 0.4901F,
    0.4642F, 0.4414F, 0.4211F, 0.4028F, 0.3863F, 0.3714F, 0.3578F, 0.3453F,
};
// clang-format on

constexpr const char* kDiffuseModelNames[] = {"Lambert", "Burley", "OrenNayar"};
static_assert(sizeof(kDiffuseModelNames) / sizeof(kDiffuseModelNames[0]) ==
              static_cast<usize>(DiffuseModel::Count));

}  // namespace

f32 alpha_from_roughness(f32 perceptual_roughness) noexcept {
    const f32 clamped = math::max(perceptual_roughness, kMinPerceptualRoughness);
    return clamped * clamped;
}

const char* diffuse_model_name(DiffuseModel model) noexcept {
    const auto index = static_cast<usize>(model);
    return (index < static_cast<usize>(DiffuseModel::Count)) ? kDiffuseModelNames[index]
                                                             : "<invalid>";
}

// --- Specular -------------------------------------------------------------------------------

f32 distribution_ggx(f32 n_dot_h, f32 alpha) noexcept {
    // k = α / (1 − NoH² + (NoH·α)²), D = k² / π. See the header for why this form rather than the
    // textbook one.
    //
    // THE `NoH·α` IS LOAD-BEARING AND IS THE ONE PLACE THIS FORM CAN BE MIS-TRANSCRIBED. Expanding
    // the denominator gives 1 + NoH²(α² − 1), so k²/π is exactly the textbook
    // α² / (π·(NoH²(α² − 1) + 1)²) — the same function, evaluated without the catastrophic
    // cancellation the textbook form suffers when α is small and NoH is near one.
    //
    // Written with a bare `α²` instead, the distribution stops being normalised: ∫D·NoH·dω falls to
    // 0.80 at α = 0.5 and 0.60 at α = 0.81, so every rough specular highlight loses energy that
    // multi-scatter compensation then cannot tell from the loss it exists to correct. It looks like
    // a slightly dull material rather than like an arithmetic error, which is precisely the class
    // of defect `rendering-materials-and-shading` gives exact terms in order to prevent. The
    // normalisation is asserted in `material_ibl`, over the roughness range, so the transcription
    // cannot come back.
    const f32 alpha_times_n_dot_h = alpha * n_dot_h;
    const f32 denominator =
        1.0F - (n_dot_h * n_dot_h) + (alpha_times_n_dot_h * alpha_times_n_dot_h);
    if (!(denominator > 0.0F)) {
        return 0.0F;
    }
    const f32 k = alpha / denominator;
    return k * k * kInvPi;
}

f32 visibility_smith_hammon(f32 n_dot_l, f32 n_dot_v, f32 alpha) noexcept {
    const f32 denominator = lerp(2.0F * n_dot_l * n_dot_v, n_dot_l + n_dot_v, alpha);
    return (denominator > 0.0F) ? (0.5F / denominator) : 0.0F;
}

Vec3 fresnel_schlick(Vec3 f0, f32 f90, f32 v_dot_h) noexcept {
    const f32 factor = pow5(1.0F - saturate(v_dot_h));
    return Vec3{f0.x + ((f90 - f0.x) * factor), f0.y + ((f90 - f0.y) * factor),
                f0.z + ((f90 - f0.z) * factor)};
}

Vec3 compute_f0(Vec3 albedo, f32 metallic, f32 specular) noexcept {
    const f32 clamped_metallic = saturate(metallic);
    const f32 clamped_specular = saturate(specular);
    const f32 dielectric = kDielectricF0 * clamped_specular * clamped_specular;
    return Vec3{lerp(dielectric, albedo.x, clamped_metallic),
                lerp(dielectric, albedo.y, clamped_metallic),
                lerp(dielectric, albedo.z, clamped_metallic)};
}

f32 compute_f90(Vec3 f0) noexcept {
    return saturate((f0.x + f0.y + f0.z) * kF90Scale);
}

// --- Diffuse --------------------------------------------------------------------------------

f32 diffuse_lambert() noexcept {
    return kInvPi;
}

f32 diffuse_burley(f32 n_dot_v, f32 n_dot_l, f32 v_dot_h, f32 perceptual_roughness) noexcept {
    const f32 f90 = 0.5F + (2.0F * v_dot_h * v_dot_h * perceptual_roughness);
    const f32 light_scatter = 1.0F + ((f90 - 1.0F) * pow5(1.0F - saturate(n_dot_l)));
    const f32 view_scatter = 1.0F + ((f90 - 1.0F) * pow5(1.0F - saturate(n_dot_v)));
    return light_scatter * view_scatter * kInvPi;
}

f32 diffuse_oren_nayar(f32 n_dot_v, f32 n_dot_l, f32 l_dot_v, f32 perceptual_roughness) noexcept {
    // The qualitative model: two terms in σ², which is the form worth having in real time. σ is the
    // surface slope's standard deviation, taken as the perceptual roughness.
    const f32 sigma_squared = perceptual_roughness * perceptual_roughness;
    const f32 a = 1.0F - (0.5F * sigma_squared / (sigma_squared + 0.33F));
    const f32 b = 0.45F * sigma_squared / (sigma_squared + 0.09F);
    // s = LoV − NoL·NoV, and t = 1 when s <= 0, else max(NoL, NoV). The pair is the standard
    // closed form for max(0, cos(φl − φv))·sin(α)·tan(β) without any trigonometry.
    const f32 s = l_dot_v - (n_dot_l * n_dot_v);
    const f32 t = (s <= 0.0F) ? 1.0F : math::max(n_dot_l, n_dot_v);
    const f32 safe_t = (t > 1e-4F) ? t : 1e-4F;
    return (a + (b * s / safe_t)) * kInvPi;
}

// --- Evaluation -----------------------------------------------------------------------------

Vec3 evaluate_specular(const SurfaceParameters& surface, const LightSample& light) noexcept {
    const f32 alpha = alpha_from_roughness(surface.roughness);
    const Vec3 f0 = compute_f0(surface.albedo, surface.metallic, surface.specular);
    const f32 f90 = compute_f90(f0);

    const f32 d = distribution_ggx(saturate(light.n_dot_h), alpha);
    const f32 v = visibility_smith_hammon(saturate(light.n_dot_l), saturate(light.n_dot_v), alpha);
    const Vec3 f = fresnel_schlick(f0, f90, light.v_dot_h);
    Vec3 specular = f * (d * v);

    if (surface.energy_compensation) {
        // The compensation factor needs the DFG table's directional albedo, and `Ess` is all of it
        // that the factor reads — so the built-in table carries the sum rather than the two
        // channels, and the bias is passed as zero. See `directional_albedo` for why this function
        // takes no table parameter.
        const f32 ess = directional_albedo(light.n_dot_v, surface.roughness);
        const Vec3 compensation = multiscatter_compensation(f0, ess, 0.0F);
        specular = Vec3{specular.x * compensation.x, specular.y * compensation.y,
                        specular.z * compensation.z};
    }
    return specular;
}

Vec3 evaluate_direct(const SurfaceParameters& surface, const LightSample& light) noexcept {
    const f32 n_dot_l = saturate(light.n_dot_l);
    if (n_dot_l <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }

    f32 diffuse_term = 0.0F;
    switch (surface.diffuse) {
        case DiffuseModel::Lambert:
            diffuse_term = diffuse_lambert();
            break;
        case DiffuseModel::Burley:
            diffuse_term = diffuse_burley(light.n_dot_v, n_dot_l, light.v_dot_h, surface.roughness);
            break;
        case DiffuseModel::OrenNayar:
            diffuse_term =
                diffuse_oren_nayar(light.n_dot_v, n_dot_l, light.l_dot_v, surface.roughness);
            break;
        case DiffuseModel::Count:
            break;
    }

    // A METAL HAS NO DIFFUSE, and it falls out of `f0` rather than being a branch: the same
    // `metallic` that makes `f0` the albedo takes the albedo out of the diffuse term. One number,
    // two consequences, no way for them to disagree.
    const f32 metallic = saturate(surface.metallic);
    const Vec3 diffuse_color = surface.albedo * (1.0F - metallic);
    const Vec3 diffuse = diffuse_color * diffuse_term;

    const Vec3 specular = evaluate_specular(surface, light);
    return (diffuse + specular) * n_dot_l;
}

Vec3 multiscatter_compensation(Vec3 f0, f32 dfg_scale, f32 dfg_bias) noexcept {
    // Ess, the single-scattering directional albedo, is what the table's two channels sum to for a
    // white f0. The energy the single-scatter lobe lost is (1 − Ess), and returning it in
    // proportion to f0 is Fdez-Aguera's compensation: 1 + f0·(1/Ess − 1).
    const f32 ess = math::max(dfg_scale + dfg_bias, 1e-4F);
    const f32 gain = (1.0F / ess) - 1.0F;
    return Vec3{1.0F + (f0.x * gain), 1.0F + (f0.y * gain), 1.0F + (f0.z * gain)};
}

// --- The DFG table ---------------------------------------------------------------------------

Status generate_dfg_table(u32 size, u32 samples, Array<DfgEntry>& out) noexcept {
    if (size == 0 || samples == 0) {
        return fail(ErrorCode::InvalidArgument, "a DFG table needs a size and a sample count");
    }
    if (Status sized = out.resize(static_cast<usize>(size) * size); !sized) {
        return sized;
    }

    for (u32 row = 0; row < size; ++row) {
        // Texel centres, so the table's first row is not roughness zero — a perfectly smooth
        // surface is a delta lobe and its integral is not what bilinear filtering should reach for.
        const f32 perceptual_roughness = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(size);
        const f32 alpha = alpha_from_roughness(perceptual_roughness);

        for (u32 column = 0; column < size; ++column) {
            const f32 n_dot_v =
                math::max((static_cast<f32>(column) + 0.5F) / static_cast<f32>(size), 1e-4F);
            // The view vector in tangent space, with the normal along +Z.
            const Vec3 view{std::sqrt(1.0F - (n_dot_v * n_dot_v)), 0.0F, n_dot_v};

            f32 scale = 0.0F;
            f32 bias = 0.0F;
            for (u32 index = 0; index < samples; ++index) {
                // GGX importance sampling: the Hammersley point maps to a half-vector distributed
                // as D·NoH, so the estimator's D and its pdf cancel and what remains is V and F.
                const f32 u1 = static_cast<f32>(index) / static_cast<f32>(samples);
                const f32 u2 = radical_inverse_base2(index);
                const f32 phi = 2.0F * kPi * u1;
                const f32 cos_theta =
                    std::sqrt((1.0F - u2) / (1.0F + (((alpha * alpha) - 1.0F) * u2)));
                const f32 sin_theta = std::sqrt(math::max(1.0F - (cos_theta * cos_theta), 0.0F));
                const Vec3 half{sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};

                const Vec3 light = (half * (2.0F * dot(view, half))) - view;
                const f32 n_dot_l = light.z;
                if (!(n_dot_l > 0.0F)) {
                    continue;
                }
                const f32 n_dot_h = math::max(half.z, 0.0F);
                const f32 v_dot_h = math::max(dot(view, half), 0.0F);

                const f32 visibility = visibility_smith_ggx_correlated(n_dot_l, n_dot_v, alpha) *
                                       n_dot_l * v_dot_h / math::max(n_dot_h, 1e-6F);
                const f32 fresnel = pow5(1.0F - v_dot_h);
                scale += (1.0F - fresnel) * visibility;
                bias += fresnel * visibility;
            }
            const f32 normalisation = 4.0F / static_cast<f32>(samples);
            out[(static_cast<usize>(row) * size) + column] =
                DfgEntry{scale * normalisation, bias * normalisation};
        }
    }
    return ok();
}

DfgEntry sample_dfg(Span<const DfgEntry> table, u32 size, f32 n_dot_v,
                    f32 perceptual_roughness) noexcept {
    if (size == 0 || table.size() < static_cast<usize>(size) * size) {
        return DfgEntry{};
    }
    const auto coordinate = [size](f32 value, u32& low, u32& high, f32& fraction) noexcept {
        const f32 texel = bilinear_coordinate(value, size);
        const f32 floored = std::floor(texel);
        fraction = texel - floored;
        low = static_cast<u32>(floored);
        high = math::min(low + 1U, size - 1U);
    };

    u32 x0 = 0;
    u32 x1 = 0;
    f32 fx = 0.0F;
    u32 y0 = 0;
    u32 y1 = 0;
    f32 fy = 0.0F;
    coordinate(n_dot_v, x0, x1, fx);
    coordinate(perceptual_roughness, y0, y1, fy);

    const auto at = [&table, size](u32 x, u32 y) noexcept {
        return table[(static_cast<usize>(y) * size) + x];
    };
    const DfgEntry a = at(x0, y0);
    const DfgEntry b = at(x1, y0);
    const DfgEntry c = at(x0, y1);
    const DfgEntry d = at(x1, y1);
    return DfgEntry{lerp(lerp(a.scale, b.scale, fx), lerp(c.scale, d.scale, fx), fy),
                    lerp(lerp(a.bias, b.bias, fx), lerp(c.bias, d.bias, fx), fy)};
}

f32 directional_albedo(f32 n_dot_v, f32 perceptual_roughness) noexcept {
    const f32 x = bilinear_coordinate(n_dot_v, kDirectionalAlbedoSize);
    const f32 y = bilinear_coordinate(perceptual_roughness, kDirectionalAlbedoSize);
    const auto x0 = static_cast<u32>(x);
    const auto y0 = static_cast<u32>(y);
    const u32 x1 = math::min(x0 + 1U, kDirectionalAlbedoSize - 1U);
    const u32 y1 = math::min(y0 + 1U, kDirectionalAlbedoSize - 1U);
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);

    const auto at = [](u32 column, u32 row) noexcept {
        return kDirectionalAlbedo[(row * kDirectionalAlbedoSize) + column];
    };
    return lerp(lerp(at(x0, y0), at(x1, y0), fx), lerp(at(x0, y1), at(x1, y1), fx), fy);
}

Vec3 indirect_specular(Vec3 prefiltered_radiance, Vec3 f0, f32 f90, DfgEntry dfg) noexcept {
    const Vec3 weight{(f0.x * dfg.scale) + (f90 * dfg.bias), (f0.y * dfg.scale) + (f90 * dfg.bias),
                      (f0.z * dfg.scale) + (f90 * dfg.bias)};
    return Vec3{prefiltered_radiance.x * weight.x, prefiltered_radiance.y * weight.y,
                prefiltered_radiance.z * weight.z};
}

f32 prefiltered_mip_for(f32 perceptual_roughness, u32 mip_count) noexcept {
    if (mip_count <= 1) {
        return 0.0F;
    }
    return saturate(perceptual_roughness) * static_cast<f32>(mip_count - 1);
}

// --- Octahedral maps -------------------------------------------------------------------------

Vec2 octahedral_uv_from_direction(Vec3 direction) noexcept {
    const f32 magnitude = std::fabs(direction.x) + std::fabs(direction.y) + std::fabs(direction.z);
    if (!(magnitude > 0.0F)) {
        return Vec2{0.5F, 0.5F};
    }
    const f32 inv = 1.0F / magnitude;
    f32 x = direction.x * inv;
    f32 y = direction.y * inv;
    if (direction.z < 0.0F) {
        const f32 folded_x = (1.0F - std::fabs(y)) * ((x < 0.0F) ? -1.0F : 1.0F);
        const f32 folded_y = (1.0F - std::fabs(x)) * ((y < 0.0F) ? -1.0F : 1.0F);
        x = folded_x;
        y = folded_y;
    }
    return Vec2{(x * 0.5F) + 0.5F, (y * 0.5F) + 0.5F};
}

Vec3 direction_from_octahedral_uv(Vec2 uv) noexcept {
    const f32 x = (uv.x * 2.0F) - 1.0F;
    const f32 y = (uv.y * 2.0F) - 1.0F;
    Vec3 direction{x, y, 1.0F - std::fabs(x) - std::fabs(y)};
    if (direction.z < 0.0F) {
        const f32 unfolded_x = (1.0F - std::fabs(direction.y)) * ((x < 0.0F) ? -1.0F : 1.0F);
        const f32 unfolded_y = (1.0F - std::fabs(direction.x)) * ((y < 0.0F) ? -1.0F : 1.0F);
        direction.x = unfolded_x;
        direction.y = unfolded_y;
    }
    return normalized_or(direction, Vec3{0.0F, 0.0F, 1.0F});
}

void octahedral_border_source(i32 x, i32 y, u32 size, u32& out_x, u32& out_y) noexcept {
    const auto last = static_cast<i32>(size) - 1;
    // A step off one edge of the octahedral square lands on the same edge, mirrored about its
    // centre — the two halves of the fold meet there. Doing it as coordinates rather than by
    // re-projecting a direction keeps the replication exact at every texel rather than correct to
    // within a rounding.
    if (y < 0) {
        y = 0;
        x = last - x;
    } else if (y > last) {
        y = last;
        x = last - x;
    }
    if (x < 0) {
        x = 0;
        y = last - y;
    } else if (x > last) {
        x = last;
        y = last - y;
    }
    out_x = static_cast<u32>(math::max(math::min(x, last), 0));
    out_y = static_cast<u32>(math::max(math::min(y, last), 0));
}

// --- Irradiance ------------------------------------------------------------------------------

void sh_basis_l2(Vec3 direction, f32 out[9]) noexcept {
    // The real orthonormal basis, in the order (0,0), (1,-1), (1,0), (1,1), (2,-2) ... (2,2).
    const f32 x = direction.x;
    const f32 y = direction.y;
    const f32 z = direction.z;
    out[0] = 0.282094791F;      // 1/(2√π)
    out[1] = 0.488602512F * y;  // √(3/4π)
    out[2] = 0.488602512F * z;
    out[3] = 0.488602512F * x;
    out[4] = 1.092548431F * x * y;  // √(15/4π)
    out[5] = 1.092548431F * y * z;
    out[6] = 0.315391565F * ((3.0F * z * z) - 1.0F);  // √(5/16π)
    out[7] = 1.092548431F * x * z;
    out[8] = 0.546274215F * ((x * x) - (y * y));  // √(15/16π)
}

void ShL2::accumulate(Vec3 direction, Vec3 radiance, f32 solid_angle) noexcept {
    f32 basis[9];
    sh_basis_l2(direction, basis);
    for (u32 index = 0; index < 9; ++index) {
        coefficients[index] = coefficients[index] + (radiance * (basis[index] * solid_angle));
    }
}

Vec3 ShL2::irradiance(Vec3 normal) const noexcept {
    // The cosine-lobe convolution constants: Â0 = π, Â1 = 2π/3, Â2 = π/4, each divided by π so the
    // caller multiplies by albedo/π and nothing else. Folding them in here is what keeps every
    // consumer from carrying three magic numbers.
    constexpr f32 kA0 = 1.0F;
    constexpr f32 kA1 = 2.0F / 3.0F;
    constexpr f32 kA2 = 0.25F;
    constexpr f32 kBand[9] = {kA0, kA1, kA1, kA1, kA2, kA2, kA2, kA2, kA2};

    f32 basis[9];
    sh_basis_l2(normal, basis);
    Vec3 result{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < 9; ++index) {
        result = result + (coefficients[index] * (basis[index] * kBand[index]));
    }
    return Vec3{math::max(result.x, 0.0F), math::max(result.y, 0.0F), math::max(result.z, 0.0F)};
}

}  // namespace cy::rendering
