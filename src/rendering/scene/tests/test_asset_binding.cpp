// An authored asset reference becomes a renderer handle. M8.b task 11.3, the second half.
//
// The regression these cases hold shut is the one M8.a's artefact photograph showed, one layer
// further along than `test_reflection.cpp`'s: with the component reflected, a `.cyworld` carries
// the mesh a designer picked — and until `bind_render_assets` existed, nothing turned that
// reference into the `render::MeshHandle` the extract stage publishes, so every authored instance
// still reached the renderer naming mesh nothing.
//
// The case that matters most is the stale one: an unresolved reference CLEARS the handle. A handle
// is a slot index and a generation, the slot is reused, and an instance that kept a stale handle
// draws whatever asset is in that slot now — a wrong mesh, which looks like an authoring mistake
// and is not one.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/asset_binding.h>
#include <cy/rendering/scene/components.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u64;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

constexpr u64 kKnownMesh = 0x1000;
constexpr u64 kKnownMaterial = 0x2000;
constexpr u64 kUnknown = 0x9999;

/// The only asset library these cases have: one mesh and one material, answered by identity.
///
/// A table rather than a mock of an asset system, for the reason the header gives — the resolver is
/// the seam, and what is on the far side of it is not this module's business.
MeshResolution resolve_mesh(AssetRef reference, void* /*user*/) noexcept {
    MeshResolution resolved;
    if (reference.low != kKnownMesh) {
        return resolved;
    }
    resolved.mesh = cy::render::MeshHandle::from_slot(7, 3);
    resolved.local_bounds =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 1.0F, 0.0F}, cy::Vec3{2.0F, 2.0F, 2.0F});
    resolved.found = true;
    return resolved;
}

MaterialResolution resolve_material(AssetRef reference, void* /*user*/) noexcept {
    MaterialResolution resolved;
    if (reference.low != kKnownMaterial) {
        return resolved;
    }
    resolved.material = cy::render::MaterialHandle::from_slot(4, 2);
    resolved.found = true;
    return resolved;
}

[[nodiscard]] AssetResolver library() noexcept {
    AssetResolver resolver;
    resolver.mesh = &resolve_mesh;
    resolver.material = &resolve_material;
    return resolver;
}

/// A world with the renderer's components, and entities carrying nothing but a `MeshRenderer` —
/// binding needs no placement, which is the point: it runs at load time and not in a frame.
struct Fixture {
    Fixture() noexcept : world(allocator()) {
        ok = world.initialize().has_value();
        auto registered = RenderComponents::register_all(world);
        ok = ok && registered.has_value();
        if (registered.has_value()) {
            render = *registered;
        }
    }

    /// An entity whose `MeshRenderer` names `mesh` and `material`, with handles already set to
    /// `stale` so that a case can tell "left alone" from "cleared" from "bound".
    [[nodiscard]] cy::ecs::Entity spawn(u64 mesh, u64 material, u32 stale) noexcept {
        cy::ecs::ComponentTypeId components[1] = {render.mesh_renderer};
        const auto entity = world.create(cy::Span<const cy::ecs::ComponentTypeId>(components, 1));
        if (!entity) {
            return cy::ecs::Entity{};
        }
        MeshRenderer renderer;
        renderer.mesh = AssetRef{0, mesh};
        renderer.material = AssetRef{0, material};
        renderer.mesh_handle = cy::render::MeshHandle::from_slot(stale, 1);
        renderer.material_handle = cy::render::MaterialHandle::from_slot(stale, 1);
        if (!world.set(*entity, render.mesh_renderer, renderer).has_value()) {
            return cy::ecs::Entity{};
        }
        return *entity;
    }

    [[nodiscard]] MeshRenderer read(cy::ecs::Entity entity) const noexcept {
        MeshRenderer value;
        const void* stored = world.get(entity, render.mesh_renderer);
        if (stored != nullptr) {
            value = *static_cast<const MeshRenderer*>(stored);
        }
        return value;
    }

    cy::ecs::World world;
    RenderComponents render;
    bool ok = false;
};

}  // namespace

CY_TEST_CASE("an authored asset reference becomes the handle the extract stage publishes") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    const cy::ecs::Entity entity = fixture.spawn(kKnownMesh, kKnownMaterial, 11);
    CY_REQUIRE(entity.valid());

    BindReport report;
    CY_REQUIRE(bind_render_assets(fixture.world, fixture.render, library(), report).has_value());

    CY_CHECK_EQ(report.examined, 1U);
    CY_CHECK_EQ(report.meshes_bound, 1U);
    CY_CHECK_EQ(report.materials_bound, 1U);
    CY_CHECK_EQ(report.unresolved_meshes, 0U);

    const MeshRenderer bound = fixture.read(entity);
    CY_CHECK(bound.mesh_handle == cy::render::MeshHandle::from_slot(7, 3));
    CY_CHECK(bound.material_handle == cy::render::MaterialHandle::from_slot(4, 2));
    // THE BOUNDS TRAVEL WITH THE HANDLE. `components.h` requires the component to hold the mesh's
    // own bounds rather than look them up, and a binding that moved the handle without them would
    // leave every authored instance culled against the default half-unit box.
    CY_CHECK_EQ(bound.local_bounds.max.y, 3.0F);
}

CY_TEST_CASE("a reference nothing resolves clears the handle rather than keeping a stale one") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    const cy::ecs::Entity entity = fixture.spawn(kUnknown, kUnknown, 11);
    CY_REQUIRE(entity.valid());

    BindReport report;
    CY_REQUIRE(bind_render_assets(fixture.world, fixture.render, library(), report).has_value());

    CY_CHECK_EQ(report.unresolved_meshes, 1U);
    CY_CHECK_EQ(report.unresolved_materials, 1U);
    CY_CHECK_EQ(report.meshes_bound, 0U);

    const MeshRenderer bound = fixture.read(entity);
    CY_CHECK(bound.mesh_handle.is_null());
    CY_CHECK(bound.material_handle.is_null());
}

CY_TEST_CASE("a component that names no asset keeps the handle its caller assigned") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    // What `samples/03-first-light` and M3's own suites do: assign a handle directly and never
    // author a reference. A binding pass over the same world must not take it away.
    const cy::ecs::Entity entity = fixture.spawn(0, 0, 11);
    CY_REQUIRE(entity.valid());

    BindReport report;
    CY_REQUIRE(bind_render_assets(fixture.world, fixture.render, library(), report).has_value());

    CY_CHECK_EQ(report.unreferenced_meshes, 1U);
    CY_CHECK_EQ(report.unreferenced_materials, 1U);
    CY_CHECK_EQ(report.unresolved_meshes, 0U);

    const MeshRenderer bound = fixture.read(entity);
    CY_CHECK(bound.mesh_handle == cy::render::MeshHandle::from_slot(11, 1));
}

CY_TEST_CASE("binding is idempotent and reports every component it visited") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    CY_REQUIRE(fixture.spawn(kKnownMesh, kKnownMaterial, 11).valid());
    CY_REQUIRE(fixture.spawn(kKnownMesh, kUnknown, 12).valid());
    const cy::ecs::Entity third = fixture.spawn(kUnknown, kKnownMaterial, 13);
    CY_REQUIRE(third.valid());

    BindReport first;
    CY_REQUIRE(bind_render_assets(fixture.world, fixture.render, library(), first).has_value());
    BindReport second;
    CY_REQUIRE(bind_render_assets(fixture.world, fixture.render, library(), second).has_value());

    CY_CHECK_EQ(first.examined, 3U);
    CY_CHECK_EQ(first.meshes_bound, 2U);
    CY_CHECK_EQ(first.unresolved_meshes, 1U);
    CY_CHECK_EQ(first.materials_bound, 2U);
    CY_CHECK_EQ(first.unresolved_materials, 1U);
    // The second pass sees the same world and says the same thing — which is what lets a host run
    // it after every load and every streaming step without tracking what it already bound.
    CY_CHECK_EQ(second.examined, first.examined);
    CY_CHECK_EQ(second.meshes_bound, first.meshes_bound);
    CY_CHECK_EQ(second.unresolved_meshes, first.unresolved_meshes);
    CY_CHECK(fixture.read(third).mesh_handle.is_null());
}

CY_TEST_CASE("a pass with no mesh resolver is refused instead of clearing the world") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    const cy::ecs::Entity entity = fixture.spawn(kKnownMesh, kKnownMaterial, 11);
    CY_REQUIRE(entity.valid());

    BindReport report;
    AssetResolver empty;
    CY_CHECK_FALSE(bind_render_assets(fixture.world, fixture.render, empty, report).has_value());
    CY_CHECK(fixture.read(entity).mesh_handle == cy::render::MeshHandle::from_slot(11, 1));

    // And a world the components were never registered in is refused rather than silently visiting
    // nothing and reporting success.
    cy::ecs::World other(allocator());
    CY_REQUIRE(other.initialize().has_value());
    CY_CHECK_FALSE(bind_render_assets(other, fixture.render, library(), report).has_value());
}
