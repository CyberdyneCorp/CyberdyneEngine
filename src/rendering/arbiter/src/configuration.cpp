#include <cy/rendering/arbiter/configuration.h>

namespace cy::rendering {
namespace {

/// Pairs that cannot both be enabled. Two features occupying one slot of the post chain are the
/// only real exclusions the engine has today, and naming them here rather than resolving them
/// silently is the difference between a configuration that is refused and a configuration whose
/// second entry is quietly ignored.
struct ExclusivePair {
    RenderFeature first;
    RenderFeature second;
    const char* why;
};

constexpr ExclusivePair kExclusions[] = {
    // `rendering-post-processing` step 5 is ONE stage: either the frame is antialiased at output
    // resolution or it is reconstructed from a lower one. Both enabled is two consumers of one
    // history with two different output resolutions.
    {RenderFeature::TemporalUpscaling, RenderFeature::MotionBlur,
     "temporal upscaling reconstructs from history that motion blur has already smeared; run one "
     "or the other"},
};

/// Feature prerequisites, restated here because `validate_configuration` reports them as a list of
/// issues rather than refusing at the first, which `validate_profile` does. The two agree by test
/// (`test_configuration.cpp`: 'the two prerequisite tables agree').
struct Prerequisite {
    RenderFeature feature;
    RenderFeature needs;
};

constexpr Prerequisite kPrerequisites[] = {
    {RenderFeature::TemporalUpscaling, RenderFeature::TemporalAntialiasing},
    {RenderFeature::RayTracedReflections, RenderFeature::ScreenSpaceReflections},
};

void apply_feature_layer(RendererProfile& profile, const bool (&stated)[kRenderFeatureCount],
                         const FeatureSetting (&settings)[kRenderFeatureCount]) noexcept {
    for (u32 index = 0; index < kRenderFeatureCount; ++index) {
        if (stated[index]) {
            profile.features[index] = settings[index];
        }
    }
}

void push_issue(Array<ConfigurationIssue>& out, const ConfigurationIssue& issue,
                Status& status) noexcept {
    if (auto placed = out.push_back(issue); !placed) {
        status = placed;
    }
}

void push_difference(Array<ConfigurationDifference>& out, const ConfigurationDifference& entry,
                     Status& status) noexcept {
    if (auto placed = out.push_back(entry); !placed) {
        status = placed;
    }
}

void diff_scalar(Array<ConfigurationDifference>& out, ConfigurationField field, f64 before,
                 f64 after, Status& status) noexcept {
    if (before == after) {
        return;
    }
    ConfigurationDifference entry;
    entry.field = field;
    entry.from = before;
    entry.to = after;
    push_difference(out, entry, status);
}

void diff_features(const RenderConfiguration& before, const RenderConfiguration& after,
                   Array<ConfigurationDifference>& out, Status& status) noexcept {
    for (u32 index = 0; index < kRenderFeatureCount; ++index) {
        const FeatureSetting& lhs = before.features[index];
        const FeatureSetting& rhs = after.features[index];
        const bool lhs_stated = before.overrides_feature[index];
        const bool rhs_stated = after.overrides_feature[index];
        if (!lhs_stated && !rhs_stated) {
            continue;
        }
        const auto feature = static_cast<RenderFeature>(index);
        const auto emit = [&](ConfigurationField field, f64 from, f64 to) {
            if (from == to) {
                return;
            }
            ConfigurationDifference entry;
            entry.field = field;
            entry.feature = feature;
            entry.from = from;
            entry.to = to;
            push_difference(out, entry, status);
        };
        emit(ConfigurationField::FeatureEnabled, lhs_stated && lhs.enabled ? 1.0 : 0.0,
             rhs_stated && rhs.enabled ? 1.0 : 0.0);
        emit(ConfigurationField::FeatureQualityLevel,
             lhs_stated ? static_cast<f64>(lhs.quality_level) : -1.0,
             rhs_stated ? static_cast<f64>(rhs.quality_level) : -1.0);
        emit(ConfigurationField::FeatureDeclaredCostMs,
             lhs_stated ? static_cast<f64>(lhs.declared_cost_ms) : -1.0,
             rhs_stated ? static_cast<f64>(rhs.declared_cost_ms) : -1.0);
    }
}

void diff_platforms(const RenderConfiguration& before, const RenderConfiguration& after,
                    Array<ConfigurationDifference>& out, Status& status) noexcept {
    for (u32 index = 0; index < kConfigurationPlatformCount; ++index) {
        const PlatformOverride& lhs = before.platform[index];
        const PlatformOverride& rhs = after.platform[index];
        const auto platform = static_cast<ConfigurationPlatform>(index);
        const auto emit = [&](ConfigurationField field, RenderFeature feature, f64 from, f64 to) {
            if (from == to) {
                return;
            }
            ConfigurationDifference entry;
            entry.field = field;
            entry.feature = feature;
            entry.platform = platform;
            entry.from = from;
            entry.to = to;
            push_difference(out, entry, status);
        };
        emit(ConfigurationField::PlatformOverrideActive, RenderFeature::Count, lhs.active ? 1.0 : 0.0,
             rhs.active ? 1.0 : 0.0);
        emit(ConfigurationField::PlatformOverrideProfile, RenderFeature::Count,
             lhs.overrides_profile ? static_cast<f64>(lhs.profile) : -1.0,
             rhs.overrides_profile ? static_cast<f64>(rhs.profile) : -1.0);
        for (u32 slot = 0; slot < kRenderFeatureCount; ++slot) {
            const bool lhs_stated = lhs.overrides_feature[slot];
            const bool rhs_stated = rhs.overrides_feature[slot];
            emit(ConfigurationField::PlatformOverrideFeature, static_cast<RenderFeature>(slot),
                 lhs_stated && lhs.features[slot].enabled ? 1.0 : 0.0,
                 rhs_stated && rhs.features[slot].enabled ? 1.0 : 0.0);
        }
    }
}

void check_feature_rules(const RendererProfile& resolved, ConfigurationPlatform platform,
                         Array<ConfigurationIssue>& out, Status& status) noexcept {
    for (const Prerequisite& rule : kPrerequisites) {
        if (!resolved.features[static_cast<u32>(rule.feature)].enabled ||
            resolved.features[static_cast<u32>(rule.needs)].enabled) {
            continue;
        }
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::PrerequisiteDisabled;
        issue.platform = platform;
        issue.feature = rule.feature;
        issue.other = rule.needs;
        issue.message = "an enabled feature's prerequisite feature is disabled";
        push_issue(out, issue, status);
    }
    for (const ExclusivePair& rule : kExclusions) {
        if (!resolved.features[static_cast<u32>(rule.first)].enabled ||
            !resolved.features[static_cast<u32>(rule.second)].enabled) {
            continue;
        }
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::MutuallyExclusive;
        issue.platform = platform;
        issue.feature = rule.first;
        issue.other = rule.second;
        issue.message = rule.why;
        push_issue(out, issue, status);
    }
}

}  // namespace

const char* configuration_platform_name(ConfigurationPlatform platform) noexcept {
    switch (platform) {
        case ConfigurationPlatform::Desktop:
            return "desktop";
        case ConfigurationPlatform::Handheld:
            return "handheld";
        case ConfigurationPlatform::Console:
            return "console";
        case ConfigurationPlatform::Count:
            break;
    }
    return "unknown";
}

const char* configuration_finding_name(ConfigurationFinding finding) noexcept {
    switch (finding) {
        case ConfigurationFinding::UnknownVersion:
            return "unknown-version";
        case ConfigurationFinding::PrerequisiteDisabled:
            return "prerequisite-disabled";
        case ConfigurationFinding::MutuallyExclusive:
            return "mutually-exclusive";
        case ConfigurationFinding::OverSubscribedBudget:
            return "over-subscribed-budget";
        case ConfigurationFinding::MissingCapability:
            return "missing-capability";
        case ConfigurationFinding::UnpriceableLadder:
            return "unpriceable-ladder";
        case ConfigurationFinding::Count:
            break;
    }
    return "unknown";
}

const char* configuration_field_name(ConfigurationField field) noexcept {
    switch (field) {
        case ConfigurationField::Version:
            return "version";
        case ConfigurationField::Profile:
            return "profile";
        case ConfigurationField::Pipeline:
            return "pipeline";
        case ConfigurationField::FrameBudgetMs:
            return "frame-budget-ms";
        case ConfigurationField::FeatureEnabled:
            return "feature.enabled";
        case ConfigurationField::FeatureQualityLevel:
            return "feature.quality-level";
        case ConfigurationField::FeatureDeclaredCostMs:
            return "feature.declared-cost-ms";
        case ConfigurationField::BudgetShare:
            return "budget-share";
        case ConfigurationField::PlatformOverrideActive:
            return "platform.active";
        case ConfigurationField::PlatformOverrideProfile:
            return "platform.profile";
        case ConfigurationField::PlatformOverrideFeature:
            return "platform.feature";
        case ConfigurationField::Count:
            break;
    }
    return "unknown";
}

Expected<RendererProfile, Error> resolve_configuration(const RenderConfiguration& configuration,
                                                       ConfigurationPlatform platform) noexcept {
    if (configuration.version == 0 || configuration.version > kConfigurationVersion) {
        return fail(ErrorCode::Unsupported,
                    "resolve_configuration: the configuration version is newer than this engine "
                    "knows how to read");
    }
    if (platform >= ConfigurationPlatform::Count) {
        return fail(ErrorCode::InvalidArgument, "resolve_configuration: platform out of range");
    }
    const PlatformOverride& override_layer = configuration.platform[static_cast<u32>(platform)];

    ProfileName base = configuration.profile;
    if (override_layer.active && override_layer.overrides_profile) {
        base = override_layer.profile;
    }
    if (base >= ProfileName::Count) {
        return fail(ErrorCode::InvalidArgument, "resolve_configuration: profile out of range");
    }
    RendererProfile profile = named_profile(base);

    if (configuration.overrides_pipeline) {
        profile.pipeline = configuration.pipeline;
    }
    if (configuration.overrides_frame_budget) {
        profile.arbiter.frame_budget_ms = configuration.frame_budget_ms;
    }
    apply_feature_layer(profile, configuration.overrides_feature, configuration.features);

    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        const f32 share = configuration.budget_share[index];
        if (share > 0.0F && profile.registered[index]) {
            // A share is a fraction of the ALLOCATABLE budget, and it lands on `base_cost_ms`
            // because that is the only absolute a declaration carries. The ladder's ratios are
            // untouched: a project may say what a subsystem should start from and may not say what
            // its own quality steps cost.
            const f32 allocatable =
                profile.arbiter.frame_budget_ms - profile.arbiter.non_allocatable_ms;
            profile.subsystems[index].base_cost_ms = allocatable * share;
        }
    }

    if (override_layer.active) {
        if (override_layer.overrides_pipeline) {
            profile.pipeline = override_layer.pipeline;
        }
        if (override_layer.overrides_frame_budget) {
            profile.arbiter.frame_budget_ms = override_layer.frame_budget_ms;
        }
        apply_feature_layer(profile, override_layer.overrides_feature, override_layer.features);
    }
    return profile;
}

Status validate_configuration(const RenderConfiguration& configuration,
                              ConfigurationPlatform platform, CapabilityMask device,
                              Array<ConfigurationIssue>& out) noexcept {
    Status status;
    if (configuration.version == 0 || configuration.version > kConfigurationVersion) {
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::UnknownVersion;
        issue.platform = platform;
        issue.message = "the configuration version is newer than this engine knows how to read";
        push_issue(out, issue, status);
        return status;
    }

    auto resolved = resolve_configuration(configuration, platform);
    if (!resolved) {
        return fail(resolved.error().code, resolved.error().message);
    }
    const RendererProfile& profile = resolved.value();

    check_feature_rules(profile, platform, out, status);

    const f32 declared = declared_feature_cost_ms(profile);
    if (declared > profile.arbiter.frame_budget_ms) {
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::OverSubscribedBudget;
        issue.platform = platform;
        issue.declared_ms = declared;
        issue.budget_ms = profile.arbiter.frame_budget_ms;
        issue.message = "the sum of enabled features' declared costs exceeds the frame budget";
        push_issue(out, issue, status);
    }

    if (const ProfileRefusal refusal = check_profile(profile, device); refusal.refused) {
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::MissingCapability;
        issue.platform = platform;
        issue.capability = refusal.missing;
        issue.feature = refusal.asked_by;
        issue.message = refusal.message;
        push_issue(out, issue, status);
    }

    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (!profile.registered[index]) {
            continue;
        }
        SubsystemController controller;
        if (controller.declare(profile.subsystems[index])) {
            continue;
        }
        ConfigurationIssue issue;
        issue.finding = ConfigurationFinding::UnpriceableLadder;
        issue.platform = platform;
        issue.message = "a subsystem's declared ladder is not one the arbiter can price";
        push_issue(out, issue, status);
    }
    return status;
}

Status diff_configurations(const RenderConfiguration& before, const RenderConfiguration& after,
                           Array<ConfigurationDifference>& out) noexcept {
    Status status;
    diff_scalar(out, ConfigurationField::Version, before.version, after.version, status);
    diff_scalar(out, ConfigurationField::Profile, static_cast<f64>(before.profile),
                static_cast<f64>(after.profile), status);
    diff_scalar(out, ConfigurationField::Pipeline,
                before.overrides_pipeline ? static_cast<f64>(before.pipeline) : -1.0,
                after.overrides_pipeline ? static_cast<f64>(after.pipeline) : -1.0, status);
    diff_scalar(out, ConfigurationField::FrameBudgetMs,
                before.overrides_frame_budget ? static_cast<f64>(before.frame_budget_ms) : -1.0,
                after.overrides_frame_budget ? static_cast<f64>(after.frame_budget_ms) : -1.0,
                status);

    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (before.budget_share[index] == after.budget_share[index]) {
            continue;
        }
        ConfigurationDifference entry;
        entry.field = ConfigurationField::BudgetShare;
        entry.subsystem = static_cast<BudgetSubsystem>(index);
        entry.from = static_cast<f64>(before.budget_share[index]);
        entry.to = static_cast<f64>(after.budget_share[index]);
        push_difference(out, entry, status);
    }

    diff_features(before, after, out, status);
    diff_platforms(before, after, out, status);
    return status;
}

}  // namespace cy::rendering
