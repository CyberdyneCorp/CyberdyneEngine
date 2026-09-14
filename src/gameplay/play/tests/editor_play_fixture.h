#pragma once
// The authored world the two editor-play suites drive, and the physics server they drive it with.
// M11.b sections 3.1 and 3.2.
//
// Shared by `integration.editor_play` and `integration.editor_live_edit` because the two suites are
// two halves of one claim — the modes carry a world, and a live edit reaches the world they carry —
// and because a world written twice is a world the two suites can disagree about.
//
// THE TYPE AND FIELD NAMES IN IT ARE A CONTRACT, and the same one `src/gameplay/play/tests/
// test_play.cpp` pins: `editor/crates/cy-editor-services/src/bodies.rs` writes `Transform`,
// `StaticBody`, `RigidBody` and `Collider` with exactly these field names, and
// `cy::gameplay::live`'s `declare_engine_policies` declares a policy per field against exactly
// these names. A spelling that drifted would be a live edit that reaches nothing, reported as a
// green run.
//
// `Character` is this fixture's own and is not an engine type. It exists because `live-editing`'s
// runtime-state scenario is about a component with BOTH classifications in it — *"raising a
// character's maximum health while it stands at 53 leaves it at 53"* — and no component a play
// session creates has one: every field of `Transform`, `RigidBody` and `Collider` is `Authoring`.
// Inventing the fixture is the honest way to test the mechanism; pretending an engine component had
// runtime state would not be.

#include <cy/core/base/expected.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy_reflect_generated_scene.h>

#include <string>
#include <string_view>

namespace cy::test_editor {

namespace ser = cy::scene::serialization;

inline cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// The path the editor opens this world by. Both sides hash exactly this string into a
/// `DocumentId`, so it is not a detail.
inline constexpr std::string_view kAssetPath = "worlds/authoring.cyworld";

/// A box on the ground, a sphere above it, and a character on the sphere.
[[nodiscard]] inline std::string authored_world() {
    return "cyworld 1\n"
           "type 1 runtime \"Transform\"\n"
           "  field 1 vec3 \"translation\" \"\"\n"
           "  field 2 quat \"rotation\" \"\"\n"
           "  field 3 vec3 \"scale\" \"\"\n"
           "type 2 runtime \"StaticBody\"\n"
           "type 3 runtime \"RigidBody\"\n"
           "  field 4 float \"mass\" \"\"\n"
           "  field 5 float \"gravity_scale\" \"\"\n"
           "type 4 runtime \"Collider\"\n"
           "  field 6 text \"shape\" \"\"\n"
           "  field 7 vec3 \"extent\" \"\"\n"
           "  field 8 float \"radius\" \"\"\n"
           "  field 9 float \"height\" \"\"\n"
           "type 5 runtime \"Character\"\n"
           "  field 10 float \"maximum_health\" \"\"\n"
           "  field 11 float \"health\" \"\"\n"
           "node 0 - \"default\"\n"
           "  component 1\n"
           "    field 1 0 0 0\n"
           "    field 2 0 0 0 1\n"
           "    field 3 1 1 1\n"
           "  component 4\n"
           "    field 6 \"box\"\n"
           "    field 7 5 0.5 5\n"
           "    field 8 0.5\n"
           "    field 9 1\n"
           "  component 2\n"
           "node 1 - \"default\"\n"
           "  component 1\n"
           "    field 1 0 4 0\n"
           "    field 2 0 0 0 1\n"
           "    field 3 1 1 1\n"
           "  component 4\n"
           "    field 6 \"sphere\"\n"
           "    field 7 0.5 0.5 0.5\n"
           "    field 8 0.5\n"
           "    field 9 1\n"
           "  component 3\n"
           "    field 4 0\n"
           "    field 5 1\n"
           "  component 5\n"
           "    field 10 100\n"
           "    field 11 53\n";
}

/// The reference backend, which integrates motion and resolves no contacts. Every case here only
/// needs "did play run and did stop put it back", so the cases hold in every configuration.
class Reference {
public:
    Reference() noexcept {
        const cy::Expected<cy::physics::PhysicsServer*, cy::Error> made =
            cy::physics::reference::create_server(allocator());
        if (!made) {
            return;
        }
        server_ = *made;
        ready_ = server_->initialize().has_value();
    }

    ~Reference() {
        if (server_ != nullptr) {
            server_->shutdown();
            cy::physics::reference::destroy_server(server_, allocator());
        }
    }

    Reference(const Reference&) = delete;
    Reference& operator=(const Reference&) = delete;
    Reference(Reference&&) = delete;
    Reference& operator=(Reference&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] cy::physics::PhysicsServer* server() const noexcept { return server_; }

private:
    cy::physics::PhysicsServer* server_ = nullptr;
    bool ready_ = false;
};

/// A world read, resolved against the engine's schema, ready to play.
struct Authored {
    Authored() noexcept : world(allocator()), schema(allocator()) {
        started = cy::reflect::register_scene_types(registry).has_value() &&
                  ser::build_authoring_schema(registry, schema).has_value();
        if (!started) {
            return;
        }
        text = authored_world();
        started = ser::read_world(text, kAssetPath, world).has_value() &&
                  ser::resolve_against(world, schema).has_value();
    }

    Authored(const Authored&) = delete;
    Authored& operator=(const Authored&) = delete;
    Authored(Authored&&) = delete;
    Authored& operator=(Authored&&) = delete;
    ~Authored() = default;

    std::string text;
    cy::reflect::TypeRegistry registry;
    ser::World world;
    ser::AuthoringSchema schema;
    bool started = false;
};

[[nodiscard]] inline cy::gameplay::PlayConfiguration configuration_over(
    cy::physics::PhysicsServer* server, const ser::AuthoringSchema& schema) {
    cy::gameplay::PlayConfiguration configuration;
    configuration.physics = server;
    configuration.schema = &schema;
    configuration.body_capacity = 64;
    return configuration;
}

/// The world's own bytes, which is what an authoring change is measured in.
[[nodiscard]] inline std::string bytes_of(const ser::World& world) {
    cy::Array<char> out(allocator());
    if (!ser::write_world(world, out)) {
        return {};
    }
    return {out.data(), out.size()};
}

/// One field of one component of one node, read back out of the document.
[[nodiscard]] inline cy::f32 authored_float(const ser::World& world, cy::u64 identity,
                                            std::string_view type, std::string_view field) {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) != type) {
            continue;
        }
        for (const ser::WorldFieldDecl& field_decl : declared.fields()) {
            if (world.text(field_decl.name) != field) {
                continue;
            }
            for (const ser::WorldNode& node : world.nodes()) {
                if (!node.live || node.identity != identity) {
                    continue;
                }
                const ser::WorldComponent* component = node.find(declared.file_type);
                if (component == nullptr) {
                    return 0.0F;
                }
                const ser::WorldField* held = component->find(field_decl.file_field);
                if (held == nullptr) {
                    return 0.0F;
                }
                return held->value.kind == ser::WorldValueKind::Float
                           ? held->value.lanes[0]
                           : static_cast<cy::f32>(held->value.integer);
            }
        }
    }
    return 0.0F;
}

/// A float value in the world file's own vocabulary.
[[nodiscard]] inline ser::WorldValue float_value(cy::f32 value) {
    ser::WorldValue out;
    out.kind = ser::WorldValueKind::Float;
    out.lanes[0] = value;
    return out;
}

[[nodiscard]] inline ser::WorldValue vec3_value(cy::f32 x, cy::f32 y, cy::f32 z) {
    ser::WorldValue out;
    out.kind = ser::WorldValueKind::Vec3;
    out.lanes[0] = x;
    out.lanes[1] = y;
    out.lanes[2] = z;
    return out;
}

}  // namespace cy::test_editor
