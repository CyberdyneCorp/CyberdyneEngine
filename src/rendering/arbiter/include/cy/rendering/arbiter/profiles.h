#pragma once
// Renderer profiles: `Mobile`, `Standard`, `HighEnd` and `Cinematic`, and the fallback when a
// device cannot serve the one that was asked for. Task 10.2.
//
// `rendering-architecture` — "Renderer profiles".
//
// ================================================================================================
// A PROFILE IS CONFIGURATION. THERE IS NO SECOND RENDERER ANYWHERE IN THIS FILE
// ================================================================================================
//
// "A profile SHALL NOT be a separate renderer implementation. Where a difference cannot be
// expressed as configuration, it belongs in the pipeline, which is already a replaceable
// component."
//
// The enforcement is that `RendererProfile` is a plain aggregate of values and every shipped
// profile is a function returning one. There is no virtual call, no `#if`, and nothing a profile
// can select that a project cannot also select: `named_profile()` and a project's own literal
// produce the same type, which is what "Projects SHALL be able to define additional profiles"
// means when it is a property of the code rather than a promise.
//
// ================================================================================================
// CONTENT RENDERS UNDER EVERY PROFILE, SO A PROFILE MAY NOT REMOVE A SUBSYSTEM
// ================================================================================================
//
// "Content SHALL render under every profile it targets, differing in fidelity and performance, not
// in whether it appears. A profile requiring content changes SHALL be treated as a defect in the
// profile."
//
// A profile therefore chooses a subsystem's LADDER POSITION and its budget share; it never
// unregisters a subsystem that carries geometry or lighting. `Mobile` sits at the coarse end of
// every ladder and turns off the features that are additive — ray-traced reflections, volumetrics
// — and that is a different act from not drawing the scene. `validate_profile()` refuses a profile
// that disables a feature another enabled feature requires, which is the shape "a profile
// requiring content changes" actually takes in a code base.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/subsystem.h>

namespace cy::rendering {

/// The pipelines `rendering-architecture` requires the engine to ship, plus the seat a project's
/// own takes. "Pipelines SHALL differ in their strengths, and the documentation SHALL state them
/// rather than presenting one as strictly better" — `pipeline_strengths()` is that sentence made
/// into a string a diagnostic can print.
enum class RenderPipelineKind : u8 {
    /// Clustered forward. Transparency, MSAA and varied shading models directly.
    ForwardPlus = 0,
    /// Deferred material evaluation. Very high geometric density and many materials, at the cost of
    /// a more constrained transparency and MSAA story. Where virtual geometry realises its benefit.
    VisibilityBuffer,
    /// Reduced feature set, tile friendly.
    Mobile,
    /// Draws nothing. The pipeline a headless test and a cook run under.
    Null,
    /// A project's own, registered at runtime.
    Custom,
    Count,
};

[[nodiscard]] const char* pipeline_name(RenderPipelineKind pipeline) noexcept;
[[nodiscard]] const char* pipeline_strengths(RenderPipelineKind pipeline) noexcept;

/// What a profile can require of a device. A profile naming one the device lacks is not an error —
/// it is a fallback with a diagnostic that names the capability.
enum class RenderCapability : u8 {
    ComputeShaders = 0,
    IndirectDraw,
    BindlessResources,
    RayQuery,
    MeshShaders,
    SparseResources,
    Count,
};

[[nodiscard]] const char* capability_name(RenderCapability capability) noexcept;

using CapabilityMask = u32;

[[nodiscard]] constexpr CapabilityMask capability_bit(RenderCapability capability) noexcept {
    return static_cast<CapabilityMask>(1U) << static_cast<u32>(capability);
}

/// The render features a profile switches. `rendering-architecture`'s "Render features" names
/// ambient occlusion, screen-space reflections, global illumination, volumetrics, decals and debug
/// visualisation as engine features implemented through the public interface; the rest are M7's own
/// subsystems seen as features.
enum class RenderFeature : u8 {
    AmbientOcclusion = 0,
    ScreenSpaceReflections,
    RayTracedReflections,
    GlobalIllumination,
    Volumetrics,
    Decals,
    VirtualGeometry,
    VirtualShadows,
    VirtualTexturing,
    TemporalAntialiasing,
    TemporalUpscaling,
    MotionBlur,
    DepthOfField,
    Bloom,
    DebugVisualisation,
    Count,
};

inline constexpr u32 kRenderFeatureCount = static_cast<u32>(RenderFeature::Count);

[[nodiscard]] const char* feature_name(RenderFeature feature) noexcept;

/// What a feature costs the frame, per quality level, and what it needs to run at all.
struct FeatureSetting {
    bool enabled = false;
    /// 0 is the feature's own best quality; higher is cheaper. Not a ladder position of a
    /// subsystem — a feature that is not a budget subsystem still has quality levels.
    u8 quality_level = 0;
    /// The declared cost at this quality level, in milliseconds. The number
    /// `validate_configuration()` sums against the frame budget, which is what
    /// "Over-subscribed budget is caught early" is measured with.
    f32 declared_cost_ms = 0.0F;
    /// Everything this feature needs of the device.
    CapabilityMask requires_capabilities = 0;
};

/// The shipped profiles. A project adds its own by constructing a `RendererProfile`; this
/// enumeration names only the four the engine ships.
enum class ProfileName : u8 {
    Mobile = 0,
    Standard,
    HighEnd,
    Cinematic,
    Count,
};

inline constexpr u32 kProfileCount = static_cast<u32>(ProfileName::Count);

[[nodiscard]] const char* profile_name(ProfileName profile) noexcept;

/// One profile: a configuration over one renderer.
struct RendererProfile {
    /// The profile's own spelling. A project's profile carries its own name here and never has to
    /// appear in `ProfileName`.
    const char* name = "standard";
    RenderPipelineKind pipeline = RenderPipelineKind::ForwardPlus;

    /// The arbiter's configuration under this profile — chiefly the frame budget, which is what a
    /// profile is mostly choosing.
    ArbiterConfig arbiter;

    /// Which budget subsystems exist under this profile, and how each is priced. A profile does not
    /// unregister a subsystem that carries content; `Mobile` declares shorter, cheaper ladders.
    bool registered[kBudgetSubsystemCount] = {};
    SubsystemDeclaration subsystems[kBudgetSubsystemCount] = {};

    FeatureSetting features[kRenderFeatureCount] = {};

    /// Everything the profile as a whole needs of the device, over and above what each feature
    /// needs. A pipeline requirement lands here.
    CapabilityMask requires_capabilities = 0;
};

/// One of the four shipped profiles, fully populated.
[[nodiscard]] RendererProfile named_profile(ProfileName profile) noexcept;

/// Everything a device can do. The renderer's own summary — deliberately not `cy::rhi`'s
/// capability structure, which this module may not name: layer 4 above the backends is fine, but a
/// profile table that included a Vulkan physical-device feature would be a profile table that could
/// not be written down without a device.
[[nodiscard]] CapabilityMask capabilities_of_shipped_desktop() noexcept;

/// Why a profile could not be used as asked.
struct ProfileRefusal {
    bool refused = false;
    /// The first capability the device lacks. `RenderCapability::Count` when the refusal is not a
    /// capability one.
    RenderCapability missing = RenderCapability::Count;
    /// The feature that asked for it, or `RenderFeature::Count` when the profile itself did.
    RenderFeature asked_by = RenderFeature::Count;
    const char* message = "";
};

/// Whether this device can run this profile as declared, and what is missing if not.
[[nodiscard]] ProfileRefusal check_profile(const RendererProfile& profile,
                                           CapabilityMask device) noexcept;

/// What a profile selection produced.
struct ProfileSelection {
    RendererProfile profile;
    /// True when the requested profile was not the one returned.
    bool fell_back = false;
    /// Why. Populated whenever `fell_back` is true, and it names the capability — "the engine SHALL
    /// fall back to a profile the device supports, with a diagnostic naming the missing
    /// capability".
    ProfileRefusal refusal;
};

/// Select `requested`, or the best shipped profile this device does support. Walks down from the
/// requested profile through `HighEnd`, `Standard` and `Mobile`; `Mobile` requires nothing beyond
/// compute and indirect draw, so a device that cannot run it cannot run the renderer and the
/// selection says so rather than returning something that will fail later.
[[nodiscard]] Expected<ProfileSelection, Error> select_profile(ProfileName requested,
                                                               CapabilityMask device) noexcept;

/// Refuse a profile that is internally inconsistent: a feature enabled whose prerequisite is off, a
/// declared cost that cannot fit, a subsystem whose ladder the arbiter would reject.
[[nodiscard]] Status validate_profile(const RendererProfile& profile) noexcept;

/// Register every subsystem this profile declares against an arbiter, and configure it. The join
/// that makes a profile a thing the frame obeys rather than a table nothing reads.
[[nodiscard]] Status apply_profile(const RendererProfile& profile, BudgetArbiter& arbiter) noexcept;

/// The sum of every enabled feature's declared cost, in milliseconds.
[[nodiscard]] f32 declared_feature_cost_ms(const RendererProfile& profile) noexcept;

}  // namespace cy::rendering
