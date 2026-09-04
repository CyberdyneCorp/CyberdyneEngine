// The cluster grid and its assignment. Task 4.3.1.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/servers/render/types.h>

#include <cmath>

namespace {

using cy::rendering::ClusterAssignment;
using cy::rendering::ClusterElement;
using cy::rendering::ClusterElementType;
using cy::rendering::ClusterGrid;
using cy::rendering::ClusterGridConfig;
using cy::rendering::kClusterElementTypeCount;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A small grid, so a case can reason about every cluster. The real one is 60x34x32.
ClusterGrid small_grid(cy::u32 max_per_cluster = 4) noexcept {
    ClusterGridConfig config;
    config.tile_size = 32;
    config.slice_count = 4;
    config.max_elements_per_cluster = max_per_cluster;
    const cy::Expected<ClusterGrid, cy::Error> grid =
        cy::rendering::make_cluster_grid(config, 64, 64, 1.0F, 100.0F);
    return grid.has_value() ? *grid : ClusterGrid{};
}

/// A cluster's `(offset, count)` header for one type.
cy::rendering::ClusterHeader header_of(const ClusterAssignment& assignment, cy::u32 cluster,
                                       ClusterElementType type) noexcept {
    return assignment.headers[(static_cast<cy::usize>(cluster) * kClusterElementTypeCount) +
                              static_cast<cy::u32>(type)];
}

}  // namespace

CY_TEST_CASE("the grid keeps its tile size constant when the resolution changes") {
    // "WHEN the view resolution changes THEN the cluster grid SHALL be resized so tile size in
    // pixels stays constant, and buffers SHALL be reallocated only when the count changes."
    ClusterGridConfig config;
    config.tile_size = 32;
    config.slice_count = 32;

    const cy::Expected<ClusterGrid, cy::Error> small =
        cy::rendering::make_cluster_grid(config, 640, 360, 0.1F, 1000.0F);
    const cy::Expected<ClusterGrid, cy::Error> large =
        cy::rendering::make_cluster_grid(config, 1920, 1080, 0.1F, 1000.0F);
    CY_REQUIRE(small.has_value());
    CY_REQUIRE(large.has_value());

    CY_CHECK_EQ(small->dimensions[0], 20U);  // 640 / 32
    CY_CHECK_EQ(large->dimensions[0], 60U);  // 1920 / 32
    CY_CHECK_EQ(large->dimensions[2], 32U);
    // A viewport that is not a multiple of the tile is covered rather than clipped.
    const cy::Expected<ClusterGrid, cy::Error> odd =
        cy::rendering::make_cluster_grid(config, 641, 361, 0.1F, 1000.0F);
    CY_REQUIRE(odd.has_value());
    CY_CHECK_EQ(odd->dimensions[0], 21U);
}

CY_TEST_CASE("an infinite far plane is refused rather than producing a grid with no last slice") {
    ClusterGridConfig config;
    CY_CHECK_FALSE(
        cy::rendering::make_cluster_grid(config, 640, 360, 0.1F, cy::math::kInfinity).has_value());
    CY_CHECK_FALSE(cy::rendering::make_cluster_grid(config, 640, 360, 0.0F, 100.0F).has_value());
    CY_CHECK_FALSE(cy::rendering::make_cluster_grid(config, 0, 360, 0.1F, 100.0F).has_value());
}

CY_TEST_CASE("depth slices are exponential: thin near the camera, thick far away") {
    // The specification's own mapping, and the property that makes it worth having:
    // `slice = floor(log(z / near) · count / log(far / near))`.
    const ClusterGrid grid = small_grid();
    CY_REQUIRE_NE(grid.cluster_count(), 0U);

    CY_CHECK_EQ(cy::rendering::cluster_slice_of(grid, 1.0F), 0U);
    CY_CHECK_EQ(cy::rendering::cluster_slice_of(grid, 99.0F), 3U);
    // Anything at or in front of the near plane lands in the nearest slice rather than
    // underflowing.
    CY_CHECK_EQ(cy::rendering::cluster_slice_of(grid, 0.001F), 0U);
    CY_CHECK_EQ(cy::rendering::cluster_slice_of(grid, 1e9F), 3U);

    // The thickness test: the depth range of slice 0 is far smaller than that of the last slice.
    cy::f32 boundaries[5] = {};
    for (cy::u32 slice = 0; slice < 5; ++slice) {
        boundaries[slice] =
            grid.near_plane * std::exp2(static_cast<cy::f32>(slice) / grid.slice_scale);
    }
    CY_CHECK_LT(boundaries[1] - boundaries[0], boundaries[4] - boundaries[3]);
}

CY_TEST_CASE("a point light is assigned to the clusters it reaches and to no others") {
    const ClusterGrid grid = small_grid();
    ClusterAssignment assignment(allocator());

    // A small light just in front of the camera, on the axis.
    ClusterElement light;
    light.view_position = cy::Vec3{0.0F, 0.0F, -2.0F};
    light.radius = 0.5F;
    light.payload_index = 11;
    const cy::Span<const ClusterElement> elements(&light, 1);

    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);
    CY_REQUIRE(cy::rendering::assign_clusters(grid, elements, cy::render::kAllLayers, tan_half,
                                              1.0F, assignment)
                   .has_value());

    CY_CHECK_GT(assignment.indices.size(), 0U);
    CY_CHECK_LT(assignment.stats.non_empty_clusters, grid.cluster_count());
    CY_CHECK_EQ(assignment.stats.overflow, 0U);
    // Every index written is the payload index the caller supplied, never the element's position in
    // the input.
    for (const cy::u32 written : assignment.indices.span()) {
        CY_CHECK_EQ(written, 11U);
    }
}

CY_TEST_CASE("the camera inside a light still gets the light, from the nearest slice") {
    // "WHEN the camera is inside a point light's radius THEN the light SHALL be assigned from the
    // nearest depth slice, since its bounding volume would otherwise be clipped by the near plane."
    const ClusterGrid grid = small_grid();
    ClusterAssignment assignment(allocator());

    ClusterElement light;
    light.view_position = cy::Vec3{0.0F, 0.0F, 0.0F};  // the camera's own position
    light.radius = 5.0F;
    light.payload_index = 3;
    const cy::Span<const ClusterElement> elements(&light, 1);
    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);
    CY_REQUIRE(cy::rendering::assign_clusters(grid, elements, cy::render::kAllLayers, tan_half,
                                              1.0F, assignment)
                   .has_value());

    // Every cluster in slice 0 sees it, which is what "assigned from the nearest depth slice" means
    // when the test is against view-space bounds rather than against a clipped projection.
    for (cy::u32 y = 0; y < grid.dimensions[1]; ++y) {
        for (cy::u32 x = 0; x < grid.dimensions[0]; ++x) {
            const cy::u32 cluster = cy::rendering::cluster_index_of(grid, x, y, 0);
            CY_CHECK_EQ(header_of(assignment, cluster, ClusterElementType::Light).count, 1U);
        }
    }
}

CY_TEST_CASE("a wide spot is bounded as a sphere, past the specification's 60 degrees") {
    CY_CHECK(cy::rendering::element_is_cone(0.5F));
    CY_CHECK(cy::rendering::element_is_cone(cy::rendering::kSpotConeThresholdRadians));
    // "Spot lights whose half-angle exceeds a threshold (60°) SHALL be treated as spheres, because
    // a cone bound becomes both loose and, past 90°, incorrect."
    CY_CHECK_FALSE(cy::rendering::element_is_cone(1.2F));
    CY_CHECK_FALSE(cy::rendering::element_is_cone(2.0F));
}

CY_TEST_CASE("a narrow cone reaches fewer clusters than the sphere that contains it") {
    const ClusterGrid grid = small_grid(16);
    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);

    ClusterElement sphere;
    sphere.view_position = cy::Vec3{0.0F, 0.0F, -10.0F};
    sphere.radius = 20.0F;
    sphere.payload_index = 1;

    ClusterElement cone = sphere;
    cone.cone_cos = std::cos(0.2F);
    cone.view_direction = cy::Vec3{0.0F, 1.0F, 0.0F};  // pointing away from the view

    ClusterAssignment as_sphere(allocator());
    ClusterAssignment as_cone(allocator());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(&sphere, 1),
                                              cy::render::kAllLayers, tan_half, 1.0F, as_sphere)
                   .has_value());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(&cone, 1),
                                              cy::render::kAllLayers, tan_half, 1.0F, as_cone)
                   .has_value());
    CY_CHECK_LT(as_cone.indices.size(), as_sphere.indices.size());
}

CY_TEST_CASE("light channels are one bitfield test during assignment") {
    // "Channel resolution SHALL be a compact bitfield test during light assignment, not a
    // per-object light loop, so channels cost nothing at shading time."
    const ClusterGrid grid = small_grid();
    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);

    ClusterElement light;
    light.view_position = cy::Vec3{0.0F, 0.0F, -2.0F};
    light.radius = 5.0F;
    light.layer_mask = 1U << 2U;

    ClusterAssignment matched(allocator());
    ClusterAssignment excluded(allocator());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(&light, 1),
                                              1U << 2U, tan_half, 1.0F, matched)
                   .has_value());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(&light, 1),
                                              1U << 5U, tan_half, 1.0F, excluded)
                   .has_value());
    CY_CHECK_GT(matched.indices.size(), 0U);
    CY_CHECK_EQ(excluded.indices.size(), 0U);
}

CY_TEST_CASE("each element type gets its own header, so a fragment does not walk the others") {
    const ClusterGrid grid = small_grid();
    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);

    ClusterElement elements[2];
    elements[0].view_position = cy::Vec3{0.0F, 0.0F, -2.0F};
    elements[0].radius = 5.0F;
    elements[0].payload_index = 7;
    elements[0].type = ClusterElementType::Light;
    elements[1] = elements[0];
    elements[1].payload_index = 9;
    elements[1].type = ClusterElementType::Decal;

    ClusterAssignment assignment(allocator());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(elements, 2),
                                              cy::render::kAllLayers, tan_half, 1.0F, assignment)
                   .has_value());

    const cy::u32 cluster = cy::rendering::cluster_index_of(grid, 0, 0, 0);
    const cy::rendering::ClusterHeader lights =
        header_of(assignment, cluster, ClusterElementType::Light);
    const cy::rendering::ClusterHeader decals =
        header_of(assignment, cluster, ClusterElementType::Decal);
    CY_CHECK_EQ(lights.count, 1U);
    CY_CHECK_EQ(decals.count, 1U);
    CY_CHECK_EQ(assignment.indices[lights.offset], 7U);
    CY_CHECK_EQ(assignment.indices[decals.offset], 9U);
}

CY_TEST_CASE("overflow keeps the nearest, counts the drops, and is identical between two runs") {
    // "WHEN more elements affect a cluster than the per-cluster limit THEN the excess SHALL be
    // dropped deterministically (nearest kept) and an overflow counter SHALL be reported."
    const ClusterGrid grid = small_grid(2);
    const cy::f32 tan_half = std::tan(1.0471975512F * 0.5F);

    // Four lights covering the whole grid, at increasing distances. Only two fit per cluster.
    ClusterElement elements[4];
    for (cy::u32 index = 0; index < 4; ++index) {
        elements[index].view_position =
            cy::Vec3{0.0F, 0.0F, -2.0F - (static_cast<cy::f32>(index) * 20.0F)};
        elements[index].radius = 500.0F;
        elements[index].payload_index = index;
    }

    ClusterAssignment first(allocator());
    ClusterAssignment second(allocator());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(elements, 4),
                                              cy::render::kAllLayers, tan_half, 1.0F, first)
                   .has_value());
    CY_REQUIRE(cy::rendering::assign_clusters(grid, cy::Span<const ClusterElement>(elements, 4),
                                              cy::render::kAllLayers, tan_half, 1.0F, second)
                   .has_value());

    CY_CHECK_GT(first.stats.overflow, 0U);
    CY_CHECK_LE(first.stats.max_elements, 2U * kClusterElementTypeCount);
    // Deterministic: the same scene assigns to the same buffers, which is what design.md §6 asks of
    // everything in the frame and what a golden image depends on.
    CY_REQUIRE_EQ(first.indices.size(), second.indices.size());
    for (cy::usize index = 0; index < first.indices.size(); ++index) {
        CY_CHECK_EQ(first.indices[index], second.indices[index]);
    }
    CY_CHECK_EQ(first.stats.overflow, second.stats.overflow);

    // The survivors of the nearest cluster are the two nearest lights, and they are written in
    // ascending payload order.
    const cy::u32 cluster = cy::rendering::cluster_index_of(grid, 0, 0, 0);
    const cy::rendering::ClusterHeader lights =
        header_of(first, cluster, ClusterElementType::Light);
    CY_REQUIRE_EQ(lights.count, 2U);
    CY_CHECK_LT(first.indices[lights.offset], first.indices[lights.offset + 1]);
    CY_CHECK_EQ(first.indices[lights.offset], 0U);
}

CY_TEST_CASE("the grid layout is the shader's, field for field") {
    // The static assertions in cluster.h are the contract; this is the part a reader can check
    // against cy/cluster.slang by eye.
    const ClusterGrid grid = small_grid();
    CY_CHECK_EQ(sizeof(ClusterGrid), 32U);
    CY_CHECK_NEAR(grid.slice_scale, 4.0F / std::log2(grid.far_plane / grid.near_plane), 1e-4F);
    CY_CHECK_NEAR(
        grid.slice_bias,
        -(4.0F * std::log2(grid.near_plane)) / std::log2(grid.far_plane / grid.near_plane), 1e-4F);
}
