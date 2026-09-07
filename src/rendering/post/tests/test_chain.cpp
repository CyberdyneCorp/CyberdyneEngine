// The chain's order and colour spaces, which are the criterion of task 8.4. And the quality table,
// whose declared costs are what lets a preset be assembled against a frame budget.

#include <cy/test/test.h>

#include <cy/rendering/post/chain.h>
#include <cy/rendering/post/quality.h>

namespace {

using cy::rendering::ColourSpace;
using cy::rendering::fit_preset_to_budget;
using cy::rendering::PostChainConfig;
using cy::rendering::PostChainRefusal;
using cy::rendering::PostChainResult;
using cy::rendering::PostQualityPreset;
using cy::rendering::PostStage;
using cy::rendering::preset_cost_ms;
using cy::rendering::QualityLevel;

PostChainConfig everything() noexcept {
    PostChainConfig config;
    config.ambient_occlusion = true;
    config.subsurface_scattering = true;
    config.volumetric_fog = true;
    config.screen_space_reflections = true;
    config.temporal_antialiasing = true;
    config.auto_exposure = true;
    config.depth_of_field = true;
    config.motion_blur = true;
    config.bloom = true;
    config.colour_grading = true;
    config.display_space_effects = true;
    config.post_tonemap_antialiasing = true;
    config.motion_vectors_available = true;
    return config;
}

/// Where `stage` sits in a chain, or -1.
cy::i32 position_of(const PostStage* chain, cy::u32 count, PostStage stage) noexcept {
    for (cy::u32 index = 0; index < count; ++index) {
        if (chain[index] == stage) {
            return static_cast<cy::i32>(index);
        }
    }
    return -1;
}

}  // namespace

CY_TEST_CASE("the chain runs in the order the specification fixes, and never in another") {
    PostStage chain[cy::rendering::kMaxPostStages];
    const PostChainResult result =
        build_post_chain(everything(), chain, cy::rendering::kMaxPostStages);
    CY_REQUIRE(result.ok());
    CY_REQUIRE_EQ(result.count, 15U);

    // Strictly increasing in the specification's own step numbers. Asserted against the numbering
    // rather than against a list copied out of it, so a stage that moves is caught here and not by
    // a reader noticing.
    for (cy::u32 index = 1; index < result.count; ++index) {
        CY_CHECK_LT(cy::rendering::post_stage_step(chain[index - 1]),
                    cy::rendering::post_stage_step(chain[index]) + 1U);
        CY_CHECK_LT(static_cast<cy::u32>(chain[index - 1]), static_cast<cy::u32>(chain[index]));
    }

    // Task 8.4's own nine names, in the relative order it gives them.
    // "exposure" in that list is the metering, which is step 6 and therefore before depth of
    // field; the exposure MULTIPLY is step 10, just before tonemapping, and both are asserted.
    const PostStage named[] = {
        PostStage::AmbientOcclusion, PostStage::VolumetricFog, PostStage::AutoExposureMeasurement,
        PostStage::DepthOfField,     PostStage::Bloom,         PostStage::ExposureApply,
        PostStage::Tonemap,          PostStage::ColourGrading, PostStage::PostTonemapAntiAliasing};
    cy::i32 previous = -1;
    for (const PostStage stage : named) {
        const cy::i32 at = position_of(chain, result.count, stage);
        CY_REQUIRE(at >= 0);
        CY_CHECK_GT(at, previous);
        previous = at;
    }
    // Temporal reconstruction is step 5 and therefore EARLIER than exposure, which is where the
    // specification puts it and where the task list's shorthand would not. See this module's
    // README.
    CY_CHECK_LT(position_of(chain, result.count, PostStage::TemporalAntiAliasing),
                position_of(chain, result.count, PostStage::ExposureApply));
}

CY_TEST_CASE("a disabled stage is absent from the chain rather than skipped inside it") {
    PostStage chain[cy::rendering::kMaxPostStages];
    // Nothing switched on. What survives is what a frame cannot do without.
    const PostChainResult result =
        build_post_chain(PostChainConfig{}, chain, cy::rendering::kMaxPostStages);
    CY_REQUIRE(result.ok());
    CY_REQUIRE_EQ(result.count, 3U);
    CY_CHECK_EQ(chain[0], PostStage::ExposureApply);
    CY_CHECK_EQ(chain[1], PostStage::Tonemap);
    CY_CHECK_EQ(chain[2], PostStage::OutputEncoding);
    CY_CHECK_EQ(position_of(chain, result.count, PostStage::Bloom), -1);
}

CY_TEST_CASE("bloom is scene-referred and film grain is not, and the boundary is tonemapping") {
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::Bloom),
                ColourSpace::SceneReferred);
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::ExposureApply),
                ColourSpace::SceneReferred);
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::Tonemap),
                ColourSpace::SceneReferred);
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::ColourGrading),
                ColourSpace::DisplayReferred);
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::DisplaySpaceEffects),
                ColourSpace::DisplayReferred);
    CY_CHECK_EQ(cy::rendering::post_stage_colour_space(PostStage::OutputEncoding),
                ColourSpace::DisplayReferred);

    // Every stage before tonemapping is scene-referred and every stage after it is not. Stated as a
    // sweep so a new stage cannot be added on the wrong side without failing here.
    for (cy::u32 index = 0; index < static_cast<cy::u32>(PostStage::Count); ++index) {
        const auto stage = static_cast<PostStage>(index);
        const ColourSpace expected = index <= static_cast<cy::u32>(PostStage::Tonemap)
                                         ? ColourSpace::SceneReferred
                                         : ColourSpace::DisplayReferred;
        CY_CHECK_EQ(cy::rendering::post_stage_colour_space(stage), expected);
    }
}

CY_TEST_CASE("a temporal stage without motion vectors is enabled or refused, never ignored") {
    PostStage chain[cy::rendering::kMaxPostStages];

    PostChainConfig config;
    config.temporal_antialiasing = true;
    config.motion_vectors_available = false;
    config.may_enable_motion_vectors = true;
    PostChainResult result = build_post_chain(config, chain, cy::rendering::kMaxPostStages);
    CY_CHECK(result.ok());
    CY_CHECK(result.requires_motion_vectors);

    config.may_enable_motion_vectors = false;
    result = build_post_chain(config, chain, cy::rendering::kMaxPostStages);
    CY_CHECK_FALSE(result.ok());
    CY_CHECK_EQ(result.refusal, PostChainRefusal::MotionVectorsUnavailable);
    CY_CHECK_NE(result.diagnostic[0], '\0');

    // Both temporal stages are the same step, so they are alternatives.
    config.motion_vectors_available = true;
    config.temporal_upscaling = true;
    result = build_post_chain(config, chain, cy::rendering::kMaxPostStages);
    CY_CHECK_FALSE(result.ok());
    CY_CHECK_EQ(result.refusal, PostChainRefusal::BothTemporalStages);

    // A short array reports the count it would have needed rather than truncating quietly.
    config.temporal_upscaling = false;
    const PostChainResult small = build_post_chain(everything(), chain, 2);
    CY_CHECK_EQ(small.refusal, PostChainRefusal::OutputTooSmall);
    CY_CHECK_EQ(small.count, 15U);
}

CY_TEST_CASE("a preset's declared costs sum to a number, and fit into a budget by giving ground") {
    PostStage chain[cy::rendering::kMaxPostStages];
    const PostChainResult result =
        build_post_chain(everything(), chain, cy::rendering::kMaxPostStages);
    CY_REQUIRE(result.ok());
    const cy::Span<const PostStage> stages(chain, result.count);
    const cy::rendering::PostQualityTable& table = cy::rendering::default_post_quality_table();

    PostQualityPreset ultra = cy::rendering::post_quality_preset(QualityLevel::Ultra);
    PostQualityPreset medium = cy::rendering::post_quality_preset(QualityLevel::Medium);
    PostQualityPreset low = cy::rendering::post_quality_preset(QualityLevel::Low);

    const cy::f32 ultra_cost = preset_cost_ms(stages, ultra, table, 1920, 1080);
    const cy::f32 medium_cost = preset_cost_ms(stages, medium, table, 1920, 1080);
    const cy::f32 low_cost = preset_cost_ms(stages, low, table, 1920, 1080);
    CY_CHECK_GT(ultra_cost, medium_cost);
    CY_CHECK_GT(medium_cost, low_cost);
    CY_CHECK_GT(low_cost, 0.0F);

    // The declared costs are stated at 1920x1080, so a bigger target costs proportionally more.
    CY_CHECK_NEAR(preset_cost_ms(stages, medium, table, 3840, 2160), medium_cost * 4.0F, 1e-3F);

    // Every stage in the chain is priced. A stage with no declared cost would make the sum silently
    // optimistic, which is exactly what an arbiter cannot afford.
    for (const PostStage stage : stages) {
        CY_CHECK_GT(table.at(stage, QualityLevel::Medium).cost_ms, 0.0F);
    }

    // Fitting: the preset gives ground from the end of the chain until it fits, and says so
    // honestly when it cannot.
    PostQualityPreset fitted = ultra;
    CY_CHECK(fit_preset_to_budget(stages, table, 1920, 1080, medium_cost, fitted));
    CY_CHECK_LE(preset_cost_ms(stages, fitted, table, 1920, 1080), medium_cost);

    PostQualityPreset impossible = ultra;
    CY_CHECK_FALSE(fit_preset_to_budget(stages, table, 1920, 1080, 0.01F, impossible));
    CY_CHECK_EQ(impossible.level(PostStage::AmbientOcclusion), QualityLevel::Low);
}
