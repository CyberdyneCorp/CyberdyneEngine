// The signal table, the quality ladder, and the two states that are not filtering. Task 9.3.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/denoise/denoiser.h>

#include <vector>

namespace {

using cy::rendering::denoise::default_config;
using cy::rendering::denoise::Denoiser;
using cy::rendering::denoise::GuidanceBuffers;
using cy::rendering::denoise::HistoryGuidance;
using cy::rendering::denoise::kQualityPositionCount;
using cy::rendering::denoise::kSignalCount;
using cy::rendering::denoise::LobeShape;
using cy::rendering::denoise::NoisySignal;
using cy::rendering::denoise::quality_ladder;
using cy::rendering::denoise::signal_name;
using cy::rendering::denoise::SignalDomain;
using cy::rendering::denoise::SignalKind;

}  // namespace

CY_TEST_CASE("every signal declares what kind of signal it is") {
    // `denoising` — "Signal-specific configuration". The two rows that carry the requirement's own
    // examples are the specular one, which must be lobe-dependent and roughness-modulated, and the
    // two visibility terms, which must not be reconstructed as colour.
    const auto diffuse = default_config(SignalKind::IndirectDiffuse);
    CY_CHECK_EQ(diffuse.domain, SignalDomain::Radiance);
    CY_CHECK_EQ(diffuse.lobe, LobeShape::Hemispherical);
    CY_CHECK_EQ(diffuse.roughness_widening, 0.0F);

    const auto specular = default_config(SignalKind::IndirectSpecular);
    CY_CHECK_EQ(specular.lobe, LobeShape::Lobe);
    CY_CHECK_GT(specular.roughness_widening, 0.0F);

    CY_CHECK_EQ(default_config(SignalKind::RayTracedShadow).domain, SignalDomain::Visibility);
    CY_CHECK_EQ(default_config(SignalKind::AmbientOcclusion).domain, SignalDomain::Visibility);
    CY_CHECK_EQ(default_config(SignalKind::StochasticDirect).domain, SignalDomain::Radiance);

    // A visibility term keeps a tighter value tolerance than a radiance one: that is what preserves
    // contact hardening rather than filtering it away as colour.
    CY_CHECK_LT(default_config(SignalKind::RayTracedShadow).sigma_value, diffuse.sigma_value);

    for (cy::u32 index = 0; index < kSignalCount; ++index) {
        CY_CHECK_NE(signal_name(static_cast<SignalKind>(index)), nullptr);
        CY_CHECK_GT(default_config(static_cast<SignalKind>(index)).history_length, 0U);
    }
}

CY_TEST_CASE("the quality ladder is discrete, ordered and priced") {
    // The shape `design.md` §2.10 asks every lever for: a discrete ladder of declared positions,
    // coarsest last, each carrying what it costs relative to position 0. An arbiter allocating
    // milliseconds over a ladder it cannot price is choosing blind.
    const auto ladder = quality_ladder();
    CY_REQUIRE_EQ(ladder.size(), kQualityPositionCount);
    CY_CHECK_EQ(ladder[0].relative_cost, 1.0F);
    for (cy::usize index = 1; index < ladder.size(); ++index) {
        CY_CHECK_LT(ladder[index].relative_cost, ladder[index - 1].relative_cost);
        CY_CHECK_LE(ladder[index].max_passes, ladder[index - 1].max_passes);
        CY_CHECK_LE(ladder[index].history_length, ladder[index - 1].history_length);
    }
    // Every position still denoises. A ladder whose last rung is "off" would make the budget's
    // last step a visual discontinuity rather than a coarser frame.
    CY_CHECK_GT(ladder[ladder.size() - 1].max_passes, 0U);
    CY_CHECK_GT(ladder[ladder.size() - 1].kernel_extent, 0U);
}

CY_TEST_CASE("the raw signal is inspectable and the bypass says so") {
    // `denoising`: "Denoising SHALL be disableable for reference comparison and validation, so the
    // raw stochastic signal can be inspected." Off returns the input unchanged — not a cheaper
    // filter, which would be a different image and useless as a reference.
    constexpr cy::u32 kSize = 8;
    constexpr cy::u32 kPixels = kSize * kSize;
    Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(kSize, kSize).has_value());
    denoiser.set_enabled(false);

    std::vector<cy::Vec3> values(kPixels);
    std::vector<cy::f32> depth(kPixels, 4.0F);
    for (cy::u32 pixel = 0; pixel < kPixels; ++pixel) {
        const cy::f32 value = static_cast<cy::f32>(pixel) * 0.01F;
        values[pixel] = cy::Vec3{value, value * 2.0F, value * 3.0F};
    }

    GuidanceBuffers guidance;
    guidance.width = kSize;
    guidance.height = kSize;
    guidance.depth = {depth.data(), depth.size()};

    NoisySignal noisy;
    noisy.values = {values.data(), values.size()};

    const auto result =
        denoiser.denoise(SignalKind::IndirectDiffuse, noisy, guidance, HistoryGuidance{});
    CY_REQUIRE(result.has_value());
    for (cy::u32 pixel = 0; pixel < kPixels; ++pixel) {
        CY_CHECK_EQ(result.value()[pixel].x, values[pixel].x);
        CY_CHECK_EQ(result.value()[pixel].y, values[pixel].y);
        CY_CHECK_EQ(result.value()[pixel].z, values[pixel].z);
    }
    CY_CHECK(denoiser.diagnostics(SignalKind::IndirectDiffuse).bypassed);
}

CY_TEST_CASE("a mismatched buffer is refused rather than read past its end") {
    constexpr cy::u32 kSize = 8;
    Denoiser denoiser;
    CY_CHECK_FALSE(denoiser.resize(0, 8).has_value());
    CY_REQUIRE(denoiser.resize(kSize, kSize).has_value());

    std::vector<cy::Vec3> values(4);
    std::vector<cy::f32> depth(static_cast<size_t>(kSize) * kSize, 1.0F);
    GuidanceBuffers guidance;
    guidance.width = kSize;
    guidance.height = kSize;
    guidance.depth = {depth.data(), depth.size()};
    NoisySignal noisy;
    noisy.values = {values.data(), values.size()};
    CY_CHECK_FALSE(denoiser.denoise(SignalKind::IndirectDiffuse, noisy, guidance, HistoryGuidance{})
                       .has_value());

    guidance.width = 4;
    CY_CHECK_FALSE(denoiser.denoise(SignalKind::IndirectDiffuse, noisy, guidance, HistoryGuidance{})
                       .has_value());
}
