// SPDX-License-Identifier: MIT
// A real Swift game module driving the engine's global illumination through CyberdyneKit alone.
// Issue #35: ABI 1.9 from the language a game is written in.
//
// `fixtures/illumination/LightProbe.swift` is a behaviour that, in its first frame, asks for high
// quality, places a GI volume and removes it, samples the indirect light where a guard stands,
// switches a lamp's glass off by material name and marks a cart static; in its second frame — the
// host converged the GI in between — it samples the same spot again; in every fixed step it tries
// to sample and is refused. The GI is the suites' lit room (src/rendering/gi/tests/rooms.h) with a
// lamp and a cart registered, behind `cy::game_backend::IlluminationAdapter`, and every number the
// behaviour exported is checked against the system itself. State is read through the module's own
// `serialize`, as `integration.swift_reload` reads it.

#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/illumination_backend.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "rooms.h"

namespace {

namespace gi = cy::rendering::gi;
using cy::f32;
using cy::i64;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr const char* kManifestText =
    "name = \"swift_illumination_fixture\"\n"
    "entry_symbol = \"cy_module_entry\"\n"
    "min_abi_major = 1\n"
    "min_abi_minor = 9\n"
    "hot_reload = false\n";

constexpr CyEntity kLampEntity = 0x1'0000'0042ULL;
constexpr CyEntity kCartEntity = 0x1'0000'0043ULL;
constexpr u32 kLampMaterial = 5;
const Vec3 kGuard{2.0F, -1.5F, 2.0F};

/// The room, a 30 cm lamp of `lamp-glass` in its far corner, and a movable cart.
struct Scene {
    gi_rooms::LitRoom room;
    std::vector<gi::Surfel> lamp_cards;
    std::vector<gi::Surfel> cart_cards;
    cy::game_backend::IlluminationAdapter adapter{allocator(), room.system};
    u64 frame = 1;

    Scene() {
        gi::Surfel lamp;
        lamp.normal = Vec3{0.0F, -1.0F, 0.0F};
        lamp.area = 0.09F;
        lamp.emission = Vec3{2000.0F, 1900.0F, 1750.0F};
        lamp.instance_id = 77;
        lamp.material_id = kLampMaterial;
        lamp_cards.push_back(lamp);
        gi::GiObjectDesc desc;
        desc.id = 42;
        desc.local_bounds = cy::Aabb::from_center_extents(Vec3{}, Vec3{0.15F, 0.05F, 0.15F});
        desc.transform = cy::Mat4::from_translation(Vec3{2.0F, 1.8F, 2.0F});
        desc.surfels = {lamp_cards.data(), lamp_cards.size()};
        CY_REQUIRE(room.system.add_object(desc, 0).has_value());
        gi::Surfel side;
        side.normal = Vec3{1.0F, 0.0F, 0.0F};
        side.area = 0.36F;
        side.instance_id = 88;
        cart_cards.push_back(side);
        gi::GiObjectDesc cart;
        cart.id = 43;
        cart.mobility = gi::ObjectMobility::Movable;
        cart.local_bounds = cy::Aabb::from_center_extents(Vec3{}, Vec3{0.3F, 0.3F, 0.3F});
        cart.transform = cy::Mat4::from_translation(Vec3{-1.0F, -1.7F, 1.0F});
        cart.surfels = {cart_cards.data(), cart_cards.size()};
        CY_REQUIRE(room.system.add_object(cart, 0).has_value());
        CY_REQUIRE(adapter.bind_object(kLampEntity, 42, 77).has_value());
        CY_REQUIRE(adapter.bind_object(kCartEntity, 43, 88).has_value());
        CY_REQUIRE(adapter.add_material("lamp-glass", kLampMaterial).has_value());
        converge();
    }

    void converge() {
        adapter.begin_frame(frame);
        frame = room.converge(frame, 48);
    }
};

struct Module {
    char manifest_text[256] = {};
    cy::abi::ModuleManifest manifest;
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};
    cy::abi::BehaviourRuntime runtime{allocator(), host};

    explicit Module(cy::abi::game::IlluminationBackend& illumination) {
        std::strncpy(manifest_text, kManifestText, sizeof(manifest_text) - 1);
        cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
            cy::abi::parse_module_manifest(manifest_text);
        CY_REQUIRE(parsed.has_value());
        manifest = parsed.value();
        CY_REQUIRE(world.initialize().has_value());
        host.bind_world(&binding);
        host.game.illumination = &illumination;
    }
};

/// The probe's exported integers, by name, from its own `serialize`.
std::map<std::string, i64> exported(cy::abi::BehaviourRuntime& runtime, u32 slot) {
    std::map<std::string, i64> out;
    const cy::abi::BehaviourInstance* live = runtime.instance(slot);
    CY_REQUIRE(live != nullptr);
    const auto& vtable = live->record->vtable;
    const u32 required = vtable.serialize(live->instance, nullptr, 0, vtable.user_data);
    std::vector<cy::u8> blob(required);
    CY_REQUIRE_EQ(vtable.serialize(live->instance, blob.data(), required, vtable.user_data),
                  required);
    usize offset = 12;  // 'CYST', schema, count
    u32 count = 0;
    std::memcpy(&count, blob.data() + 8, sizeof(count));
    for (u32 index = 0; index < count; ++index) {
        u32 length = 0;
        std::memcpy(&length, blob.data() + offset, sizeof(length));
        offset += sizeof(length);
        const std::string key(reinterpret_cast<const char*>(blob.data() + offset), length);
        offset += length;
        u32 kind = 0;
        std::memcpy(&kind, blob.data() + offset, sizeof(kind));
        offset += sizeof(kind);
        CY_REQUIRE_EQ(kind, static_cast<u32>(CY_VAR_I64));
        i64 value = 0;
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        offset += sizeof(value);
        out[key] = value;
    }
    return out;
}

[[nodiscard]] i64 micro_luminance(const gi::IlluminationSystem& system) {
    Vec3 sum{};
    const Vec3 axes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                          Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
    for (const Vec3 axis : axes) {
        sum = sum + system.indirect_diffuse(kGuard, axis, gi::SurfaceProperties{}).radiance;
    }
    const f32 luminance = gi::emission_luminance(sum * (1.0F / 6.0F));
    return std::llround(static_cast<double>(luminance) * 1'000'000.0);
}

}  // namespace

CY_TEST_CASE("a Swift game drives the engine's GI through CyberdyneKit, and only in a frame") {
    Scene scene;
    Module module(scene.adapter);
    CY_REQUIRE(module.runtime.load(module.manifest, CY_SWIFT_ILLUMINATION_MODULE).has_value());
    const cy::Expected<u32, cy::Error> slot =
        module.runtime.create("LightProbe", cy::abi::to_abi(cy::ecs::Entity{}));
    CY_REQUIRE(slot.has_value());

    const i64 lit = micro_luminance(scene.room.system);
    module.runtime.fixed_update(1.0F / 60.0F);
    module.runtime.frame_update(1.0F / 60.0F);
    std::map<std::string, i64> state = exported(module.runtime, slot.value());
    CY_CHECK_EQ(state["refusedInFrame"], 0);
    CY_CHECK_EQ(state["refusedInStep"], 1);
    CY_CHECK_EQ(state["quality"], static_cast<i64>(CY_GI_QUALITY_HIGH));
    CY_CHECK_EQ(scene.room.system.runtime_settings().quality, gi::GiQuality::High);
    CY_CHECK_EQ(state["volumesPlaced"], 1);
    CY_CHECK_EQ(state["volumesAfter"], 0);
    CY_CHECK(scene.room.system.volumes().empty());
    // The light a guard would see is the system's own six-direction mean, to the micro-unit.
    CY_CHECK_EQ(state["litMicro"], lit);
    CY_CHECK_EQ(state["lightsAfterOff"], 0);
    CY_CHECK(scene.room.system.emitters().lights().empty());
    CY_CHECK_EQ(state["cartClass"], static_cast<i64>(CY_GI_CLASS_STATIC));
    CY_CHECK_EQ(scene.room.system.object(43)->klass, gi::IlluminationClass::Static);

    // The host converges; the next frame the same spot is darker with the lamp out.
    scene.converge();
    module.runtime.frame_update(1.0F / 60.0F);
    state = exported(module.runtime, slot.value());
    CY_CHECK_EQ(state["darkMicro"], micro_luminance(scene.room.system));
    std::printf("  a guard's spot through Swift: %lld micro-units lit, %lld with the lamp out\n",
                static_cast<long long>(state["litMicro"]),
                static_cast<long long>(state["darkMicro"]));
    CY_CHECK_LT(state["darkMicro"] * 10, state["litMicro"] * 6);
    CY_CHECK_EQ(state["refusedInFrame"], 0);
}
