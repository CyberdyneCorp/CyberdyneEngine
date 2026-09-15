// The frame, assembled out of the renderer's own parts. M8.b task 11.2.
//
// ================================================================================================
// WHAT THIS SUITE IS FOR, AND WHY IT IS NOT A SCREENSHOT
// ================================================================================================
//
// The defect is a LINK GRAPH defect: eight modules that nothing but their own tests linked. A
// screenshot would not have caught it and would not catch it coming back. What catches it is a
// suite that reads one number out of each of the eight from ONE frame — so a module that stopped
// being called stops producing its number, and the case fails naming it.
//
// The last case runs the assembled frame through the NULL backend, end to end: an ECS world with
// authored `cy::rendering::MeshRenderer` components, extracted at the commit boundary, applied to a
// spatial index, culled, sorted into draws, declared as thirteen passes and EXECUTED. That is the
// chain M8.a's artefact report said was broken — "the mesh a primitive references reaches no
// renderer" — asserted rather than photographed.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/determinism/commit.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/capture_manifest.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/assembly/scene_index.h>
#include <cy/rendering/scene/extract.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <new>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering;
using namespace cy::rendering::assembly;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;

[[nodiscard]] AssemblyDescription make_description() noexcept {
    AssemblyDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.near_plane = 0.1F;
    description.far_plane = 200.0F;
    description.clusters = ClusterGridConfig{32, 16, 32};
    // Every stage that changes the frame's SHAPE is on, so the case is about the whole chain rather
    // than about the default. Temporal antialiasing is the interesting one: it is what forces the
    // prepass into `DepthNormalVelocity`, and that is the frame's structure changing because a post
    // stage asked for it — which is the join this module exists to make.
    description.post.ambient_occlusion = true;
    description.post.temporal_antialiasing = true;
    description.post.bloom = true;
    description.post.colour_grading = true;
    description.shadows.slots = 64;
    description.material_capacity = 32;
    description.gpu_culling = false;  // no device in the CPU cases
    return description;
}

/// A camera looking down -Z from the origin, and the view it produces.
[[nodiscard]] AssemblyView make_view(cy::Span<const cy::render::LightDescription> lights) noexcept {
    AssemblyView view;
    view.fov_y_radians = 1.0471975512F;
    view.projection = cy::perspective_reversed_z(
        view.fov_y_radians, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F, 200.0F);
    view.view =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    view.cull.frustum = cy::Frustum::from_view_projection(view.projection * view.view);
    view.cull.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    view.cull.camera_forward = Vec3{0.0F, 0.0F, -1.0F};
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = lights;
    view.sun_direction = Vec3{0.0F, 1.0F, 0.0F};
    return view;
}

/// A point light in front of the camera, and a sun. Two kinds, because the assembly routes them
/// differently: a directional light is not clustered and a point light is.
[[nodiscard]] cy::render::LightDescription point_light(Vec3 at, u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Point;
    light.transform = cy::Transform::from_translation(at);
    light.intensity = 1000.0F;
    light.range = 20.0F;
    light.stable_id = id;
    return light;
}

[[nodiscard]] cy::render::LightDescription sun(u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Directional;
    light.transform = cy::Transform::identity();
    light.intensity = 100000.0F;
    light.stable_id = id;
    return light;
}

/// A world, a scene tree, the renderer's components and an extractor over them.
///
/// NOTHING IS MOCKED. The world is the ECS's, the transforms are the scene layer's, the snapshot is
/// the render server's and the extractor is `cy::rendering::SnapshotExtractor` — which is the whole
/// point: the chain being asserted is the chain a runtime uses.
struct WorldFixture {
    WorldFixture() noexcept
        : world(allocator()),
          tree(world),
          ok(world.initialize().has_value() && tree.initialize().has_value()),
          scene(tree.components()),
          buffer(allocator()) {
        auto registered = RenderComponents::register_all(world);
        ok = ok && registered.has_value();
        if (registered.has_value()) {
            render = *registered;
        }
        extractor = new (storage) SnapshotExtractor(allocator(), world, render, scene, buffer);
    }

    ~WorldFixture() { extractor->~SnapshotExtractor(); }

    WorldFixture(const WorldFixture&) = delete;
    WorldFixture& operator=(const WorldFixture&) = delete;

    /// An entity with a placement and a mesh, at `position`, referencing mesh slot `mesh`.
    [[nodiscard]] cy::ecs::Entity spawn(Vec3 position, u32 mesh) noexcept {
        cy::ecs::ComponentTypeId components[2] = {scene.world_transform, render.mesh_renderer};
        const auto entity = world.create(cy::Span<const cy::ecs::ComponentTypeId>(components, 2));
        if (!entity) {
            return cy::ecs::Entity{};
        }
        cy::scene::WorldTransform transform;
        transform.value = cy::Transform::from_translation(position);
        if (!world.set(*entity, scene.world_transform, transform).has_value()) {
            return cy::ecs::Entity{};
        }
        MeshRenderer renderer;
        // WHAT A NODE REFERENCES: an asset id, reflected and serialisable, and the handle it
        // resolved to in this process. Task 11.3 is what made the first of the two exist.
        renderer.mesh = AssetRef{0, 0x1000ULL + mesh};
        renderer.mesh_handle = cy::render::MeshHandle::from_slot(mesh, 1);
        renderer.material_handle = cy::render::MaterialHandle::from_slot(mesh, 1);
        renderer.local_bounds =
            cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F});
        if (!world.set(*entity, render.mesh_renderer, renderer).has_value()) {
            return cy::ecs::Entity{};
        }
        return *entity;
    }

    [[nodiscard]] bool commit(u64 version) const noexcept {
        cy::determinism::CommitRecord record;
        record.state_version = version;
        return extractor->on_commit(record).has_value();
    }

    [[nodiscard]] const cy::render::RenderSnapshot& published() const noexcept {
        return *buffer.readable();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    bool ok = false;
    cy::scene::SceneComponents scene;
    RenderComponents render;
    cy::render::SnapshotBuffer buffer;
    alignas(SnapshotExtractor) unsigned char storage[sizeof(SnapshotExtractor)]{};
    SnapshotExtractor* extractor = nullptr;
};

/// A surface query that resolves a draw's mesh through the scene index — which is what "the runtime
/// draws what a node references" reduces to when the plumbing exists.
cy::Span<const DrawSurface> surfaces_from_index(const VisibleInstance& instance,
                                                void* user) noexcept {
    static thread_local DrawSurface surface;
    const auto* index = static_cast<const SceneIndex*>(user);
    const SceneIndex::Surface found = index->surface_of(instance.slot);
    surface = DrawSurface{};
    surface.mesh = found.mesh.index();
    surface.material = found.material.index();
    surface.blend = cy::render::BlendMode::Opaque;
    return {&surface, 1};
}

}  // namespace

CY_TEST_CASE("the assembly refuses a description that would render nothing") {
    FrameAssembly assembly(allocator());
    AssemblyDescription description = make_description();

    description.width = 0;
    CY_CHECK_FALSE(assembly.initialize(description).has_value());

    description = make_description();
    description.far_plane = std::numeric_limits<f32>::infinity();
    CY_CHECK_FALSE(assembly.initialize(description).has_value());

    description = make_description();
    description.material_capacity = 0;
    CY_CHECK_FALSE(assembly.initialize(description).has_value());

    // And it refuses to assemble at all before it is initialized, rather than declaring an empty
    // frame that compiles.
    SpatialIndex index(allocator());
    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_CHECK_FALSE(
        assembly.assemble(index, make_view({}), FrameSinks{}, graph, report).has_value());
}

CY_TEST_CASE("one frame reads a number out of every one of the eight modules") {
    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());

    SpatialIndex index(allocator());
    for (u32 which = 0; which < 12; ++which) {
        SpatialEntry entry;
        const f32 z = -5.0F - static_cast<f32>(which);
        entry.bounds = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, z}, Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 100U + which;
        entry.radius = 0.9F;
        CY_REQUIRE(index.insert(entry).has_value());
    }

    const cy::render::LightDescription lights[] = {sun(1), point_light(Vec3{0.0F, 0.0F, -6.0F}, 2),
                                                   point_light(Vec3{2.0F, 0.0F, -8.0F}, 3)};
    const AssemblyView view = make_view({lights, 3});

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(assembly.assemble(index, view, FrameSinks{}, graph, report).has_value());

    // cy::rendering-culling — the survivors.
    CY_CHECK_EQ(report.cull.tested, 12U);
    CY_CHECK_GT(report.cull.visible, 0U);
    // cy::rendering-forward — the draw list and the cluster assignment.
    CY_CHECK_EQ(report.draws, report.cull.visible);
    CY_CHECK_EQ(report.opaque, report.draws);
    // 320/32 by 180/32 tiles, rounded up, times the sixteen depth slices the config asked for.
    CY_CHECK_EQ(report.clusters.clusters, 10U * 6U * 16U);
    CY_CHECK_GT(report.clusters.elements, 0U);
    // cy::rendering-lighting — the records. Three lights in, three records out; only the two
    // punctual ones are clustered, because a directional light reaches every cluster.
    CY_CHECK_EQ(report.lights, 3U);
    // cy::rendering-post — the chain, in order, with its refusal clear.
    CY_CHECK_EQ(report.post_refusal, PostChainRefusal::None);
    CY_CHECK_GT(report.post_stages, 0U);
    CY_CHECK_EQ(report.post_stage[0], PostStage::AmbientOcclusion);
    // cy::rendering-shadows — the pages the frame asked the cache for. Two shadow-casting lights
    // times three clip levels; none is resident on the first frame.
    CY_CHECK_EQ(report.shadow_pages_requested, 9U);
    CY_CHECK_EQ(report.shadow_pages_resident, 0U);
    // cy::rendering-sky — the table, rebuilt on the first frame.
    CY_CHECK(report.sky_rebuilt);
    CY_CHECK(assembly.sky().built());
    // REGRESSION (M10). This is the SKY's own diffuse contribution to an upward-facing surface,
    // not the sun's — a clear sky at noon delivers several thousand lux of it against the sun's
    // hundred thousand — and the BAND is the assertion rather than "greater than zero": the check
    // read zero-or-more for a whole milestone while the frame was integrating the sky from the
    // CENTRE OF THE PLANET. See `update_sky()`, where the camera's world position is now converted
    // into the planet-centred one the sky's own frame is written in.
    CY_CHECK_GT(assembly.sky_irradiance().y, 2000.0F);
    CY_CHECK_LT(assembly.sky_irradiance().y, 100000.0F);
    // cy::rendering-temporal — one frame advanced, and jitter applied in one place.
    CY_CHECK_EQ(report.temporal_frame, 1U);
    // cy::rendering-material — the table the draws index, AND the comparison between the two. The
    // gate's sentence was "the material compiler fills a GPU material table nothing draws with";
    // `material_slots` alone would only prove the table exists. `draws_without_material` is the
    // join: every draw's slot was checked against the table's length. The default surface query
    // gives each instance its own slot, and there are fewer instances than the table's 32.
    CY_CHECK_EQ(report.material_slots, 32U);
    CY_CHECK_EQ(report.draws_without_material, 0U);
    CY_CHECK_EQ(report.material_slots_live, 0U);
    // And the frame itself: a temporal stage in the chain is what puts the prepass into
    // `DepthNormalVelocity` and allocates a velocity target. That is post/ deciding forward/'s
    // structure, which is the join this module exists to make.
    CY_CHECK_EQ(report.prepass, PrepassMode::DepthNormalVelocity);
    CY_CHECK(assembly.resources().velocity != kInvalidResource);
    CY_CHECK(assembly.resources().normal_roughness != kInvalidResource);
    CY_CHECK_GT(report.passes_declared, 10U);
    CY_REQUIRE(graph.status().has_value());
}

CY_TEST_CASE("a second frame finds its shadow pages resident and its sky table reused") {
    // The half of the frame that is STATE rather than a computation, and the half a suite that only
    // ever assembled one frame would never see.
    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());

    SpatialIndex index(allocator());
    SpatialEntry entry;
    entry.bounds = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, -6.0F}, Vec3{0.5F, 0.5F, 0.5F});
    entry.stable_id = 7;
    entry.radius = 0.9F;
    CY_REQUIRE(index.insert(entry).has_value());

    const cy::render::LightDescription lights[] = {point_light(Vec3{0.0F, 0.0F, -6.0F}, 2)};
    const AssemblyView view = make_view({lights, 1});

    RenderGraph first(allocator());
    AssemblyReport a;
    CY_REQUIRE(assembly.assemble(index, view, FrameSinks{}, first, a).has_value());
    CY_CHECK_EQ(a.shadow_pages_resident, 0U);
    CY_CHECK(a.sky_rebuilt);

    // The cache has to be told a page was rendered before it counts as resident — a page that was
    // requested and never rasterised is still dirty, and reporting it otherwise would be the cache
    // lying about its own budget.
    for (cy::u8 level = 0; level < 3; ++level) {
        VirtualPage page;
        page.light_slot = 0;
        page.level = level;
        assembly.shadows().record_render(page, 0.02F);
    }

    RenderGraph second(allocator());
    AssemblyReport b;
    CY_REQUIRE(assembly.assemble(index, view, FrameSinks{}, second, b).has_value());
    CY_CHECK_EQ(b.shadow_pages_requested, 3U);
    CY_CHECK_EQ(b.shadow_pages_resident, 3U);
    // The sun has not moved, so the sky table is reused rather than integrated again.
    CY_CHECK_FALSE(b.sky_rebuilt);
    CY_CHECK_EQ(assembly.sky().stats().reuses, 1U);
    CY_CHECK_EQ(b.temporal_frame, 2U);
}

CY_TEST_CASE("a post chain that cannot be built stops the frame instead of dropping a stage") {
    // "WHEN temporal antialiasing and temporal upscaling are both enabled" — they are alternatives
    // at the same step, and a frame that quietly picked one would be a project shipping without the
    // antialiasing somebody enabled.
    FrameAssembly assembly(allocator());
    AssemblyDescription description = make_description();
    description.post.temporal_antialiasing = true;
    description.post.temporal_upscaling = true;
    CY_REQUIRE(assembly.initialize(description).has_value());

    SpatialIndex index(allocator());
    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_CHECK_FALSE(
        assembly.assemble(index, make_view({}), FrameSinks{}, graph, report).has_value());
    CY_CHECK_EQ(report.post_refusal, PostChainRefusal::BothTemporalStages);
    // Nothing was declared: a refusal is not a half-built frame.
    CY_CHECK_EQ(report.passes_declared, 0U);
}

CY_TEST_CASE("an authored mesh reaches the renderer and becomes a draw naming that mesh") {
    // THE CHAIN M8.a's ARTEFACT REPORT SAID WAS BROKEN, end to end and with nothing mocked:
    //
    //   MeshRenderer component -> SnapshotExtractor -> RenderSnapshot -> SceneIndex -> cull ->
    //   draw list -> a DrawItem whose surface names the mesh the node referenced.
    WorldFixture world;
    CY_REQUIRE(world.ok);

    const cy::ecs::Entity near_entity = world.spawn(Vec3{0.0F, 0.0F, -6.0F}, 11);
    const cy::ecs::Entity far_entity = world.spawn(Vec3{1.0F, 0.0F, -9.0F}, 12);
    const cy::ecs::Entity behind = world.spawn(Vec3{0.0F, 0.0F, 40.0F}, 13);
    CY_REQUIRE(near_entity.valid());
    CY_REQUIRE(far_entity.valid());
    CY_REQUIRE(behind.valid());
    CY_REQUIRE(world.commit(1));
    CY_REQUIRE_EQ(world.published().changed.size(), 3U);

    SceneIndex scene(allocator());
    SceneIndexReport applied;
    CY_REQUIRE(scene.apply(world.published(), 1.0F, applied).has_value());
    CY_CHECK_EQ(applied.inserted, 3U);
    CY_CHECK_EQ(scene.live(), 3U);

    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());

    FrameSinks sinks;
    sinks.surfaces = &surfaces_from_index;
    sinks.surfaces_user = &scene;

    const cy::render::LightDescription lights[] = {sun(1)};
    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(
        assembly.assemble(scene.index(), make_view({lights, 1}), sinks, graph, report).has_value());

    // The one behind the camera is culled; the two in front become draws.
    CY_CHECK_EQ(report.cull.tested, 3U);
    CY_CHECK_EQ(report.cull.rejected_by_frustum, 1U);
    CY_REQUIRE_EQ(report.draws, 2U);

    // AND THE DRAWS NAME THE MESHES THE NODES REFERENCED. Not "two draws happened" — the mesh
    // index in the sort key's own inputs is the handle the `MeshRenderer` carried, which is the
    // claim "the runtime draws what a node references" reduces to.
    // Resolved through the draw's STABLE ID and not through its slot: `DrawItem::instance_slot` is
    // the GPU scene's numbering, which a `SceneIndex` does not own and leaves at zero
    // (scene_index.h says so). The stable id is the one identity every stage of the chain carries
    // unchanged, which is the whole reason snapshot.h keys everything by it.
    bool saw_near = false;
    bool saw_far = false;
    for (const cy::render::DrawItem& item : assembly.layer(cy::render::SortLayer::Opaque)) {
        const SceneIndex::Surface surface = scene.surface_of(scene.slot_of(item.stable_id));
        saw_near = saw_near || surface.mesh.index() == 11U;
        saw_far = saw_far || surface.mesh.index() == 12U;
    }
    CY_CHECK(saw_near);
    CY_CHECK(saw_far);
    CY_CHECK_EQ(assembly.layer(cy::render::SortLayer::Opaque).size(), 2U);
    CY_CHECK_EQ(assembly.layer(cy::render::SortLayer::Transparent).size(), 0U);
}

CY_TEST_CASE("the assembled frame compiles and executes on a device") {
    // The null backend, so this runs on a machine with no GPU. `src/rendering/assembly/tests/
    // test_assembly_device.cpp` runs the same frame on Vulkan with validation on; this one is what
    // makes "the frame executes" a claim every developer's machine checks.
    (void)cy::rhi::null::register_null_backend();
    cy::rhi::DeviceDescription device_description;
    device_description.application_name = "cy_test_integration_render_assembly";
    cy::rhi::BackendSelection selection{};
    const auto device = cy::rhi::create_device(allocator(), "null", device_description, selection);
    CY_REQUIRE(device.has_value());

    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());
    CY_REQUIRE(assembly.attach_device(*device.value()).has_value());

    SpatialIndex index(allocator());
    for (u32 which = 0; which < 4; ++which) {
        SpatialEntry entry;
        entry.bounds = cy::Aabb::from_center_extents(Vec3{static_cast<f32>(which), 0.0F, -6.0F},
                                                     Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 200U + which;
        entry.radius = 0.9F;
        CY_REQUIRE(index.insert(entry).has_value());
    }
    const cy::render::LightDescription lights[] = {sun(1), point_light(Vec3{0.0F, 0.0F, -6.0F}, 2)};

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(
        assembly.assemble(index, make_view({lights, 2}), FrameSinks{}, graph, report).has_value());

    {
        // INSIDE A DEVICE FRAME. The executor acquires its command buffer from the device's current
        // frame, so the host's `begin_frame`/`end_frame` bracket is the caller's — see `execute()`.
        //
        // AND INSIDE A SCOPE THAT CLOSES BEFORE THE DEVICE DOES: `~GraphExecutor` releases the
        // views it realised, through the device. An executor outliving its device is a use after
        // free, and it is a crash in a destructor, which is the shape that reads as a test harness
        // bug rather than as the ordering mistake it is.
        CY_REQUIRE(device.value()->begin_frame().has_value());
        GraphExecutor executor(allocator(), *device.value());
        CY_REQUIRE(assembly.execute(executor, graph, report).has_value());
        CY_CHECK(device.value()->end_frame().has_value());
    }
    CY_CHECK(report.executed);
    CY_CHECK_GT(report.execution.passes_recorded, 0U);
    CY_CHECK_GT(report.execution.submits, 0U);
    // A frame that allocated nothing is a frame with no targets. The graph's own aliasing figure is
    // what says the transient set was real.
    CY_CHECK_GT(report.execution.transient_bytes, 0U);

    CY_REQUIRE(device.value()->wait_idle().has_value());
    cy::rhi::destroy_device(allocator(), device.value());
}

CY_TEST_CASE("a draw whose material is past the end of the table is counted, not hidden") {
    // The other side of the join, and the reason it is a counter rather than a refusal: a draw with
    // an out-of-range slot shades with slot zero on the device — the descriptor's length is what
    // decides — so it is a frame with one object wrongly shaded, not a frame that must not render.
    // A frame that never compared the two would report the same numbers either way, which is
    // exactly the state M7's gate found.
    FrameAssembly assembly(allocator());
    AssemblyDescription description = make_description();
    description.material_capacity = 4;
    CY_REQUIRE(assembly.initialize(description).has_value());

    SpatialIndex index(allocator());
    for (u32 which = 0; which < 6; ++which) {
        SpatialEntry entry;
        entry.bounds = cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, -5.0F - static_cast<f32>(which)}, Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 500U + which;
        entry.radius = 0.9F;
        CY_REQUIRE(index.insert(entry).has_value());
    }

    // The default surface query gives instance i material slot i, so the two beyond the table's
    // four are the two the count must find.
    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(assembly.assemble(index, make_view({}), FrameSinks{}, graph, report).has_value());
    CY_CHECK_EQ(report.draws, 6U);
    CY_CHECK_EQ(report.material_slots, 4U);
    CY_CHECK_EQ(report.draws_without_material, 2U);

    // And a table with slots allocated in it reports them, so `material_slots_live` is the render
    // server's number and not the description's.
    CY_REQUIRE(assembly.materials().allocate().has_value());
    CY_REQUIRE(assembly.materials().allocate().has_value());
    RenderGraph second(allocator());
    AssemblyReport again;
    CY_REQUIRE(assembly.assemble(index, make_view({}), FrameSinks{}, second, again).has_value());
    CY_CHECK_EQ(again.material_slots_live, 2U);
    // Two slots were written, so the frame has a material upload interval to transfer.
    CY_CHECK_GT(again.material_upload_size, 0U);
}

// ================================================================================================
// THE PICTURE'S OWN PROVENANCE. M11.c tasks 3.2 and 3.4.
// ================================================================================================
//
// The two cases below are what `m11c:frame-passes-through-post` runs, and they are written as
// COMPARISONS rather than as searches, because this project has shipped seven criteria that could
// not go red and the ones about a picture are the easiest of all to fake. Each assembles a frame,
// EXECUTES it, asks the frame for its own stage list, and then assembles the SAME frame with the
// thing under test removed and requires the two answers to differ.
//
// A note on what is deliberately NOT asserted: no image is compared. The M11.c design refuses a
// golden-image test of a tonemapped still — "a tolerance wide enough to survive a second GPU would
// not detect a changed picture either" — and what replaces it is exactly this: the provenance, the
// difference, and the caption checked against the frame.

namespace {

/// A frame assembled and executed through the null backend, so both cases below observe a frame
/// that RAN rather than one that was described.
struct ExecutedFrame {
    explicit ExecutedFrame(cy::Allocator& alloc) noexcept : assembly(alloc), graph(alloc) {}

    [[nodiscard]] bool run(const AssemblyDescription& description) noexcept {
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription device_description;
        device_description.application_name = "cy_test_integration_render_assembly";
        cy::rhi::BackendSelection selection{};
        auto opened = cy::rhi::create_device(allocator(), "null", device_description, selection);
        if (!opened) {
            return false;
        }
        device = opened.value();
        if (!assembly.initialize(description) || !assembly.attach_device(*device)) {
            return false;
        }
        SpatialIndex index(allocator());
        for (u32 which = 0; which < 3; ++which) {
            SpatialEntry entry;
            entry.bounds = cy::Aabb::from_center_extents(Vec3{static_cast<f32>(which), 0.0F, -6.0F},
                                                         Vec3{0.5F, 0.5F, 0.5F});
            entry.stable_id = 900U + which;
            entry.radius = 0.9F;
            if (!index.insert(entry)) {
                return false;
            }
        }
        const cy::render::LightDescription lights[] = {sun(1)};
        if (!assembly.assemble(index, make_view({lights, 1}), FrameSinks{}, graph, report)) {
            return false;
        }
        if (!device->begin_frame()) {
            return false;
        }
        {
            GraphExecutor executor(allocator(), *device);
            const bool executed = assembly.execute(executor, graph, report).has_value();
            const bool ended = device->end_frame().has_value();
            if (!executed || !ended) {
                return false;
            }
        }
        return device->wait_idle().has_value();
    }

    ~ExecutedFrame() {
        if (device != nullptr) {
            cy::rhi::destroy_device(allocator(), device);
        }
    }

    ExecutedFrame(const ExecutedFrame&) = delete;
    ExecutedFrame& operator=(const ExecutedFrame&) = delete;

    FrameAssembly assembly;
    RenderGraph graph;
    AssemblyReport report;
    cy::rhi::Device* device = nullptr;
};

/// The provenance a capture recipe fills in: pinned arbiter, a measured exposure, the medium
/// preset. Nothing here is the stage list.
[[nodiscard]] CaptureProvenance publication_provenance() noexcept {
    CaptureProvenance provenance;
    provenance.title = "integration.render_assembly";
    provenance.arbiter_pinned = true;
    provenance.purpose = CapturePurpose::Publication;
    provenance.ev100 = 12.5F;
    provenance.quality = post_quality_preset(QualityLevel::Medium);
    return provenance;
}

}  // namespace

CY_TEST_CASE("the frame passes through tone mapping, and says so itself") {
    AssemblyDescription description = make_description();
    description.post.bloom = true;
    description.post.colour_grading = true;

    ExecutedFrame frame(allocator());
    CY_REQUIRE(frame.run(description));
    CY_REQUIRE(frame.report.executed);

    // 1. THE FRAME EMITS THE LIST, and it is refused for a frame that did not run — which is what
    //    stops a stage list from being published for a picture that does not exist.
    AssemblyReport unexecuted;
    CY_CHECK_EQ(capture_refusal(unexecuted, publication_provenance()),
                CaptureRefusal::FrameNotExecuted);

    const auto manifest = capture_manifest(description, frame.report, publication_provenance());
    CY_REQUIRE(manifest.has_value());
    CY_CHECK(manifest->ran(PostStage::Tonemap));
    CY_CHECK(manifest->ran(PostStage::ExposureApply));
    CY_CHECK(manifest->ran(PostStage::OutputEncoding));

    // 2. IN THE SPECIFICATION'S ORDER AND NOT IN THE ONE A CAPTION WOULD GUESS. Exposure before
    //    tonemapping, grading after it, and the colour space flipping at exactly that boundary.
    CY_CHECK_LT(manifest->position_of(PostStage::ExposureApply),
                manifest->position_of(PostStage::Tonemap));
    CY_CHECK_LT(manifest->position_of(PostStage::Tonemap),
                manifest->position_of(PostStage::ColourGrading));
    CY_CHECK_EQ(manifest->stages[manifest->position_of(PostStage::Bloom)].space,
                ColourSpace::SceneReferred);
    CY_CHECK_EQ(manifest->stages[manifest->position_of(PostStage::ColourGrading)].space,
                ColourSpace::DisplayReferred);

    // 3. THE PICTURE COMES OUT OF THE CHAIN. `ForwardFrame` declares the Composite blit ONLY when
    //    the composite did not already write the output — "with post-processing on, tonemapping
    //    writes the swapchain image and this pass does not exist". So the presence of the post pass
    //    and the ABSENCE of the blit is the frame saying the published image passed through it.
    CY_CHECK_NE(frame.assembly.frame().pass_of(FramePassKind::PostProcess), kInvalidPass);
    CY_CHECK_EQ(frame.assembly.frame().pass_of(FramePassKind::Composite), kInvalidPass);

    // 4. AND THE COMPARISON, because a frame that always looks like this proves nothing. The same
    //    frame with post switched OFF routes its picture through the blit instead, and has no post
    //    pass at all. Two answers, and they differ.
    ForwardFrame bare(allocator());
    RenderGraph bare_graph(allocator());
    FrameDescription bare_description;
    bare_description.width = kWidth;
    bare_description.height = kHeight;
    bare_description.features.post_process = false;
    CY_REQUIRE(bare.build(bare_graph, bare_description).has_value());
    CY_CHECK_EQ(bare.pass_of(FramePassKind::PostProcess), kInvalidPass);
    CY_CHECK_NE(bare.pass_of(FramePassKind::Composite), kInvalidPass);

    // 5. THE CAPTION IS CHECKED AGAINST THE MANIFEST, IN BOTH DIRECTIONS. A caption claiming a
    //    stage the frame ran is confirmed; one claiming a stage it did not is refused NAMING the
    //    stage, rather than the difference resting on a reader's judgement.
    const CaptionCheck honest =
        check_caption(*manifest, "Tone mapping and colour grading, captured through the frame.");
    CY_CHECK(honest.ok());
    CY_CHECK_EQ(honest.claimed, 2U);
    CY_CHECK_EQ(honest.confirmed, 2U);

    const CaptionCheck overclaimed =
        check_caption(*manifest, "Tone mapping, colour grading and depth of field.");
    CY_CHECK_FALSE(overclaimed.ok());
    CY_CHECK_EQ(overclaimed.missing, PostStage::DepthOfField);

    // 6. AND THE TEXT PUBLISHED BESIDE THE PICTURE IS THE MANIFEST'S OWN.
    char text[2048] = {};
    const auto written = write_capture_manifest(*manifest, text, sizeof(text));
    CY_REQUIRE(written.has_value());
    CY_CHECK_GT(*written, 0U);
    CY_CHECK(std::strstr(text, "Tonemap") != nullptr);
    CY_CHECK(std::strstr(text, "arbiter pinned") != nullptr);

    // 7. A PUBLICATION CAPTURE IS REFUSED WHILE THE ARBITER IS FREE TO DEGRADE THE FRAME, which is
    //    the specification's own third scenario and the difference between an authored frame and
    //    one the arbiter chose not to render.
    CaptureProvenance unpinned = publication_provenance();
    unpinned.arbiter_pinned = false;
    CY_CHECK_EQ(capture_refusal(frame.report, unpinned), CaptureRefusal::ArbiterUnpinned);
    CY_CHECK_FALSE(capture_manifest(description, frame.report, unpinned).has_value());
    // The same frame is a legitimate DIAGNOSTIC capture. The refusal is about the claim, not the
    // numbers.
    unpinned.purpose = CapturePurpose::Diagnostic;
    CY_CHECK(capture_manifest(description, frame.report, unpinned).has_value());
}

CY_TEST_CASE("the frame passes through anti-aliasing, and the structure under it agrees") {
    // TEMPORAL ANTI-ALIASING IS THE ONE THAT CHANGES THE FRAME'S SHAPE, which is why it is the one
    // a manifest alone cannot certify: a temporal stage in a frame whose prepass produced no
    // velocity is a stage reading a target that was never allocated. So this case asserts the
    // stage AND the structure, and then asserts that removing the stage removes the structure.
    AssemblyDescription with = make_description();
    with.post.temporal_antialiasing = true;
    with.pin_jitter = true;
    with.pinned_jitter_index = 0;

    ExecutedFrame antialiased(allocator());
    CY_REQUIRE(antialiased.run(with));

    const auto manifest = capture_manifest(with, antialiased.report, publication_provenance());
    CY_REQUIRE(manifest.has_value());
    CY_CHECK(manifest->ran(PostStage::TemporalAntiAliasing));
    CY_CHECK_EQ(manifest->stages[manifest->position_of(PostStage::TemporalAntiAliasing)].step, 5U);
    CY_CHECK_EQ(manifest->prepass, PrepassMode::DepthNormalVelocity);
    CY_CHECK(manifest->velocity_written);
    CY_CHECK_NE(antialiased.assembly.resources().velocity, kInvalidResource);
    CY_CHECK_NE(antialiased.assembly.frame().pass_of(FramePassKind::Temporal), kInvalidPass);

    // DETERMINISM AND CAPTURE, which is the requirement a beauty shot is most tempted to skip: the
    // sequence is PINNED, so the same frame index gives the same sub-pixel offset on every run.
    CY_CHECK(manifest->jitter_pinned);

    // AND THE COMPARISON. The same description with the stage removed: no TAA in the list, a
    // depth-only prepass, and NO VELOCITY TARGET AT ALL — "their passes SHALL be absent from the
    // graph and their targets unallocated". If these two frames were the same, nothing in this
    // suite would be measuring anti-aliasing.
    AssemblyDescription without = with;
    without.post.temporal_antialiasing = false;

    ExecutedFrame aliased(allocator());
    CY_REQUIRE(aliased.run(without));
    const auto bare = capture_manifest(without, aliased.report, publication_provenance());
    CY_REQUIRE(bare.has_value());
    CY_CHECK_FALSE(bare->ran(PostStage::TemporalAntiAliasing));
    // NOT `DepthOnly`: `make_description()` leaves ambient occlusion on, which asks for normals.
    // The claim is the narrower and truer one — the prepass stopped producing VELOCITY, which is
    // what the temporal stage was the only consumer of.
    CY_CHECK_NE(bare->prepass, PrepassMode::DepthNormalVelocity);
    CY_CHECK_FALSE(bare->velocity_written);
    CY_CHECK_EQ(aliased.assembly.resources().velocity, kInvalidResource);
    CY_CHECK_EQ(aliased.assembly.frame().pass_of(FramePassKind::Temporal), kInvalidPass);
    CY_CHECK_LT(bare->stage_count, manifest->stage_count);

    // AND THE CAPTION IS REFUSED FOR THE FRAME THAT DID NOT ANTI-ALIAS. This is the failure the
    // whole mechanism exists for: the same sentence under two pictures, true of one and false of
    // the other, and the check tells them apart.
    CY_CHECK(check_caption(*manifest, "1080p, tone mapped, with anti-aliasing.").ok());
    const CaptionCheck wrong = check_caption(*bare, "1080p, tone mapped, with anti-aliasing.");
    CY_CHECK_FALSE(wrong.ok());
    CY_CHECK_EQ(wrong.missing, PostStage::TemporalAntiAliasing);
}
