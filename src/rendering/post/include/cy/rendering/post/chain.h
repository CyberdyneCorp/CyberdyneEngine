#pragma once
// The post-process chain, in its defined order, with the colour space each stage operates in.
// Task 8.4.
//
// `rendering-post-processing` — "Chain order and colour space" lists fifteen steps and requires
// that the chain "SHALL execute in this order, operating on linear HDR scene colour until
// tonemapping", and that "effects operating after tonemapping SHALL be documented as
// display-referred; effects before it as scene-referred".
//
// ================================================================================================
// THE ORDER IS DATA, AND THE CHAIN IS BUILT FROM IT
// ================================================================================================
//
// The enumerator values ARE the order. `build_post_chain()` walks the enumeration once and appends
// the stages that are enabled, so a chain cannot come out in a different order than the
// specification lists, and a stage cannot be inserted in the wrong place without moving an
// enumerator — which is a diff a reviewer sees. The alternative, a hand-written sequence of `if`
// statements, is how a bloom that runs after tonemapping gets into a renderer and stays there,
// because at that point the threshold has to be re-tuned and the wrongness becomes load-bearing.
//
// A DISABLED STAGE IS ABSENT, NOT SKIPPED. `rendering-forward-clustered` already requires this of
// the frame's passes — "their passes SHALL be absent from the graph and their targets unallocated"
// — and the same argument applies here: a stage that is present and branches internally still has
// its target allocated and its barriers derived.
//
// ================================================================================================
// WHY TWO ANTI-ALIASING STAGES, AT TWO DIFFERENT PLACES
// ================================================================================================
//
// Temporal antialiasing and temporal upscaling are step 5, before exposure and tonemapping, because
// they reconstruct scene-referred colour and a neighbourhood clamp over display-referred values
// clamps against a curve rather than against radiance. FXAA and SMAA are step 14, after grading,
// because they are edge filters over the image a viewer actually sees. M7's task list names them
// together as "AA"; the specification's order is the one implemented here, and the reconciliation
// is in this module's README.

#include <cy/core/base/types.h>

namespace cy::rendering {

/// The chain, in the order `rendering-post-processing` fixes. The enumerator values are the order.
///
/// Sixteen enumerators for fifteen steps: temporal antialiasing and temporal upscaling are both
/// step 5 and are mutually exclusive, which `build_post_chain()` enforces rather than trusting.
enum class PostStage : u8 {
    AmbientOcclusion = 0,
    SubsurfaceScattering,
    VolumetricFog,
    ScreenSpaceReflections,
    TemporalAntiAliasing,
    TemporalUpscaling,
    AutoExposureMeasurement,
    DepthOfField,
    MotionBlur,
    Bloom,
    ExposureApply,
    Tonemap,
    ColourGrading,
    DisplaySpaceEffects,
    PostTonemapAntiAliasing,
    OutputEncoding,
    Count,
};

[[nodiscard]] const char* post_stage_name(PostStage stage) noexcept;

/// The specification's own step number, 1 to 15. Two stages share step 5.
[[nodiscard]] u32 post_stage_step(PostStage stage) noexcept;

/// Which colour space a stage operates in. The boundary is `Tonemap`, which is the transition
/// itself and is classified scene-referred because its input is.
enum class ColourSpace : u8 {
    /// Linear HDR radiance. Thresholds are meaningful in physical units here.
    SceneReferred = 0,
    /// After tonemapping. Strengths are perceptually uniform here.
    DisplayReferred,
    Count,
};

[[nodiscard]] const char* colour_space_name(ColourSpace space) noexcept;

[[nodiscard]] ColourSpace post_stage_colour_space(PostStage stage) noexcept;

/// What the project has switched on. Everything defaults to the state a project gets before it has
/// touched anything, which is why tonemapping and output encoding are not in this list: they are
/// unconditional, and a chain without them would produce values no display can show.
struct PostChainConfig {
    bool ambient_occlusion = false;
    bool subsurface_scattering = false;
    bool volumetric_fog = false;
    bool screen_space_reflections = false;
    bool temporal_antialiasing = false;
    bool temporal_upscaling = false;
    bool auto_exposure = false;
    bool depth_of_field = false;
    bool motion_blur = false;
    bool bloom = false;
    bool colour_grading = false;
    bool display_space_effects = false;
    bool post_tonemap_antialiasing = false;

    /// Whether the frame produces motion vectors. Not a post-process setting — it is a property of
    /// the prepass — and it is here because two stages cannot run without it.
    bool motion_vectors_available = false;
    /// Whether the temporal framework may be asked to turn motion vectors on. The specification
    /// allows either resolution: "the temporal framework SHALL enable their production, or TAA
    /// SHALL be refused with a diagnostic".
    bool may_enable_motion_vectors = true;
};

/// Why a chain could not be built as configured. `None` is a chain that was.
enum class PostChainRefusal : u8 {
    None = 0,
    /// Temporal antialiasing and temporal upscaling are both step 5 and are alternatives.
    BothTemporalStages,
    /// A temporal stage without motion vectors, and no permission to enable them.
    MotionVectorsUnavailable,
    /// The caller's array was too small. The count is still reported.
    OutputTooSmall,
    Count,
};

[[nodiscard]] const char* post_chain_refusal_name(PostChainRefusal refusal) noexcept;

struct PostChainResult {
    /// How many stages the chain has. Written to `out` when it fits.
    u32 count = 0;
    PostChainRefusal refusal = PostChainRefusal::None;
    /// A sentence naming what to change. Never null.
    const char* diagnostic = "";
    /// True when the chain needs motion vectors the frame was not producing, and the temporal
    /// framework must be asked to produce them. The caller acts on it; this module does not reach
    /// into the framework, because a post chain that could switch on a prepass mode would be a
    /// second owner of the frame's structure.
    bool requires_motion_vectors = false;

    [[nodiscard]] constexpr bool ok() const noexcept { return refusal == PostChainRefusal::None; }
};

/// Build the chain. Walks the enumeration once, in order, so the result cannot be out of order.
[[nodiscard]] PostChainResult build_post_chain(const PostChainConfig& config, PostStage* out,
                                               u32 out_capacity) noexcept;

/// The maximum a chain can ever be — every stage minus the temporal alternative that is not taken.
inline constexpr u32 kMaxPostStages = static_cast<u32>(PostStage::Count);

}  // namespace cy::rendering
