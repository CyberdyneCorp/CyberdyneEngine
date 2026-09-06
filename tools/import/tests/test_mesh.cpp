// Mesh processing: welding, normals, tangents, the two orderings, and simplification. M5 task 5.1.
//
// The simplification cases are the ones worth reading. `asset-import-pipeline` names two properties
// that are easy to claim and easy to get wrong — an error bound that actually stops a collapse, and
// a UV seam that survives — and both are asserted here against a mesh built to have them.

#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>
#include <cy/import/gltf.h>
#include <cy/import/mesh.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy::import;
using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec2;
using cy::Vec3;

namespace {

/// A grid of `n` by `n` quads in the XZ plane, two triangles each, with texture coordinates.
///
/// Flat rather than curved on purpose: a flat mesh has a quadric error of zero for every interior
/// collapse, so the simplifier reduces it all the way and the cases below can state exact counts.
MeshData grid(u32 n) {
    MeshData mesh;
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            CY_REQUIRE(
                mesh.positions.push_back(Vec3{static_cast<f32>(x), 0.0f, static_cast<f32>(z)})
                    .has_value());
            CY_REQUIRE(mesh.uvs
                           .push_back(Vec2{static_cast<f32>(x) / static_cast<f32>(n),
                                           static_cast<f32>(z) / static_cast<f32>(n)})
                           .has_value());
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = (z * (n + 1)) + x;
            const u32 b = a + 1;
            const u32 c = a + n + 1;
            const u32 d = c + 1;
            for (const u32 index : {a, c, b, b, c, d}) {
                CY_REQUIRE(mesh.indices.push_back(index).has_value());
            }
        }
    }
    return mesh;
}

/// A cube of six flat faces, twenty-four vertices, so every edge is hard.
MeshData cube() {
    MeshData mesh;
    const Vec3 corners[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                             {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const u32 faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    for (const auto& face : faces) {
        const auto base = static_cast<u32>(mesh.positions.size());
        for (const u32 corner : face) {
            CY_REQUIRE(mesh.positions.push_back(corners[corner]).has_value());
        }
        for (const u32 index : {0U, 1U, 2U, 0U, 2U, 3U}) {
            CY_REQUIRE(mesh.indices.push_back(base + index).has_value());
        }
    }
    return mesh;
}

/// A copy, through the cooked form — `MeshData` is move-only by construction.
MeshData copy_of(const MeshData& mesh) {
    cy::Array<cy::u8> bytes;
    CY_REQUIRE(write_cooked_mesh(mesh, bytes).has_value());
    MeshData copy;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const cy::u8>(bytes.data(), bytes.size()), copy).has_value());
    return copy;
}

}  // namespace

CY_TEST_CASE("mesh: a malformed mesh is refused before any step reads out of bounds") {
    MeshData mesh;
    CY_REQUIRE(mesh.positions.push_back(Vec3{0, 0, 0}).has_value());
    CY_REQUIRE(mesh.indices.push_back(3).has_value());
    CY_REQUIRE(mesh.indices.push_back(0).has_value());
    CY_REQUIRE(mesh.indices.push_back(0).has_value());
    CY_CHECK(!mesh.validate().has_value());
}

CY_TEST_CASE("mesh: welding merges coincident vertices and keeps hard edges split") {
    // Two triangles that share an edge, written as six separate vertices.
    MeshData mesh;
    const Vec3 shared[6] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}};
    for (const Vec3& position : shared) {
        CY_REQUIRE(mesh.positions.push_back(position).has_value());
        CY_REQUIRE(mesh.normals.push_back(Vec3{0, 1, 0}).has_value());
    }
    for (u32 index = 0; index < 6; ++index) {
        CY_REQUIRE(mesh.indices.push_back(index).has_value());
    }

    auto removed = weld(mesh, WeldOptions{});
    CY_REQUIRE(removed.has_value());
    CY_CHECK(removed.value() == 2);
    CY_CHECK(mesh.vertex_count() == 4);
    CY_CHECK(mesh.triangle_count() == 2);
    CY_CHECK(mesh.validate().has_value());

    // The same positions, but the shared vertices have opposite normals: a hard edge, which must
    // not merge however close the positions are.
    MeshData hard;
    for (usize index = 0; index < 6; ++index) {
        CY_REQUIRE(hard.positions.push_back(shared[index]).has_value());
        CY_REQUIRE(hard.normals.push_back(index < 3 ? Vec3{0, 1, 0} : Vec3{0, -1, 0}).has_value());
    }
    for (u32 index = 0; index < 6; ++index) {
        CY_REQUIRE(hard.indices.push_back(index).has_value());
    }
    auto none = weld(hard, WeldOptions{});
    CY_REQUIRE(none.has_value());
    CY_CHECK(none.value() == 0);
}

CY_TEST_CASE("mesh: welding is idempotent") {
    // The first vertex of a group wins rather than an average, so welding twice is welding once —
    // which a pipeline that re-runs its steps depends on.
    MeshData mesh = grid(4);
    CY_REQUIRE(weld(mesh, WeldOptions{}).has_value());
    const usize after_once = mesh.vertex_count();
    CY_REQUIRE(weld(mesh, WeldOptions{}).has_value());
    CY_CHECK(mesh.vertex_count() == after_once);
}

CY_TEST_CASE("mesh: a smoothing angle splits a cube's corners and keeps a plane smooth") {
    MeshData sharp = cube();
    CY_REQUIRE(generate_normals(sharp, 30.0f).has_value());
    // Every corner belongs to three faces at ninety degrees, so nothing merges: twenty-four
    // vertices in and twenty-four out, each with its own face's normal.
    CY_CHECK(sharp.vertex_count() == 24);
    for (const Vec3& normal : sharp.normals) {
        CY_CHECK(cy::math::nearly_equal(length(normal), 1.0f, 1e-4f));
    }

    MeshData flat = grid(3);
    flat.normals.clear();
    CY_REQUIRE(generate_normals(flat, 30.0f).has_value());
    // A flat grid's faces all agree, so no vertex splits and every normal is the plane's.
    CY_CHECK(flat.vertex_count() == 16);
    for (const Vec3& normal : flat.normals) {
        const bool faces_up_or_down = cy::math::nearly_equal(normal.y, -1.0f, 1e-4f) ||
                                      cy::math::nearly_equal(normal.y, 1.0f, 1e-4f);
        CY_CHECK(faces_up_or_down);
    }
}

CY_TEST_CASE("mesh: tangents need normals and texture coordinates, and say so") {
    MeshData mesh = grid(2);
    CY_CHECK(!generate_tangents(mesh).has_value());
    CY_REQUIRE(generate_normals(mesh, 60.0f).has_value());
    CY_REQUIRE(generate_tangents(mesh).has_value());
    CY_CHECK(mesh.tangents.size() == mesh.vertex_count());
    for (const cy::Vec4& tangent : mesh.tangents) {
        // The convention: `w` is the handedness the shader multiplies the bitangent by, and it is
        // ±1 rather than a magnitude.
        CY_CHECK(cy::math::nearly_equal(std::fabs(tangent.w), 1.0f, 1e-4f));
    }
}

CY_TEST_CASE("mesh: the cache ordering keeps the triangles and improves reuse") {
    MeshData mesh = grid(8);
    const usize before = mesh.triangle_count();
    CY_REQUIRE(optimise_vertex_cache(mesh).has_value());
    CY_CHECK(mesh.triangle_count() == before);
    CY_CHECK(mesh.validate().has_value());

    // The measurable property: how many vertices a simulated cache misses. It must not get worse,
    // and on a grid it gets markedly better.
    const auto misses = [](const MeshData& subject, usize cache_size) noexcept {
        cy::Array<u32> cache;
        usize missed = 0;
        for (const u32 index : subject.indices) {
            bool present = false;
            for (const u32 held : cache) {
                present = present || held == index;
            }
            if (present) {
                continue;
            }
            ++missed;
            if (cache.size() == cache_size) {
                // The oldest goes; a first-in-first-out cache is what the hardware models.
                for (usize slot = 1; slot < cache.size(); ++slot) {
                    cache[slot - 1] = cache[slot];
                }
                cache.pop_back();
            }
            (void)cache.push_back(index);
        }
        return missed;
    };

    MeshData unordered = grid(8);
    CY_CHECK(misses(mesh, 16) <= misses(unordered, 16));
}

CY_TEST_CASE("mesh: the fetch ordering renumbers vertices into first-use order") {
    MeshData mesh = grid(4);
    // Reverse the triangles so the first index is the last vertex; the fetch pass must renumber.
    cy::Array<u32> reversed;
    for (usize triangle = mesh.triangle_count(); triangle > 0; --triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            CY_REQUIRE(reversed.push_back(mesh.indices[((triangle - 1) * 3) + corner]).has_value());
        }
    }
    mesh.indices = std::move(reversed);

    const usize vertices = mesh.vertex_count();
    CY_REQUIRE(optimise_vertex_fetch(mesh).has_value());
    CY_CHECK(mesh.vertex_count() == vertices);
    CY_CHECK(mesh.indices[0] == 0);
    CY_CHECK(mesh.validate().has_value());
}

CY_TEST_CASE("mesh: the fetch ordering drops vertices no triangle references") {
    MeshData mesh = grid(2);
    CY_REQUIRE(mesh.positions.push_back(Vec3{99, 99, 99}).has_value());
    CY_REQUIRE(mesh.uvs.push_back(Vec2{0, 0}).has_value());
    const usize before = mesh.vertex_count();
    CY_REQUIRE(optimise_vertex_fetch(mesh).has_value());
    CY_CHECK(mesh.vertex_count() == before - 1);
}

CY_TEST_CASE("mesh: simplification reaches its target and is deterministic") {
    MeshData mesh = grid(6);
    CY_REQUIRE(generate_normals(mesh, 60.0f).has_value());
    const usize before = mesh.triangle_count();

    SimplifyOptions options;
    options.target_ratio = 0.5f;
    options.preserve_seams = false;

    MeshData first = copy_of(mesh);
    MeshData second = copy_of(mesh);
    auto report = simplify(first, options);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report.value().triangles_before == before);
    CY_CHECK(report.value().triangles_after < before);
    CY_CHECK(first.validate().has_value());

    auto again = simplify(second, options);
    CY_REQUIRE(again.has_value());
    // The determinism `asset-import-pipeline` requires of every cooked output, and the property a
    // simplifier loses the moment it breaks a tie by heap address.
    CY_CHECK(again.value().triangles_after == report.value().triangles_after);
    CY_REQUIRE(first.vertex_count() == second.vertex_count());
    for (usize index = 0; index < first.vertex_count(); ++index) {
        CY_CHECK(first.positions[index].x == second.positions[index].x);
        CY_CHECK(first.positions[index].y == second.positions[index].y);
        CY_CHECK(first.positions[index].z == second.positions[index].z);
    }
}

CY_TEST_CASE("mesh: an error bound stops before it damages the mesh") {
    // A cube cannot lose a triangle without moving a corner, and moving a corner costs a quadric
    // error of one unit at least. With a bound far below that, the mesh comes back LARGER than
    // asked for rather than damaged — which is what makes an unattended level-of-detail chain safe.
    MeshData bounded = cube();
    SimplifyOptions options;
    options.target_ratio = 0.25f;
    options.error_bound = 1.0e-6f;
    options.preserve_seams = false;
    auto report = simplify(bounded, options);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report.value().bounded);
    CY_CHECK(report.value().triangles_after == report.value().triangles_before);
}

CY_TEST_CASE("mesh: a texture seam is not collapsed across") {
    // `asset-import-pipeline`: "WHEN a mesh with UV seams is simplified THEN seam vertices SHALL be
    // preserved or collapsed only along the seam, avoiding texture distortion."
    //
    // The mesh is a grid split down the middle column into two texture charts: the vertices at
    // x == 3 are duplicated, the two copies carry different texture coordinates, and the triangles
    // on the far side use the copies. The positions are identical, which is precisely what marks
    // those points as a seam.
    MeshData mesh = grid(6);
    CY_REQUIRE(generate_normals(mesh, 60.0f).has_value());

    const usize original_vertices = mesh.vertex_count();
    cy::Array<u32> duplicate_of;
    CY_REQUIRE(duplicate_of.resize(original_vertices).has_value());
    for (usize index = 0; index < original_vertices; ++index) {
        duplicate_of[index] = 0xFFFFFFFFU;
        if (!cy::math::nearly_equal(mesh.positions[index].x, 3.0f, 1e-4f)) {
            continue;
        }
        CY_REQUIRE(mesh.positions.push_back(mesh.positions[index]).has_value());
        CY_REQUIRE(mesh.normals.push_back(mesh.normals[index]).has_value());
        // A different texture coordinate is the whole of what makes it a seam.
        CY_REQUIRE(mesh.uvs.push_back(Vec2{0.0f, mesh.uvs[index].y}).has_value());
        duplicate_of[index] = static_cast<u32>(mesh.positions.size() - 1);
    }

    for (usize triangle = 0; triangle < mesh.triangle_count(); ++triangle) {
        f32 centre = 0.0f;
        for (usize corner = 0; corner < 3; ++corner) {
            centre += mesh.positions[mesh.indices[(triangle * 3) + corner]].x;
        }
        if (centre / 3.0f <= 3.0f) {
            continue;
        }
        for (usize corner = 0; corner < 3; ++corner) {
            u32& index = mesh.indices[(triangle * 3) + corner];
            if (index < original_vertices && duplicate_of[index] != 0xFFFFFFFFU) {
                index = duplicate_of[index];
            }
        }
    }
    CY_REQUIRE(mesh.validate().has_value());

    MeshData preserved = copy_of(mesh);
    SimplifyOptions options;
    options.target_ratio = 0.3f;
    options.preserve_seams = true;
    auto report = simplify(preserved, options);
    CY_REQUIRE(report.has_value());
    // Not vacuously true: the mesh did get smaller.
    CY_CHECK(report.value().triangles_after < report.value().triangles_before);

    // Nothing was pulled OFF the seam plane. A collapse across the seam would have moved one of its
    // points to a midpoint between the charts, which is the texture sliding.
    for (usize index = 0; index < preserved.vertex_count(); ++index) {
        const f32 x = preserved.positions[index].x;
        const bool off_the_seam =
            x <= 2.0f + 1e-4f || x >= 4.0f - 1e-4f || cy::math::nearly_equal(x, 3.0f, 1e-6f);
        CY_CHECK(off_the_seam);
    }

    // And BOTH charts still meet at the seam: a vertex on the plane carrying the near chart's
    // texture coordinate and another carrying the far chart's. Losing one of them would be the
    // seam having been welded shut.
    bool near_chart = false;
    bool far_chart = false;
    for (usize index = 0; index < preserved.vertex_count(); ++index) {
        if (!cy::math::nearly_equal(preserved.positions[index].x, 3.0f, 1e-6f)) {
            continue;
        }
        near_chart = near_chart || cy::math::nearly_equal(preserved.uvs[index].x, 0.5f, 1e-4f);
        far_chart = far_chart || cy::math::nearly_equal(preserved.uvs[index].x, 0.0f, 1e-4f);
    }
    CY_CHECK(near_chart);
    CY_CHECK(far_chart);
}

CY_TEST_CASE("mesh: a convex hull comes out of the source positions") {
    MeshData hull;
    CY_REQUIRE(convex_hull(cube(), hull).has_value());
    CY_CHECK(hull.triangle_count() >= 12);
    CY_CHECK(hull.vertex_count() == 8);
    CY_CHECK(hull.validate().has_value());
    // A hull carries no render attributes: normals, texture coordinates and tangents are the
    // renderer's and no solver reads them.
    CY_CHECK(hull.normals.empty());
    CY_CHECK(hull.uvs.empty());
}

CY_TEST_CASE("mesh: the cooked form round-trips exactly") {
    MeshData mesh = grid(3);
    CY_REQUIRE(generate_normals(mesh, 60.0f).has_value());
    CY_REQUIRE(generate_tangents(mesh).has_value());
    MeshSection section;
    section.first_index = 0;
    section.index_count = static_cast<u32>(mesh.indices.size());
    section.material = 2;
    CY_REQUIRE(mesh.sections.push_back(section).has_value());

    cy::Array<cy::u8> first;
    CY_REQUIRE(write_cooked_mesh(mesh, first).has_value());
    MeshData read;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const cy::u8>(first.data(), first.size()), read).has_value());
    cy::Array<cy::u8> second;
    CY_REQUIRE(write_cooked_mesh(read, second).has_value());

    // Byte-identical, which is `asset-import-pipeline`'s determinism requirement and the property
    // the cook cache's content addressing rests on.
    CY_REQUIRE(first.size() == second.size());
    for (usize index = 0; index < first.size(); ++index) {
        CY_CHECK(first[index] == second[index]);
    }
    CY_CHECK(read.sections.size() == 1);
    CY_CHECK(read.sections[0].material == 2);
}
