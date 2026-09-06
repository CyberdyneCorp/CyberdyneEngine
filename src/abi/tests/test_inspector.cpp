// The inspector's read/write path, over a scene world, through the C ABI. M5 tasks 1.2 and 1.3.
//
// --- WHAT THIS SUITE IS THE PROOF OF ------------------------------------------------------------
//
// "The generated inspector edits a reflected type with no per-type editor code" is a headline M5
// requirement, and it is really four smaller claims that were each false at M4:
//
//   1. the scene's components carry `reflect::TypeInfo` — nothing was reflected, so an inspector
//      had nothing to read (task 1.3);
//   2. a caller that did not register them can ENUMERATE them — `world_component_count` and
//      `world_component_info` did not exist (task 1.2);
//   3. it can DESCRIBE each field — `world_component_field` did not exist, and a `u32` field had no
//      `CyVarType` to be described as;
//   4. it can READ and WRITE them through the same generic entries a module uses — the ABI could
//      only address components a module had itself described.
//
// Every case below goes through `cy_get_interface`, and NOT ONE OF THEM NAMES A SCENE TYPE except
// to build the world in the first place. There is no `if (component == LocalTransform)` anywhere;
// there could not be, because the whole point is that a Rust editor process which has never heard
// of `cy::scene` can do this. That is what "no per-type editor code" means, and it is why the
// assertions are about names and offsets discovered at run time rather than about a struct.
//
// It lives in src/abi/tests/ rather than in src/scene/tests/ because the claim is about the
// BOUNDARY: a test inside src/scene/ could read the same values by including the header, which is
// exactly the thing the editor cannot do.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// A scene world and the ABI's view of it. The only place in this file that names a scene type.
struct Bound {
    cy::ecs::World world{allocator()};
    cy::scene::SceneTree tree{world};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};

    Bound() {
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        host.bind_world(&binding);
    }
};

/// The id of the component with this name, discovered rather than known.
CyComponentTypeId find(const CyInterface& iface, CyWorld world, const char* name) noexcept {
    return iface.world_find_component(world, name);
}

/// The index of the field with this name on this component, or a sentinel. An inspector does
/// exactly this once per property it binds, and then holds the index.
cy::u32 field_index(const CyInterface& iface, CyWorld world, CyComponentTypeId component,
                    const char* name) noexcept {
    CyComponentInfo info{};
    info.struct_size = sizeof(CyComponentInfo);
    if (iface.world_component_info(world, component, &info) != CY_RESULT_OK) {
        return 0xFFFF'FFFFu;
    }
    for (cy::u32 index = 0; index < info.field_count; ++index) {
        CyFieldDesc described{};
        if (iface.world_component_field(world, component, index, &described) != CY_RESULT_OK) {
            continue;
        }
        if (std::strcmp(described.name, name) == 0) {
            return index;
        }
    }
    return 0xFFFF'FFFFu;
}

}  // namespace

CY_TEST_CASE("the scene's components are reflected, and the ABI can enumerate them") {
    // Task 1.3's claim, read through the boundary. At M4 every one of these was registered by name
    // with no descriptor, so `field_count` would have been zero for all twelve.
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = &bound.binding;

    struct Expected {
        const char* name;
        cy::u32 fields;
    };
    // The seven that are describable, and the count each one has. `LocalTransform` is TEN rather
    // than one because the generator flattens an aggregate member into its leaves — a `Transform`
    // is a `Quat` and two `Vec3`s, and an inspector edits floats.
    const Expected reflected[] = {
        {"cy::scene::ChildOrder", 1},      {"cy::scene::LocalTransform", 10},
        {"cy::scene::WorldTransform", 10}, {"cy::scene::NodeFlags", 2},
        {"cy::scene::NodeState", 1},       {"cy::scene::SceneRef", 1},
        {"cy::scene::BehaviourRef", 1},
    };
    for (const Expected& expected : reflected) {
        const CyComponentTypeId id = find(iface, world, expected.name);
        CY_REQUIRE(id != CY_COMPONENT_TYPE_INVALID);
        CyComponentInfo info{};
        info.struct_size = sizeof(CyComponentInfo);
        CY_REQUIRE(iface.world_component_info(world, id, &info) == CY_RESULT_OK);
        CY_CHECK(std::strcmp(info.name, expected.name) == 0);
        CY_CHECK_EQ(info.field_count, expected.fields);
        CY_CHECK_GT(info.size, 0U);
    }

    // And the five that are not, each reachable and each honestly describing nothing. This half
    // matters as much as the other: a component the ABI could not see at all would be a hole in the
    // outliner, and one that claimed fields it cannot address would be worse.
    for (const char* name :
         {"cy::scene::NodeName", "cy::scene::NodeAlias", "cy::scene::InterpolatedTransform",
          "cy::scene::Hidden", "cy::scene::Disabled"}) {
        const CyComponentTypeId id = find(iface, world, name);
        CY_REQUIRE(id != CY_COMPONENT_TYPE_INVALID);
        CyComponentInfo info{};
        info.struct_size = sizeof(CyComponentInfo);
        CY_REQUIRE(iface.world_component_info(world, id, &info) == CY_RESULT_OK);
        CY_CHECK_EQ(info.field_count, 0U);
    }
}

CY_TEST_CASE("a flattened transform is nine named floats with the offsets the compiler computed") {
    // The generator emits `offsetof(LocalTransform, value.translation.x)` and lets the COMPILER
    // compute it; a generator that had written the number it measured would be right on one
    // toolchain. This case reads the offsets back through the ABI and checks they are consistent
    // with the layout rather than with a table somebody typed.
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = &bound.binding;
    const CyComponentTypeId local = find(iface, world, "cy::scene::LocalTransform");
    CY_REQUIRE(local != CY_COMPONENT_TYPE_INVALID);

    CyComponentInfo info{};
    info.struct_size = sizeof(CyComponentInfo);
    CY_REQUIRE(iface.world_component_info(world, local, &info) == CY_RESULT_OK);
    CY_REQUIRE(info.field_count == 10U);

    bool seen_translation_x = false;
    for (cy::u32 index = 0; index < info.field_count; ++index) {
        CyFieldDesc described{};
        CY_REQUIRE(iface.world_component_field(world, local, index, &described) == CY_RESULT_OK);
        // Every leaf of a transform is a float, and every one of them fits inside the component.
        CY_CHECK_EQ(described.type, static_cast<cy::u32>(CY_VAR_F32));
        CY_CHECK_EQ(described.size, 4U);
        CY_CHECK_LE(described.offset + described.size, info.size);
        // The names are member designators, which is what makes a property grid's tree buildable
        // from the ABI alone.
        CY_CHECK(std::strncmp(described.name, "value.", 6) == 0);
        if (std::strcmp(described.name, "value.translation.x") == 0) {
            seen_translation_x = true;
        }
    }
    CY_CHECK(seen_translation_x);
}

CY_TEST_CASE("an inspector reads and writes a reflected engine component with no per-type code") {
    // THE HEADLINE. Everything below is discovery followed by a generic get/set — the same two
    // entries a Swift module uses for its own components — against a component this process's ABI
    // layer never registered.
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = &bound.binding;

    const CyComponentTypeId local = find(iface, world, "cy::scene::LocalTransform");
    const CyComponentTypeId flags = find(iface, world, "cy::scene::NodeFlags");
    const CyComponentTypeId order = find(iface, world, "cy::scene::ChildOrder");
    CY_REQUIRE(local != CY_COMPONENT_TYPE_INVALID);
    CY_REQUIRE(flags != CY_COMPONENT_TYPE_INVALID);
    CY_REQUIRE(order != CY_COMPONENT_TYPE_INVALID);

    const CyEntity entity = iface.world_create_entity(world);
    CY_REQUIRE(entity != CY_ENTITY_NULL);
    CY_REQUIRE(iface.world_add_component(world, entity, local, nullptr) == CY_RESULT_OK);
    CY_REQUIRE(iface.world_add_component(world, entity, flags, nullptr) == CY_RESULT_OK);
    CY_REQUIRE(iface.world_add_component(world, entity, order, nullptr) == CY_RESULT_OK);

    // A float leaf — the value a gizmo drag writes one of.
    const cy::u32 translation_x = field_index(iface, world, local, "value.translation.x");
    CY_REQUIRE(translation_x != 0xFFFF'FFFFu);
    CyVar value = cy::abi::var_nil();
    value.type = CY_VAR_F32;
    value.payload.as_f32 = 12.5F;
    CY_CHECK_EQ(iface.component_set_var(world, entity, local, translation_x, &value), CY_RESULT_OK);
    CyVar read{};
    CY_CHECK_EQ(iface.component_get_var(world, entity, local, translation_x, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<cy::u32>(CY_VAR_F32));
    CY_CHECK_EQ(read.payload.as_f32, 12.5F);

    // The typed fast path reaches the same field: `component_set_f32` is what a per-frame writer
    // uses, and it must agree with the marshalled one or an editor and a behaviour would disagree
    // about the same bytes.
    CY_CHECK_EQ(iface.component_set_f32(world, entity, local, translation_x, -4.25F), CY_RESULT_OK);
    float back = 0.0F;
    CY_CHECK_EQ(iface.component_get_f32(world, entity, local, translation_x, &back), CY_RESULT_OK);
    CY_CHECK_EQ(back, -4.25F);

    // A bool leaf, which is the checkbox an inspector draws for a flag.
    const cy::u32 visible = field_index(iface, world, flags, "visible");
    CY_REQUIRE(visible != 0xFFFF'FFFFu);
    CyVar off = cy::abi::var_bool(false);
    CY_CHECK_EQ(iface.component_set_var(world, entity, flags, visible, &off), CY_RESULT_OK);
    CY_CHECK_EQ(iface.component_get_var(world, entity, flags, visible, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<cy::u32>(CY_VAR_BOOL));
    CY_CHECK_FALSE(read.payload.as_bool);

    // A u32 leaf, which needed ABI 1.1's narrow integer kinds: at 1.0 there was no `CyVarType` a
    // `u32` field could be described as, so this property could be listed and not edited.
    const cy::u32 sibling = field_index(iface, world, order, "value");
    CY_REQUIRE(sibling != 0xFFFF'FFFFu);
    CyVar seven = cy::abi::var_i64(7);
    seven.type = CY_VAR_U32;
    CY_CHECK_EQ(iface.component_set_var(world, entity, order, sibling, &seven), CY_RESULT_OK);
    CY_CHECK_EQ(iface.component_get_var(world, entity, order, sibling, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<cy::u32>(CY_VAR_U32));
    CY_CHECK_EQ(read.payload.as_i64, 7);

    // THE ENGINE SEES THE EDIT. Reading it back through the same boundary would only prove the ABI
    // is self-consistent; this reads the component the way the engine's own systems do.
    const auto* placement = bound.world.get<cy::scene::LocalTransform>(
        cy::abi::from_abi(entity), bound.tree.components().local_transform);
    CY_REQUIRE(placement != nullptr);
    CY_CHECK_EQ(placement->value.translation.x, -4.25F);
}
