#include <cy/rendering/post/chain.h>

namespace cy::rendering {
namespace {

/// Whether the config switched this stage on. The three unconditional stages answer true here, and
/// that is where "a chain always tonemaps and always encodes" is stated.
[[nodiscard]] bool enabled(const PostChainConfig& config, PostStage stage) noexcept {
    switch (stage) {
        case PostStage::AmbientOcclusion:
            return config.ambient_occlusion;
        case PostStage::SubsurfaceScattering:
            return config.subsurface_scattering;
        case PostStage::VolumetricFog:
            return config.volumetric_fog;
        case PostStage::ScreenSpaceReflections:
            return config.screen_space_reflections;
        case PostStage::TemporalAntiAliasing:
            return config.temporal_antialiasing;
        case PostStage::TemporalUpscaling:
            return config.temporal_upscaling;
        case PostStage::AutoExposureMeasurement:
            return config.auto_exposure;
        case PostStage::DepthOfField:
            return config.depth_of_field;
        case PostStage::MotionBlur:
            return config.motion_blur;
        case PostStage::Bloom:
            return config.bloom;
        case PostStage::ColourGrading:
            return config.colour_grading;
        case PostStage::DisplaySpaceEffects:
            return config.display_space_effects;
        case PostStage::PostTonemapAntiAliasing:
            return config.post_tonemap_antialiasing;
        case PostStage::ExposureApply:
        case PostStage::Tonemap:
        case PostStage::OutputEncoding:
            // Unconditional. A chain without them produces values no display can show.
            return true;
        case PostStage::Count:
            break;
    }
    return false;
}

}  // namespace

const char* post_stage_name(PostStage stage) noexcept {
    switch (stage) {
        case PostStage::AmbientOcclusion:
            return "AmbientOcclusion";
        case PostStage::SubsurfaceScattering:
            return "SubsurfaceScattering";
        case PostStage::VolumetricFog:
            return "VolumetricFog";
        case PostStage::ScreenSpaceReflections:
            return "ScreenSpaceReflections";
        case PostStage::TemporalAntiAliasing:
            return "TemporalAntiAliasing";
        case PostStage::TemporalUpscaling:
            return "TemporalUpscaling";
        case PostStage::AutoExposureMeasurement:
            return "AutoExposureMeasurement";
        case PostStage::DepthOfField:
            return "DepthOfField";
        case PostStage::MotionBlur:
            return "MotionBlur";
        case PostStage::Bloom:
            return "Bloom";
        case PostStage::ExposureApply:
            return "ExposureApply";
        case PostStage::Tonemap:
            return "Tonemap";
        case PostStage::ColourGrading:
            return "ColourGrading";
        case PostStage::DisplaySpaceEffects:
            return "DisplaySpaceEffects";
        case PostStage::PostTonemapAntiAliasing:
            return "PostTonemapAntiAliasing";
        case PostStage::OutputEncoding:
            return "OutputEncoding";
        case PostStage::Count:
            break;
    }
    return "Unknown";
}

u32 post_stage_step(PostStage stage) noexcept {
    const u32 index = static_cast<u32>(stage);
    if (index >= static_cast<u32>(PostStage::Count)) {
        return 0;
    }
    // Steps 1 to 15, with temporal antialiasing and temporal upscaling both at 5.
    const u32 upscaling = static_cast<u32>(PostStage::TemporalUpscaling);
    return index <= upscaling ? index + 1U : index;
}

const char* colour_space_name(ColourSpace space) noexcept {
    switch (space) {
        case ColourSpace::SceneReferred:
            return "SceneReferred";
        case ColourSpace::DisplayReferred:
            return "DisplayReferred";
        case ColourSpace::Count:
            break;
    }
    return "Unknown";
}

ColourSpace post_stage_colour_space(PostStage stage) noexcept {
    // The boundary is tonemapping, and `Tonemap` itself is scene-referred because its INPUT is.
    // Bloom lands on the scene-referred side, which is what makes its threshold meaningful in
    // physical units; film grain lands on the other, which is what makes its strength perceptually
    // uniform. Both are scenarios in the specification.
    return static_cast<u32>(stage) <= static_cast<u32>(PostStage::Tonemap)
               ? ColourSpace::SceneReferred
               : ColourSpace::DisplayReferred;
}

const char* post_chain_refusal_name(PostChainRefusal refusal) noexcept {
    switch (refusal) {
        case PostChainRefusal::None:
            return "None";
        case PostChainRefusal::BothTemporalStages:
            return "BothTemporalStages";
        case PostChainRefusal::MotionVectorsUnavailable:
            return "MotionVectorsUnavailable";
        case PostChainRefusal::OutputTooSmall:
            return "OutputTooSmall";
        case PostChainRefusal::Count:
            break;
    }
    return "Unknown";
}

PostChainResult build_post_chain(const PostChainConfig& config, PostStage* out,
                                 u32 out_capacity) noexcept {
    PostChainResult result;

    if (config.temporal_antialiasing && config.temporal_upscaling) {
        result.refusal = PostChainRefusal::BothTemporalStages;
        result.diagnostic =
            "temporal antialiasing and temporal upscaling are the same step of the chain; "
            "enable one";
        return result;
    }

    const bool temporal = config.temporal_antialiasing || config.temporal_upscaling;
    if (temporal && !config.motion_vectors_available) {
        if (!config.may_enable_motion_vectors) {
            result.refusal = PostChainRefusal::MotionVectorsUnavailable;
            result.diagnostic =
                "a temporal stage needs motion vectors; enable them in the prepass or disable the "
                "temporal stage";
            return result;
        }
        // The other resolution the specification allows: ask the temporal framework for them. The
        // caller does the asking — a post chain that switched on a prepass mode would be a second
        // owner of the frame's structure.
        result.requires_motion_vectors = true;
    }

    for (u32 index = 0; index < static_cast<u32>(PostStage::Count); ++index) {
        const auto stage = static_cast<PostStage>(index);
        if (!enabled(config, stage)) {
            continue;
        }
        if (result.count < out_capacity && out != nullptr) {
            out[result.count] = stage;
        }
        ++result.count;
    }

    if (out == nullptr || result.count > out_capacity) {
        result.refusal = PostChainRefusal::OutputTooSmall;
        result.diagnostic = "the chain does not fit the array it was given";
    }
    return result;
}

}  // namespace cy::rendering
