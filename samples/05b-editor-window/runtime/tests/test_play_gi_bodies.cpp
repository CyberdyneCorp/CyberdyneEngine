// SPDX-License-Identifier: MIT
// The editor's Play and the GI the viewport is lit by: what the simulation moves and what the
// game placed survive the author. Issue #35, #127's two gaps.
//
// The room of `tests/data/gi_test_room.cyworld`, its cart given a box collider so Play simulates
// it — it has no floor to stand on, so it falls. ABI 1.9 is bound over the preview's own system
// as `main.cpp`'s Play start binds it (`AuthoredIllumination::bind_play`), and the game is this
// file calling the adapter directly, so no Swift toolchain is needed. Asserted:
//
//   bodies move their GI objects   `follow_play` moves the cart's GI object to the Play entity's
//                                  world transform as it falls; Stop puts it back where the
//                                  document has it
//   an edit applied in place       the author changes the document's volume during Play: the GI
//                                  holds the author's new volume AND the game's, which the edit
//                                  used to replace
//   an edit that rebuilds          the author moves the crate during Play, which rebuilds the GI:
//                                  the same adapter, bound again over the new system
//                                  (`rebind_play`), places the game's volume there again, and its
//                                  handle still names it
//   Stop                           the game's volumes go with it; the document's stay

#include <cy/abi/cy_abi.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/components.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/scene/tree.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "gi_runtime.h"

using namespace cy;
using namespace cy::sample::editor_window;
namespace gi = cy::rendering::gi;
namespace ser = cy::scene::serialization;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// The room file's `Transform` type and its `translation` field.
constexpr u64 kTransformType = 1;
constexpr u64 kTranslationField = 2;

/// The room's primitives in a temporary project, and its world with the cart simulated.
struct Project {
    std::filesystem::path root;
    std::string world;

    Project() {
        static u32 counter = 0;
        root =
            std::filesystem::temp_directory_path() /
            ("cy-play-gi-bodies-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(root);
        for (const char* name : {"gi_box.cyprim", "gi_floor.cyprim"}) {
            std::filesystem::copy_file(std::filesystem::path(CY_GI_TEST_DATA) / name, root / name);
        }
        Array<u8> bytes(allocator());
        const std::string path = std::string(CY_GI_TEST_DATA) + "/gi_test_room.cyworld";
        CY_REQUIRE(assets::fs::read_whole(path.c_str(), bytes).has_value());
        world.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // A box collider on the cart, so Play simulates its rigid body (it refuses one without).
        replace("node 0 - ",
                "type 9 runtime \"Collider\"\n  field 80 text \"shape\" \"\"\n"
                "  field 81 vec3 \"extent\" \"\"\nnode 0 - ");
        replace("  component 8\n    field 70 40\n",
                "  component 8\n    field 70 40\n  component 9\n    field 80 \"box\"\n"
                "    field 81 0.5 0.5 0.5\n");
    }
    ~Project() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    void replace(std::string_view from, std::string_view to) {
        const usize at = world.find(from);
        CY_REQUIRE(at != std::string::npos);
        world.replace(at, from.size(), to);
    }
};

/// The document, its schema and the GI read from it; and the physics Play runs on.
struct Editor {
    reflect::TypeRegistry registry;
    ser::AuthoringSchema schema{allocator()};
    ser::World world{allocator()};
    AuthoredIllumination gi_source;
    physics::PhysicsServer* physics = nullptr;

    explicit Editor(const Project& project) : gi_source(allocator(), project.root.string()) {
        CY_REQUIRE(reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        CY_REQUIRE(ser::read_world(project.world, "worlds/gi.cyworld", world).has_value());
        CY_REQUIRE(ser::resolve_against(world, schema).has_value());
        gi_source.bind(&world);
        const Expected<physics::PhysicsServer*, Error> created =
            physics::reference::create_server(allocator());
        CY_REQUIRE(created.has_value());
        physics = *created;
        CY_REQUIRE(physics->initialize().has_value());
    }
    ~Editor() {
        physics->shutdown();
        physics::reference::destroy_server(physics, allocator());
    }
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    [[nodiscard]] gi::IlluminationSystem& system() {
        AuthoredGi lit;
        CY_REQUIRE(gi_source.current(lit));
        return *lit.system;
    }

    [[nodiscard]] u64 identity(std::string_view name) const {
        for (const ser::WorldNode& node : world.nodes()) {
            if (node.live && world.text(node.name) == name) {
                return node.identity;
            }
        }
        CY_REQUIRE_MESSAGE(false, "no node named " << name);
        return 0;
    }

    /// An author's edit: the lanes of one field of one node's component — by the identifiers
    /// the room's file gives them — written into the document.
    void edit(u64 node, u64 file_type, u64 file_field, Span<const f32> values) {
        bool written = false;
        for (ser::WorldNode& row : world.nodes()) {
            ser::WorldComponent* component = row.identity == node ? row.find(file_type) : nullptr;
            ser::WorldField* field = component != nullptr ? component->find(file_field) : nullptr;
            if (field == nullptr) {
                continue;
            }
            for (usize index = 0; index < values.size(); ++index) {
                field->value.lanes[index] = values[index];
            }
            written = true;
        }
        CY_REQUIRE(written);
        gi_source.world_changed();
    }
};

/// A game's volume over the room's lamp corner, at epic quality.
[[nodiscard]] CyGiVolumeDesc game_volume() noexcept {
    CyGiVolumeDesc desc{};
    desc.struct_size = sizeof(CyGiVolumeDesc);
    desc.sets = CY_GI_VOLUME_SETS_QUALITY;
    desc.bounds_min[0] = -3.0F;
    desc.bounds_min[1] = 0.0F;
    desc.bounds_min[2] = -3.0F;
    desc.bounds_max[0] = 0.0F;
    desc.bounds_max[1] = 3.0F;
    desc.bounds_max[2] = 0.0F;
    desc.priority = 5;
    desc.weight = 1.0F;
    desc.quality = CY_GI_QUALITY_EPIC;
    desc.probe_density = 1.0F;
    desc.update_rate = 1.0F;
    desc.diffuse_intensity = 1.0F;
    desc.specular_intensity = 1.0F;
    desc.far_field_metres = 60.0F;
    return desc;
}

[[nodiscard]] bool holds_game_volume(const gi::IlluminationSystem& system) noexcept {
    const Span<const gi::GiVolume> placed = system.volumes_of(gi::GiVolumeOwner::Game);
    return placed.size() == 1 && placed[0].quality == gi::GiQuality::Epic &&
           placed[0].bounds.min.x == -3.0F && placed[0].bounds.max.y == 3.0F;
}

/// Play over the editor's document, ABI 1.9 bound as `main.cpp` binds it.
struct Playing {
    gameplay::PlaySession session;
    std::unique_ptr<game_backend::IlluminationAdapter> adapter;

    explicit Playing(Editor& editor) : session(allocator(), editor.world) {
        gameplay::PlayConfiguration configuration;
        configuration.physics = editor.physics;
        configuration.schema = &editor.schema;
        CY_REQUIRE(session.enter(configuration).has_value());
        adapter = editor.gi_source.bind_play(session);
        CY_REQUIRE(adapter != nullptr);
    }
    ~Playing() {
        adapter.reset();
        (void)session.stop();
    }
    Playing(const Playing&) = delete;
    Playing& operator=(const Playing&) = delete;
};

}  // namespace

CY_TEST_CASE("Play's bodies move their GI objects, and Stop puts them back") {
    const Project project;
    Editor editor(project);
    const u64 cart = cy::editor::illumination_object_id(editor.identity("Cart"));
    const gi::GiObject* registered = editor.system().object(cart);
    CY_REQUIRE(registered != nullptr);
    const Mat4 authored = registered->transform;
    CY_CHECK_NEAR(authored.columns[3].y, 0.4F, 1.0e-6F);
    {
        Playing play(editor);
        u32 moved = 0;
        for (u32 tick = 0; tick < 30; ++tick) {
            CY_REQUIRE(play.session.tick().has_value());
            moved += editor.gi_source.follow_play(play.session);
        }
        // The cart fell: its GI object followed it, frame by frame.
        CY_CHECK(moved >= 10U);
        const ecs::Entity entity = play.session.entity_for(editor.identity("Cart"));
        CY_REQUIRE(entity.valid());
        const auto* placed = play.session.world()->get<scene::WorldTransform>(
            entity, play.session.tree()->components().world_transform);
        CY_REQUIRE(placed != nullptr);
        const Mat4 simulated = placed->value.to_matrix();
        CY_CHECK(simulated.columns[3].y < authored.columns[3].y - 0.1F);
        const gi::GiObject* followed = editor.system().object(cart);
        CY_REQUIRE(followed != nullptr);
        for (u32 column = 0; column < 4; ++column) {
            CY_CHECK_EQ(followed->transform.columns[column].x, simulated.columns[column].x);
            CY_CHECK_EQ(followed->transform.columns[column].y, simulated.columns[column].y);
            CY_CHECK_EQ(followed->transform.columns[column].z, simulated.columns[column].z);
        }
        // A static object is not the simulation's to move.
        const gi::GiObject* crate =
            editor.system().object(cy::editor::illumination_object_id(editor.identity("Crate")));
        CY_REQUIRE(crate != nullptr);
        CY_CHECK_NEAR(crate->transform.columns[3].x, 1.5F, 1.0e-6F);
    }
    editor.gi_source.end_play();
    const gi::GiObject* restored = editor.system().object(cart);
    CY_REQUIRE(restored != nullptr);
    CY_CHECK_EQ(restored->transform.columns[3].y, authored.columns[3].y);
}

CY_TEST_CASE("an edit during Play applied in place keeps the volumes the game placed") {
    const Project project;
    Editor editor(project);
    const u64 rebuilds = editor.gi_source.preview()->rebuilds();
    Playing play(editor);
    CyGiVolume handle = CY_GI_VOLUME_NULL;
    const CyGiVolumeDesc desc = game_volume();
    CY_REQUIRE_EQ(play.adapter->create_volume(desc, handle), CY_RESULT_OK);
    CY_REQUIRE(holds_game_volume(editor.system()));
    CY_CHECK_EQ(editor.system().volumes().size(), 2U);

    // The author moves the document's crate volume: applied in place, nothing rebuilt.
    const f32 moved[3] = {1.0F, 0.5F, 0.0F};
    editor.edit(editor.identity("Crate Volume"), kTransformType, kTranslationField, {moved, 3});
    gi::IlluminationSystem& system = editor.system();
    CY_CHECK_EQ(editor.gi_source.preview()->rebuilds(), rebuilds);
    // The author's volume moved, and the game's is still placed.
    const Span<const gi::GiVolume> authored = system.volumes_of(gi::GiVolumeOwner::Authored);
    CY_REQUIRE_EQ(authored.size(), 1U);
    CY_CHECK_NEAR(authored[0].bounds.center().x, 1.0F, 1.0e-5F);
    CY_CHECK(holds_game_volume(system));
    CY_CHECK_EQ(system.volumes().size(), 2U);
    // And the game still owns it by its handle.
    CY_CHECK_EQ(play.adapter->destroy_volume(handle), CY_RESULT_OK);
    CY_CHECK_EQ(editor.system().volumes_of(gi::GiVolumeOwner::Game).size(), 0U);
    CY_CHECK_EQ(editor.system().volumes().size(), 1U);
}

CY_TEST_CASE("an edit during Play that rebuilds the GI keeps the volumes the game placed") {
    const Project project;
    Editor editor(project);
    const u64 rebuilds = editor.gi_source.preview()->rebuilds();
    {
        Playing play(editor);
        CyGiVolume handle = CY_GI_VOLUME_NULL;
        const CyGiVolumeDesc desc = game_volume();
        CY_REQUIRE_EQ(play.adapter->create_volume(desc, handle), CY_RESULT_OK);

        // The author moves the crate: geometry changed, so the GI is rebuilt under the game.
        const f32 moved[3] = {2.0F, 0.5F, 0.5F};
        editor.edit(editor.identity("Crate"), kTransformType, kTranslationField, {moved, 3});
        gi::IlluminationSystem& rebuilt = editor.system();
        CY_REQUIRE_EQ(editor.gi_source.preview()->rebuilds(), rebuilds + 1U);
        // What main.cpp does when the generation moves under a running game.
        CY_REQUIRE(editor.gi_source.rebind_play(*play.adapter, play.session).has_value());
        CY_CHECK(holds_game_volume(rebuilt));
        CY_CHECK_EQ(rebuilt.volumes().size(), 2U);
        // The game's handle names its volume on the new system, and the objects are bound again.
        CyGiVolumeDesc wider = desc;
        wider.bounds_max[0] = 1.0F;
        CY_CHECK_EQ(play.adapter->update_volume(handle, wider), CY_RESULT_OK);
        CY_CHECK_EQ(rebuilt.volumes_of(gi::GiVolumeOwner::Game).size(), 1U);
        u32 klass = 0;
        CY_CHECK_EQ(play.adapter->object_class(
                        play.session.entity_for(editor.identity("Crate")).bits(), klass),
                    CY_RESULT_OK);
    }
    // STOP: the game's volumes go with it, and the document's one stays.
    editor.gi_source.end_play();
    editor.gi_source.world_changed();
    CY_CHECK_EQ(editor.system().volumes_of(gi::GiVolumeOwner::Game).size(), 0U);
    CY_CHECK_EQ(editor.system().volumes().size(), 1U);
}
