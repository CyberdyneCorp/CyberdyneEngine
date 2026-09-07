// The rendering configuration asset: that it layers, that it validates the three things the
// specification names, and that a change to it appears as a diff. Task 10.2.

#include <cy/test/test.h>

#include <cy/core/memory/array.h>
#include <cy/rendering/arbiter/configuration.h>

namespace {

using cy::Array;
using cy::f32;
using cy::u32;
using cy::rendering::capabilities_of_shipped_desktop;
using cy::rendering::capability_bit;
using cy::rendering::ConfigurationDifference;
using cy::rendering::ConfigurationField;
using cy::rendering::ConfigurationFinding;
using cy::rendering::ConfigurationIssue;
using cy::rendering::ConfigurationPlatform;
using cy::rendering::diff_configurations;
using cy::rendering::kConfigurationVersion;
using cy::rendering::ProfileName;
using cy::rendering::RenderCapability;
using cy::rendering::RenderConfiguration;
using cy::rendering::RenderFeature;
using cy::rendering::RenderPipelineKind;
using cy::rendering::resolve_configuration;
using cy::rendering::validate_configuration;

[[nodiscard]] bool has(const Array<ConfigurationIssue>& issues,
                       ConfigurationFinding finding) noexcept {
    for (cy::usize index = 0; index < issues.size(); ++index) {
        if (issues[index].finding == finding) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_field(const Array<ConfigurationDifference>& diff,
                             ConfigurationField field) noexcept {
    for (cy::usize index = 0; index < diff.size(); ++index) {
        if (diff[index].field == field) {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("configuration: the layers apply base, then the asset, then the platform") {
    RenderConfiguration configuration;
    configuration.profile = ProfileName::Standard;
    configuration.overrides_frame_budget = true;
    configuration.frame_budget_ms = 12.0F;

    auto& handheld = configuration.platform[static_cast<u32>(ConfigurationPlatform::Handheld)];
    handheld.active = true;
    handheld.overrides_profile = true;
    handheld.profile = ProfileName::Mobile;
    handheld.overrides_feature[static_cast<u32>(RenderFeature::Bloom)] = true;
    handheld.features[static_cast<u32>(RenderFeature::Bloom)].enabled = false;

    const auto desktop = resolve_configuration(configuration, ConfigurationPlatform::Desktop);
    CY_REQUIRE(desktop);
    CY_CHECK_EQ(desktop.value().pipeline, RenderPipelineKind::ForwardPlus);
    CY_CHECK_EQ(desktop.value().arbiter.frame_budget_ms, 12.0F);
    CY_CHECK(desktop.value().features[static_cast<u32>(RenderFeature::Bloom)].enabled);

    const auto portable = resolve_configuration(configuration, ConfigurationPlatform::Handheld);
    CY_REQUIRE(portable);
    // The platform chose the base profile...
    CY_CHECK_EQ(portable.value().pipeline, RenderPipelineKind::Mobile);
    // ...the asset's own budget override still applies over it...
    CY_CHECK_EQ(portable.value().arbiter.frame_budget_ms, 12.0F);
    // ...and the platform's feature override is last.
    CY_CHECK_FALSE(portable.value().features[static_cast<u32>(RenderFeature::Bloom)].enabled);
}

CY_TEST_CASE("configuration: an over-subscribed budget is caught at configuration time") {
    RenderConfiguration configuration;
    configuration.profile = ProfileName::Standard;
    configuration.overrides_frame_budget = true;
    configuration.frame_budget_ms = 2.0F;  // less than the enabled features declare between them

    Array<ConfigurationIssue> issues;
    CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                      capabilities_of_shipped_desktop(), issues));
    CY_CHECK(has(issues, ConfigurationFinding::OverSubscribedBudget));

    for (cy::usize index = 0; index < issues.size(); ++index) {
        if (issues[index].finding != ConfigurationFinding::OverSubscribedBudget) {
            continue;
        }
        // The finding carries both numbers, so a diagnostic can say by how much.
        CY_CHECK_GT(issues[index].declared_ms, issues[index].budget_ms);
        CY_CHECK_EQ(issues[index].budget_ms, 2.0F);
    }
}

CY_TEST_CASE("configuration: unsupported, mutually exclusive and unknown-version are all reported") {
    CY_TEST_SUBCASE("a capability the device lacks") {
        RenderConfiguration configuration;
        configuration.profile = ProfileName::HighEnd;
        Array<ConfigurationIssue> issues;
        CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                          capabilities_of_shipped_desktop(), issues));
        CY_CHECK(has(issues, ConfigurationFinding::MissingCapability));
    }

    CY_TEST_SUBCASE("two features that cannot both run") {
        RenderConfiguration configuration;
        configuration.profile = ProfileName::Standard;
        configuration.overrides_feature[static_cast<u32>(RenderFeature::TemporalUpscaling)] = true;
        auto& upscaling = configuration.features[static_cast<u32>(RenderFeature::TemporalUpscaling)];
        upscaling.enabled = true;
        upscaling.declared_cost_ms = 0.5F;
        // `standard` already enables motion blur, which is the exclusion.
        Array<ConfigurationIssue> issues;
        CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                          capabilities_of_shipped_desktop(), issues));
        CY_CHECK(has(issues, ConfigurationFinding::MutuallyExclusive));
    }

    CY_TEST_SUBCASE("a prerequisite that was switched off") {
        RenderConfiguration configuration;
        configuration.profile = ProfileName::Standard;
        configuration.overrides_feature[static_cast<u32>(RenderFeature::TemporalAntialiasing)] = true;
        configuration.features[static_cast<u32>(RenderFeature::TemporalAntialiasing)].enabled = false;
        configuration.overrides_feature[static_cast<u32>(RenderFeature::TemporalUpscaling)] = true;
        configuration.features[static_cast<u32>(RenderFeature::TemporalUpscaling)].enabled = true;
        Array<ConfigurationIssue> issues;
        CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                          capabilities_of_shipped_desktop(), issues));
        CY_CHECK(has(issues, ConfigurationFinding::PrerequisiteDisabled));
    }

    CY_TEST_SUBCASE("a version this engine cannot read") {
        RenderConfiguration configuration;
        configuration.version = kConfigurationVersion + 1U;
        Array<ConfigurationIssue> issues;
        CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                          capabilities_of_shipped_desktop(), issues));
        CY_REQUIRE_EQ(issues.size(), 1U);
        CY_CHECK_EQ(issues[0].finding, ConfigurationFinding::UnknownVersion);
        // And it stops there rather than reporting a list of findings derived from fields whose
        // meaning it does not know.
        CY_CHECK_FALSE(resolve_configuration(configuration, ConfigurationPlatform::Desktop));
    }
}

CY_TEST_CASE("configuration: a shipped configuration validates clean on a shipped device") {
    RenderConfiguration configuration;
    configuration.profile = ProfileName::Standard;
    Array<ConfigurationIssue> issues;
    CY_REQUIRE(validate_configuration(configuration, ConfigurationPlatform::Desktop,
                                      capabilities_of_shipped_desktop(), issues));
    CY_CHECK_EQ(issues.size(), 0U);
}

CY_TEST_CASE("configuration: a rendering change appears as a diff of the asset") {
    RenderConfiguration before;
    before.profile = ProfileName::Standard;

    RenderConfiguration after = before;
    after.overrides_feature[static_cast<u32>(RenderFeature::GlobalIllumination)] = true;
    auto& gi = after.features[static_cast<u32>(RenderFeature::GlobalIllumination)];
    gi.enabled = true;
    gi.quality_level = 0;
    gi.declared_cost_ms = 1.90F;
    after.budget_share[static_cast<u32>(cy::rendering::BudgetSubsystem::GlobalIllumination)] = 0.18F;

    Array<ConfigurationDifference> diff;
    CY_REQUIRE(diff_configurations(before, after, diff));
    CY_CHECK(has_field(diff, ConfigurationField::FeatureQualityLevel));
    CY_CHECK(has_field(diff, ConfigurationField::FeatureDeclaredCostMs));
    CY_CHECK(has_field(diff, ConfigurationField::BudgetShare));
    // The diff names the feature, so a reviewer reads "global-illumination quality 0" rather than
    // "a file changed".
    bool named = false;
    for (cy::usize index = 0; index < diff.size(); ++index) {
        named = named || diff[index].feature == RenderFeature::GlobalIllumination;
    }
    CY_CHECK(named);

    Array<ConfigurationDifference> none;
    CY_REQUIRE(diff_configurations(before, before, none));
    CY_CHECK_EQ(none.size(), 0U);
}

CY_TEST_CASE("configuration: a budget share moves what a subsystem starts from, not its ladder") {
    RenderConfiguration configuration;
    configuration.profile = ProfileName::Standard;
    const auto slot = static_cast<u32>(cy::rendering::BudgetSubsystem::Geometry);
    configuration.budget_share[slot] = 0.30F;

    const auto resolved = resolve_configuration(configuration, ConfigurationPlatform::Desktop);
    CY_REQUIRE(resolved);
    const auto& profile = resolved.value();
    const f32 allocatable = profile.arbiter.frame_budget_ms - profile.arbiter.non_allocatable_ms;
    CY_CHECK_NEAR(profile.subsystems[slot].base_cost_ms, allocatable * 0.30F, 0.001F);

    // The ladder's ratios are the subsystem's, not the project's: a project says what a subsystem
    // should start from and never what its own quality steps cost.
    const auto shipped = cy::rendering::named_profile(ProfileName::Standard);
    for (cy::u8 position = 0; position < profile.subsystems[slot].ladder.positions; ++position) {
        CY_CHECK_EQ(profile.subsystems[slot].ladder.cost_at(position),
                    shipped.subsystems[slot].ladder.cost_at(position));
    }
}
