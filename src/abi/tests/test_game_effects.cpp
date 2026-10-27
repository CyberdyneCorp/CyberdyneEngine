// SPDX-License-Identifier: MIT
// ABI 1.15's effect thunks against a fake backend.
//
// What the thunks own: the phase rule — `effect_find` `[N F U]`, `effect_play` `[N U]` — the
// argument and range checks, `struct_size`, and UNAVAILABLE with no effects bound. The simulation
// world itself is integration.game_backend_effects's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/effects.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyEffect kSparks = 7;

class FakeEffects final : public cy::abi::game::EffectsBackend {
public:
    CyEffectPlay played{};
    CyEffect last = CY_EFFECT_NULL;
    u32 plays = 0;

    CyResult find(const char* name, CyEffect& out) const noexcept override {
        if (std::strcmp(name, "impact.sparks") != 0) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such effect");
        }
        out = kSparks;
        return CY_RESULT_OK;
    }
    CyResult play(CyEffect effect, const CyEffectPlay& play) noexcept override {
        if (effect != kSparks) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "an effect the host did not hand out");
        }
        played = play;
        last = effect;
        ++plays;
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeEffects effects;
    Bound() { host.game.effects = &effects; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyEffectPlay burst() noexcept {
    CyEffectPlay made{};
    made.struct_size = sizeof(made);
    made.position[0] = 2.0F;
    made.position[1] = 1.0F;
    made.position[2] = -3.0F;
    made.scale = 1.0F;
    made.parameter_count = 3;
    made.parameters[1] = 1.0F;
    return made;
}

}  // namespace

CY_TEST_CASE("effects: without bound effects every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    CyEffect effect = CY_EFFECT_NULL;
    CY_CHECK_EQ(table().effect_find(&host, "impact.sparks", &effect), CY_RESULT_UNAVAILABLE);
    const CyEffectPlay play = burst();
    CY_CHECK_EQ(table().effect_play(&host, kSparks, &play), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().effect_play(nullptr, kSparks, &play), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("effects: a name resolves in every phase and an unknown one is NOT_FOUND") {
    Bound bound;
    CyEffect effect = CY_EFFECT_NULL;
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().effect_find(bound.engine(), "impact.sparks", &effect), CY_RESULT_OK);
    }
    CY_CHECK_EQ(effect, kSparks);
    CyEffect missing = 99;
    CY_CHECK_EQ(table().effect_find(bound.engine(), "muzzle.smoke", &missing), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(missing, 99U);
    CY_CHECK_EQ(table().effect_find(bound.engine(), nullptr, &effect), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().effect_find(bound.engine(), "impact.sparks", nullptr),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("effects: a frame update plays an effect; a fixed step may not") {
    Bound bound;
    const CyEffectPlay play = burst();
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(table().effect_play(bound.engine(), kSparks, &play),
                    CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(bound.effects.plays, 0U);
    cy::abi::game::PhaseScope frame(bound.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_REQUIRE_EQ(table().effect_play(bound.engine(), kSparks, &play), CY_RESULT_OK);
    CY_CHECK_EQ(bound.effects.plays, 1U);
    CY_CHECK_EQ(bound.effects.last, kSparks);
    CY_CHECK_EQ(bound.effects.played.position[2], -3.0F);
    CY_CHECK_EQ(bound.effects.played.parameter_count, 3U);
    CY_CHECK_EQ(bound.effects.played.parameters[1], 1.0F);
    CY_CHECK_EQ(table().effect_play(bound.engine(), 3, &play), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("effects: a play is whole and in range before the backend sees it") {
    Bound bound;
    // An older module's shorter struct: what it did not know reads as zero.
    CyEffectPlay shorter = burst();
    shorter.struct_size = offsetof(CyEffectPlay, parameters);
    shorter.parameters[0] = 5.0F;
    CY_REQUIRE_EQ(table().effect_play(bound.engine(), kSparks, &shorter), CY_RESULT_OK);
    CY_CHECK_EQ(bound.effects.played.parameters[0], 0.0F);
    CY_CHECK_EQ(bound.effects.played.struct_size, static_cast<u32>(sizeof(CyEffectPlay)));

    const u32 before = bound.effects.plays;
    const auto refused = [&](const CyEffectPlay& bad) {
        CY_CHECK_EQ(table().effect_play(bound.engine(), kSparks, &bad), CY_RESULT_INVALID_ARGUMENT);
    };
    CyEffectPlay bad = burst();
    bad.parameter_count = 9;
    refused(bad);
    bad = burst();
    bad.scale = 0.0F;
    refused(bad);
    bad = burst();
    bad.scale = -1.0F;
    refused(bad);
    bad = burst();
    bad.position[1] = std::numeric_limits<float>::infinity();
    refused(bad);
    bad = burst();
    bad.parameters[7] = std::numeric_limits<float>::quiet_NaN();
    refused(bad);
    bad = burst();
    bad.reserved[0] = 1;
    refused(bad);
    bad = burst();
    bad.struct_size = 2;
    refused(bad);
    CY_CHECK_EQ(table().effect_play(bound.engine(), kSparks, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.effects.plays, before);
}
