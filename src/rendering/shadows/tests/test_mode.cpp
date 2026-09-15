// WHICH MODE A LIGHT ACTUALLY GETS. M11.c task 3.5.
//
// `virtual-shadows`' "Shadow modes" requirement has two sentences and until this rung NEITHER had
// an implementation: `ShadowMode` was six enumerators and a name function, named by one switch in
// `address_space.cpp` and by nothing else in the tree. The scenario these cases are written from is
// the requirement's own second one —
//
//   > WHEN a device cannot support virtual shadows
//   > THEN the light SHALL fall back to its conventional mode with a diagnostic,
//   > NOT LOSE ITS SHADOW
//
// — and the last case is the one that makes "not lose its shadow" checkable in the only direction
// that matters: a profile with nothing left reports `NothingAvailable` by name rather than quietly
// returning `None`, because a light that silently stopped casting is the defect, not the report.

#include <cy/rendering/shadows/mode.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::rendering;

namespace {

[[nodiscard]] ShadowModeProfile everything() noexcept {
    ShadowModeProfile profile;
    profile.baked = true;
    profile.conventional = true;
    profile.virtual_pages = true;
    profile.traced = true;
    return profile;
}

[[nodiscard]] ShadowModeRequest declaring(ShadowMode mode) noexcept {
    ShadowModeRequest request;
    request.declared = mode;
    request.casts_shadow = true;
    return request;
}

}  // namespace

CY_TEST_CASE("a light gets the mode it declared when the profile has it") {
    const ShadowModeProfile profile = everything();
    for (u8 index = 0; index < static_cast<u8>(ShadowMode::Count); ++index) {
        const auto mode = static_cast<ShadowMode>(index);
        if (mode == ShadowMode::None) {
            continue;
        }
        const ShadowModeSelection selection = select_shadow_mode(declaring(mode), profile);
        CY_CHECK_EQ(selection.selected, mode);
        CY_CHECK_FALSE(selection.degraded());
        CY_CHECK_EQ(selection.fallback, ShadowModeFallback::None);
    }
}

CY_TEST_CASE("a traced mode on a frame with no trace falls back and says which") {
    // THIS IS THE CASE EVERY DEVICE IN THIS TREE IS IN. `cy::rhi::Capability::RayTracing` is an
    // enumerator nothing sets, so `traced` is false on the RTX this was written on — and a light
    // declaring `RayTraced` must come back with a shadow and a reason rather than with nothing.
    ShadowModeProfile profile = everything();
    profile.traced = false;

    const ShadowModeSelection traced =
        select_shadow_mode(declaring(ShadowMode::RayTraced), profile);
    CY_CHECK_EQ(traced.selected, ShadowMode::Virtual);
    CY_CHECK(traced.degraded());
    CY_CHECK_EQ(traced.fallback, ShadowModeFallback::NoTraceThisFrame);

    const ShadowModeSelection hybrid = select_shadow_mode(declaring(ShadowMode::Hybrid), profile);
    CY_CHECK_EQ(hybrid.selected, ShadowMode::Virtual);
    CY_CHECK(hybrid.degraded());

    // And with a trace available the SAME declaration is honoured. Two answers from one function,
    // differing only in what the caller said was available this frame.
    profile.traced = true;
    CY_CHECK_EQ(select_shadow_mode(declaring(ShadowMode::RayTraced), profile).selected,
                ShadowMode::RayTraced);
}

CY_TEST_CASE("a device without virtual pages keeps its shadow, conventionally") {
    ShadowModeProfile profile = everything();
    profile.traced = false;
    profile.virtual_pages = false;

    const ShadowModeSelection selection =
        select_shadow_mode(declaring(ShadowMode::Virtual), profile);
    CY_CHECK_EQ(selection.selected, ShadowMode::Conventional);
    CY_CHECK(selection.degraded());
    CY_CHECK_EQ(selection.fallback, ShadowModeFallback::NoVirtualPages);

    // The traced declaration walks the same ladder and lands in the same place: one ladder, so two
    // declarations cannot disagree about what "the fallback" is.
    CY_CHECK_EQ(select_shadow_mode(declaring(ShadowMode::Hybrid), profile).selected,
                ShadowMode::Conventional);
}

CY_TEST_CASE("a light that casts no shadow is not a degraded light") {
    ShadowModeRequest request = declaring(ShadowMode::Virtual);
    request.casts_shadow = false;
    const ShadowModeSelection selection = select_shadow_mode(request, everything());
    CY_CHECK_EQ(selection.selected, ShadowMode::None);
    CY_CHECK_FALSE(selection.degraded());
    CY_CHECK_EQ(selection.fallback, ShadowModeFallback::LightCastsNoShadow);
}

CY_TEST_CASE("a profile with nothing left reports losing the shadow rather than returning None") {
    ShadowModeProfile profile;
    profile.baked = false;
    profile.conventional = false;
    profile.virtual_pages = false;
    profile.traced = false;

    const ShadowModeSelection selection =
        select_shadow_mode(declaring(ShadowMode::Virtual), profile);
    CY_CHECK_EQ(selection.selected, ShadowMode::None);
    CY_CHECK(selection.degraded());
    CY_CHECK_EQ(selection.fallback, ShadowModeFallback::NothingAvailable);
}

CY_TEST_CASE("the ledger counts what the lights ended up with, not what they asked for") {
    ShadowModeProfile profile = everything();
    profile.traced = false;

    ShadowModeLedger ledger;
    ledger.record(select_shadow_mode(declaring(ShadowMode::RayTraced), profile));
    ledger.record(select_shadow_mode(declaring(ShadowMode::Virtual), profile));
    ShadowModeRequest unshadowed = declaring(ShadowMode::Virtual);
    unshadowed.casts_shadow = false;
    ledger.record(select_shadow_mode(unshadowed, profile));

    CY_CHECK_EQ(ledger.lights, 3U);
    CY_CHECK_EQ(ledger.counts[static_cast<usize>(ShadowMode::Virtual)], 2U);
    CY_CHECK_EQ(ledger.counts[static_cast<usize>(ShadowMode::None)], 1U);
    CY_CHECK_EQ(ledger.counts[static_cast<usize>(ShadowMode::RayTraced)], 0U);
    // One of the three did not get what it declared, and the light that casts no shadow is not it.
    CY_CHECK_EQ(ledger.degraded, 1U);
}
