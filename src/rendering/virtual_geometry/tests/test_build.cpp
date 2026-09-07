// The cluster hierarchy: that it is built, that it is crack-free, and that it is deterministic.
// M7 tasks 7.1 and 7.5.
//
// THE CENTRAL CASE IS "no cracks at a transition", and it is asserted the only way that sentence
// can be asserted: a cut of the DAG is taken at a sweep of camera distances and thresholds, and
// every edge of the resulting triangle set is counted. A closed source mesh must produce a closed
// cut. An edge used once IS the hole, so the check cannot pass on a build whose simplifier moved a
// locked vertex — which is what a check of the bookkeeping would do.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/build.h>

#include "meshes.h"

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

vg::BuildOptions small_policy() noexcept {
    vg::BuildOptions options;
    // Small clusters and small groups, so that a mesh a unit test can afford still produces several
    // levels and several groups. The defaults are tuned for a real asset and would give a
    // two-level hierarchy here, which would not exercise a transition at all.
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.policy.root_cluster_limit = 2;
    return options;
}

}  // namespace

CY_TEST_CASE("a closed mesh cooks into a hierarchy with several levels") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    CY_REQUIRE_EQ(mesh.indices.size() / 3, 1280U);

    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    CY_CHECK_EQ(build->source_triangles, 1280U);
    // 1280 triangles at 32 per cluster is 40 clusters at level 0, and the hierarchy above them.
    CY_CHECK_GT(build->clusters.size(), 40U);
    CY_CHECK_GT(build->levels, 2U);
    CY_CHECK_GT(build->groups.size(), 0U);
    CY_CHECK_FALSE(build->level_limit_reached);

    // A group with neighbours locked something. A group whose locked set were empty while other
    // clusters of its level existed outside it would be a group whose boundary the simplifier was
    // free to move, which is the defect the whole design exists to prevent — so a count of zero
    // there is a failure even though nothing else would notice.
    //
    // THE EXCEPTION IS REAL AND IS NOT A LOOSENING: a group that contains every remaining cluster
    // of a closed surface has no neighbour to crack against, and locking anything in it would only
    // stop the hierarchy from reaching a root. That is the top of the hierarchy, and it is why this
    // counts groups rather than asserting on all of them.
    u32 groups_with_neighbours = 0;
    u32 groups_that_locked = 0;
    for (const vg::Group& group : build->groups) {
        if (group.outside_clusters == 0) {
            continue;
        }
        ++groups_with_neighbours;
        groups_that_locked += group.locked_vertices > 0 ? 1U : 0U;
    }
    CY_CHECK(groups_with_neighbours > 0U);
    CY_CHECK_EQ(groups_that_locked, groups_with_neighbours);

    // No cluster exceeds the policy it was cooked under.
    for (const vg::Cluster& cluster : build->clusters) {
        CY_CHECK_LE(cluster.triangle_count(), 32U);
        CY_CHECK_LE(cluster.vertex_count, 64U);
        CY_CHECK_GT(cluster.index_count, 0U);
    }
}

CY_TEST_CASE("the cut is watertight at every threshold and every distance") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    Expected<vg::WatertightReport, Error> report = vg::check_watertight(*build, allocator);
    CY_REQUIRE(report.has_value());

    CY_CHECK(report->closed_source);
    CY_CHECK_EQ(report->monotonicity_violations, 0U);
    CY_CHECK_EQ(report->boundary_mismatches, 0U);
    CY_CHECK_GT(report->thresholds_tested, 100U);
    CY_CHECK_EQ(report->open_cuts, 0U);
    CY_CHECK(report->watertight());
}

CY_TEST_CASE("an open mesh reports not-applicable rather than a pass") {
    // A check that silently passed on an open mesh would pass on every mesh whose import went
    // wrong, which is why `closed_source` is a field and not an assumption.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::grid(allocator, 24);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    Expected<vg::WatertightReport, Error> report = vg::check_watertight(*build, allocator);
    CY_REQUIRE(report.has_value());
    CY_CHECK_FALSE(report->closed_source);
    CY_CHECK_EQ(report->thresholds_tested, 0U);
    // The two checks that ARE applicable to an open mesh still ran.
    CY_CHECK_EQ(report->monotonicity_violations, 0U);
    CY_CHECK_EQ(report->boundary_mismatches, 0U);
}

CY_TEST_CASE("a parent's error is strictly above its children's and its sphere contains theirs") {
    // The two invariants the selection test rests on, asserted directly as well as through the cut:
    // equal errors put a hole at exactly one threshold, which a sweep can step over.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    u32 checked = 0;
    for (const vg::Cluster& cluster : build->clusters) {
        if (cluster.parent_error >= vg::kRootError) {
            continue;
        }
        CY_CHECK_GT(cluster.parent_error, cluster.lod_error);
        CY_CHECK(cluster.parent_sphere.contains(cluster.lod_sphere));
        ++checked;
    }
    CY_CHECK_GT(checked, 0U);
}

CY_TEST_CASE("group membership is re-partitioned between levels") {
    // `virtual-geometry`: "Group membership SHALL be re-partitioned between levels rather than
    // nested rigidly, so that boundaries do not accumulate across the hierarchy." Asserted as the
    // property it is: the groups of level 1 are not a relabelling of the groups of level 0, so a
    // boundary locked at one level is interior at the next.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    u32 level_zero_groups = 0;
    u32 level_one_groups = 0;
    for (const vg::Group& group : build->groups) {
        level_zero_groups += group.level == 0 ? 1U : 0U;
        level_one_groups += group.level == 1 ? 1U : 0U;
    }
    CY_REQUIRE(level_zero_groups > 1U);
    CY_REQUIRE(level_one_groups > 0U);
    // A level-1 group's members are level-1 clusters, which were PRODUCED by level-0 groups. If
    // grouping were nested rigidly, every level-1 group would draw its members from exactly one
    // level-0 group. At least one must not.
    bool mixes = false;
    for (const vg::Group& group : build->groups) {
        if (group.level != 1) {
            continue;
        }
        u32 first_source = vg::kInvalidGroup;
        for (u32 slot = 0; slot < group.member_count; ++slot) {
            const vg::Cluster& member =
                build->clusters[build->group_members[group.first_member + slot]];
            // The group that produced this member is the one whose children include it. Its own
            // `group` field names the group it was simplified INTO, which is this one, so the
            // producing group is found through the cluster's child range instead.
            const u32 producing =
                member.child_count > 0
                    ? build->clusters[build->cluster_children[member.first_child]].group
                    : vg::kInvalidGroup;
            if (first_source == vg::kInvalidGroup) {
                first_source = producing;
            } else if (producing != first_source) {
                mixes = true;
            }
        }
    }
    CY_CHECK(mixes);
}

CY_TEST_CASE("two builds of one mesh are byte-identical") {
    // Task 7.5. The claim is about the cook, so it is asserted on the ENCODED asset rather than on
    // the in-memory build: a build that agreed field by field and encoded differently would still
    // miss its cache on every run.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);

    Array<u8> first(allocator);
    Array<u8> second(allocator);
    for (u32 run = 0; run < 2; ++run) {
        Expected<vg::GeometryBuild, Error> build =
            vg::build_geometry(mesh.source(), small_policy(), allocator);
        CY_REQUIRE(build.has_value());
        vg::VertexEncoding encoding;
        CY_REQUIRE(vg::encode_asset(*build, encoding, run == 0 ? first : second).has_value());
    }
    CY_REQUIRE_EQ(first.size(), second.size());
    usize differing = 0;
    for (usize index = 0; index < first.size(); ++index) {
        differing += first[index] != second[index] ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_GT(first.size(), 1024U);
}

CY_TEST_CASE("clustering never mixes materials") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::two_material_sphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), small_policy(), allocator);
    CY_REQUIRE(build.has_value());

    bool saw_zero = false;
    bool saw_one = false;
    for (const vg::Cluster& cluster : build->clusters) {
        saw_zero = saw_zero || cluster.material == 0U;
        saw_one = saw_one || cluster.material == 1U;
        CY_CHECK_LE(cluster.material, 1U);
    }
    CY_CHECK(saw_zero);
    CY_CHECK(saw_one);
}

CY_TEST_CASE("a policy that cannot be honoured is refused rather than clamped") {
    vg::ClusterPolicy policy;
    policy.max_vertices = 512;  // above the byte index the page format uses
    CY_CHECK_FALSE(policy.validate().has_value());

    policy = vg::ClusterPolicy{};
    policy.group_size = 1;  // a group of one cannot be simplified below itself
    CY_CHECK_FALSE(policy.validate().has_value());

    policy = vg::ClusterPolicy{};
    policy.min_triangles = 200;  // above target
    CY_CHECK_FALSE(policy.validate().has_value());

    CY_CHECK(vg::ClusterPolicy{}.validate().has_value());
}

CY_TEST_CASE("an unsuitable surface class is reported rather than cooked silently") {
    // `virtual-geometry`: "Where a class is not well served by virtual geometry, the cooker SHALL
    // say so at import rather than producing a poor result silently."
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    vg::BuildOptions options = small_policy();
    options.surface = vg::SurfaceClass::Foliage;
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options, allocator);
    CY_REQUIRE(build.has_value());
    CY_CHECK(build->suitability_warning);
    CY_CHECK(build->suitability_reason[0] != '\0');

    options.surface = vg::SurfaceClass::Solid;
    Expected<vg::GeometryBuild, Error> solid =
        vg::build_geometry(mesh.source(), options, allocator);
    CY_REQUIRE(solid.has_value());
    CY_CHECK_FALSE(solid->suitability_warning);
}
