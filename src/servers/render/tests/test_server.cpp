// The handle-based render server, and the snapshot it consumes. Tasks 4.1.1, 4.1.2, 4.1.6.
//
// THE FIRST SCENARIO IN `rendering-architecture` IS THE PREMISE OF THIS WHOLE FILE:
//
//   "WHEN a test drives RenderServer directly with handles THEN it SHALL produce a frame without an
//    ECS world or scene tree existing"
//
// Nothing below constructs a world, a node, a device or a window, and none of those headers is even
// included — the render server is layer 2 and cannot reach them. That is the requirement being a
// property of the build rather than a discipline.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/server.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

struct Fixture {
    Fixture() noexcept : server(allocator()), draws(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        // A case here submits no debug primitives, so the store is sized to the smallest thing that
        // is still a store. The default is 4096 primitives in each of two buffers — around 850 KiB
        // that `initialize()` constructs — and paying it once per test case is a millisecond at
        // `-O0`, which is the whole of the `unit` budget spent measuring a default.
        RenderServerConfig config;
        config.debug_primitive_capacity = 8;
        config.debug_label_capacity = 4;
        if (!server.configure(config).has_value()) {
            return false;
        }
        if (!server.initialize().has_value()) {
            return false;
        }
        SceneDescription scene_desc;
        scene_desc.name = cy::Name::intern("test-scene");
        scene_desc.instance_capacity = 64;
        auto made_scene = server.create_scene(scene_desc);
        if (!made_scene) {
            return false;
        }
        scene = *made_scene;

        MeshDescription mesh_desc;
        mesh_desc.name = cy::Name::intern("cube");
        mesh_desc.vertex_count = 24;
        mesh_desc.index_count = 36;
        MeshSurface surface;
        surface.vertex_count = 24;
        surface.index_count = 36;
        auto made_mesh = server.create_mesh(mesh_desc, cy::Span<const MeshSurface>(&surface, 1),
                                            cy::Span<const MeshLod>());
        if (!made_mesh) {
            return false;
        }
        mesh = *made_mesh;

        MaterialRecord material_desc;
        material_desc.name = cy::Name::intern("standard");
        material_desc.program = 7;
        auto made_material = server.create_material(material_desc);
        if (!made_material) {
            return false;
        }
        material = *made_material;

        ViewDescription view_desc;
        view_desc.name = cy::Name::intern("main");
        view_desc.scene = scene;
        view_desc.viewport = ViewportRect{0, 0, 1920, 1080};
        // Looking down −Z from the origin, which is the engine's convention and the identity view.
        view_desc.camera = cy::Transform::identity();
        auto made_view = server.create_view(view_desc);
        if (!made_view) {
            return false;
        }
        view = *made_view;
        return true;
    }

    [[nodiscard]] InstanceDescription instance_at(f32 z, u64 stable_id) const noexcept {
        InstanceDescription desc;
        desc.mesh = mesh;
        desc.material = material;
        desc.transform = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, z});
        desc.stable_id = stable_id;
        return desc;
    }

    RenderServer server;
    SceneHandle scene;
    MeshHandle mesh;
    MaterialHandle material;
    ViewHandle view;
    cy::Array<DrawItem> draws;
    ViewStatistics stats;
};

}  // namespace

CY_TEST_CASE("a server produces a sorted draw list with no world and no device") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    // Three instances in front of the camera, published in an order that is not their depth order.
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-20.0F, 3)).has_value());
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 1)).has_value());
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-12.0F, 2)).has_value());

    const View* view = fixture.server.view(fixture.view);
    CY_REQUIRE(view != nullptr);
    CY_REQUIRE(fixture.server.collect_draws(fixture.scene, *view, fixture.draws, fixture.stats)
                   .has_value());

    CY_CHECK_EQ(fixture.stats.instances_visible, 3U);
    CY_CHECK_EQ(fixture.draws.size(), 3U);
    CY_CHECK(draws_are_ordered(fixture.draws.span()));
    // One pipeline, one material, one mesh — so depth is what separates them, front to back.
    CY_CHECK_EQ(fixture.draws[0].stable_id, 1ULL);
    CY_CHECK_EQ(fixture.draws[1].stable_id, 2ULL);
    CY_CHECK_EQ(fixture.draws[2].stable_id, 3ULL);
}

CY_TEST_CASE("an instance with no stable identity is refused at creation") {
    // design.md §6, enforced where it is cheap. Without a stable id an instance's draw order is
    // publication order, and the only place to catch that is here.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    InstanceDescription desc = fixture.instance_at(-5.0F, 0);
    const auto created = fixture.server.create_instance(fixture.scene, desc);
    CY_REQUIRE_FALSE(created.has_value());
    CY_CHECK_EQ(created.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("two instances cannot share a stable identity in one scene") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 42)).has_value());
    const auto duplicate =
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-6.0F, 42));
    CY_REQUIRE_FALSE(duplicate.has_value());
    CY_CHECK_EQ(duplicate.error().code, cy::ErrorCode::AlreadyExists);
}

CY_TEST_CASE("an instance outside the frustum is culled, and one behind the camera is too") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    // In front, and behind. The camera looks down −Z, so a positive z is behind it.
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 1)).has_value());
    CY_REQUIRE(
        fixture.server.create_instance(fixture.scene, fixture.instance_at(50.0F, 2)).has_value());

    const View* view = fixture.server.view(fixture.view);
    CY_REQUIRE(view != nullptr);
    CY_REQUIRE(fixture.server.collect_draws(fixture.scene, *view, fixture.draws, fixture.stats)
                   .has_value());
    CY_CHECK_EQ(fixture.stats.instances_considered, 2U);
    CY_CHECK_EQ(fixture.stats.instances_visible, 1U);
    CY_CHECK_EQ(fixture.draws[0].stable_id, 1ULL);
}

CY_TEST_CASE("an invisible instance draws in no view") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto handle = fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 1));
    CY_REQUIRE(handle.has_value());
    CY_REQUIRE(fixture.server.set_instance_visible(*handle, false).has_value());

    const View* view = fixture.server.view(fixture.view);
    CY_REQUIRE(view != nullptr);
    CY_REQUIRE(fixture.server.collect_draws(fixture.scene, *view, fixture.draws, fixture.stats)
                   .has_value());
    CY_CHECK_EQ(fixture.stats.instances_visible, 0U);
    CY_CHECK(fixture.draws.empty());
}

CY_TEST_CASE("a layer mask excludes an instance from a view without touching the instance") {
    // "Views SHALL be first class and plural": a shadow view for a light that only lights the world
    // draws a narrower mask, and the mask is what expresses that.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    InstanceDescription desc = fixture.instance_at(-5.0F, 1);
    desc.layer_mask = 1U << 3U;
    CY_REQUIRE(fixture.server.create_instance(fixture.scene, desc).has_value());

    View* view = fixture.server.view(fixture.view);
    CY_REQUIRE(view != nullptr);
    view->desc.layer_mask = 1U << 4U;
    CY_REQUIRE(fixture.server.collect_draws(fixture.scene, *view, fixture.draws, fixture.stats)
                   .has_value());
    CY_CHECK_EQ(fixture.stats.instances_considered, 0U);

    view->desc.layer_mask = kAllLayers;
    CY_REQUIRE(fixture.server.collect_draws(fixture.scene, *view, fixture.draws, fixture.stats)
                   .has_value());
    CY_CHECK_EQ(fixture.stats.instances_visible, 1U);
}

CY_TEST_CASE("moving an instance keeps the previous transform a frame behind") {
    // What motion vectors and shadow invalidation both read. Getting it wrong produces zero motion
    // vectors, which looks like nothing being wrong.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto handle = fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 1));
    CY_REQUIRE(handle.has_value());
    CY_REQUIRE(fixture.server
                   .set_instance_transform(
                       *handle, cy::Transform::from_translation(cy::Vec3{1.0F, 0.0F, -5.0F}))
                   .has_value());

    const Scene* scene = fixture.server.scene(fixture.scene);
    const InstanceRecord* record = fixture.server.instance(*handle);
    CY_REQUIRE(scene != nullptr);
    CY_REQUIRE(record != nullptr);
    const GpuInstance& gpu = scene->gpu.at(record->range.first);
    // Row-major 4x3: the translation is the fourth entry of each row.
    CY_CHECK_NEAR(gpu.transform[3], 1.0F, 1e-6F);
    CY_CHECK_NEAR(gpu.previous_transform[3], 0.0F, 1e-6F);
    CY_CHECK((gpu.flags & kInstanceMoved) != 0U);
}

CY_TEST_CASE("destroying an instance frees its slot and clears its record") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto handle = fixture.server.create_instance(fixture.scene, fixture.instance_at(-5.0F, 1));
    CY_REQUIRE(handle.has_value());
    const InstanceRecord* record = fixture.server.instance(*handle);
    CY_REQUIRE(record != nullptr);
    const u32 slot = record->range.first;

    fixture.server.destroy_instance(*handle);
    CY_CHECK(fixture.server.instance(*handle) == nullptr);
    CY_CHECK_FALSE(fixture.server.find_instance(fixture.scene, 1));

    const Scene* scene = fixture.server.scene(fixture.scene);
    CY_REQUIRE(scene != nullptr);
    CY_CHECK_FALSE(scene->gpu.at(slot).active());
}

CY_TEST_CASE("destroying a shader invalidates the materials that depend on it") {
    // "WHEN a material's shader is destroyed THEN dependent cached pipelines and descriptor sets
    // SHALL be invalidated through a dependency-tracking mechanism, not left dangling."
    struct Observed {
        u64 destroyed = 0;
        u64 dependent = 0;
        u32 calls = 0;
    };
    Observed observed;

    Fixture fixture;
    CY_REQUIRE(fixture.start());
    fixture.server.set_invalidation_observer(
        [](u64 destroyed, u64 dependent, void* user) noexcept {
            auto* record = static_cast<Observed*>(user);
            record->destroyed = destroyed;
            record->dependent = dependent;
            ++record->calls;
        },
        &observed);

    // A shader handle the server does not own — the shader system is another module's — recorded as
    // a dependency by hand, which is the interface a caller above uses.
    const u64 shader_bits = 0xDEADBEEFULL;
    MaterialRecord desc;
    desc.name = cy::Name::intern("dependent");
    auto material = fixture.server.create_material(desc);
    CY_REQUIRE(material.has_value());
    CY_REQUIRE(fixture.server.add_dependency(shader_bits, material->bits()).has_value());

    fixture.server.invalidate_dependents(shader_bits);
    CY_CHECK_EQ(observed.calls, 1U);
    CY_CHECK_EQ(observed.destroyed, shader_bits);
    CY_CHECK_EQ(observed.dependent, material->bits());

    // Forgotten afterwards: a second destruction of the same producer notifies nothing, because a
    // dependency that fires twice is a dependency that outlived what it described.
    fixture.server.invalidate_dependents(shader_bits);
    CY_CHECK_EQ(observed.calls, 1U);
}

CY_TEST_CASE("material table indices are dense and are reused") {
    // The index is the server's to assign — two owners of a shared dense table eventually collide.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    MaterialRecord desc;
    auto first = fixture.server.create_material(desc);
    auto second = fixture.server.create_material(desc);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    const u32 second_index = fixture.server.material(*second)->table_index;
    CY_CHECK_NE(fixture.server.material(*first)->table_index, second_index);

    fixture.server.destroy_material(*second);
    auto third = fixture.server.create_material(desc);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(fixture.server.material(*third)->table_index, second_index);
}

CY_TEST_CASE("a stale handle is answered no rather than aliasing what replaced it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    MaterialRecord desc;
    auto first = fixture.server.create_material(desc);
    CY_REQUIRE(first.has_value());
    const MaterialHandle stale = *first;
    fixture.server.destroy_material(stale);
    auto replacement = fixture.server.create_material(desc);
    CY_REQUIRE(replacement.has_value());

    CY_CHECK(fixture.server.material(stale) == nullptr);
    CY_CHECK(fixture.server.material(*replacement) != nullptr);
}

CY_TEST_CASE("views are creation-ordered and there is no main view") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    ViewDescription shadow;
    shadow.name = cy::Name::intern("shadow");
    shadow.scene = fixture.scene;
    shadow.purpose = ViewPurpose::Shadow;
    shadow.viewport = ViewportRect{0, 0, 1024, 1024};
    auto shadow_view = fixture.server.create_view(shadow);
    CY_REQUIRE(shadow_view.has_value());

    CY_REQUIRE_EQ(fixture.server.views().size(), 2U);
    CY_CHECK_EQ(fixture.server.views()[0], fixture.view);
    CY_CHECK_EQ(fixture.server.views()[1], *shadow_view);

    fixture.server.destroy_view(fixture.view);
    CY_REQUIRE_EQ(fixture.server.views().size(), 1U);
    CY_CHECK_EQ(fixture.server.views()[0], *shadow_view);
}

CY_TEST_CASE("an unimplemented family is an error rather than a handle to nothing") {
    // Eight of the twenty families have storage at M3. A light does; nothing here creates a decal,
    // and there is no `create_decal` to call — the absence is the interface, and the handle type
    // exists so the spelling does not change when M7 adds the pool.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    LightDescription light;
    light.stable_id = 0;
    const auto anonymous = fixture.server.create_light(light);
    CY_REQUIRE_FALSE(anonymous.has_value());
    light.stable_id = 5;
    CY_CHECK(fixture.server.create_light(light).has_value());
}

CY_TEST_CASE("the memory report counts what the server knows the size of") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    TextureRecord texture;
    texture.name = cy::Name::intern("albedo");
    texture.format = TextureFormat::Bc7Srgb;
    texture.width = 256;
    texture.height = 256;
    texture.mip_levels = 0;  // meaning "a full chain"
    auto handle = fixture.server.create_texture(texture);
    CY_REQUIRE(handle.has_value());

    fixture.server.refresh_statistics(fixture.scene);
    const FrameStatistics& stats = fixture.server.frame_statistics();
    CY_CHECK_GT(stats.memory_bytes[static_cast<u32>(MemoryCategory::Textures)], 0ULL);
    CY_CHECK_GT(stats.memory_bytes[static_cast<u32>(MemoryCategory::Meshes)], 0ULL);
    CY_CHECK_EQ(stats.gpu_scene_capacity, 64U);

    fixture.server.destroy_texture(*handle);
    fixture.server.refresh_statistics(fixture.scene);
    CY_CHECK_EQ(stats.memory_bytes[static_cast<u32>(MemoryCategory::Textures)], 0ULL);
}
