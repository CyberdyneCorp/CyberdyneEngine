#pragma once
// Source meshes for the CyberGeometry suites, and the reason each one exists.
//
// THE SPHERE IS THE ONE THAT MATTERS. The crack-free requirement is checkable only on a CLOSED
// surface: "a frame that renders some clusters at one level and neighbours at another SHALL produce
// no holes" becomes "every edge of the cut is used by exactly two triangles", and that sentence is
// meaningless on an open sheet, where an edge used once is the border rather than a crack. A
// subdivided icosahedron is closed, has no seam and no pole, and every vertex has five or six
// neighbours — so a simplifier that is going to move a boundary vertex has every opportunity to.
//
// THE GRID IS THE OPEN CASE, and it is here so that `check_watertight` reporting "not applicable"
// is itself tested. A check that silently passed on an open mesh would pass on every mesh whose
// import went wrong.
//
// THE TWO-MATERIAL SPHERE exists for one requirement — clustering never mixes materials — and for
// the material binning in visbuffer.h, which needs more than one bin to be a bin.

#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_geometry/build.h>

#include <cmath>

namespace cy::rendering::vg::test {

/// Positions, normals, UVs and indices in the caller's own arrays, so a `SourceMesh` can point at
/// them for as long as the test needs.
struct MeshData {
    explicit MeshData(Allocator& allocator) noexcept
        : positions(allocator),
          normals(allocator),
          uvs(allocator),
          indices(allocator),
          materials(allocator) {}

    MeshData(const MeshData&) = delete;
    MeshData& operator=(const MeshData&) = delete;
    MeshData(MeshData&&) noexcept = default;

    Array<Vec3> positions;
    Array<Vec3> normals;
    Array<Vec2> uvs;
    Array<u32> indices;
    Array<u32> materials;

    [[nodiscard]] SourceMesh source() const noexcept {
        SourceMesh mesh;
        mesh.positions = positions.span();
        mesh.normals = normals.span();
        mesh.uvs = uvs.span();
        mesh.indices = indices.span();
        mesh.triangle_materials = materials.span();
        return mesh;
    }
};

namespace detail {

inline void push_triangle(MeshData& mesh, u32 a, u32 b, u32 c) noexcept {
    (void)mesh.indices.push_back(a);
    (void)mesh.indices.push_back(b);
    (void)mesh.indices.push_back(c);
}

/// The midpoint of an edge, projected back onto the unit sphere, memoised so that the two triangles
/// sharing an edge share the vertex. Without the memo the subdivided mesh would be a soup of
/// duplicated corners and the weld would be doing all the work, which is not what the test is for.
inline u32 subdivide_midpoint(MeshData& mesh, Array<u64>& keys, Array<u32>& values, u32 a,
                              u32 b) noexcept {
    const u64 key = (static_cast<u64>(a < b ? a : b) << 32U) | static_cast<u64>(a < b ? b : a);
    for (usize index = 0; index < keys.size(); ++index) {
        if (keys[index] == key) {
            return values[index];
        }
    }
    const Vec3 midpoint = (mesh.positions[a] + mesh.positions[b]) * 0.5F;
    const f32 magnitude = length(midpoint);
    const Vec3 unit = magnitude > 0.0F ? midpoint * (1.0F / magnitude) : Vec3{0.0F, 0.0F, 1.0F};
    const u32 fresh = static_cast<u32>(mesh.positions.size());
    (void)mesh.positions.push_back(unit);
    (void)mesh.normals.push_back(unit);
    (void)mesh.uvs.push_back(Vec2{(unit.x + 1.0F) * 0.5F, (unit.y + 1.0F) * 0.5F});
    (void)keys.push_back(key);
    (void)values.push_back(fresh);
    return fresh;
}

}  // namespace detail

/// A closed, seamless sphere: an icosahedron subdivided `subdivisions` times, radius `radius`.
/// Triangle count is 20 * 4^subdivisions.
[[nodiscard]] inline MeshData icosphere(Allocator& allocator, u32 subdivisions,
                                        f32 radius = 1.0F) noexcept {
    MeshData mesh(allocator);
    const f32 golden = (1.0F + std::sqrt(5.0F)) * 0.5F;
    const Vec3 seed[12] = {{-1.0F, golden, 0.0F},  {1.0F, golden, 0.0F},   {-1.0F, -golden, 0.0F},
                           {1.0F, -golden, 0.0F},  {0.0F, -1.0F, golden},  {0.0F, 1.0F, golden},
                           {0.0F, -1.0F, -golden}, {0.0F, 1.0F, -golden},  {golden, 0.0F, -1.0F},
                           {golden, 0.0F, 1.0F},   {-golden, 0.0F, -1.0F}, {-golden, 0.0F, 1.0F}};
    for (const Vec3 point : seed) {
        const Vec3 unit = point * (1.0F / length(point));
        (void)mesh.positions.push_back(unit);
        (void)mesh.normals.push_back(unit);
        (void)mesh.uvs.push_back(Vec2{(unit.x + 1.0F) * 0.5F, (unit.y + 1.0F) * 0.5F});
    }
    const u32 faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (const auto& face : faces) {
        detail::push_triangle(mesh, face[0], face[1], face[2]);
    }

    Array<u64> keys(allocator);
    Array<u32> values(allocator);
    for (u32 pass = 0; pass < subdivisions; ++pass) {
        keys.clear();
        values.clear();
        Array<u32> next(allocator);
        for (usize corner = 0; corner < mesh.indices.size(); corner += 3) {
            const u32 a = mesh.indices[corner];
            const u32 b = mesh.indices[corner + 1];
            const u32 c = mesh.indices[corner + 2];
            const u32 ab = detail::subdivide_midpoint(mesh, keys, values, a, b);
            const u32 bc = detail::subdivide_midpoint(mesh, keys, values, b, c);
            const u32 ca = detail::subdivide_midpoint(mesh, keys, values, c, a);
            const u32 built[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
            for (const auto& triangle : built) {
                for (const u32 index : triangle) {
                    (void)next.push_back(index);
                }
            }
        }
        mesh.indices = std::move(next);
    }
    for (Vec3& position : mesh.positions) {
        position = position * radius;
    }
    return mesh;
}

/// An open sheet: `side` by `side` quads in the XZ plane. The case where a watertightness check
/// must report "not applicable" rather than a pass.
[[nodiscard]] inline MeshData grid(Allocator& allocator, u32 side, f32 extent = 1.0F) noexcept {
    MeshData mesh(allocator);
    const u32 line = side + 1U;
    for (u32 z = 0; z < line; ++z) {
        for (u32 x = 0; x < line; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(side);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(side);
            (void)mesh.positions.push_back(Vec3{(u - 0.5F) * extent, 0.0F, (v - 0.5F) * extent});
            (void)mesh.normals.push_back(Vec3{0.0F, 1.0F, 0.0F});
            (void)mesh.uvs.push_back(Vec2{u, v});
        }
    }
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            const u32 base = (z * line) + x;
            detail::push_triangle(mesh, base, base + line, base + 1U);
            detail::push_triangle(mesh, base + 1U, base + line, base + line + 1U);
        }
    }
    return mesh;
}

/// The sphere, with the northern hemisphere on material 1 and the southern on material 0.
[[nodiscard]] inline MeshData two_material_sphere(Allocator& allocator, u32 subdivisions) noexcept {
    MeshData mesh = icosphere(allocator, subdivisions);
    for (usize corner = 0; corner < mesh.indices.size(); corner += 3) {
        const f32 height =
            (mesh.positions[mesh.indices[corner]].y + mesh.positions[mesh.indices[corner + 1]].y +
             mesh.positions[mesh.indices[corner + 2]].y) /
            3.0F;
        (void)mesh.materials.push_back(height >= 0.0F ? 1U : 0U);
    }
    return mesh;
}

}  // namespace cy::rendering::vg::test
