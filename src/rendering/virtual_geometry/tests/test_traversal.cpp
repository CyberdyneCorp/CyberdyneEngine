// The reference traversal: detail follows distance, one mesh renders at several levels at once,
// instances are culled before clusters, and a missing page falls back rather than disappearing.
// M7 tasks 7.2 and 7.3.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/traversal.h>

#include "meshes.h"

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

vg::BuildOptions test_options() noexcept {
    vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.page_bytes = 2048;
    options.resident_budget_bytes = 2048;
    return options;
}

/// A view looking down −Z from `distance`, with a frustum wide enough that nothing at the origin is
/// clipped. The frustum planes are built by hand rather than from a projection matrix so the case
/// is about traversal rather than about the matrix conventions, which tests/render/ already owns.
vg::TraversalView view_at(f32 distance, f32 threshold) noexcept {
    vg::TraversalView view;
    view.projection.camera_position = Vec3{0.0F, 0.0F, distance};
    view.projection.viewport_height = 1080.0F;
    view.projection.fov_y_radians = 1.0471975512F;
    view.threshold_pixels = threshold;
    view.minimum_instance_pixels = 0.0F;
    view.cone_culling = false;
    // A frustum that contains everything: six planes whose half-spaces are the whole world.
    for (Plane& plane : view.frustum.planes) {
        plane = Plane{Vec3{0.0F, 0.0F, 1.0F}, 1.0e9F};
    }
    view.frustum.refresh_corner_masks();
    return view;
}

struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept : bytes(allocator), asset(allocator) {}

    Array<u8> bytes;
    vg::DecodedAsset asset;
};

}  // namespace

namespace {

/// Cook a sphere and decode it, so a case has an asset to traverse in three lines.
[[nodiscard]] Expected<vg::DecodedAsset, Error> sphere_asset(Allocator& allocator, Array<u8>& bytes,
                                                             u32 subdivisions = 3) noexcept {
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, subdivisions);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    if (!build) {
        return make_unexpected(build.error());
    }
    if (Status encoded = vg::encode_asset(*build, vg::VertexEncoding{}, bytes); !encoded) {
        return make_unexpected(encoded.error());
    }
    return vg::decode_asset(bytes.span(), allocator);
}

}  // namespace

CY_TEST_CASE("detail follows distance without a discrete switch") {
    // `virtual-geometry` — "Detail follows distance continuously": as the camera approaches,
    // progressively finer clusters are selected. Asserted as a MONOTONE relationship over a sweep
    // rather than at two distances, because a single pair passes on a hierarchy with one level.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());

    const vg::DecodedAsset* assets[1] = {&*asset};
    vg::GeometryInstance instance;
    const vg::GeometryInstance instances[1] = {instance};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);

    vg::TraversalResult result(allocator);
    u32 previous = 0;
    u32 increases = 0;
    for (u32 step = 0; step < 8; ++step) {
        const f32 distance = 64.0F / static_cast<f32>(1U << step);
        CY_REQUIRE(vg::traverse_reference(inputs, view_at(distance, 1.0F), result).has_value());
        CY_CHECK_GT(result.stats.visible_clusters, 0U);
        if (step > 0) {
            CY_CHECK_GE(result.stats.visible_triangles, previous);
            increases += result.stats.visible_triangles > previous ? 1U : 0U;
        }
        previous = result.stats.visible_triangles;
    }
    // The count must actually move over the sweep; a hierarchy that always returned the root would
    // satisfy the monotone check and nothing else.
    CY_CHECK_GT(increases, 2U);
}

CY_TEST_CASE("one mesh renders at several levels in one frame") {
    // `virtual-geometry` — "One mesh, several levels": "WHEN a large object spans a range of
    // distances THEN its near regions SHALL render at finer levels than its far regions, in the
    // same frame."
    //
    // The case SWEEPS the camera rather than picking one distance, and that is the finding rather
    // than caution. A level transition happens at one distance — the distance at which a level's
    // error projects to the threshold — and a sphere one unit across only straddles it while the
    // camera is within about a radius of it. A case that had picked a distance would have asserted
    // either "everything is at level 0" or "everything is at level 2", both of which pass a naive
    // check for "clusters were returned". Asserting that SOME frame in the sweep is mixed is the
    // claim the requirement actually makes.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());

    const vg::DecodedAsset* assets[1] = {&*asset};
    const vg::GeometryInstance instances[1] = {vg::GeometryInstance{}};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);

    vg::TraversalResult result(allocator);
    u32 mixed_frames = 0;
    u8 widest = 0;
    f32 widest_distance = 0.0F;
    for (u32 step = 0; step < 24; ++step) {
        const f32 distance = 1.05F + (static_cast<f32>(step) * 1.5F);
        CY_REQUIRE(vg::traverse_reference(inputs, view_at(distance, 1.0F), result).has_value());
        if (result.visible.empty()) {
            continue;
        }
        u8 lowest = 255;
        u8 highest = 0;
        for (const vg::VisibleCluster& visible : result.visible) {
            const u8 level = asset->clusters[visible.cluster].level;
            lowest = level < lowest ? level : lowest;
            highest = level > highest ? level : highest;
        }
        if (highest > lowest) {
            ++mixed_frames;
            if (highest - lowest > widest) {
                widest = static_cast<u8>(highest - lowest);
                widest_distance = distance;
            }
        }
    }
    CY_TEST_MESSAGE("frames with more than one level present: "
                    << mixed_frames << " of 24; widest spread " << static_cast<u32>(widest)
                    << " levels at distance " << widest_distance);
    CY_CHECK_GT(mixed_frames, 0U);
}

CY_TEST_CASE("a shadow view's threshold scaling selects coarser geometry") {
    // `virtual-geometry` — "Shadows use coarser geometry".
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};
    const vg::GeometryInstance instances[1] = {vg::GeometryInstance{}};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);

    vg::TraversalResult primary(allocator);
    vg::TraversalResult shadow(allocator);
    vg::TraversalView view = view_at(4.0F, 1.0F);
    CY_REQUIRE(vg::traverse_reference(inputs, view, primary).has_value());
    view.secondary_scale = 8.0F;
    CY_REQUIRE(vg::traverse_reference(inputs, view, shadow).has_value());

    CY_CHECK_LT(shadow.stats.visible_triangles, primary.stats.visible_triangles);
    CY_CHECK_GT(shadow.stats.visible_clusters, 0U);
}

CY_TEST_CASE("importance keeps detail where it is asked for") {
    // "WHEN the scene is overloaded THEN background geometry SHALL coarsen before
    // gameplay-critical geometry does."
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};

    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    vg::TraversalResult result(allocator);

    u32 triangles[4] = {};
    const vg::Importance order[4] = {vg::Importance::Critical, vg::Importance::Gameplay,
                                     vg::Importance::Normal, vg::Importance::Background};
    for (u32 index = 0; index < 4; ++index) {
        vg::GeometryInstance instance;
        instance.importance = order[index];
        const vg::GeometryInstance one[1] = {instance};
        inputs.instances = Span<const vg::GeometryInstance>(one, 1);
        CY_REQUIRE(vg::traverse_reference(inputs, view_at(4.0F, 2.0F), result).has_value());
        triangles[index] = result.stats.visible_triangles;
    }
    CY_CHECK_GE(triangles[0], triangles[1]);
    CY_CHECK_GE(triangles[1], triangles[2]);
    CY_CHECK_GE(triangles[2], triangles[3]);
    CY_CHECK_GT(triangles[0], triangles[3]);
}

CY_TEST_CASE("instances that cannot matter are removed before any cluster work") {
    // "WHEN millions of instances exist and a small fraction are visible THEN instance culling
    // SHALL reduce the set before any cluster work is performed."
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes, 2);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};

    Array<vg::GeometryInstance> instances(allocator);
    for (u32 index = 0; index < 64; ++index) {
        vg::GeometryInstance instance;
        // A line of instances marching away from the camera. Everything past the far plane below
        // is rejected without a cluster being touched.
        instance.translation = Vec3{0.0F, 0.0F, -static_cast<f32>(index) * 8.0F};
        (void)instances.push_back(instance);
    }
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = instances.span();

    vg::TraversalView view = view_at(8.0F, 1.0F);
    // A far plane at −100: everything beyond it is outside. Plane normals point INTO the frustum.
    view.frustum.planes[Frustum::Far] = Plane{Vec3{0.0F, 0.0F, 1.0F}, 100.0F};
    view.frustum.refresh_corner_masks();

    vg::TraversalResult result(allocator);
    CY_REQUIRE(vg::traverse_reference(inputs, view, result).has_value());
    CY_CHECK_EQ(result.stats.instances_tested, 64U);
    CY_CHECK_GT(result.stats.instances_rejected_by_frustum, 0U);
    CY_CHECK_LT(result.stats.instances_visible, 64U);
    CY_CHECK_EQ(result.stats.instances_visible + result.stats.instances_rejected_by_frustum +
                    result.stats.instances_rejected_by_size +
                    result.stats.instances_rejected_by_layer,
                64U);

    // A layer mask the view does not carry rejects before any geometry is looked at.
    for (vg::GeometryInstance& instance : instances) {
        instance.layer_mask = 0x2U;
    }
    view.layer_mask = 0x1U;
    CY_REQUIRE(vg::traverse_reference(inputs, view, result).has_value());
    CY_CHECK_EQ(result.stats.instances_rejected_by_layer, 64U);
    CY_CHECK_EQ(result.stats.visible_clusters, 0U);
}

namespace {

/// Only the pages a test names are resident. The residency answer the traversal asks for, made a
/// property of the case rather than of a cache.
struct ResidencyMask {
    Span<const u8> resident;
};

bool page_resident(u32 /*asset*/, u32 page, void* user) noexcept {
    const auto* mask = static_cast<const ResidencyMask*>(user);
    return page < mask->resident.size() && mask->resident[page] != 0U;
}

}  // namespace

CY_TEST_CASE("a missing page falls back to the nearest resident ancestor and asks for the page") {
    // `virtual-geometry` — "Streaming has not caught up": "WHEN the camera moves rapidly toward an
    // object whose fine pages are not resident THEN it SHALL render at the coarsest resident level
    // and refine as pages arrive, never disappearing."
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());
    CY_REQUIRE(asset->pages.size() > 2U);

    const vg::DecodedAsset* assets[1] = {&*asset};
    const vg::GeometryInstance instances[1] = {vg::GeometryInstance{}};

    Array<u8> resident(allocator);
    CY_REQUIRE(resident.resize(asset->pages.size()).has_value());
    for (u8& page : resident) {
        page = 0;
    }
    resident[0] = 1;  // the root only: the state a frame is in the instant an object streams in

    ResidencyMask mask{resident.span()};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);
    inputs.resident = &page_resident;
    inputs.resident_user = &mask;

    vg::TraversalResult starved(allocator);
    CY_REQUIRE(vg::traverse_reference(inputs, view_at(1.6F, 0.25F), starved).has_value());

    // THE OBJECT IS STILL THERE. That is the whole requirement.
    CY_CHECK_GT(starved.stats.visible_clusters, 0U);
    CY_CHECK_GT(starved.stats.fallback_clusters, 0U);
    CY_CHECK_GT(starved.stats.missing_pages, 0U);
    CY_CHECK_GT(starved.requests.size(), 0U);
    // Every cluster it drew came from a page that IS resident.
    for (const vg::VisibleCluster& visible : starved.visible) {
        CY_CHECK(resident[asset->clusters[visible.cluster].page] != 0U);
    }

    // Requests are deduplicated: one entry per page, however many clusters wanted it.
    for (usize a = 0; a < starved.requests.size(); ++a) {
        for (usize b = a + 1; b < starved.requests.size(); ++b) {
            CY_CHECK_NE(starved.requests[a].page, starved.requests[b].page);
        }
        CY_CHECK_GT(starved.requests[a].priority, 0.0F);
    }

    // And with everything resident the same view draws strictly more.
    vg::TraversalInputs full = inputs;
    full.resident = &vg::all_pages_resident;
    full.resident_user = nullptr;
    vg::TraversalResult complete(allocator);
    CY_REQUIRE(vg::traverse_reference(full, view_at(1.6F, 0.25F), complete).has_value());
    CY_CHECK_GT(complete.stats.visible_triangles, starved.stats.visible_triangles);
    CY_CHECK_EQ(complete.stats.missing_pages, 0U);
    CY_CHECK_EQ(complete.requests.size(), 0U);
    CY_TEST_MESSAGE("root only: " << starved.stats.visible_triangles << " triangles from "
                                  << starved.stats.visible_clusters << " clusters, "
                                  << starved.requests.size() << " pages requested; fully resident: "
                                  << complete.stats.visible_triangles << " triangles");
}

CY_TEST_CASE("traversal prunes rather than testing every cluster") {
    // "WHEN a hierarchy node is entirely outside the frustum or occluded THEN its subtree SHALL not
    // be traversed." Asserted by counting: a traversal that visited every cluster would report
    // `nodes_visited` equal to the cluster count.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};
    const vg::GeometryInstance instances[1] = {vg::GeometryInstance{}};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);

    vg::TraversalResult result(allocator);
    // Far away: the root is within threshold, so the descent stops at the top.
    CY_REQUIRE(vg::traverse_reference(inputs, view_at(200.0F, 4.0F), result).has_value());
    CY_CHECK_LT(result.stats.nodes_visited, asset->clusters.size());
    CY_TEST_MESSAGE("visited " << result.stats.nodes_visited << " of " << asset->clusters.size()
                               << " clusters at 200 units");

    // A frustum cutting the sphere in half prunes the subtrees behind the plane.
    vg::TraversalView half = view_at(4.0F, 0.5F);
    half.frustum.planes[Frustum::Left] = Plane{Vec3{1.0F, 0.0F, 0.0F}, 0.0F};
    half.frustum.refresh_corner_masks();
    vg::TraversalResult halved(allocator);
    CY_REQUIRE(vg::traverse_reference(inputs, half, halved).has_value());
    CY_CHECK_GT(halved.stats.nodes_pruned_by_frustum, 0U);

    vg::TraversalResult whole(allocator);
    CY_REQUIRE(vg::traverse_reference(inputs, view_at(4.0F, 0.5F), whole).has_value());
    CY_CHECK_LT(halved.stats.visible_clusters, whole.stats.visible_clusters);
}

CY_TEST_CASE("the normal cone rejects the far side of a closed object") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};
    const vg::GeometryInstance instances[1] = {vg::GeometryInstance{}};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(instances, 1);

    vg::TraversalResult without(allocator);
    vg::TraversalResult with(allocator);
    vg::TraversalView view = view_at(4.0F, 0.5F);
    CY_REQUIRE(vg::traverse_reference(inputs, view, without).has_value());
    view.cone_culling = true;
    CY_REQUIRE(vg::traverse_reference(inputs, view, with).has_value());

    CY_CHECK_GT(with.stats.rejected_by_cone, 0U);
    CY_CHECK_LT(with.stats.visible_clusters, without.stats.visible_clusters);
    CY_TEST_MESSAGE("cone culling rejected " << with.stats.rejected_by_cone << " of "
                                             << without.stats.visible_clusters << " clusters");
}

CY_TEST_CASE("instances of one asset share its hierarchy and differ only in transform") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes, 2);
    CY_REQUIRE(asset.has_value());
    const vg::DecodedAsset* assets[1] = {&*asset};

    vg::GeometryInstance near_instance;
    near_instance.translation = Vec3{-2.0F, 0.0F, 0.0F};
    vg::GeometryInstance far_instance;
    far_instance.translation = Vec3{2.0F, 0.0F, -200.0F};
    far_instance.material_offset = 7;
    const vg::GeometryInstance pair[2] = {near_instance, far_instance};

    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = Span<const vg::GeometryInstance>(pair, 2);
    vg::TraversalResult result(allocator);
    CY_REQUIRE(vg::traverse_reference(inputs, view_at(6.0F, 1.0F), result).has_value());

    u32 per_instance[2] = {};
    for (const vg::VisibleCluster& visible : result.visible) {
        // Bounded by an `if` rather than by an assertion: `CY_REQUIRE` is a macro the analyser
        // cannot see through, so a bare index after one reads as an out-of-bounds access.
        if (visible.instance >= 2U) {
            CY_CHECK(visible.instance < 2U);
            continue;
        }
        ++per_instance[visible.instance];
        if (visible.instance == 1) {
            CY_CHECK_GE(visible.material, 7U);
        }
    }
    CY_CHECK_GT(per_instance[0], 0U);
    CY_CHECK_GT(per_instance[1], 0U);
    // The near instance is drawn at finer detail than the far one, from the same pages.
    CY_CHECK_GT(per_instance[0], per_instance[1]);
}

CY_TEST_CASE("the packed GPU records carry what the shaders read") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    Array<u8> bytes(allocator);
    Expected<vg::DecodedAsset, Error> asset = sphere_asset(allocator, bytes, 2);
    CY_REQUIRE(asset.has_value());

    Array<vg::GpuCluster> clusters(allocator);
    CY_REQUIRE(vg::pack_clusters(*asset, clusters).has_value());
    CY_REQUIRE_EQ(clusters.size(), asset->clusters.size());
    for (usize index = 0; index < clusters.size(); ++index) {
        CY_CHECK_EQ(clusters[index].lod_error, asset->clusters[index].lod_error);
        CY_CHECK_EQ(clusters[index].parent_error, asset->clusters[index].parent_error);
        CY_CHECK_EQ(clusters[index].page, asset->clusters[index].page);
        CY_CHECK_EQ(clusters[index].child_count, asset->clusters[index].child_count);
    }

    vg::GeometryInstance instance;
    instance.importance = vg::Importance::Background;
    instance.quality_bias = 2.0F;
    const vg::GeometryInstance one[1] = {instance};
    Array<vg::GpuInstance> packed(allocator);
    CY_REQUIRE(vg::pack_instances(Span<const vg::GeometryInstance>(one, 1), packed).has_value());
    CY_REQUIRE_EQ(packed.size(), 1U);
    CY_CHECK_EQ(packed[0].threshold_scale, instance.threshold_scale());

    const vg::GpuView view = vg::pack_view(view_at(4.0F, 1.5F), 1, 32);
    CY_CHECK_EQ(view.instance_count, 1U);
    CY_CHECK_EQ(view.cluster_count, 32U);
    CY_CHECK_EQ(view.threshold, 1.5F);
    CY_CHECK_GT(view.pixels_scale, 0.0F);
}
