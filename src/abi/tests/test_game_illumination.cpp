// SPDX-License-Identifier: MIT
// ABI 1.9's global illumination thunks against a fake backend. Issue #35.
//
// What the thunks own: the phase rule — every entry `[N U]`, a fixed step refused — the argument
// and enumerator checks, `struct_size` both ways, and UNAVAILABLE with no GI bound. The GI itself
// is integration.game_backend_illumination's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/illumination.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstddef>
#include <limits>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeIllumination final : public cy::abi::game::IlluminationBackend {
public:
    CyGiSettings applied{};
    CyGiVolumeDesc last_volume{};
    u32 volumes = 0;
    u32 calls = 0;
    CyEntity last_entity = CY_ENTITY_NULL;
    u64 last_material = 0;
    u32 last_mode = 99;
    bool had_normal = false;

    CyResult settings(CyGiSettings& out) const noexcept override {
        out.mode = CY_GI_MODE_HYBRID;
        out.quality = CY_GI_QUALITY_HIGH;
        out.budget_ms = 4.0F;
        out.flags = CY_GI_HARDWARE_RAY_TRACING;
        return CY_RESULT_OK;
    }
    CyResult set_settings(const CyGiSettings& settings) noexcept override {
        applied = settings;
        ++calls;
        return CY_RESULT_OK;
    }
    CyResult create_volume(const CyGiVolumeDesc& desc, CyGiVolume& out) noexcept override {
        last_volume = desc;
        out = ++volumes;
        return CY_RESULT_OK;
    }
    CyResult update_volume(CyGiVolume volume, const CyGiVolumeDesc& desc) noexcept override {
        last_volume = desc;
        return volume <= volumes ? CY_RESULT_OK
                                 : cy::abi::report(CY_RESULT_NOT_FOUND, "no such volume");
    }
    CyResult destroy_volume(CyGiVolume volume) noexcept override {
        return volume <= volumes ? CY_RESULT_OK
                                 : cy::abi::report(CY_RESULT_NOT_FOUND, "no such volume");
    }
    CyResult set_object_class(CyEntity entity, u32 klass) noexcept override {
        last_entity = entity;
        last_mode = klass;
        return CY_RESULT_OK;
    }
    CyResult object_class(CyEntity entity, u32& out) const noexcept override {
        out = static_cast<u32>(entity & 0x3U);
        return CY_RESULT_OK;
    }
    CyResult set_emission(CyEntity entity, u64 material, u32 mode) noexcept override {
        last_entity = entity;
        last_material = material;
        last_mode = mode;
        return CY_RESULT_OK;
    }
    CyResult sample_irradiance(const f32 (&position)[3], const f32* normal,
                               CyGiIrradiance& out) const noexcept override {
        out.radiance[0] = position[0];
        out.radiance[1] = position[1];
        out.radiance[2] = position[2];
        out.luminance = 0.5F;
        out.confidence = normal != nullptr ? 1.0F : 0.25F;
        out.source = CY_GI_SOURCE_RADIANCE_CACHE;
        return CY_RESULT_OK;
    }
    CyResult status(CyGiStatus& out) const noexcept override {
        out.volumes = volumes;
        out.frame = 77;
        out.world_tier = CY_GI_SOURCE_HARDWARE_TRACE;
        return CY_RESULT_OK;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeIllumination gi;
    Fixture() noexcept { host.game.illumination = &gi; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyGiSettings good_settings() noexcept {
    CyGiSettings settings{};
    settings.struct_size = sizeof(settings);
    settings.mode = CY_GI_MODE_DYNAMIC;
    settings.quality = CY_GI_QUALITY_EPIC;
    settings.tier = CY_GI_TIER_WORLD_ONLY;
    settings.budget_ms = 2.0F;
    return settings;
}

[[nodiscard]] CyGiVolumeDesc box() noexcept {
    CyGiVolumeDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.bounds_max[0] = desc.bounds_max[1] = desc.bounds_max[2] = 1.0F;
    desc.weight = 1.0F;
    desc.sets = CY_GI_VOLUME_SETS_QUALITY | CY_GI_VOLUME_SETS_FAR_FIELD;
    desc.quality = CY_GI_QUALITY_LOW;
    desc.far_field_metres = 30.0F;
    return desc;
}

}  // namespace

CY_TEST_CASE("illumination: every entry answers in N and U and refuses a fixed step") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CyGiSettings settings = good_settings();
    const CyGiVolumeDesc desc = box();
    const f32 at[3] = {1.0F, 2.0F, 3.0F};
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        const CyResult expected =
            phase == CY_PHASE_FIXED_UPDATE ? CY_RESULT_PERMISSION_DENIED : CY_RESULT_OK;
        CyGiSettings read{};
        read.struct_size = sizeof(read);
        CyGiVolume volume = CY_GI_VOLUME_NULL;
        u32 klass = 0;
        CyGiIrradiance light{};
        light.struct_size = sizeof(light);
        CyGiStatus status{};
        status.struct_size = sizeof(status);
        CY_TEST_INFO("phase " << static_cast<int>(phase));
        CY_CHECK_EQ(iface.gi_get_settings(fixture.engine(), &read), expected);
        CY_CHECK_EQ(iface.gi_set_settings(fixture.engine(), &settings), expected);
        CY_CHECK_EQ(iface.gi_volume_create(fixture.engine(), &desc, &volume), expected);
        CY_CHECK_EQ(iface.gi_volume_update(fixture.engine(), 1, &desc), expected);
        CY_CHECK_EQ(iface.gi_volume_destroy(fixture.engine(), 1), expected);
        CY_CHECK_EQ(iface.gi_object_set_class(fixture.engine(), 5, CY_GI_CLASS_STATIC), expected);
        CY_CHECK_EQ(iface.gi_object_class(fixture.engine(), 5, &klass), expected);
        CY_CHECK_EQ(iface.gi_emission_set(fixture.engine(), 5, 0, CY_GI_EMISSION_OFF), expected);
        CY_CHECK_EQ(iface.gi_sample_irradiance(fixture.engine(), at, nullptr, &light), expected);
        CY_CHECK_EQ(iface.gi_status(fixture.engine(), &status), expected);
    }
    // Two of the three phases reached the backend.
    CY_CHECK_EQ(fixture.gi.calls, 2U);
    CY_CHECK_EQ(fixture.gi.volumes, 2U);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("illumination: no GI bound is UNAVAILABLE, and a null engine is refused") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyInterface& iface = table();
    CyGiStatus status{};
    status.struct_size = sizeof(status);
    CY_CHECK_EQ(iface.gi_status(&host, &status), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.gi_volume_destroy(&host, 1), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.gi_status(nullptr, &status), CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("illumination: the arguments a thunk refuses before the backend sees them") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CyEngine engine = fixture.engine();

    CyGiSettings settings = good_settings();
    settings.mode = CY_GI_MODE_HYBRID + 1U;
    CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    settings = good_settings();
    settings.quality = CY_GI_QUALITY_EPIC + 1U;
    CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    settings = good_settings();
    settings.tier = CY_GI_TIER_WORLD_ONLY + 1U;
    CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    settings = good_settings();
    settings.flags = 0x2U;
    CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    for (const f32 budget : {0.0F, -1.0F, std::numeric_limits<f32>::infinity(),
                             std::numeric_limits<f32>::quiet_NaN()}) {
        settings = good_settings();
        settings.budget_ms = budget;
        CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    }
    // A prefix that stops before the budget reads a budget of zero, which is no budget.
    settings = good_settings();
    settings.struct_size = offsetof(CyGiSettings, budget_ms);
    CY_CHECK_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_set_settings(engine, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.gi.calls, 0U);

    CyGiVolume volume = CY_GI_VOLUME_NULL;
    CyGiVolumeDesc desc = box();
    desc.sets = 0x80U;
    CY_CHECK_EQ(iface.gi_volume_create(engine, &desc, &volume), CY_RESULT_INVALID_ARGUMENT);
    desc = box();
    desc.quality = 4;
    CY_CHECK_EQ(iface.gi_volume_create(engine, &desc, &volume), CY_RESULT_INVALID_ARGUMENT);
    desc = box();
    desc.tier = 3;
    CY_CHECK_EQ(iface.gi_volume_update(engine, 1, &desc), CY_RESULT_INVALID_ARGUMENT);
    desc = box();
    CY_CHECK_EQ(iface.gi_volume_create(engine, &desc, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_volume_create(engine, nullptr, &volume), CY_RESULT_INVALID_ARGUMENT);
    desc.struct_size = 2;
    CY_CHECK_EQ(iface.gi_volume_create(engine, &desc, &volume), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.gi.volumes, 0U);

    CY_CHECK_EQ(iface.gi_object_set_class(engine, 5, CY_GI_CLASS_AUTOMATIC + 1U),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_object_class(engine, 5, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_emission_set(engine, 5, 0, CY_GI_EMISSION_OFF + 1U),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.gi.last_mode, 99U);

    const f32 nan[3] = {0.0F, std::numeric_limits<f32>::quiet_NaN(), 0.0F};
    const f32 at[3] = {1.0F, 2.0F, 3.0F};
    CyGiIrradiance light{};
    light.struct_size = sizeof(light);
    CY_CHECK_EQ(iface.gi_sample_irradiance(engine, nullptr, nullptr, &light),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_sample_irradiance(engine, nan, nullptr, &light),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_sample_irradiance(engine, at, nan, &light), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.gi_sample_irradiance(engine, at, nullptr, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("illumination: what crosses, both ways, and only the prefix a caller declared") {
    Fixture fixture;
    const CyInterface& iface = table();
    const CyEngine engine = fixture.engine();

    const CyGiSettings settings = good_settings();
    CY_REQUIRE_EQ(iface.gi_set_settings(engine, &settings), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.gi.applied.quality, CY_GI_QUALITY_EPIC);
    CY_CHECK_EQ(fixture.gi.applied.tier, CY_GI_TIER_WORLD_ONLY);
    CY_CHECK_EQ(fixture.gi.applied.budget_ms, 2.0F);
    CY_CHECK_EQ(fixture.gi.applied.struct_size, sizeof(CyGiSettings));

    // An older caller's shorter output receives its prefix and learns how much was written.
    CyGiSettings read{};
    read.struct_size = offsetof(CyGiSettings, flags);
    read.budget_ms = -7.0F;
    CY_REQUIRE_EQ(iface.gi_get_settings(engine, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.mode, CY_GI_MODE_HYBRID);
    CY_CHECK_EQ(read.quality, CY_GI_QUALITY_HIGH);
    CY_CHECK_EQ(read.struct_size, offsetof(CyGiSettings, flags));
    CY_CHECK_EQ(read.budget_ms, -7.0F);

    const CyGiVolumeDesc desc = box();
    CyGiVolume volume = CY_GI_VOLUME_NULL;
    CY_REQUIRE_EQ(iface.gi_volume_create(engine, &desc, &volume), CY_RESULT_OK);
    CY_CHECK_EQ(volume, 1U);
    CY_CHECK_EQ(fixture.gi.last_volume.far_field_metres, 30.0F);
    CY_CHECK_EQ(iface.gi_volume_destroy(engine, 9), CY_RESULT_NOT_FOUND);

    CY_REQUIRE_EQ(iface.gi_emission_set(engine, CY_ENTITY_NULL, 0xABCULL, CY_GI_EMISSION_PROMOTE),
                  CY_RESULT_OK);
    CY_CHECK_EQ(fixture.gi.last_entity, CY_ENTITY_NULL);
    CY_CHECK_EQ(fixture.gi.last_material, 0xABCULL);
    CY_CHECK_EQ(fixture.gi.last_mode, CY_GI_EMISSION_PROMOTE);

    u32 klass = 99;
    CY_REQUIRE_EQ(iface.gi_object_class(engine, 6, &klass), CY_RESULT_OK);
    CY_CHECK_EQ(klass, 2U);

    const f32 at[3] = {1.0F, 2.0F, 3.0F};
    const f32 up[3] = {0.0F, 1.0F, 0.0F};
    CyGiIrradiance light{};
    light.struct_size = sizeof(light);
    CY_REQUIRE_EQ(iface.gi_sample_irradiance(engine, at, up, &light), CY_RESULT_OK);
    CY_CHECK_EQ(light.radiance[2], 3.0F);
    CY_CHECK_EQ(light.confidence, 1.0F);
    CY_CHECK_EQ(light.struct_size, sizeof(CyGiIrradiance));
    CY_REQUIRE_EQ(iface.gi_sample_irradiance(engine, at, nullptr, &light), CY_RESULT_OK);
    CY_CHECK_EQ(light.confidence, 0.25F);

    CyGiStatus status{};
    status.struct_size = 0;  // zero is this header's size
    CY_REQUIRE_EQ(iface.gi_status(engine, &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.frame, 77U);
    CY_CHECK_EQ(status.volumes, 1U);
    CY_CHECK_EQ(status.struct_size, sizeof(CyGiStatus));
    cy::abi::clear_last_error();
}
