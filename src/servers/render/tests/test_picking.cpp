// Engine-side picking. Task 4.2, `editor-viewport-and-gizmos`.
//
// The requirement these cases exist to hold is one sentence — "Picking SHALL be engine-side, so
// that what is picked matches what is rendered" — and it is only testable because `pick_ray`
// resolves against the DRAW LIST rather than against the scene. So most of what is checked below is
// a negative: an instance the renderer did not draw is not a candidate, and no case here tells the
// picker *why* it was not drawn. That is the point. A picker that re-implemented visibility would
// pass a test written against one reason and fail against the next one somebody added.
//
// There is no world, no node, no device and no window in this file, for the same reason there is
// none in test_server.cpp: the render server is layer 2 and cannot reach one.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/picking.h>
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

/// A scene of unit cubes on the camera's axis, and the draw list a view produces for it.
///
/// Deliberately the same shape as test_server.cpp's fixture — one mesh, one material, instances
/// separated only by depth — so that a candidate list and a draw list can be compared entry for
/// entry without a difference in the fixture explaining a difference in the result.
struct Fixture {
    Fixture() noexcept : server(allocator()), draws(allocator()), candidates(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        RenderServerConfig config;
        config.debug_primitive_capacity = 8;
        config.debug_label_capacity = 4;
        if (!server.configure(config).has_value() || !server.initialize().has_value()) {
            return false;
        }

        SceneDescription scene_desc;
        scene_desc.name = cy::Name::intern("pick-scene");
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

        MaterialRecord opaque_desc;
        opaque_desc.name = cy::Name::intern("standard");
        opaque_desc.program = 7;
        auto made_opaque = server.create_material(opaque_desc);
        if (!made_opaque) {
            return false;
        }
        opaque = *made_opaque;

        MaterialRecord glass_desc;
        glass_desc.name = cy::Name::intern("glass");
        glass_desc.program = 9;
        glass_desc.blend = BlendMode::Translucent;
        auto made_glass = server.create_material(glass_desc);
        if (!made_glass) {
            return false;
        }
        glass = *made_glass;

        ViewDescription view_desc;
        view_desc.name = cy::Name::intern("viewport");
        view_desc.scene = scene;
        view_desc.purpose = ViewPurpose::EditorViewport;
        view_desc.viewport = ViewportRect{0, 0, 1920, 1080};
        view_desc.camera = cy::Transform::identity();
        auto made_view = server.create_view(view_desc);
        if (!made_view) {
            return false;
        }
        view = *made_view;
        return true;
    }

    /// A cube `distance` metres down the camera's forward axis, offset sideways by `x`.
    [[nodiscard]] cy::Expected<InstanceHandle, cy::Error> place(f32 x, f32 z, u64 stable_id,
                                                                MaterialHandle material) noexcept {
        InstanceDescription desc;
        desc.mesh = mesh;
        desc.material = material;
        desc.transform = cy::Transform::from_translation(cy::Vec3{x, 0.0F, z});
        desc.stable_id = stable_id;
        return server.create_instance(scene, desc);
    }

    [[nodiscard]] const View& current_view() noexcept { return *server.view(view); }

    /// Collect the frame. Every pick below runs against the list this produced.
    [[nodiscard]] bool render() noexcept {
        return server.collect_draws(scene, current_view(), draws, stats).has_value();
    }

    [[nodiscard]] cy::Span<const GpuInstance> records() noexcept {
        return server.scene(scene)->gpu.instances();
    }

    /// A ray through the centre of the viewport: what a click in the middle of the image aims.
    [[nodiscard]] cy::Ray centre_ray() noexcept {
        return ray_through_pixel(current_view(), 960.0F, 540.0F);
    }

    RenderServer server;
    SceneHandle scene;
    MeshHandle mesh;
    MaterialHandle opaque;
    MaterialHandle glass;
    ViewHandle view;
    cy::Array<DrawItem> draws;
    cy::Array<PickCandidate> candidates;
    ViewStatistics stats;
};

/// The identities a candidate list holds, in order.
[[nodiscard]] cy::Array<u64> identities(const cy::Array<PickCandidate>& candidates) noexcept {
    cy::Array<u64> out(allocator());
    for (const PickCandidate& candidate : candidates) {
        (void)out.push_back(candidate.stable_id);
    }
    return out;
}

}  // namespace

CY_TEST_CASE("a click down the view axis finds every cube on it, nearest first") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    // Published back to front, so an ordering that came from publication order would be visible.
    CY_REQUIRE(fixture.place(0.0F, -20.0F, 3, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -5.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -12.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    PickFilter filter;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                        fixture.candidates)
                   .has_value());

    const cy::Array<u64> found = identities(fixture.candidates);
    CY_REQUIRE_EQ(found.size(), 3U);
    CY_CHECK_EQ(found[0], 1ULL);
    CY_CHECK_EQ(found[1], 2ULL);
    CY_CHECK_EQ(found[2], 3ULL);
    // The nearest cube's centre is five metres away and its bounding sphere encloses a unit cube,
    // so the entry point is a little under five. A distance of zero would mean the ray started
    // inside, and a distance of five would mean the centre rather than the surface.
    CY_CHECK_LT(fixture.candidates[0].distance, 5.0F);
    CY_CHECK_GT(fixture.candidates[0].distance, 4.0F);
    // Dead centre of the sphere: the tie-break is at its minimum.
    CY_CHECK_NEAR(fixture.candidates[0].centrality, 0.0F, 1e-4F);
}

CY_TEST_CASE("what the renderer did not draw cannot be picked, whatever the reason") {
    // THE REQUIREMENT, and the reason `pick_ray` takes a draw list rather than a scene. Two
    // instances sit exactly on the ray. One is hidden and one is on a layer the view does not draw;
    // neither reaches the draw list, and this file contains no code that knows either rule.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto visible = fixture.place(0.0F, -5.0F, 1, fixture.opaque);
    auto hidden = fixture.place(0.0F, -6.0F, 2, fixture.opaque);
    CY_REQUIRE(visible.has_value());
    CY_REQUIRE(hidden.has_value());
    CY_REQUIRE(fixture.server.set_instance_visible(*hidden, false).has_value());

    InstanceDescription other_layer;
    other_layer.mesh = fixture.mesh;
    other_layer.material = fixture.opaque;
    other_layer.transform = cy::Transform::from_translation(cy::Vec3{0.0F, 0.0F, -7.0F});
    other_layer.layer_mask = 1U << 4U;
    other_layer.stable_id = 3;
    CY_REQUIRE(fixture.server.create_instance(fixture.scene, other_layer).has_value());

    View* view = fixture.server.view(fixture.view);
    view->desc.layer_mask = kDefaultLayer;
    view->refresh();
    CY_REQUIRE(fixture.render());
    CY_REQUIRE_EQ(fixture.draws.size(), 1U);

    PickFilter filter;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                        fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 1U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);
}

CY_TEST_CASE("a frustum-culled instance is not a candidate") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -5.0F, 1, fixture.opaque).has_value());
    // Behind the camera. The frustum rejects it, so it never reaches the draw list — and a picker
    // that walked the scene would have to remember to reject a negative ray parameter itself.
    CY_REQUIRE(fixture.place(0.0F, 5.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());
    CY_CHECK_EQ(fixture.draws.size(), 1U);

    // The ray reversed: straight at the cube behind the camera.
    const cy::Ray backwards{cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.0F, 0.0F, 1.0F}};
    PickFilter filter;
    CY_REQUIRE(
        pick_ray(fixture.records(), fixture.draws.span(), backwards, filter, fixture.candidates)
            .has_value());
    CY_CHECK(fixture.candidates.empty());
}

CY_TEST_CASE("cycling visits overlapping candidates in a stable order and wraps") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -5.0F, 10, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -9.0F, 20, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    PickFilter filter;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                        fixture.candidates)
                   .has_value());
    const cy::Span<const PickCandidate> found = fixture.candidates.span();
    CY_REQUIRE_EQ(found.size(), 2U);
    CY_CHECK_EQ(cycle_candidate(found, 0), 10ULL);
    CY_CHECK_EQ(cycle_candidate(found, 1), 20ULL);
    CY_CHECK_EQ(cycle_candidate(found, 2), 10ULL);
    CY_CHECK_EQ(cycle_candidate(found, 97), 20ULL);
    CY_CHECK_EQ(cycle_candidate(cy::Span<const PickCandidate>(), 0), 0ULL);
}

CY_TEST_CASE("the candidate order does not depend on publication order") {
    // The same property `sort.h` argues for the draw list, checked one layer up: two scenes with
    // the same contents created in opposite orders must produce the same candidate sequence,
    // because slot allocation is allocation history and nothing a user sees may depend on it.
    cy::Array<u64> first(allocator());
    cy::Array<u64> second(allocator());

    for (int pass = 0; pass < 2; ++pass) {
        Fixture fixture;
        CY_REQUIRE(fixture.start());
        if (pass == 0) {
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 100, fixture.opaque).has_value());
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 200, fixture.opaque).has_value());
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 300, fixture.opaque).has_value());
        } else {
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 300, fixture.opaque).has_value());
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 100, fixture.opaque).has_value());
            CY_REQUIRE(fixture.place(0.0F, -5.0F, 200, fixture.opaque).has_value());
        }
        CY_REQUIRE(fixture.render());
        PickFilter filter;
        CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                            fixture.candidates)
                       .has_value());
        cy::Array<u64>& into = (pass == 0) ? first : second;
        into = identities(fixture.candidates);
    }

    CY_REQUIRE_EQ(first.size(), 3U);
    CY_REQUIRE_EQ(second.size(), 3U);
    for (cy::usize index = 0; index < first.size(); ++index) {
        CY_CHECK_EQ(first[index], second[index]);
    }
}

CY_TEST_CASE("a locked object is excluded by identity, and nothing else is") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -5.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -9.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    const u64 locked[] = {1ULL};
    PickFilter filter;
    filter.excluded = cy::Span<const u64>(locked, 1);
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                        fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 1U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 2ULL);
}

CY_TEST_CASE("a click passes through glass unless the user says otherwise") {
    // "selecting through transparent surfaces by intent". The transparency of the candidate comes
    // from the sort layer the RENDERER put the draw in, so this cannot disagree with how the
    // surface was drawn.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -3.0F, 1, fixture.glass).has_value());
    CY_REQUIRE(fixture.place(0.0F, -8.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    PickFilter through;
    through.include_transparent = false;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), through,
                        fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 1U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 2ULL);

    PickFilter onto;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), onto,
                        fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 2U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);
    CY_CHECK(fixture.candidates[0].transparent);
    CY_CHECK_FALSE(fixture.candidates[1].transparent);
}

CY_TEST_CASE("the cap returns the nearest candidates rather than the first ones published") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -30.0F, 3, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -20.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(0.0F, -10.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    PickFilter filter;
    filter.max_candidates = 2;
    CY_REQUIRE(pick_ray(fixture.records(), fixture.draws.span(), fixture.centre_ray(), filter,
                        fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 2U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);
    CY_CHECK_EQ(fixture.candidates[1].stable_id, 2ULL);
}

CY_TEST_CASE("a ray through a pixel and the projection back to it are inverses") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    // A camera that is neither at the origin nor axis-aligned, so a transposed matrix or a missed
    // flip cannot pass by symmetry.
    View* view = fixture.server.view(fixture.view);
    view->desc.camera = cy::Transform{cy::Quat::from_axis_angle(cy::kAxisUp, 0.6F),
                                      cy::Vec3{12.0F, 3.0F, -4.0F}, cy::Vec3{1.0F, 1.0F, 1.0F}};
    view->refresh();

    const f32 pixels[][2] = {{960.0F, 540.0F}, {10.0F, 10.0F}, {1900.0F, 1000.0F}};
    for (const auto& pixel : pixels) {
        const cy::Ray ray = ray_through_pixel(*view, pixel[0], pixel[1]);
        const cy::Vec3 world = ray.at(25.0F);
        cy::Vec2 projected{0.0F, 0.0F};
        CY_REQUIRE(project_to_pixel(*view, world, projected));
        CY_CHECK_NEAR(projected.x, pixel[0], 0.05F);
        CY_CHECK_NEAR(projected.y, pixel[1], 0.05F);
    }

    // A point behind the camera has no pixel, and saying so is the difference between an overlay
    // drawn nowhere and one drawn at the wrong end of the viewport.
    cy::Vec2 unused{0.0F, 0.0F};
    const cy::Vec3 behind = view->desc.camera.translation - view->desc.camera.forward() * 5.0F;
    CY_CHECK_FALSE(project_to_pixel(*view, behind, unused));
}

CY_TEST_CASE("rectangle selection takes what projects inside it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -10.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(-4.0F, -10.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());
    CY_REQUIRE_EQ(fixture.draws.size(), 2U);

    // A rectangle around the middle of the image. The offset cube projects to the left of it.
    const PickRect middle{900.0F, 480.0F, 1020.0F, 600.0F};
    PickFilter filter;
    CY_REQUIRE(pick_rect(fixture.records(), fixture.current_view(), fixture.draws.span(), middle,
                         filter, fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 1U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);

    // The whole image takes both, in identity order — there is no depth to order an area pick by.
    const PickRect everything{0.0F, 0.0F, 1920.0F, 1080.0F};
    CY_REQUIRE(pick_rect(fixture.records(), fixture.current_view(), fixture.draws.span(),
                         everything, filter, fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 2U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);
    CY_CHECK_EQ(fixture.candidates[1].stable_id, 2ULL);
}

CY_TEST_CASE("a lasso encloses what a rectangle would, and two points are not a lasso") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -10.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.place(-4.0F, -10.0F, 2, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    const cy::Vec2 triangle[] = {{900.0F, 400.0F}, {1100.0F, 400.0F}, {1000.0F, 700.0F}};
    PickFilter filter;
    CY_REQUIRE(pick_polygon(fixture.records(), fixture.current_view(), fixture.draws.span(),
                            cy::Span<const cy::Vec2>(triangle, 3), filter, fixture.candidates)
                   .has_value());
    CY_REQUIRE_EQ(fixture.candidates.size(), 1U);
    CY_CHECK_EQ(fixture.candidates[0].stable_id, 1ULL);

    const cy::Vec2 line[] = {{0.0F, 0.0F}, {10.0F, 10.0F}};
    CY_CHECK_FALSE(pick_polygon(fixture.records(), fixture.current_view(), fixture.draws.span(),
                                cy::Span<const cy::Vec2>(line, 2), filter, fixture.candidates)
                       .has_value());
}

CY_TEST_CASE("a draw list from another scene is skipped rather than misread") {
    // Pairing a draw list with the wrong records is a programmer error, and the honest answer is no
    // candidate rather than a confident wrong one read out of a neighbouring slot.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.place(0.0F, -5.0F, 1, fixture.opaque).has_value());
    CY_REQUIRE(fixture.render());

    PickFilter filter;
    CY_REQUIRE(pick_ray(cy::Span<const GpuInstance>(), fixture.draws.span(), fixture.centre_ray(),
                        filter, fixture.candidates)
                   .has_value());
    CY_CHECK(fixture.candidates.empty());
}

CY_TEST_CASE("the editor's camera model and this one agree about where a pixel points") {
    // THE OTHER HALF OF A CROSS-LANGUAGE AGREEMENT. The editor does not resolve picks — it sends a
    // pixel and a frame identifier, and this file builds the ray — but it DOES build a ray for
    // gizmo manipulation, because turning a cursor's motion into a translation is editor-side
    // intent. If the two camera models disagree, a drag moves the object to somewhere the cursor is
    // not, worst at the edges of the image, and it gets blamed on the projection.
    //
    // `editor/crates/cy-editor-viewport/tests/rays_agree_with_the_engine.rs` asserts these same six
    // constants, computed once from the definition of a perspective frustum. Two independent
    // implementations checked against a third thing is the only arrangement that catches both
    // drifting the same way.
    //
    // The configuration is deliberately off-centre: a transposed matrix, a missed aspect ratio and
    // an unflipped Y all survive a centre-pixel test and none survives a corner.
    View view;
    view.desc.viewport = ViewportRect{0, 0, 1920, 1080};
    view.desc.camera = cy::Transform::identity();
    view.desc.projection.fov_y_radians = 1.0471975512F;  // sixty degrees, the engine's default
    view.refresh();

    constexpr f32 kTolerance = 1e-3F;

    const cy::Ray centre = ray_through_pixel(view, 960.0F, 540.0F);
    CY_CHECK_NEAR(centre.direction.x, 0.0F, kTolerance);
    CY_CHECK_NEAR(centre.direction.y, 0.0F, kTolerance);
    CY_CHECK_NEAR(centre.direction.z, -1.0F, kTolerance);

    // ndc (1, 0): tan(30°) times 16/9 sideways, one forward, normalised. A missed aspect ratio
    // gives 0.5 here rather than 0.716.
    const cy::Ray right = ray_through_pixel(view, 1920.0F, 540.0F);
    CY_CHECK_NEAR(right.direction.x, 0.716258F, kTolerance);
    CY_CHECK_NEAR(right.direction.y, 0.0F, kTolerance);
    CY_CHECK_NEAR(right.direction.z, -0.697835F, kTolerance);

    // ndc (−1, 1). The sign of the Y lane is the flip between a top-left pixel origin and a
    // bottom-left device origin.
    const cy::Ray corner = ray_through_pixel(view, 0.0F, 0.0F);
    CY_CHECK_NEAR(corner.direction.x, -0.664364F, kTolerance);
    CY_CHECK_NEAR(corner.direction.y, 0.373705F, kTolerance);
    CY_CHECK_NEAR(corner.direction.z, -0.647275F, kTolerance);

    // ndc (0, −1): thirty degrees below the axis, so exactly (0, −sin 30, −cos 30).
    const cy::Ray bottom = ray_through_pixel(view, 960.0F, 1080.0F);
    CY_CHECK_NEAR(bottom.direction.x, 0.0F, kTolerance);
    CY_CHECK_NEAR(bottom.direction.y, -0.5F, kTolerance);
    CY_CHECK_NEAR(bottom.direction.z, -0.866025F, kTolerance);
}
