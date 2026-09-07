// The three mesh-processing steps M6 added: the lightmap unwrap over xatlas, the overdraw reorder,
// and convex decomposition. Task 8.2.
//
// `asset-import-pipeline` — "Mesh processing" lists six steps and M5 delivered four of them. What
// is asserted here is what the specification says each of the missing three must do, plus the
// property that governs all of them and is the one a cook cache cannot survive losing: running the
// step twice on one mesh produces one answer.

#include <cy/core/math/scalar.h>
#include <cy/import/mesh.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec2;
using cy::Vec3;

namespace {

/// A grid of `cells` by `cells` quads in the XZ plane, with normals, texture coordinates and one
/// section. Big enough that an unwrap has something to segment and a reorder something to reorder.
MeshData grid(u32 cells, f32 size) {
    MeshData mesh;
    const u32 stride = cells + 1;
    for (u32 row = 0; row <= cells; ++row) {
        for (u32 column = 0; column <= cells; ++column) {
            const f32 u = static_cast<f32>(column) / static_cast<f32>(cells);
            const f32 v = static_cast<f32>(row) / static_cast<f32>(cells);
            CY_REQUIRE(mesh.positions.push_back(Vec3{u * size, 0.0f, v * size}).has_value());
            CY_REQUIRE(mesh.normals.push_back(Vec3{0.0f, 1.0f, 0.0f}).has_value());
            CY_REQUIRE(mesh.uvs.push_back(Vec2{u, v}).has_value());
        }
    }
    for (u32 row = 0; row < cells; ++row) {
        for (u32 column = 0; column < cells; ++column) {
            const u32 base = (row * stride) + column;
            for (const u32 index :
                 {base, base + 1, base + stride, base + 1, base + stride + 1, base + stride}) {
                CY_REQUIRE(mesh.indices.push_back(index).has_value());
            }
        }
    }
    MeshSection section;
    section.first_index = 0;
    section.index_count = static_cast<u32>(mesh.indices.size());
    CY_REQUIRE(mesh.sections.push_back(section).has_value());
    return mesh;
}

/// A box, as eight corners and twelve triangles. Convex, so a decomposition must not split it.
MeshData box(Vec3 lo, Vec3 hi) {
    MeshData mesh;
    for (u32 corner = 0; corner < 8; ++corner) {
        CY_REQUIRE(mesh.positions
                       .push_back(Vec3{(corner & 1U) != 0U ? hi.x : lo.x,
                                       (corner & 2U) != 0U ? hi.y : lo.y,
                                       (corner & 4U) != 0U ? hi.z : lo.z})
                       .has_value());
    }
    static constexpr u32 kFaces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6},
                                          {0, 1, 4}, {1, 5, 4}, {2, 6, 3}, {3, 6, 7},
                                          {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& face : kFaces) {
        for (const u32 index : face) {
            CY_REQUIRE(mesh.indices.push_back(index).has_value());
        }
    }
    MeshSection section;
    section.first_index = 0;
    section.index_count = static_cast<u32>(mesh.indices.size());
    CY_REQUIRE(mesh.sections.push_back(section).has_value());
    return mesh;
}

/// Two boxes with a gap between them, as one mesh. Concave by construction: one hull would swallow
/// the gap entirely.
MeshData dumbbell() {
    MeshData mesh = box(Vec3{-3.0f, -1.0f, -1.0f}, Vec3{-1.0f, 1.0f, 1.0f});
    const MeshData other = box(Vec3{1.0f, -1.0f, -1.0f}, Vec3{3.0f, 1.0f, 1.0f});
    const auto base = static_cast<u32>(mesh.positions.size());
    for (const Vec3& position : other.positions) {
        CY_REQUIRE(mesh.positions.push_back(position).has_value());
    }
    for (const u32 index : other.indices) {
        CY_REQUIRE(mesh.indices.push_back(base + index).has_value());
    }
    mesh.sections[0].index_count = static_cast<u32>(mesh.indices.size());
    return mesh;
}

f32 mean_view_depth_of_first_half(const MeshData& mesh, Vec3 centre) {
    const usize half = (mesh.triangle_count() / 2) * 3;
    f32 total = 0.0f;
    for (usize index = 0; index < half; ++index) {
        total += length(mesh.positions[mesh.indices[index]] - centre);
    }
    return half == 0 ? 0.0f : total / static_cast<f32>(half);
}

}  // namespace

// --- The lightmap unwrap ------------------------------------------------------------------------

CY_TEST_CASE("unwrap: a mesh gains a lightmap coordinate set inside the unit square") {
    MeshData mesh = grid(4, 2.0f);
    Uv2Options options;
    options.texel_density = 16.0f;
    options.padding = 2;
    const auto report = generate_uv2(mesh, options);
    CY_REQUIRE(report.has_value());

    CY_CHECK(report.value().charts >= 1);
    CY_CHECK(report.value().width > 0);
    CY_CHECK(report.value().height > 0);
    CY_REQUIRE(mesh.uv2.size() == mesh.vertex_count());
    for (const Vec2& uv : mesh.uv2) {
        CY_CHECK(uv.x >= 0.0f);
        CY_CHECK(uv.x <= 1.0f);
        CY_CHECK(uv.y >= 0.0f);
        CY_CHECK(uv.y <= 1.0f);
    }
    // The first coordinate set is untouched: UV0 is the artist's and UV2 is the baker's.
    CY_CHECK(mesh.uvs.size() == mesh.vertex_count());
    CY_CHECK(mesh.normals.size() == mesh.vertex_count());
    CY_CHECK(mesh.validate().has_value());
}

CY_TEST_CASE("unwrap: unwrapping twice produces the same atlas, byte for byte") {
    // The determinism requirement, on the step whose implementation is third-party. A packer that
    // seeded itself from the clock or from an address would make every cook of every lightmapped
    // mesh a cache miss, and would do it silently.
    MeshData first = grid(5, 3.0f);
    MeshData second = grid(5, 3.0f);
    const Uv2Options options;
    const auto left = generate_uv2(first, options);
    const auto right = generate_uv2(second, options);
    CY_REQUIRE(left.has_value());
    CY_REQUIRE(right.has_value());

    CY_CHECK(left.value().charts == right.value().charts);
    CY_CHECK(left.value().width == right.value().width);
    CY_REQUIRE(first.vertex_count() == second.vertex_count());
    for (usize index = 0; index < first.vertex_count(); ++index) {
        CY_REQUIRE(first.uv2[index].x == second.uv2[index].x);
        CY_REQUIRE(first.uv2[index].y == second.uv2[index].y);
        CY_REQUIRE(first.positions[index].x == second.positions[index].x);
    }
    CY_REQUIRE(first.indices.size() == second.indices.size());
    for (usize index = 0; index < first.indices.size(); ++index) {
        CY_REQUIRE(first.indices[index] == second.indices[index]);
    }
}

CY_TEST_CASE("unwrap: a higher texel density asks for a larger atlas") {
    MeshData sparse = grid(4, 4.0f);
    MeshData dense = grid(4, 4.0f);
    Uv2Options low;
    low.texel_density = 4.0f;
    Uv2Options high;
    high.texel_density = 64.0f;
    const auto small = generate_uv2(sparse, low);
    const auto large = generate_uv2(dense, high);
    CY_REQUIRE(small.has_value());
    CY_REQUIRE(large.has_value());
    CY_CHECK(large.value().width >= small.value().width);
}

CY_TEST_CASE("unwrap: an unusable request is refused rather than approximated") {
    MeshData empty;
    Uv2Options options;
    CY_CHECK(!generate_uv2(empty, options).has_value());

    MeshData mesh = grid(2, 1.0f);
    Uv2Options impossible;
    impossible.texel_density = 0.0f;
    impossible.resolution = 0;
    CY_CHECK(!generate_uv2(mesh, impossible).has_value());

    Uv2Options no_stretch;
    no_stretch.max_distortion = 0.5f;
    CY_CHECK(!generate_uv2(mesh, no_stretch).has_value());
}

// --- Overdraw -----------------------------------------------------------------------------------

CY_TEST_CASE("overdraw: the reorder puts nearer runs first and leaves the mesh valid") {
    MeshData mesh = grid(8, 4.0f);
    CY_REQUIRE(optimise_vertex_cache(mesh).has_value());
    Vec3 centre{0.0f, 0.0f, 0.0f};
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        centre = centre + mesh.positions[index];
    }
    centre = centre * (1.0f / static_cast<f32>(mesh.vertex_count()));

    const usize triangles_before = mesh.triangle_count();
    const f32 before = mean_view_depth_of_first_half(mesh, centre);
    CY_REQUIRE(optimise_overdraw(mesh, 3.0f).has_value());
    const f32 after = mean_view_depth_of_first_half(mesh, centre);

    CY_CHECK(mesh.triangle_count() == triangles_before);
    CY_CHECK(mesh.validate().has_value());
    // The point of the step: the half drawn first is nearer the centre than it was, so early-Z has
    // something to reject with.
    CY_CHECK(after <= before);
}

CY_TEST_CASE("overdraw: a threshold of 1 changes nothing, and below 1 is refused") {
    MeshData strict = grid(6, 3.0f);
    CY_REQUIRE(optimise_vertex_cache(strict).has_value());
    const std::vector<u32> before(strict.indices.begin(), strict.indices.end());
    CY_REQUIRE(optimise_overdraw(strict, 1.0f).has_value());
    const std::vector<u32> after(strict.indices.begin(), strict.indices.end());
    CY_CHECK(before == after);

    CY_CHECK(!optimise_overdraw(strict, 0.5f).has_value());
}

CY_TEST_CASE("overdraw: sections still tile the index list after the reorder") {
    // A reorder that moved a triangle across a section boundary would change which material draws
    // it, which is a defect nothing downstream could detect.
    MeshData mesh = grid(6, 3.0f);
    const u32 half = static_cast<u32>(mesh.indices.size() / 6) * 3;
    mesh.sections[0].index_count = half;
    MeshSection second;
    second.first_index = half;
    second.index_count = static_cast<u32>(mesh.indices.size()) - half;
    second.material = 1;
    CY_REQUIRE(mesh.sections.push_back(second).has_value());

    CY_REQUIRE(optimise_vertex_cache(mesh).has_value());
    CY_REQUIRE(optimise_overdraw(mesh, 3.0f).has_value());
    CY_REQUIRE(mesh.validate().has_value());
    CY_CHECK(mesh.sections.size() == 2);
    CY_CHECK(mesh.sections[0].first_index == 0);
    CY_CHECK(mesh.sections[0].index_count + mesh.sections[1].index_count == mesh.indices.size());
    CY_CHECK(mesh.sections[1].first_index == mesh.sections[0].index_count);
}

CY_TEST_CASE("overdraw: reordering twice produces the same index list") {
    MeshData once = grid(7, 3.0f);
    MeshData twice = grid(7, 3.0f);
    CY_REQUIRE(optimise_vertex_cache(once).has_value());
    CY_REQUIRE(optimise_vertex_cache(twice).has_value());
    CY_REQUIRE(optimise_overdraw(once, 3.0f).has_value());
    CY_REQUIRE(optimise_overdraw(twice, 3.0f).has_value());
    CY_REQUIRE(once.indices.size() == twice.indices.size());
    for (usize index = 0; index < once.indices.size(); ++index) {
        CY_REQUIRE(once.indices[index] == twice.indices[index]);
    }
}

// --- Convex decomposition -----------------------------------------------------------------------

CY_TEST_CASE("decomposition: a convex shape comes back as one part") {
    const MeshData mesh = box(Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f});
    ConvexDecompositionOptions options;
    std::vector<MeshData> parts;
    const auto produced = convex_decomposition(mesh, options, parts);
    CY_REQUIRE(produced.has_value());
    CY_CHECK(produced.value() == 1);
    CY_REQUIRE(parts.size() == 1);
    CY_CHECK(parts[0].triangle_count() >= 4);
}

CY_TEST_CASE("decomposition: a concave shape is split, and the budget is respected") {
    const MeshData mesh = dumbbell();
    ConvexDecompositionOptions options;
    options.max_parts = 4;
    options.min_triangles = 4;
    std::vector<MeshData> parts;
    const auto produced = convex_decomposition(mesh, options, parts);
    CY_REQUIRE(produced.has_value());
    CY_CHECK(produced.value() >= 2);
    CY_CHECK(produced.value() <= options.max_parts);
    CY_CHECK(parts.size() == produced.value());
    for (const MeshData& part : parts) {
        CY_CHECK(part.validate().has_value());
        CY_CHECK(part.triangle_count() > 0);
    }
}

CY_TEST_CASE("decomposition: decomposing twice produces the same parts in the same order") {
    const MeshData mesh = dumbbell();
    ConvexDecompositionOptions options;
    options.max_parts = 4;
    options.min_triangles = 4;
    std::vector<MeshData> first;
    std::vector<MeshData> second;
    CY_REQUIRE(convex_decomposition(mesh, options, first).has_value());
    CY_REQUIRE(convex_decomposition(mesh, options, second).has_value());
    CY_REQUIRE(first.size() == second.size());
    for (usize part = 0; part < first.size(); ++part) {
        CY_REQUIRE(first[part].indices.size() == second[part].indices.size());
        for (usize index = 0; index < first[part].indices.size(); ++index) {
            CY_REQUIRE(first[part].indices[index] == second[part].indices[index]);
        }
    }
}

CY_TEST_CASE("decomposition: an empty mesh and a zero budget are refused") {
    const MeshData empty;
    ConvexDecompositionOptions options;
    std::vector<MeshData> parts;
    CY_CHECK(!convex_decomposition(empty, options, parts).has_value());

    const MeshData mesh = box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});
    ConvexDecompositionOptions none;
    none.max_parts = 0;
    CY_CHECK(!convex_decomposition(mesh, none, parts).has_value());
}
