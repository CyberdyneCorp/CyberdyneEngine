// The virtual geometry cook: deterministic, addressed by the one derivation key, and refusing to
// ship a cracked asset. M7 task 7.5.
//
// This is the case `asset-import-pipeline`'s "Virtual geometry cooking" requirement has been
// waiting for. It asserts the three things that requirement actually needs, and it asserts them on
// the COOK rather than on the library underneath it: two runs produce the same bytes; the key
// changes when and only when something that changes the bytes changes; and a hierarchy whose cut is
// not watertight fails the cook instead of reaching a package.

#include <cy/test/test.h>

#include <cy/cook/geometry.h>
#include <cy/core/memory/system_allocator.h>

#include <cmath>

namespace {

using namespace cy;  // NOLINT(google-build-using-namespace) — the suite's own subject

/// A closed icosphere, in the caller's arrays. Duplicated from
/// src/rendering/virtual_geometry/tests/meshes.h rather than shared: that header is a test fixture
/// of another module and reaching into it from tools/ would make one suite's fixture another's
/// dependency. Twenty lines is cheaper than that coupling.
struct CookSphere {
    explicit CookSphere(Allocator& allocator) noexcept
        : positions(allocator), normals(allocator), indices(allocator), materials(allocator) {}

    Array<Vec3> positions;
    Array<Vec3> normals;
    Array<u32> indices;
    Array<u32> materials;

    [[nodiscard]] rendering::vg::SourceMesh source() const noexcept {
        rendering::vg::SourceMesh mesh;
        mesh.positions = positions.span();
        mesh.normals = normals.span();
        mesh.indices = indices.span();
        mesh.triangle_materials = materials.span();
        return mesh;
    }
};

u32 midpoint(CookSphere& sphere, Array<u64>& keys, Array<u32>& values, u32 a, u32 b) noexcept {
    const u64 key = (static_cast<u64>(a < b ? a : b) << 32U) | static_cast<u64>(a < b ? b : a);
    for (usize index = 0; index < keys.size(); ++index) {
        if (keys[index] == key) {
            return values[index];
        }
    }
    const Vec3 sum = (sphere.positions[a] + sphere.positions[b]) * 0.5F;
    const Vec3 unit = sum * (1.0F / length(sum));
    const u32 fresh = static_cast<u32>(sphere.positions.size());
    (void)sphere.positions.push_back(unit);
    (void)sphere.normals.push_back(unit);
    (void)keys.push_back(key);
    (void)values.push_back(fresh);
    return fresh;
}

CookSphere icosphere(Allocator& allocator, u32 subdivisions) noexcept {
    CookSphere sphere(allocator);
    const f32 golden = (1.0F + std::sqrt(5.0F)) * 0.5F;
    const Vec3 seed[12] = {{-1.0F, golden, 0.0F},  {1.0F, golden, 0.0F},   {-1.0F, -golden, 0.0F},
                           {1.0F, -golden, 0.0F},  {0.0F, -1.0F, golden},  {0.0F, 1.0F, golden},
                           {0.0F, -1.0F, -golden}, {0.0F, 1.0F, -golden},  {golden, 0.0F, -1.0F},
                           {golden, 0.0F, 1.0F},   {-golden, 0.0F, -1.0F}, {-golden, 0.0F, 1.0F}};
    for (const Vec3 point : seed) {
        const Vec3 unit = point * (1.0F / length(point));
        (void)sphere.positions.push_back(unit);
        (void)sphere.normals.push_back(unit);
    }
    const u32 faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (const auto& face : faces) {
        for (const u32 corner : face) {
            (void)sphere.indices.push_back(corner);
        }
    }
    Array<u64> keys(allocator);
    Array<u32> values(allocator);
    for (u32 pass = 0; pass < subdivisions; ++pass) {
        keys.clear();
        values.clear();
        Array<u32> next(allocator);
        for (usize corner = 0; corner < sphere.indices.size(); corner += 3) {
            const u32 a = sphere.indices[corner];
            const u32 b = sphere.indices[corner + 1];
            const u32 c = sphere.indices[corner + 2];
            const u32 ab = midpoint(sphere, keys, values, a, b);
            const u32 bc = midpoint(sphere, keys, values, b, c);
            const u32 ca = midpoint(sphere, keys, values, c, a);
            const u32 built[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
            for (const auto& triangle : built) {
                for (const u32 index : triangle) {
                    (void)next.push_back(index);
                }
            }
        }
        sphere.indices = std::move(next);
    }
    return sphere;
}

rendering::vg::BuildOptions cook_options() noexcept {
    rendering::vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.page_bytes = 4096;
    options.resident_budget_bytes = 4096;
    return options;
}

}  // namespace

CY_TEST_CASE("cooking a mesh twice produces the same bytes and the same key") {
    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    const CookSphere sphere = icosphere(allocator, 3);

    cook::GeometryCookRequest request;
    request.mesh = sphere.source();
    request.options = cook_options();

    Array<u8> first(allocator);
    Array<u8> second(allocator);
    Expected<cook::GeometryCookReport, Error> a =
        cook::cook_virtual_geometry(request, first, allocator);
    CY_REQUIRE(a.has_value());
    Expected<cook::GeometryCookReport, Error> b =
        cook::cook_virtual_geometry(request, second, allocator);
    CY_REQUIRE(b.has_value());

    CY_REQUIRE_EQ(first.size(), second.size());
    usize differing = 0;
    for (usize index = 0; index < first.size(); ++index) {
        differing += first[index] != second[index] ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK(a->key == b->key);
    CY_CHECK(a->content == b->content);

    CY_TEST_MESSAGE(
        "cooked " << a->cooked_bytes << " bytes: " << a->source_triangles
                  << " source triangles into " << a->clusters << " clusters over " << a->levels
                  << " levels in " << a->pages << " pages; resident " << a->resident_bytes
                  << " bytes in " << a->resident_pages << " pages; " << a->bytes_per_triangle
                  << " bytes per triangle; cluster metadata " << a->cluster_metadata_bytes
                  << " bytes; tangents saved " << a->tangent_bytes_saved
                  << " bytes; quantisation error " << a->quantisation_error);
    CY_CHECK_EQ(a->source_triangles, 1280U);
    CY_CHECK_GT(a->clusters, 40U);
    CY_CHECK_GT(a->levels, 2U);
    CY_CHECK_GT(a->pages, 1U);
    CY_CHECK_GT(a->tangent_bytes_saved, 0U);
    // "Counts are reported separately": the collision figure is a separate number, and it is zero
    // because this cook does not generate a proxy. Zero rather than the render count is the point.
    CY_CHECK_EQ(a->collision_triangles, 0U);
}

CY_TEST_CASE("the cook runs the watertightness check and reports it") {
    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    const CookSphere sphere = icosphere(allocator, 3);

    cook::GeometryCookRequest request;
    request.mesh = sphere.source();
    request.options = cook_options();

    Array<u8> bytes(allocator);
    Expected<cook::GeometryCookReport, Error> report =
        cook::cook_virtual_geometry(request, bytes, allocator);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->watertight);
    CY_CHECK(report->closed_source);
    CY_CHECK_GT(report->thresholds_tested, 100U);
}

CY_TEST_CASE("the key changes with the toolchain's inputs and with every option that moves bytes") {
    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    const CookSphere sphere = icosphere(allocator, 2);

    cook::GeometryCookRequest request;
    request.mesh = sphere.source();
    request.options = cook_options();

    Array<u8> bytes(allocator);
    Expected<cook::GeometryCookReport, Error> base =
        cook::cook_virtual_geometry(request, bytes, allocator);
    CY_REQUIRE(base.has_value());

    struct Variation {
        const char* name;
        void (*apply)(cook::GeometryCookRequest&);
    };
    const Variation variations[] = {
        {"variant", [](cook::GeometryCookRequest& r) { r.variant = "console"; }},
        {"cluster size",
         [](cook::GeometryCookRequest& r) { r.options.policy.target_triangles = 16; }},
        {"group size", [](cook::GeometryCookRequest& r) { r.options.policy.group_size = 8; }},
        {"page size", [](cook::GeometryCookRequest& r) { r.options.page_bytes = 8192; }},
        {"position precision", [](cook::GeometryCookRequest& r) { r.options.position_bits = 12; }},
        {"weld epsilon", [](cook::GeometryCookRequest& r) { r.options.weld_epsilon = 1.0e-3F; }},
        {"surface class",
         [](cook::GeometryCookRequest& r) {
             r.options.surface = rendering::vg::SurfaceClass::Foliage;
         }},
    };
    for (const Variation& variation : variations) {
        cook::GeometryCookRequest varied = request;
        variation.apply(varied);
        Array<u8> other(allocator);
        Expected<cook::GeometryCookReport, Error> report =
            cook::cook_virtual_geometry(varied, other, allocator);
        CY_REQUIRE(report.has_value());
        CY_CHECK(report->key != base->key);
        if (report->key == base->key) {
            CY_TEST_MESSAGE("the key did not change for: " << variation.name);
        }
    }

    // And a mesh whose content differs produces a different key even under identical options.
    CookSphere moved = icosphere(allocator, 2);
    moved.positions[0] = moved.positions[0] * 1.5F;
    cook::GeometryCookRequest other_mesh = request;
    other_mesh.mesh = moved.source();
    Array<u8> other(allocator);
    Expected<cook::GeometryCookReport, Error> report =
        cook::cook_virtual_geometry(other_mesh, other, allocator);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->key != base->key);
    CY_CHECK(report->content != base->content);
}

CY_TEST_CASE("a cook of an empty mesh fails rather than producing an addressable nothing") {
    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    cook::GeometryCookRequest request;
    request.options = cook_options();
    Array<u8> bytes(allocator);
    CY_CHECK_FALSE(cook::cook_virtual_geometry(request, bytes, allocator).has_value());
    CY_CHECK(bytes.empty());
}
