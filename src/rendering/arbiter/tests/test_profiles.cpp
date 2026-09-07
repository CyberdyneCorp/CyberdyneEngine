// The four shipped profiles: that each is internally consistent, that each fits its own budget,
// that a device that cannot serve one falls back with a diagnostic naming the capability, and that
// a profile is configuration rather than a second renderer. Task 10.2.

#include <cy/test/test.h>

#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/profiles.h>

namespace {

using cy::f32;
using cy::u32;
using cy::rendering::apply_profile;
using cy::rendering::BudgetArbiter;
using cy::rendering::BudgetSubsystem;
using cy::rendering::capabilities_of_shipped_desktop;
using cy::rendering::capability_bit;
using cy::rendering::CapabilityMask;
using cy::rendering::check_profile;
using cy::rendering::declared_feature_cost_ms;
using cy::rendering::kBudgetSubsystemCount;
using cy::rendering::kProfileCount;
using cy::rendering::kRenderFeatureCount;
using cy::rendering::named_profile;
using cy::rendering::pipeline_strengths;
using cy::rendering::ProfileName;
using cy::rendering::RenderCapability;
using cy::rendering::RendererProfile;
using cy::rendering::RenderFeature;
using cy::rendering::RenderPipelineKind;
using cy::rendering::select_profile;
using cy::rendering::validate_profile;

}  // namespace

CY_TEST_CASE("profiles: all four are internally consistent and the arbiter accepts each") {
    for (u32 index = 0; index < kProfileCount; ++index) {
        const RendererProfile profile = named_profile(static_cast<ProfileName>(index));
        CY_CHECK(validate_profile(profile));

        BudgetArbiter arbiter;
        CY_CHECK(apply_profile(profile, arbiter));

        // "Content SHALL work under any shipped pipeline" — so every profile carries every
        // subsystem that draws the scene. A profile that unregistered geometry or shadows would be
        // a profile under which content does not appear, which the specification calls a defect in
        // the profile.
        for (BudgetSubsystem subsystem :
             {BudgetSubsystem::Geometry, BudgetSubsystem::Shadows, BudgetSubsystem::MaterialEvaluation,
              BudgetSubsystem::PostProcessing}) {
            CY_CHECK(arbiter.registered(subsystem));
        }
    }
}

CY_TEST_CASE("profiles: each profile's nominal state fits its own budget with headroom") {
    // `design.md` §2.10's modelling trap, made a property of the shipped profiles rather than a
    // note in a document. The spike's first model had a 17.1 ms baseline against a 13.9 ms budget
    // and produced a 54-frame limit cycle that looked like a control-law defect and was a content
    // defect. A profile whose authored state does not fit its own budget is a renderer that begins
    // every session degrading, and the arbiter gets blamed for the profile.
    for (u32 index = 0; index < kProfileCount; ++index) {
        const RendererProfile profile = named_profile(static_cast<ProfileName>(index));
        f32 nominal = 0.0F;
        for (u32 slot = 0; slot < kBudgetSubsystemCount; ++slot) {
            if (profile.registered[slot]) {
                nominal += profile.subsystems[slot].base_cost_ms;
            }
        }
        const f32 allocatable = profile.arbiter.frame_budget_ms - profile.arbiter.non_allocatable_ms;
        CY_CHECK_LT(nominal, allocatable);
        // And the declared feature costs, which are what `validate_configuration` sums, fit too.
        CY_CHECK_LT(declared_feature_cost_ms(profile), profile.arbiter.frame_budget_ms);
    }
}

CY_TEST_CASE("profiles: a device missing a capability falls back, and the diagnostic names it") {
    // Ray query is the one this engine actually lacks today: the RTX 5060 has it and the Vulkan
    // backend does not request it. `high-end` enables ray-traced reflections, so a device without
    // it cannot run `high-end` as declared.
    const CapabilityMask without_ray_query = capabilities_of_shipped_desktop();
    CY_REQUIRE_EQ(without_ray_query & capability_bit(RenderCapability::RayQuery), 0U);

    const auto selection = select_profile(ProfileName::HighEnd, without_ray_query);
    CY_REQUIRE(selection);
    CY_CHECK(selection.value().fell_back);
    CY_CHECK_EQ(selection.value().refusal.missing, RenderCapability::RayQuery);
    CY_CHECK_EQ(selection.value().refusal.asked_by, RenderFeature::RayTracedReflections);
    // It fell back to a profile this device really can run.
    CY_CHECK_FALSE(check_profile(selection.value().profile, without_ray_query).refused);
    CY_CHECK_EQ(selection.value().profile.pipeline, RenderPipelineKind::ForwardPlus);

    // With the capability present it is used as asked, and nothing is reported.
    const auto served = select_profile(
        ProfileName::HighEnd, without_ray_query | capability_bit(RenderCapability::RayQuery));
    CY_REQUIRE(served);
    CY_CHECK_FALSE(served.value().fell_back);
    CY_CHECK_EQ(served.value().profile.pipeline, RenderPipelineKind::VisibilityBuffer);
}

CY_TEST_CASE("profiles: a device that cannot run mobile is told so, not given something broken") {
    // Mobile needs only compute shaders and indirect draw. A device without those cannot run the
    // renderer at all, and saying so is more useful than returning a profile that fails at the
    // first dispatch.
    CY_CHECK_FALSE(select_profile(ProfileName::Standard, 0U));
    CY_CHECK(select_profile(ProfileName::Standard,
                            capability_bit(RenderCapability::ComputeShaders) |
                                capability_bit(RenderCapability::IndirectDraw)));
}

CY_TEST_CASE("profiles: a profile whose feature prerequisites are broken is refused") {
    RendererProfile profile = named_profile(ProfileName::HighEnd);
    // Temporal upscaling reconstructs from the history TAA turns on. Turning TAA off and leaving
    // upscaling on is exactly "a profile requiring content changes", and it is caught here rather
    // than as a frame with no history.
    profile.features[static_cast<u32>(RenderFeature::TemporalAntialiasing)].enabled = false;
    CY_CHECK_FALSE(validate_profile(profile));
}

CY_TEST_CASE("profiles: every pipeline states its strengths rather than a ranking") {
    for (u32 index = 0; index < static_cast<u32>(RenderPipelineKind::Count); ++index) {
        const char* text = pipeline_strengths(static_cast<RenderPipelineKind>(index));
        CY_REQUIRE(text != nullptr);
        CY_CHECK(text[0] != '\0');
    }
    // The two the specification contrasts differ in what they are good at, not in rank.
    CY_CHECK_NE(pipeline_strengths(RenderPipelineKind::ForwardPlus),
                pipeline_strengths(RenderPipelineKind::VisibilityBuffer));
}

CY_TEST_CASE("profiles: a project's own profile needs no engine change") {
    // "Projects SHALL be able to define additional profiles" and "a project SHALL do so by
    // configuration, without engine modification". The proof is that a literal of the same type
    // goes through the same three functions with the same results, and appears in no enumeration.
    RendererProfile mine;
    mine.name = "a-project's-own";
    mine.pipeline = RenderPipelineKind::Custom;
    mine.arbiter.frame_budget_ms = 8.30F;  // 120 Hz
    mine.arbiter.non_allocatable_ms = 0.90F;
    mine.registered[static_cast<u32>(BudgetSubsystem::Geometry)] = true;
    auto& geometry = mine.subsystems[static_cast<u32>(BudgetSubsystem::Geometry)];
    geometry.subsystem = BudgetSubsystem::Geometry;
    geometry.base_cost_ms = 3.0F;
    geometry.reserved_minimum_ms = 0.8F;
    geometry.ladder.positions = 2;
    geometry.ladder.relative_cost[0] = 1.0F;
    geometry.ladder.relative_cost[1] = 0.6F;

    CY_CHECK(validate_profile(mine));
    CY_CHECK_FALSE(check_profile(mine, capabilities_of_shipped_desktop()).refused);
    BudgetArbiter arbiter;
    CY_CHECK(apply_profile(mine, arbiter));
    CY_CHECK(arbiter.registered(BudgetSubsystem::Geometry));
}
