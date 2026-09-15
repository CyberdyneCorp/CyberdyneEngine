#include <cy/rendering/shadows/mode.h>

namespace cy::rendering {
namespace {

/// The last two rungs of the ladder, shared by every path that reaches them: virtual pages if the
/// profile has them, then the conventional atlas, then baked, then nothing.
///
/// Written once rather than per case because the alternative is five copies of the same three
/// questions, and a mode that got one of them in the wrong order would be a light that silently
/// loses its shadow on one profile and not another.
[[nodiscard]] ShadowModeSelection degrade_to_paged(ShadowModeSelection selection,
                                                   const ShadowModeProfile& profile) noexcept {
    if (profile.virtual_pages) {
        selection.selected = ShadowMode::Virtual;
        return selection;
    }
    if (profile.conventional) {
        selection.selected = ShadowMode::Conventional;
        return selection;
    }
    if (profile.baked) {
        selection.selected = ShadowMode::Baked;
        return selection;
    }
    selection.selected = ShadowMode::None;
    selection.fallback = ShadowModeFallback::NothingAvailable;
    selection.diagnostic =
        "this profile offers no traced, virtual, conventional or baked shadows, so the light has "
        "none — which is the outcome `virtual-shadows` forbids, reported rather than rendered";
    return selection;
}

}  // namespace

const char* shadow_mode_fallback_name(ShadowModeFallback fallback) noexcept {
    switch (fallback) {
        case ShadowModeFallback::None:
            return "None";
        case ShadowModeFallback::LightCastsNoShadow:
            return "LightCastsNoShadow";
        case ShadowModeFallback::NoTraceThisFrame:
            return "NoTraceThisFrame";
        case ShadowModeFallback::NoVirtualPages:
            return "NoVirtualPages";
        case ShadowModeFallback::NoBakedData:
            return "NoBakedData";
        case ShadowModeFallback::NoConventionalAtlas:
            return "NoConventionalAtlas";
        case ShadowModeFallback::NothingAvailable:
            return "NothingAvailable";
        case ShadowModeFallback::Count:
            break;
    }
    return "?";
}

ShadowModeSelection select_shadow_mode(const ShadowModeRequest& request,
                                       const ShadowModeProfile& profile) noexcept {
    ShadowModeSelection selection;
    selection.declared = request.declared;
    selection.selected = request.declared;
    selection.diagnostic = "the light got the mode it declared";

    if (!request.casts_shadow || request.declared == ShadowMode::None) {
        selection.selected = ShadowMode::None;
        selection.fallback = request.casts_shadow ? ShadowModeFallback::None
                                                  : ShadowModeFallback::LightCastsNoShadow;
        selection.diagnostic = "the light casts no shadow";
        return selection;
    }

    switch (request.declared) {
        case ShadowMode::RayTraced:
        case ShadowMode::Hybrid:
            if (profile.traced) {
                return selection;
            }
            selection.fallback = ShadowModeFallback::NoTraceThisFrame;
            selection.diagnostic =
                "no trace is available this frame, so the traced mode fell back to the paged one; "
                "`cy::rhi::Capability::RayTracing` unset is what this reads like on every device "
                "in this tree";
            return degrade_to_paged(selection, profile);
        case ShadowMode::Virtual:
            if (profile.virtual_pages) {
                return selection;
            }
            selection.fallback = ShadowModeFallback::NoVirtualPages;
            selection.diagnostic =
                "this profile has no virtual shadow pages, so the light fell back to its "
                "conventional mode rather than losing its shadow";
            return degrade_to_paged(selection, profile);
        case ShadowMode::Baked:
            if (profile.baked) {
                return selection;
            }
            selection.fallback = ShadowModeFallback::NoBakedData;
            selection.diagnostic = "no baked shadow data, so the light is rendered instead";
            return degrade_to_paged(selection, profile);
        case ShadowMode::Conventional:
            if (profile.conventional) {
                return selection;
            }
            selection.fallback = ShadowModeFallback::NoConventionalAtlas;
            selection.diagnostic =
                "this profile has no conventional shadow atlas; the paged ladder answers instead";
            return degrade_to_paged(selection, profile);
        case ShadowMode::None:
        case ShadowMode::Count:
            break;
    }
    return selection;
}

void ShadowModeLedger::record(const ShadowModeSelection& selection) noexcept {
    const auto slot = static_cast<usize>(selection.selected);
    if (slot < static_cast<usize>(ShadowMode::Count)) {
        counts[slot] += 1;
    }
    degraded += selection.degraded() ? 1U : 0U;
    lights += 1;
}

void ShadowModeLedger::reset() noexcept { *this = ShadowModeLedger{}; }

}  // namespace cy::rendering
