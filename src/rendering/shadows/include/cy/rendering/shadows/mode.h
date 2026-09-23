#pragma once
// WHICH SHADOW MODE A LIGHT ACTUALLY GETS, and the diagnostic when it is not the one it declared.
// M11.c task 3.5.
//
// ================================================================================================
// THE ENUMERATION EXISTED AND NOTHING CHOSE FROM IT
// ================================================================================================
//
// `ShadowMode` has been in `address_space.h` since M7 with six enumerators and `shadow_mode_name()`
// beside it, and a search of the tree for the type found the switch that names them and NOTHING
// ELSE: no light declared one, no profile constrained one, and no frame selected one. The
// requirement is two sentences and both were unimplemented —
//
//   > Each light SHALL declare a shadow mode, and the renderer profile SHALL constrain which modes
//   > are available.
//   > WHEN a device cannot support virtual shadows THEN the light SHALL fall back to its
//   > conventional mode with a diagnostic, not lose its shadow.
//
// — so this file is the second sentence made into a function, and `AssemblyView::shadow_modes` is
// the first.
//
// ================================================================================================
// WHY THE PROFILE IS AN ARGUMENT AND NOT A QUERY
// ================================================================================================
//
// `traced` is the field that moves frame to frame. Since M11.c task 2.6 the Vulkan backend derives
// `cy::rhi::Capability::RayTracing` from what the driver reports (an RTX 5060 reports it), but the
// rays still go to `cy::Bvh` on the processor, and NO CALLER IN THE TREE SETS `traced` YET — so
// a `RayTraced` or `Hybrid` light falls back in every shipped frame (M11.c task 4.4). A capability
// bit is not a trace this frame either way: a selection function that asked a device for itself
// would give a different answer inside a test than in a frame, and "the caller that knows whether a
// trace is available this frame" is the phrase the task list uses for exactly this — the knowledge
// belongs to whoever assembled the frame, not to this file.
//
// NOTHING HERE ALLOCATES, LOGS OR TOUCHES A CACHE. A selection is a pure function of what was
// declared and what is available, so the same light in the same frame cannot be resolved to two
// different modes by two callers.

#include <cy/core/base/types.h>
#include <cy/rendering/shadows/address_space.h>

namespace cy::rendering {

/// Which of the six modes this renderer profile makes available. A `false` is a capability the
/// device, the build or the quality level does not have — not a preference.
///
/// `None` and `Conventional` are not fields: `Conventional` is what the specification calls "the
/// shipping path for constrained profiles and the fallback where capabilities are absent", and a
/// profile that had neither it nor anything else would be a profile with no shadows at all, which
/// `select_shadow_mode` reports rather than silently produces.
struct ShadowModeProfile {
    bool baked = true;
    bool conventional = true;
    /// Sparse virtual shadow pages. The path `cy::rendering-shadows` implements.
    bool virtual_pages = true;
    /// A trace is available THIS FRAME. Read off `ray-tracing-infrastructure`'s service by the
    /// caller; false runs every traced tier's fallback.
    bool traced = false;
};

/// What one light declared.
struct ShadowModeRequest {
    ShadowMode declared = ShadowMode::Virtual;
    /// A light that casts no shadow selects `None` whatever it declared, and that is not a
    /// degradation — it is the light saying so.
    bool casts_shadow = true;
};

/// Why the selected mode is not the declared one. `None` means it is.
enum class ShadowModeFallback : u8 {
    None = 0,
    /// The light casts no shadow.
    LightCastsNoShadow,
    /// `RayTraced` or `Hybrid` on a frame with no trace available.
    NoTraceThisFrame,
    /// `Virtual` where the profile has no virtual pages.
    NoVirtualPages,
    /// `Baked` where the profile has no baked data.
    NoBakedData,
    /// `Conventional` where the profile has no conventional atlas. The rarest of the five and the
    /// one a high-end-only profile produces.
    NoConventionalAtlas,
    /// The profile has no conventional path either, so there is nothing left to fall back to. The
    /// light loses its shadow, which is the outcome the requirement forbids, reported by name.
    NothingAvailable,
    Count,
};

[[nodiscard]] const char* shadow_mode_fallback_name(ShadowModeFallback fallback) noexcept;

struct ShadowModeSelection {
    ShadowMode declared = ShadowMode::None;
    ShadowMode selected = ShadowMode::None;
    ShadowModeFallback fallback = ShadowModeFallback::None;
    /// Never null. A sentence naming what was unavailable and what was used instead.
    const char* diagnostic = "";

    /// True when the light did not get what it declared. `LightCastsNoShadow` is NOT a degradation.
    [[nodiscard]] constexpr bool degraded() const noexcept {
        return fallback != ShadowModeFallback::None &&
               fallback != ShadowModeFallback::LightCastsNoShadow;
    }
};

/// Select one light's mode. Pure: same arguments, same answer, no device asked.
[[nodiscard]] ShadowModeSelection select_shadow_mode(const ShadowModeRequest& request,
                                                     const ShadowModeProfile& profile) noexcept;

/// How many lights ended up in each mode, and how many were degraded to get there.
struct ShadowModeLedger {
    u32 counts[static_cast<usize>(ShadowMode::Count)] = {};
    u32 degraded = 0;
    u32 lights = 0;

    void record(const ShadowModeSelection& selection) noexcept;
    void reset() noexcept;
};

}  // namespace cy::rendering
