#include <cy/rendering/arbiter/profiles.h>

namespace cy::rendering {
namespace {

/// Which feature a feature needs. The table is the whole of "a profile requiring content changes
/// SHALL be treated as a defect in the profile" that can be checked mechanically: an enabled
/// feature whose prerequisite is off is a scene that renders differently for a reason nobody wrote
/// down. `RenderFeature::Count` means "nothing".
struct FeaturePrerequisite {
    RenderFeature feature;
    RenderFeature needs;
};

constexpr FeaturePrerequisite kPrerequisites[] = {
    // Temporal upscaling reconstructs from history the temporal framework owns, and TAA is where
    // that framework is turned on. `rendering-post-processing` makes them alternatives in the same
    // slot of the chain, not independent switches.
    {RenderFeature::TemporalUpscaling, RenderFeature::TemporalAntialiasing},
    // Ray-traced reflections are a tier OF reflections; the screen-space tier is the fallback the
    // hybrid resolve blends against, and `rendering-global-illumination` has no path that is
    // hardware-only.
    {RenderFeature::RayTracedReflections, RenderFeature::ScreenSpaceReflections},
};

void set_ladder(SubsystemDeclaration& declaration, u8 positions, const f32 (&costs)[4]) noexcept {
    declaration.ladder.positions = positions;
    for (u8 index = 0; index < positions && index < 4U; ++index) {
        declaration.ladder.relative_cost[index] = costs[index];
    }
}

void declare_subsystem(RendererProfile& profile, BudgetSubsystem subsystem, u32 reduction_order,
                       f32 base_cost_ms, f32 reserved_minimum_ms, f32 resolution_sensitivity,
                       u8 positions, const f32 (&costs)[4]) noexcept {
    const auto index = static_cast<u32>(subsystem);
    SubsystemDeclaration& declaration = profile.subsystems[index];
    declaration.subsystem = subsystem;
    declaration.reduction_order = reduction_order;
    declaration.base_cost_ms = base_cost_ms;
    declaration.reserved_minimum_ms = reserved_minimum_ms;
    declaration.resolution_sensitivity = resolution_sensitivity;
    set_ladder(declaration, positions, costs);
    profile.registered[index] = true;
}

void enable(RendererProfile& profile, RenderFeature feature, u8 quality, f32 cost_ms,
            CapabilityMask capabilities) noexcept {
    FeatureSetting& setting = profile.features[static_cast<u32>(feature)];
    setting.enabled = true;
    setting.quality_level = quality;
    setting.declared_cost_ms = cost_ms;
    setting.requires_capabilities = capabilities;
}

constexpr CapabilityMask kBaseline =
    capability_bit(RenderCapability::ComputeShaders) | capability_bit(RenderCapability::IndirectDraw);

/// The seven subsystems, their declared reduction order, and the ladder each is priced on. The
/// order is `rendering-architecture`'s own argument — "the VFX and post-processing controllers
/// SHALL NOT independently reduce quality for a cost they did not incur" is about who decides, and
/// this is about who goes first: the additive effects before the things that carry the scene.
///
/// Post-processing (0) and VFX (1) reduce first, reflections (2) and global illumination (3) next,
/// shadows (4), then material evaluation (5) and geometry (6) last. Geometry last because geometry
/// is what the frame is OF; a viewer reads thinning geometry as content going missing and reads a
/// softer bloom as nothing at all.
///
/// THE DECLARED BASE COSTS SUM TO 12.15 ms AGAINST A 12.70 ms ALLOCATABLE BUDGET, AND THAT IS
/// LOAD-BEARING. `design.md` §2.10 records it as a modelling trap rather than a design one: the
/// spike's first model had a 17.1 ms baseline against a 13.9 ms budget and produced a 54-frame
/// limit cycle that looked like a control-law defect and was a content defect. A profile whose
/// nominal state does not fit its own budget is a renderer that begins every session degrading,
/// and the arbiter gets blamed for the profile.
void declare_standard_subsystems(RendererProfile& profile, f32 scale) noexcept {
    declare_subsystem(profile, BudgetSubsystem::PostProcessing, 0, 1.50F * scale, 0.25F, 1.0F, 4,
                      {1.00F, 0.72F, 0.50F, 0.34F});
    declare_subsystem(profile, BudgetSubsystem::Vfx, 1, 1.00F * scale, 0.15F, 0.8F, 4,
                      {1.00F, 0.70F, 0.46F, 0.28F});
    declare_subsystem(profile, BudgetSubsystem::Reflections, 2, 1.30F * scale, 0.20F, 1.0F, 4,
                      {1.00F, 0.66F, 0.42F, 0.24F});
    // GI's caches update on their own grid, so only part of the cost follows the pixel count.
    declare_subsystem(profile, BudgetSubsystem::GlobalIllumination, 3, 1.70F * scale, 0.30F, 0.45F,
                      4, {1.00F, 0.70F, 0.48F, 0.30F});
    // Shadow pages are rendered at their own resolution; the receiver-side lookup is the part that
    // follows the target.
    declare_subsystem(profile, BudgetSubsystem::Shadows, 4, 2.25F * scale, 0.45F, 0.35F, 4,
                      {1.00F, 0.74F, 0.55F, 0.40F});
    declare_subsystem(profile, BudgetSubsystem::MaterialEvaluation, 5, 1.95F * scale, 0.60F, 1.0F,
                      3, {1.00F, 0.78F, 0.60F, 0.60F});
    // Virtual geometry's traversal is per cluster and per instance far more than per pixel.
    declare_subsystem(profile, BudgetSubsystem::Geometry, 6, 2.45F * scale, 0.90F, 0.30F, 4,
                      {1.00F, 0.80F, 0.64F, 0.52F});
}

RendererProfile make_mobile() noexcept {
    RendererProfile profile;
    profile.name = "mobile";
    profile.pipeline = RenderPipelineKind::Mobile;
    profile.arbiter.frame_budget_ms = 16.60F;  // 60 Hz with a margin
    profile.arbiter.non_allocatable_ms = 2.20F;
    profile.requires_capabilities = kBaseline;
    declare_standard_subsystems(profile, 0.95F);
    enable(profile, RenderFeature::AmbientOcclusion, 2, 0.30F, kBaseline);
    enable(profile, RenderFeature::Decals, 1, 0.25F, kBaseline);
    enable(profile, RenderFeature::VirtualTexturing, 1, 0.35F, kBaseline);
    enable(profile, RenderFeature::TemporalAntialiasing, 2, 0.45F, kBaseline);
    enable(profile, RenderFeature::Bloom, 2, 0.20F, kBaseline);
    return profile;
}

RendererProfile make_standard() noexcept {
    RendererProfile profile;
    profile.name = "standard";
    profile.pipeline = RenderPipelineKind::ForwardPlus;
    profile.arbiter.frame_budget_ms = 13.90F;
    profile.arbiter.non_allocatable_ms = 1.20F;
    profile.requires_capabilities = kBaseline;
    declare_standard_subsystems(profile, 1.0F);
    enable(profile, RenderFeature::AmbientOcclusion, 1, 0.35F, kBaseline);
    enable(profile, RenderFeature::ScreenSpaceReflections, 1, 0.55F, kBaseline);
    enable(profile, RenderFeature::GlobalIllumination, 2, 1.10F, kBaseline);
    enable(profile, RenderFeature::Decals, 0, 0.30F, kBaseline);
    enable(profile, RenderFeature::VirtualShadows, 1, 1.20F, kBaseline);
    enable(profile, RenderFeature::VirtualTexturing, 0, 0.40F, kBaseline);
    enable(profile, RenderFeature::TemporalAntialiasing, 1, 0.50F, kBaseline);
    enable(profile, RenderFeature::MotionBlur, 1, 0.25F, kBaseline);
    enable(profile, RenderFeature::DepthOfField, 1, 0.30F, kBaseline);
    enable(profile, RenderFeature::Bloom, 1, 0.20F, kBaseline);
    return profile;
}

RendererProfile make_high_end() noexcept {
    RendererProfile profile = make_standard();
    profile.name = "high-end";
    profile.pipeline = RenderPipelineKind::VisibilityBuffer;
    profile.requires_capabilities = kBaseline | capability_bit(RenderCapability::BindlessResources);
    // NOT a fidelity multiplier: `base_cost_ms` is what the subsystem costs in MILLISECONDS on the
    // device the profile runs on, and a high-end profile runs on a faster device doing more work.
    // The two roughly cancel, which is why this is 1.0 and the fidelity difference is in the
    // features and the quality levels rather than in the clock.
    declare_standard_subsystems(profile, 1.0F);
    enable(profile, RenderFeature::AmbientOcclusion, 0, 0.45F, kBaseline);
    enable(profile, RenderFeature::ScreenSpaceReflections, 0, 0.70F, kBaseline);
    enable(profile, RenderFeature::RayTracedReflections, 1, 1.30F,
           kBaseline | capability_bit(RenderCapability::RayQuery));
    enable(profile, RenderFeature::GlobalIllumination, 1, 1.60F, kBaseline);
    enable(profile, RenderFeature::Volumetrics, 1, 0.65F, kBaseline);
    enable(profile, RenderFeature::VirtualGeometry, 0, 1.10F,
           kBaseline | capability_bit(RenderCapability::BindlessResources));
    enable(profile, RenderFeature::VirtualShadows, 0, 1.50F, kBaseline);
    enable(profile, RenderFeature::TemporalUpscaling, 1, 0.55F, kBaseline);
    return profile;
}

RendererProfile make_cinematic() noexcept {
    RendererProfile profile = make_high_end();
    profile.name = "cinematic";
    // A capture is not a 72 Hz frame. `rendering-architecture` makes pinned mode the mode a
    // cinematic capture runs in; the budget here is what the arbiter holds when it is NOT pinned,
    // which is what an editor preview of a cinematic sits at.
    profile.arbiter.frame_budget_ms = 33.30F;
    profile.arbiter.non_allocatable_ms = 1.60F;
    declare_standard_subsystems(profile, 2.20F);
    enable(profile, RenderFeature::RayTracedReflections, 0, 2.40F,
           kBaseline | capability_bit(RenderCapability::RayQuery));
    enable(profile, RenderFeature::GlobalIllumination, 0, 3.00F, kBaseline);
    enable(profile, RenderFeature::Volumetrics, 0, 1.30F, kBaseline);
    enable(profile, RenderFeature::DepthOfField, 0, 0.60F, kBaseline);
    enable(profile, RenderFeature::MotionBlur, 0, 0.45F, kBaseline);
    // A capture is reconstructed from more samples, not upscaled from fewer.
    profile.features[static_cast<u32>(RenderFeature::TemporalUpscaling)].enabled = false;
    return profile;
}

/// The order `select_profile` walks down. Descending fidelity, and `Mobile` last because it is the
/// one that requires nothing beyond the baseline.
constexpr ProfileName kFallbackOrder[] = {ProfileName::Cinematic, ProfileName::HighEnd,
                                          ProfileName::Standard, ProfileName::Mobile};

}  // namespace

const char* pipeline_name(RenderPipelineKind pipeline) noexcept {
    switch (pipeline) {
        case RenderPipelineKind::ForwardPlus:
            return "forward-plus";
        case RenderPipelineKind::VisibilityBuffer:
            return "visibility-buffer";
        case RenderPipelineKind::Mobile:
            return "mobile";
        case RenderPipelineKind::Null:
            return "null";
        case RenderPipelineKind::Custom:
            return "custom";
        case RenderPipelineKind::Count:
            break;
    }
    return "unknown";
}

const char* pipeline_strengths(RenderPipelineKind pipeline) noexcept {
    switch (pipeline) {
        case RenderPipelineKind::ForwardPlus:
            return "transparency, MSAA and varied shading models directly; cost grows with "
                   "overdraw and with material count";
        case RenderPipelineKind::VisibilityBuffer:
            return "very high geometric density and many materials; a more constrained "
                   "transparency and MSAA story";
        case RenderPipelineKind::Mobile:
            return "tile-friendly, a reduced feature set, and no full-resolution intermediate";
        case RenderPipelineKind::Null:
            return "draws nothing; what a headless test and a cook run under";
        case RenderPipelineKind::Custom:
            return "a project's own; its strengths are the project's to document";
        case RenderPipelineKind::Count:
            break;
    }
    return "unknown";
}

const char* capability_name(RenderCapability capability) noexcept {
    switch (capability) {
        case RenderCapability::ComputeShaders:
            return "compute-shaders";
        case RenderCapability::IndirectDraw:
            return "indirect-draw";
        case RenderCapability::BindlessResources:
            return "bindless-resources";
        case RenderCapability::RayQuery:
            return "ray-query";
        case RenderCapability::MeshShaders:
            return "mesh-shaders";
        case RenderCapability::SparseResources:
            return "sparse-resources";
        case RenderCapability::Count:
            break;
    }
    return "unknown";
}

const char* feature_name(RenderFeature feature) noexcept {
    switch (feature) {
        case RenderFeature::AmbientOcclusion:
            return "ambient-occlusion";
        case RenderFeature::ScreenSpaceReflections:
            return "screen-space-reflections";
        case RenderFeature::RayTracedReflections:
            return "ray-traced-reflections";
        case RenderFeature::GlobalIllumination:
            return "global-illumination";
        case RenderFeature::Volumetrics:
            return "volumetrics";
        case RenderFeature::Decals:
            return "decals";
        case RenderFeature::VirtualGeometry:
            return "virtual-geometry";
        case RenderFeature::VirtualShadows:
            return "virtual-shadows";
        case RenderFeature::VirtualTexturing:
            return "virtual-texturing";
        case RenderFeature::TemporalAntialiasing:
            return "temporal-antialiasing";
        case RenderFeature::TemporalUpscaling:
            return "temporal-upscaling";
        case RenderFeature::MotionBlur:
            return "motion-blur";
        case RenderFeature::DepthOfField:
            return "depth-of-field";
        case RenderFeature::Bloom:
            return "bloom";
        case RenderFeature::DebugVisualisation:
            return "debug-visualisation";
        case RenderFeature::Count:
            break;
    }
    return "unknown";
}

const char* profile_name(ProfileName profile) noexcept {
    switch (profile) {
        case ProfileName::Mobile:
            return "mobile";
        case ProfileName::Standard:
            return "standard";
        case ProfileName::HighEnd:
            return "high-end";
        case ProfileName::Cinematic:
            return "cinematic";
        case ProfileName::Count:
            break;
    }
    return "unknown";
}

RendererProfile named_profile(ProfileName profile) noexcept {
    switch (profile) {
        case ProfileName::Mobile:
            return make_mobile();
        case ProfileName::HighEnd:
            return make_high_end();
        case ProfileName::Cinematic:
            return make_cinematic();
        case ProfileName::Standard:
        case ProfileName::Count:
            break;
    }
    return make_standard();
}

CapabilityMask capabilities_of_shipped_desktop() noexcept {
    // What the Vulkan backend actually reports today. Ray query is deliberately absent: the device
    // this engine is developed on has it, and `cy::rhi`'s Vulkan backend does not request it — see
    // `src/rendering/gi/README.md`. A capability table that claimed it would make the hardware
    // tracing tier look available and fail at the dispatch.
    return kBaseline | capability_bit(RenderCapability::BindlessResources) |
           capability_bit(RenderCapability::SparseResources);
}

f32 declared_feature_cost_ms(const RendererProfile& profile) noexcept {
    f32 total = 0.0F;
    for (u32 index = 0; index < kRenderFeatureCount; ++index) {
        if (profile.features[index].enabled) {
            total += profile.features[index].declared_cost_ms;
        }
    }
    return total;
}

ProfileRefusal check_profile(const RendererProfile& profile, CapabilityMask device) noexcept {
    ProfileRefusal refusal;
    for (u32 bit = 0; bit < static_cast<u32>(RenderCapability::Count); ++bit) {
        const CapabilityMask mask = static_cast<CapabilityMask>(1U) << bit;
        if ((profile.requires_capabilities & mask) != 0U && (device & mask) == 0U) {
            refusal.refused = true;
            refusal.missing = static_cast<RenderCapability>(bit);
            refusal.message = "the profile itself requires a capability this device lacks";
            return refusal;
        }
    }
    for (u32 index = 0; index < kRenderFeatureCount; ++index) {
        const FeatureSetting& setting = profile.features[index];
        if (!setting.enabled) {
            continue;
        }
        for (u32 bit = 0; bit < static_cast<u32>(RenderCapability::Count); ++bit) {
            const CapabilityMask mask = static_cast<CapabilityMask>(1U) << bit;
            if ((setting.requires_capabilities & mask) != 0U && (device & mask) == 0U) {
                refusal.refused = true;
                refusal.missing = static_cast<RenderCapability>(bit);
                refusal.asked_by = static_cast<RenderFeature>(index);
                refusal.message = "an enabled feature requires a capability this device lacks";
                return refusal;
            }
        }
    }
    return refusal;
}

Status validate_profile(const RendererProfile& profile) noexcept {
    BudgetArbiter probe;
    if (auto configured = probe.configure(profile.arbiter); !configured) {
        return configured;
    }
    for (const FeaturePrerequisite& rule : kPrerequisites) {
        const bool enabled = profile.features[static_cast<u32>(rule.feature)].enabled;
        const bool prerequisite = profile.features[static_cast<u32>(rule.needs)].enabled;
        if (enabled && !prerequisite) {
            return fail(ErrorCode::InvalidArgument,
                        "validate_profile: an enabled feature's prerequisite feature is disabled");
        }
    }
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (!profile.registered[index]) {
            continue;
        }
        SubsystemController controller;
        if (auto accepted = controller.declare(profile.subsystems[index]); !accepted) {
            return accepted;
        }
    }
    return {};
}

Expected<ProfileSelection, Error> select_profile(ProfileName requested,
                                                 CapabilityMask device) noexcept {
    if (requested >= ProfileName::Count) {
        return fail(ErrorCode::InvalidArgument, "select_profile: profile name is out of range");
    }
    ProfileSelection selection;
    selection.profile = named_profile(requested);
    selection.refusal = check_profile(selection.profile, device);
    if (!selection.refusal.refused) {
        return selection;
    }

    // Walk down from the requested profile. The first refusal is the one reported, because it is
    // the one that names what the device is actually missing; a later profile's refusal would name
    // something the caller never asked for.
    const ProfileRefusal reported = selection.refusal;
    bool below = false;
    for (ProfileName candidate : kFallbackOrder) {
        if (!below) {
            below = candidate == requested;
            continue;
        }
        RendererProfile profile = named_profile(candidate);
        if (!check_profile(profile, device).refused) {
            selection.profile = profile;
            selection.fell_back = true;
            selection.refusal = reported;
            return selection;
        }
    }
    return fail(ErrorCode::Unsupported,
                "select_profile: this device cannot run any shipped renderer profile, including "
                "mobile, which needs only compute shaders and indirect draw");
}

Status apply_profile(const RendererProfile& profile, BudgetArbiter& arbiter) noexcept {
    if (auto valid = validate_profile(profile); !valid) {
        return valid;
    }
    if (auto configured = arbiter.configure(profile.arbiter); !configured) {
        return configured;
    }
    for (u32 index = 0; index < kBudgetSubsystemCount; ++index) {
        if (!profile.registered[index]) {
            continue;
        }
        if (auto declared = arbiter.declare(profile.subsystems[index]); !declared) {
            return declared;
        }
    }
    return {};
}

}  // namespace cy::rendering
