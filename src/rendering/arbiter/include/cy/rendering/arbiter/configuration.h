#pragma once
// The rendering configuration asset: one document naming the profile, the pipeline, the features
// and their quality levels, the budget allocations and the platform overrides. Task 10.2.
//
// `rendering-architecture` — "Render pipeline configuration".
//
// ================================================================================================
// IT IS AN ASSET, NOT SCATTERED SETTINGS, AND THE POINT OF THAT IS THE DIFF
// ================================================================================================
//
// "A project's rendering configuration SHALL be an asset, not scattered settings... The
// configuration SHALL be versioned and diffable, and changing it SHALL NOT require code."
//
// `diff_configurations()` is the requirement's own scenario — "a rendering change SHALL appear as a
// diff of the configuration asset" — made executable. It is a STRUCTURAL diff over the fields, not
// a text diff over a serialisation, because a text diff answers a different question: it reports
// that a file changed, and this reports which of eleven declared quantities changed and from what
// to what. A reviewer reading "global-illumination quality 1 -> 2, declared cost 1.60 -> 1.10 ms"
// knows what was proposed. A reviewer reading a re-serialised block does not.
//
// ================================================================================================
// AN OVER-SUBSCRIBED BUDGET IS A CONFIGURATION-TIME ERROR
// ================================================================================================
//
// "The engine SHALL validate a configuration and report combinations that are unsupported,
// mutually exclusive, or exceed the declared frame budget in the sum of their declared costs" —
// "at configuration time, not at runtime".
//
// `validate_configuration()` is where the three checks live, and the third is why `FeatureSetting`
// carries a `declared_cost_ms` at all. There is a real tension here worth naming: this milestone's
// arbiter exists precisely because declared costs are estimates and the frame is what actually
// happens. The two are not in competition — the sum of declared costs is a check that the
// configuration is *plausible*, and the arbiter is what holds the budget when it is not. A
// configuration whose declared costs already exceed the budget is one where the arbiter would begin
// every session degrading, and telling the author that at configuration time is cheaper than
// letting them discover it as a scene that never looks the way they authored it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/arbiter/profiles.h>

namespace cy::rendering {

/// The platforms an override can name. Coarse on purpose: an override is a statement about a class
/// of device, and a per-SKU table is a project's own business.
enum class ConfigurationPlatform : u8 {
    Desktop = 0,
    Handheld,
    Console,
    Count,
};

inline constexpr u32 kConfigurationPlatformCount = static_cast<u32>(ConfigurationPlatform::Count);

[[nodiscard]] const char* configuration_platform_name(ConfigurationPlatform platform) noexcept;

/// One platform's override. Every field is optional in the sense that a `false` `overrides_*` flag
/// means "inherit"; a zero value with the flag set is a real value.
///
/// WHY FLAGS AND NOT SENTINELS. A sentinel that means "not stated" is a value somebody eventually
/// wants to state — a frame budget of zero is nonsense, but a feature quality level of zero is the
/// best one, and the difference between "the handheld override sets quality 0" and "the handheld
/// override says nothing about quality" is the whole of what an override is.
struct PlatformOverride {
    bool active = false;

    bool overrides_profile = false;
    ProfileName profile = ProfileName::Standard;

    bool overrides_pipeline = false;
    RenderPipelineKind pipeline = RenderPipelineKind::ForwardPlus;

    bool overrides_frame_budget = false;
    f32 frame_budget_ms = 0.0F;

    /// Per feature: whether this override says anything, and what it says.
    bool overrides_feature[kRenderFeatureCount] = {};
    FeatureSetting features[kRenderFeatureCount] = {};
};

/// The version of the configuration format itself. Raised when a field is added or its meaning
/// changes; a configuration carrying an older one is migrated, never silently reinterpreted.
inline constexpr u32 kConfigurationVersion = 1;

/// The asset.
struct RenderConfiguration {
    u32 version = kConfigurationVersion;
    /// The base profile. Everything below is a delta on top of `named_profile(profile)`.
    ProfileName profile = ProfileName::Standard;

    bool overrides_pipeline = false;
    RenderPipelineKind pipeline = RenderPipelineKind::ForwardPlus;

    bool overrides_frame_budget = false;
    f32 frame_budget_ms = 0.0F;

    bool overrides_feature[kRenderFeatureCount] = {};
    FeatureSetting features[kRenderFeatureCount] = {};

    /// The budget allocation the project wants each subsystem to start from, as a fraction of the
    /// allocatable budget. Zero means "take the profile's". The fractions do not have to sum to
    /// one: what is left over is the slack a ladder wastes, and the arbiter measures it rather than
    /// book-keeping it (`design.md` §2.5).
    f32 budget_share[kBudgetSubsystemCount] = {};

    PlatformOverride platform[kConfigurationPlatformCount] = {};
};

/// Resolve a configuration for one platform into the profile the renderer runs. Base profile,
/// then the configuration's own overrides, then the platform's — in that order, each layer able to
/// state what the one below left alone.
[[nodiscard]] Expected<RendererProfile, Error> resolve_configuration(
    const RenderConfiguration& configuration, ConfigurationPlatform platform) noexcept;

/// What is wrong with a configuration, one finding at a time.
enum class ConfigurationFinding : u8 {
    /// The version is newer than this engine knows how to read.
    UnknownVersion = 0,
    /// A feature is enabled whose prerequisite feature is disabled.
    PrerequisiteDisabled,
    /// Two features are enabled that cannot both be.
    MutuallyExclusive,
    /// The sum of enabled features' declared costs exceeds the frame budget.
    OverSubscribedBudget,
    /// The device lacks a capability the resolved profile requires.
    MissingCapability,
    /// A subsystem's declared ladder is not one the arbiter can price.
    UnpriceableLadder,
    Count,
};

[[nodiscard]] const char* configuration_finding_name(ConfigurationFinding finding) noexcept;

struct ConfigurationIssue {
    ConfigurationFinding finding = ConfigurationFinding::UnknownVersion;
    ConfigurationPlatform platform = ConfigurationPlatform::Desktop;
    /// The feature the finding is about, or `RenderFeature::Count`.
    RenderFeature feature = RenderFeature::Count;
    /// The second feature of a mutual exclusion, or `RenderFeature::Count`.
    RenderFeature other = RenderFeature::Count;
    /// The capability that is missing, or `RenderCapability::Count`.
    RenderCapability capability = RenderCapability::Count;
    /// For `OverSubscribedBudget`: the declared sum and the budget, in milliseconds.
    f32 declared_ms = 0.0F;
    f32 budget_ms = 0.0F;
    const char* message = "";
};

/// Validate a configuration for one platform against one device. Appends every issue it finds
/// rather than returning at the first: an author fixing a configuration wants the list.
[[nodiscard]] Status validate_configuration(const RenderConfiguration& configuration,
                                            ConfigurationPlatform platform, CapabilityMask device,
                                            Array<ConfigurationIssue>& out) noexcept;

/// Which field changed, and from what to what. `feature` is `RenderFeature::Count` for the fields
/// that are not a feature's.
enum class ConfigurationField : u8 {
    Version = 0,
    Profile,
    Pipeline,
    FrameBudgetMs,
    FeatureEnabled,
    FeatureQualityLevel,
    FeatureDeclaredCostMs,
    BudgetShare,
    PlatformOverrideActive,
    PlatformOverrideProfile,
    PlatformOverrideFeature,
    Count,
};

[[nodiscard]] const char* configuration_field_name(ConfigurationField field) noexcept;

struct ConfigurationDifference {
    ConfigurationField field = ConfigurationField::Version;
    RenderFeature feature = RenderFeature::Count;
    BudgetSubsystem subsystem = BudgetSubsystem::Count;
    ConfigurationPlatform platform = ConfigurationPlatform::Count;
    /// Both readings, as numbers. An enumerator is its own value, a bool is 0 or 1, a millisecond
    /// count is itself — one field rather than a variant, because every quantity a rendering
    /// configuration holds is already a number and a variant here would be a type nobody printed.
    f64 from = 0.0;
    f64 to = 0.0;
};

/// The structural diff. Empty means the two configurations are the same document.
[[nodiscard]] Status diff_configurations(const RenderConfiguration& before,
                                         const RenderConfiguration& after,
                                         Array<ConfigurationDifference>& out) noexcept;

}  // namespace cy::rendering
