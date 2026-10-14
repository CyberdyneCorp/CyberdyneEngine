// SPDX-License-Identifier: MIT
// `integration.game_backend_illumination`: CyberGI through ABI 1.9's table. Issue #35.
//
// A real `gi::IlluminationSystem` — the GI suites' lit room (src/rendering/gi/tests/rooms.h) with a
// lamp hung in the far corner as an object of its own — behind the adapter, driven only through
// `cy_get_interface`'s entries, as a Swift game drives it. What the thunks refuse without a
// backend, and the phase rule, are unit.abi's (test_game_illumination.cpp).

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/illumination_backend.h>
#include <cy/test/test.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "rooms.h"

using namespace cy;
using cy::game_backend::IlluminationAdapter;
namespace gi = cy::rendering::gi;

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyEntity kLampEntity = 0x1'0000'0042ULL;
constexpr CyEntity kCrateEntity = 0x1'0000'0043ULL;
constexpr u64 kLampObject = 42;
constexpr u64 kCrateObject = 43;
constexpr u32 kLampInstance = 77;
constexpr u32 kLampMaterial = 5;
const Vec3 kLampAt{2.0F, 1.8F, 2.0F};
const Vec3 kUnderLamp{2.0F, -1.5F, 2.0F};
const Vec3 kUp{0.0F, 1.0F, 0.0F};

/// The lit room, a 30 cm lamp at radiance 2000 under its ceiling in the far corner from the room's
/// light, and a crate; both registered with the GI and bound to their entities.
struct Scene {
    gi_rooms::LitRoom room;
    std::vector<gi::Surfel> lamp_cards;
    std::vector<gi::Surfel> crate_cards;
    std::unique_ptr<IlluminationAdapter> adapter;
    abi::Host host{system_allocator(MemoryDomain::Scripting)};
    u64 frame = 1;

    Scene() {
        gi::Surfel lamp;
        lamp.normal = Vec3{0.0F, -1.0F, 0.0F};
        lamp.albedo = Vec3{0.8F, 0.8F, 0.8F};
        lamp.area = 0.09F;
        lamp.emission = Vec3{2000.0F, 1900.0F, 1750.0F};
        lamp.instance_id = kLampInstance;
        lamp.material_id = kLampMaterial;
        lamp_cards.push_back(lamp);
        gi::GiObjectDesc desc;
        desc.id = kLampObject;
        desc.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.15F, 0.05F, 0.15F});
        desc.transform = Mat4::from_translation(kLampAt);
        desc.surfels = {lamp_cards.data(), lamp_cards.size()};
        CY_REQUIRE(room.system.add_object(desc, 0).has_value());

        gi::Surfel side;
        side.normal = Vec3{1.0F, 0.0F, 0.0F};
        side.position = Vec3{0.3F, 0.0F, 0.0F};
        side.area = 0.36F;
        side.instance_id = 88;
        crate_cards.push_back(side);
        gi::GiObjectDesc crate;
        crate.id = kCrateObject;
        crate.mobility = gi::ObjectMobility::Movable;
        crate.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.3F, 0.3F, 0.3F});
        crate.transform = Mat4::from_translation(Vec3{-1.0F, -1.7F, 1.0F});
        crate.surfels = {crate_cards.data(), crate_cards.size()};
        CY_REQUIRE(room.system.add_object(crate, 0).has_value());

        adapter = std::make_unique<IlluminationAdapter>(system_allocator(MemoryDomain::World),
                                                        room.system);
        CY_REQUIRE(adapter->bind_object(kLampEntity, kLampObject, kLampInstance).has_value());
        CY_REQUIRE(adapter->bind_object(kCrateEntity, kCrateObject, 88).has_value());
        CY_REQUIRE(adapter->add_material("lamp-glass", kLampMaterial).has_value());
        game_backend::bind_illumination(host, adapter.get());
        converge();
    }

    [[nodiscard]] CyEngine engine() noexcept { return &host; }

    /// Converged mode, as a host runs it after a change it wants settled before it is measured.
    void converge() {
        adapter->begin_frame(frame);
        frame = room.converge(frame, 48);
    }

    [[nodiscard]] CyGiIrradiance sample(Vec3 at, const Vec3* normal) noexcept {
        CyGiIrradiance out{};
        out.struct_size = sizeof(out);
        const f32 position[3] = {at.x, at.y, at.z};
        const f32 facing[3] = {normal != nullptr ? normal->x : 0.0F,
                               normal != nullptr ? normal->y : 0.0F,
                               normal != nullptr ? normal->z : 0.0F};
        CY_REQUIRE_EQ(table().gi_sample_irradiance(engine(), position,
                                                   normal != nullptr ? facing : nullptr, &out),
                      CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] CyGiStatus status() noexcept {
        CyGiStatus out{};
        out.struct_size = sizeof(out);
        CY_REQUIRE_EQ(table().gi_status(engine(), &out), CY_RESULT_OK);
        return out;
    }
};

CyGiVolumeDesc volume_around(Vec3 centre, f32 half) noexcept {
    CyGiVolumeDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.bounds_min[0] = centre.x - half;
    desc.bounds_min[1] = centre.y - half;
    desc.bounds_min[2] = centre.z - half;
    desc.bounds_max[0] = centre.x + half;
    desc.bounds_max[1] = centre.y + half;
    desc.bounds_max[2] = centre.z + half;
    desc.weight = 1.0F;
    return desc;
}

}  // namespace

CY_TEST_CASE("the GI settings round-trip through the table, and a refused one changes nothing") {
    Scene scene;
    CyGiSettings settings{};
    settings.struct_size = sizeof(settings);
    CY_REQUIRE_EQ(table().gi_get_settings(scene.engine(), &settings), CY_RESULT_OK);
    CY_CHECK_EQ(settings.mode, CY_GI_MODE_DYNAMIC);
    CY_CHECK_EQ(settings.quality, CY_GI_QUALITY_MEDIUM);
    CY_CHECK_EQ(settings.tier, CY_GI_TIER_AUTOMATIC);
    CY_CHECK_EQ(settings.flags, CY_GI_HARDWARE_RAY_TRACING);

    settings.quality = CY_GI_QUALITY_EPIC;
    settings.tier = CY_GI_TIER_SOFTWARE_ONLY;
    settings.flags = 0;
    settings.budget_ms = 2.5F;
    CY_REQUIRE_EQ(table().gi_set_settings(scene.engine(), &settings), CY_RESULT_OK);
    const gi::GiRuntimeSettings applied = scene.room.system.runtime_settings();
    CY_CHECK_EQ(applied.quality, gi::GiQuality::Epic);
    CY_CHECK_EQ(applied.tier, gi::GiTierPreference::SoftwareOnly);
    CY_CHECK_FALSE(applied.hardware_ray_tracing);
    CY_CHECK_EQ(applied.budget_ms, 2.5F);
    // Epic is four times the rays at every probe placed from now on.
    scene.converge();
    for (const gi::Probe& probe : scene.room.system.radiance().probes()) {
        if (probe.live) {
            CY_CHECK_EQ(probe.ray_scale, 4.0F);
        }
    }

    CyGiSettings bad = settings;
    bad.budget_ms = -1.0F;
    CY_CHECK_EQ(table().gi_set_settings(scene.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    bad = settings;
    bad.mode = 9;
    CY_CHECK_EQ(table().gi_set_settings(scene.engine(), &bad), CY_RESULT_INVALID_ARGUMENT);
    CyGiSettings read{};
    read.struct_size = sizeof(read);
    CY_REQUIRE_EQ(table().gi_get_settings(scene.engine(), &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.quality, CY_GI_QUALITY_EPIC);
    CY_CHECK_EQ(read.budget_ms, 2.5F);
}

CY_TEST_CASE("a GI volume a script places overrides where it reaches, moves, and leaves nothing") {
    Scene scene;
    const gi::GiRegionSettings before = scene.room.system.region_settings(kUnderLamp);
    CyGiVolumeDesc desc = volume_around(kUnderLamp, 1.0F);
    desc.sets = CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY | CY_GI_VOLUME_SETS_QUALITY;
    desc.diffuse_intensity = 0.5F;
    desc.quality = CY_GI_QUALITY_HIGH;
    desc.probe_density = 0.0F;  // not set: ignored, never refused
    CyGiVolume volume = CY_GI_VOLUME_NULL;
    CY_REQUIRE_EQ(table().gi_volume_create(scene.engine(), &desc, &volume), CY_RESULT_OK);
    CY_CHECK_NE(volume, CY_GI_VOLUME_NULL);
    CY_CHECK_EQ(scene.status().volumes, 1U);
    CY_CHECK_EQ(scene.room.system.region_settings(kUnderLamp).diffuse_intensity, 0.5F);
    CY_CHECK_EQ(scene.room.system.region_settings(kUnderLamp).ray_scale, 2.0F);

    // Moved across the room: where it was is the global settings again.
    desc = volume_around(Vec3{-2.0F, -1.5F, -2.0F}, 1.0F);
    desc.sets = CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY;
    desc.diffuse_intensity = 0.5F;
    CY_REQUIRE_EQ(table().gi_volume_update(scene.engine(), volume, &desc), CY_RESULT_OK);
    CY_CHECK_EQ(scene.room.system.region_settings(kUnderLamp).diffuse_intensity, 1.0F);
    CY_CHECK_EQ(
        scene.room.system.region_settings(Vec3{-2.0F, -1.5F, -2.0F}).diffuse_intensity, 0.5F);

    // A box with no volume is the GI's to refuse, naming why, and changes nothing.
    CyGiVolumeDesc empty = volume_around(kUnderLamp, 0.0F);
    empty.bounds_max[0] = empty.bounds_min[0] - 1.0F;
    CyGiVolume refused = CY_GI_VOLUME_NULL;
    CY_CHECK_EQ(table().gi_volume_create(scene.engine(), &empty, &refused),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(refused, CY_GI_VOLUME_NULL);
    CY_CHECK_EQ(scene.adapter->volume_count(), 1U);

    CY_REQUIRE_EQ(table().gi_volume_destroy(scene.engine(), volume), CY_RESULT_OK);
    CY_CHECK_EQ(table().gi_volume_destroy(scene.engine(), volume), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().gi_volume_update(scene.engine(), volume, &desc), CY_RESULT_NOT_FOUND);
    CY_CHECK(scene.room.system.volumes().empty());
    const gi::GiRegionSettings after = scene.room.system.region_settings(kUnderLamp);
    CY_CHECK_EQ(after.diffuse_intensity, before.diffuse_intensity);
    CY_CHECK_EQ(after.ray_scale, before.ray_scale);
    CY_CHECK_EQ(after.tier, before.tier);
}

CY_TEST_CASE("a script marks an object's illumination class, and AUTOMATIC gives it back") {
    Scene scene;
    u32 klass = 99;
    CY_REQUIRE_EQ(table().gi_object_class(scene.engine(), kCrateEntity, &klass), CY_RESULT_OK);
    CY_CHECK_EQ(klass, CY_GI_CLASS_FAST_DYNAMIC);  // derived: a 0.6 m movable box
    CY_REQUIRE_EQ(table().gi_object_set_class(scene.engine(), kCrateEntity, CY_GI_CLASS_STATIC),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(table().gi_object_class(scene.engine(), kCrateEntity, &klass), CY_RESULT_OK);
    CY_CHECK_EQ(klass, CY_GI_CLASS_STATIC);
    CY_CHECK(scene.room.system.scene().cell_resident(gi::object_cell_id(kCrateObject)));
    CY_REQUIRE_EQ(table().gi_object_set_class(scene.engine(), kCrateEntity, CY_GI_CLASS_AUTOMATIC),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(table().gi_object_class(scene.engine(), kCrateEntity, &klass), CY_RESULT_OK);
    CY_CHECK_EQ(klass, CY_GI_CLASS_FAST_DYNAMIC);
    CY_CHECK_FALSE(scene.room.system.scene().cell_resident(gi::object_cell_id(kCrateObject)));

    CY_CHECK_EQ(table().gi_object_set_class(scene.engine(), 0x5'0000'0001ULL, CY_GI_CLASS_STATIC),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().gi_object_class(scene.engine(), 0x5'0000'0001ULL, &klass),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().gi_object_set_class(scene.engine(), kCrateEntity, 7U),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE(
    "a script switches a lamp off by its entity and by its material, and the light a guard would "
    "see there falls with it") {
    Scene scene;
    CY_REQUIRE_EQ(scene.status().promoted_lights, 1U);
    const CyGiIrradiance lit = scene.sample(kUnderLamp, nullptr);
    CY_CHECK_EQ(lit.flags, CY_GI_SAMPLE_OMNIDIRECTIONAL);
    CY_REQUIRE(lit.luminance > 0.0F);

    // Off by entity: the lamp is no light, its emitter disabled, and the spot under it darker.
    CY_REQUIRE_EQ(
        table().gi_emission_set(scene.engine(), kLampEntity, 0, CY_GI_EMISSION_OFF), CY_RESULT_OK);
    CyGiStatus status = scene.status();
    CY_CHECK_EQ(status.promoted_lights, 0U);
    CY_CHECK_EQ(status.disabled_emitters, 1U);
    scene.converge();
    const CyGiIrradiance dark = scene.sample(kUnderLamp, nullptr);
    std::printf("  under the lamp, omnidirectional luminance: %.4f on, %.4f off\n",
                static_cast<double>(lit.luminance), static_cast<double>(dark.luminance));
    // MEASURED: 2.150 on, 1.021 off.
    CY_CHECK_LT(dark.luminance, 0.6F * lit.luminance);

    // AUTOMATIC removes the rule: the lamp is back, and the spot as lit as it was — within the
    // converged GI's own movement from one update to the next, which over the same spot measured
    // 2.150, 2.158 and 2.228 on three successive convergences with nothing changed (3.6%).
    CY_REQUIRE_EQ(table().gi_emission_set(scene.engine(), kLampEntity, 0,
                                          CY_GI_EMISSION_AUTOMATIC),
                  CY_RESULT_OK);
    CY_CHECK_EQ(scene.status().promoted_lights, 1U);
    scene.converge();
    const CyGiIrradiance again = scene.sample(kUnderLamp, nullptr);
    std::printf("  restored: %.4f\n", static_cast<double>(again.luminance));
    CY_CHECK_NEAR_REL(again.luminance, lit.luminance, 0.06F);

    // Off by material name, every lamp of that glass at once.
    const u64 glass = abi::game::name_hash("lamp-glass");
    CY_REQUIRE_EQ(table().gi_emission_set(scene.engine(), CY_ENTITY_NULL, glass,
                                          CY_GI_EMISSION_OFF),
                  CY_RESULT_OK);
    CY_CHECK_EQ(scene.status().promoted_lights, 0U);
    // The entity's own rule is more specific than the material's: promoting it wins back the lamp.
    CY_REQUIRE_EQ(
        table().gi_emission_set(scene.engine(), kLampEntity, 0, CY_GI_EMISSION_PROMOTE),
        CY_RESULT_OK);
    CY_CHECK_EQ(scene.status().promoted_lights, 1U);

    CY_CHECK_EQ(table().gi_emission_set(scene.engine(), 0x5'0000'0001ULL, 0, CY_GI_EMISSION_OFF),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().gi_emission_set(scene.engine(), CY_ENTITY_NULL,
                                        abi::game::name_hash("no-such-glass"), CY_GI_EMISSION_OFF),
                CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("the irradiance a script samples is the resolve's own answer") {
    Scene scene;
    for (const Vec3 at : {kUnderLamp, Vec3{-2.0F, -1.5F, -2.0F}, Vec3{0.0F, 0.0F, 0.0F}}) {
        const CyGiIrradiance sampled = scene.sample(at, &kUp);
        const gi::ResolveResult resolved =
            scene.room.system.indirect_diffuse(at, kUp, gi::SurfaceProperties{});
        CY_CHECK_EQ(sampled.flags, 0U);
        CY_CHECK_EQ(sampled.radiance[0], resolved.radiance.x);
        CY_CHECK_EQ(sampled.radiance[1], resolved.radiance.y);
        CY_CHECK_EQ(sampled.radiance[2], resolved.radiance.z);
        CY_CHECK_EQ(sampled.confidence, resolved.confidence);
        CY_CHECK_EQ(sampled.source, static_cast<u32>(resolved.dominant));

        // With no normal, the mean of the six axis answers.
        Vec3 sum{};
        const Vec3 axes[6] = {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                              Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}};
        for (const Vec3 axis : axes) {
            sum = sum + scene.room.system.indirect_diffuse(at, axis, gi::SurfaceProperties{})
                            .radiance;
        }
        const CyGiIrradiance omni = scene.sample(at, nullptr);
        CY_CHECK_EQ(omni.radiance[1], (sum * (1.0F / 6.0F)).y);
    }
    const f32 nowhere[3] = {0.0F, 0.0F, 0.0F};
    const f32 zero[3] = {0.0F, 0.0F, 0.0F};
    CyGiIrradiance out{};
    out.struct_size = sizeof(out);
    CY_CHECK_EQ(table().gi_sample_irradiance(scene.engine(), nowhere, zero, &out),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("the status a script reads is the frame the GI last ran") {
    Scene scene;
    const CyGiStatus status = scene.status();
    CY_CHECK_EQ(status.objects, 2U);
    CY_CHECK_EQ(status.volumes, 0U);
    CY_CHECK_EQ(status.emitters, 1U);
    CY_CHECK_EQ(status.world_tier, CY_GI_SOURCE_SOFTWARE_TRACE);  // no device behind this suite
    CY_CHECK_GT(status.convergence, 0.0F);
    CY_CHECK_EQ(status.allocation_ms, scene.room.system.budget().settings().allocation_ms);
    CY_CHECK_EQ(status.frame, scene.room.system.profile().frame);
}
