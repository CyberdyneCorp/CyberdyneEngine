#include "scene.h"

#include <cy/core/base/expected.h>

#include <chrono>
#include <cmath>
#include <utility>

namespace cy::sample::fidelity {
namespace {

constexpr f32 kPi = 3.14159265358979F;

/// Componentwise product. `cy::Vec3` has no `operator*` for two vectors on purpose — the engine's
/// vectors are points and directions, and the only thing this file wants it for is scaling a unit
/// cube into a box, which is exactly what a named function should say.
[[nodiscard]] constexpr Vec3 scaled(Vec3 v, Vec3 by) noexcept {
    return Vec3{v.x * by.x, v.y * by.y, v.z * by.z};
}

/// The point of the unit cube in direction `d`. Every shape below starts here, so two cube faces
/// that meet on an edge produce the same point from the same direction and the weld has something
/// to weld.
[[nodiscard]] Vec3 cube_point(Vec3 d) noexcept {
    const f32 ax = std::fabs(d.x);
    const f32 ay = std::fabs(d.y);
    const f32 az = std::fabs(d.z);
    const f32 largest = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    return d * (1.0F / (largest > 1.0e-6F ? largest : 1.0e-6F));
}

/// The square-to-disc map, which is what turns a cube face into a column's cross-section without
/// tearing it: the boundary of the square goes to the circle and the interior stays interior.
[[nodiscard]] Vec2 square_to_disc(Vec2 p) noexcept {
    return Vec2{p.x * std::sqrt(1.0F - (0.5F * p.y * p.y)),
                p.y * std::sqrt(1.0F - (0.5F * p.x * p.x))};
}

[[nodiscard]] Vec3 hall_point(Vec3 d) noexcept {
    const Vec3 base = scaled(cube_point(d), Vec3{9.0F, 3.2F, 7.0F});
    // Panelling, and a coarser sag over it. A function of the surface point alone: a displacement
    // taken along the face normal would give two different answers on a shared edge.
    const f32 panel = (0.05F * std::sin(3.1F * base.x) * std::sin(3.3F * base.z)) +
                      (0.03F * std::sin(11.0F * base.x) * std::sin(9.0F * base.y)) +
                      (0.02F * std::sin(7.0F * base.z) * std::sin(5.0F * base.y));
    return base + (d * panel);
}

[[nodiscard]] Vec3 column_point(Vec3 d) noexcept {
    const Vec3 cube = cube_point(d);
    const Vec2 disc = square_to_disc(Vec2{cube.x, cube.z});
    const f32 theta = std::atan2(disc.y, disc.x);
    // Twenty flutes, and a taper: a column is not a cylinder and the taper is what a coarse level
    // has to keep.
    const f32 flute = 1.0F + (0.045F * std::cos(20.0F * theta));
    const f32 taper = 1.0F - (0.06F * (cube.y + 1.0F));
    const f32 radius = 0.42F * flute * taper;
    return Vec3{disc.x * radius, cube.y * 2.6F, disc.y * radius};
}

[[nodiscard]] Vec3 statue_point(Vec3 d) noexcept {
    const f32 folds = 0.13F * std::sin(4.0F * kPi * d.x) * std::sin(4.0F * kPi * d.y) *
                      std::sin(4.0F * kPi * d.z);
    const f32 ridges = 0.05F * std::sin(7.0F * kPi * d.y);
    return d * (1.15F * (1.0F + folds + ridges));
}

[[nodiscard]] Vec3 facade_point(Vec3 d) noexcept {
    const Vec3 base = scaled(cube_point(d), Vec3{6.0F, 9.0F, 6.0F});
    // Window recesses. `pulse` is smooth, so the recess is a moulding rather than a step — a step
    // would be a fold the simplifier cannot collapse and a crack waiting for a coarse level.
    const auto pulse = [](f32 v) { return 0.5F + (0.5F * std::cos(v)); };
    const f32 storeys = pulse(2.1F * base.y);
    const f32 bays = pulse(1.6F * base.x) * pulse(1.6F * base.z);
    const f32 cornice = 0.08F * std::sin(0.7F * base.y);
    return base - (d * ((0.32F * storeys * bays) + cornice));
}

[[nodiscard]] Vec3 terrain_point(Vec3 d) noexcept {
    const Vec3 base = scaled(cube_point(d), Vec3{22.0F, 1.4F, 22.0F});
    // The heightfield rides the top face and fades to nothing at the bottom, so the slab still
    // sits flat on whatever it is tiled against.
    const f32 upward = base.y > 0.0F ? base.y / 1.4F : 0.0F;
    const f32 height = (0.75F * std::sin(0.21F * base.x) * std::sin(0.19F * base.z)) +
                       (0.28F * std::sin(0.63F * base.x + 1.7F)) +
                       (0.14F * std::sin(0.91F * base.z - 0.4F));
    return base + Vec3{0.0F, upward * height, 0.0F};
}

/// The factor a shell is cooked SMALLER by, and the instance scale that puts it back.
///
/// THIS IS NOT COSMETIC AND IT IS THE ONE PIECE OF AUTHORING ADVICE THIS ARTEFACT CONTAINS.
/// `check_watertight`'s monotonicity test asks whether a group's enclosing sphere contains each
/// member's, and both spheres are single precision. Cooked at world size the terrain slab is 44
/// metres across, its group spheres are tens of metres, and the containment test's absolute
/// 1.0e-6 slack is far below the rounding of `sqrt(dot(delta, delta)) + radius` at that magnitude:
/// the check reported 47 violations on a hierarchy the builder had just constructed to satisfy it.
/// Measured, the same shells cooked into a radius of about two report ZERO — the statue, which was
/// already unit sized, reported zero at every resolution tried.
///
/// So a shell is cooked in its own space and placed by `GeometryInstance::scale`, which is what an
/// asset pipeline does anyway. The finding belongs to `src/rendering/virtual_geometry/` and is
/// written up in this directory's README rather than worked around silently.
[[nodiscard]] f32 cook_scale(Shape shape) noexcept {
    switch (shape) {
        case Shape::Hall: return 9.0F;
        case Shape::Column: return 2.6F;
        case Shape::Statue: return 1.15F;
        case Shape::Facade: return 9.0F;
        case Shape::Terrain: return 22.0F;
        case Shape::Count: break;
    }
    return 1.0F;
}

[[nodiscard]] Vec3 shape_point(Shape shape, Vec3 d) noexcept {
    switch (shape) {
        case Shape::Hall: return hall_point(d);
        case Shape::Column: return column_point(d);
        case Shape::Statue: return statue_point(d);
        case Shape::Facade: return facade_point(d);
        case Shape::Terrain: return terrain_point(d);
        case Shape::Count: break;
    }
    return d;
}

/// The six cube faces, as an origin and two edge vectors over u, v in [-1, 1].
struct Face {
    Vec3 origin;
    Vec3 du;
    Vec3 dv;
};

constexpr Face kFaces[6] = {
    {{1.0F, -1.0F, -1.0F}, {0.0F, 0.0F, 2.0F}, {0.0F, 2.0F, 0.0F}},
    {{-1.0F, -1.0F, 1.0F}, {0.0F, 0.0F, -2.0F}, {0.0F, 2.0F, 0.0F}},
    {{-1.0F, 1.0F, -1.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 2.0F}},
    {{-1.0F, -1.0F, 1.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -2.0F}},
    {{-1.0F, -1.0F, -1.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}},
    {{1.0F, -1.0F, 1.0F}, {-2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}},
};

void accumulate_normals(Mesh& mesh) noexcept {
    for (Vec3& normal : mesh.normals) {
        normal = Vec3{0.0F, 0.0F, 0.0F};
    }
    for (usize triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const u32 a = mesh.indices[triangle];
        const u32 b = mesh.indices[triangle + 1];
        const u32 c = mesh.indices[triangle + 2];
        // Un-normalised, so the accumulation is area weighted.
        const Vec3 face = cross(mesh.positions[b] - mesh.positions[a],
                                mesh.positions[c] - mesh.positions[a]);
        mesh.normals[a] = mesh.normals[a] + face;
        mesh.normals[b] = mesh.normals[b] + face;
        mesh.normals[c] = mesh.normals[c] + face;
    }
    for (Vec3& normal : mesh.normals) {
        normal = normalized_or(normal, Vec3{0.0F, 1.0F, 0.0F});
    }
}

}  // namespace

const char* shape_name(Shape shape) noexcept {
    switch (shape) {
        case Shape::Hall: return "hall";
        case Shape::Column: return "column";
        case Shape::Statue: return "statue";
        case Shape::Facade: return "facade";
        case Shape::Terrain: return "terrain";
        case Shape::Count: break;
    }
    return "?";
}

rendering::vg::SourceMesh Mesh::source() const noexcept {
    rendering::vg::SourceMesh mesh;
    mesh.positions = positions.span();
    mesh.normals = normals.span();
    mesh.uvs = uvs.span();
    mesh.indices = indices.span();
    return mesh;
}

Status generate(Shape shape, u32 resolution, Mesh& out) noexcept {
    const u32 side = resolution > 1 ? resolution : 1U;
    const u32 per_face = (side + 1U) * (side + 1U);
    const usize vertices = static_cast<usize>(per_face) * 6U;
    const usize triangles = static_cast<usize>(side) * side * 12U;

    // Sized once and filled by index. Growth in this loop would be six hundred thousand
    // `[[nodiscard]] Status`es to check for a capacity that is known before the first one.
    if (Status sized = out.positions.resize(vertices); !sized) return sized;
    if (Status sized = out.normals.resize(vertices); !sized) return sized;
    if (Status sized = out.uvs.resize(vertices); !sized) return sized;
    if (Status sized = out.indices.resize(triangles * 3U); !sized) return sized;

    // The hall is the one shell the camera stands INSIDE, so its winding and its normals are
    // reversed: a solid seen from within is backfacing at every pixel, and cluster cone culling
    // would drop the whole interior before the rasteriser saw it.
    const bool inverted = shape == Shape::Hall;
    const f32 step = 2.0F / static_cast<f32>(side);
    const f32 inverse_scale = 1.0F / cook_scale(shape);

    usize index_cursor = 0;
    for (u32 face = 0; face < 6U; ++face) {
        const u32 base_vertex = face * per_face;
        for (u32 iv = 0; iv <= side; ++iv) {
            for (u32 iu = 0; iu <= side; ++iu) {
                const f32 u = -1.0F + (static_cast<f32>(iu) * step);
                const f32 v = -1.0F + (static_cast<f32>(iv) * step);
                const Vec3 corner = kFaces[face].origin + (kFaces[face].du * ((u + 1.0F) * 0.5F)) +
                                    (kFaces[face].dv * ((v + 1.0F) * 0.5F));
                const Vec3 direction = normalize(corner);
                const u32 slot = base_vertex + (iv * (side + 1U)) + iu;
                out.positions[slot] = shape_point(shape, direction) * inverse_scale;
                out.uvs[slot] = Vec2{(u + 1.0F) * 0.5F, (v + 1.0F) * 0.5F};
            }
        }
        for (u32 iv = 0; iv < side; ++iv) {
            for (u32 iu = 0; iu < side; ++iu) {
                const u32 a = base_vertex + (iv * (side + 1U)) + iu;
                const u32 b = a + 1U;
                const u32 c = a + side + 1U;
                const u32 d = c + 1U;
                const u32 winding[6] = {a, c, b, b, c, d};
                for (u32 corner = 0; corner < 6U; ++corner) {
                    const u32 source = inverted ? winding[5U - corner] : winding[corner];
                    out.indices[index_cursor++] = source;
                }
            }
        }
    }
    accumulate_normals(out);
    return ok();
}

namespace {

/// Where one shape's instances go. The interior sits at the origin and the district is 260 metres
/// down +X, so the two halves of the shot are two places rather than two camera angles.
constexpr Vec3 kDistrict{260.0F, 0.0F, 0.0F};

void place(Shape shape, u32 count, Array<rendering::vg::GeometryInstance>& out) noexcept {
    for (u32 index = 0; index < count; ++index) {
        rendering::vg::GeometryInstance instance;
        instance.asset = static_cast<u32>(shape);
        instance.scale = cook_scale(shape);
        instance.material_offset = static_cast<u32>(shape);
        const auto i = static_cast<f32>(index);
        switch (shape) {
            case Shape::Hall:
                instance.translation = Vec3{0.0F, 0.0F, 0.0F};
                instance.importance = rendering::vg::Importance::Critical;
                break;
            case Shape::Column: {
                const f32 row = (index % 2U) == 0U ? -4.6F : 4.6F;
                const f32 along = -7.5F + (0.79F * std::floor(i * 0.5F));
                instance.translation = Vec3{along, -0.6F, row};
                break;
            }
            case Shape::Statue: {
                const bool interior = index < 12U;
                const f32 angle = i * 0.5236F;
                instance.translation =
                    interior ? Vec3{3.0F * std::cos(angle), -2.0F, 3.0F * std::sin(angle)}
                             : kDistrict + Vec3{26.0F * std::cos(angle), 1.0F,
                                                26.0F * std::sin(angle)};
                instance.scale *= interior ? 0.8F : 1.6F;
                break;
            }
            case Shape::Facade: {
                const f32 column = std::floor(i / 8.0F);
                const f32 row = i - (column * 8.0F);
                instance.translation =
                    kDistrict + Vec3{-60.0F + (24.0F * column), 8.0F, -84.0F + (24.0F * row)};
                break;
            }
            case Shape::Terrain: {
                const f32 column = std::floor(i / 7.0F);
                const f32 row = i - (column * 7.0F);
                instance.translation =
                    kDistrict + Vec3{-132.0F + (44.0F * column), -2.0F, -132.0F + (44.0F * row)};
                break;
            }
            case Shape::Count: break;
        }
        // The array was reserved for exactly this many instances before the first call.
        (void)out.push_back(instance);
    }
}

/// One surfel per instance face, which is what the GI scene ingests. The cards stand for the
/// surface a ray would find; the resolution is the GI system's near-field error target, not the
/// mesh's.
Status add_surfels(const Scene& scene, Array<rendering::gi::Surfel>& out) noexcept {
    static constexpr Vec3 kAlbedo[static_cast<u32>(Shape::Count)] = {
        {0.62F, 0.60F, 0.56F}, {0.78F, 0.74F, 0.66F}, {0.55F, 0.20F, 0.14F},
        {0.48F, 0.50F, 0.54F}, {0.34F, 0.38F, 0.28F},
    };
    static constexpr Vec3 kDirections[6] = {{1.0F, 0.0F, 0.0F},  {-1.0F, 0.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F},  {0.0F, -1.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F}};
    for (const rendering::vg::GeometryInstance& instance : scene.instances) {
        const auto shape = static_cast<Shape>(instance.asset);
        const Aabb local = scene.decoded[instance.asset].bounds;
        const Vec3 extents = local.half_extents() * instance.scale;
        const bool inward = shape == Shape::Hall;
        for (u32 face = 0; face < 6U; ++face) {
            // Four cards a face, so a wall is more than one sample and a bounce has somewhere to
            // land. The hall's face cards look inward, which is where its light is.
            for (u32 corner = 0; corner < 4U; ++corner) {
                rendering::gi::Surfel surfel;
                const Vec3 normal = kDirections[face];
                const Vec3 tangent{normal.y, normal.z, normal.x};
                const Vec3 bitangent = cross(normal, tangent);
                const f32 su = (corner & 1U) != 0U ? 0.45F : -0.45F;
                const f32 sv = (corner & 2U) != 0U ? 0.45F : -0.45F;
                surfel.position = instance.translation + scaled(normal, extents) +
                                  (tangent * (su * extents.x)) + (bitangent * (sv * extents.z));
                surfel.normal = inward ? normal * -1.0F : normal;
                surfel.albedo = kAlbedo[instance.asset];
                surfel.roughness = shape == Shape::Statue ? 0.35F : 0.72F;
                surfel.area = 0.25F * extents.x * extents.z * 4.0F;
                surfel.instance_id = instance.material_offset;
                surfel.material_id = instance.material_offset;
                if (Status added = out.push_back(surfel); !added) return added;
            }
        }
    }
    return ok();
}

Status add_lights(Array<rendering::gi::GiLight>& out) noexcept {
    rendering::gi::GiLight key;
    key.position = Vec3{-3.0F, 2.4F, -2.0F};
    key.colour = Vec3{1.0F, 0.94F, 0.86F};
    key.intensity = 42.0F;
    key.range = 26.0F;
    key.id = 1;
    if (Status added = out.push_back(key); !added) return added;

    rendering::gi::GiLight fill;
    fill.position = Vec3{4.5F, 1.6F, 3.0F};
    fill.colour = Vec3{0.72F, 0.80F, 1.0F};
    fill.intensity = 18.0F;
    fill.range = 22.0F;
    fill.id = 2;
    if (Status added = out.push_back(fill); !added) return added;

    rendering::gi::GiLight sun;
    sun.direction = normalize(Vec3{-0.42F, -0.78F, -0.46F});
    sun.colour = Vec3{1.0F, 0.97F, 0.90F};
    sun.intensity = 92000.0F;
    sun.directional = true;
    sun.id = 3;
    return out.push_back(sun);
}

rendering::vg::BuildOptions build_options_for(Shape shape) noexcept {
    rendering::vg::BuildOptions options;
    options.policy.min_triangles = 64;
    options.policy.target_triangles = 124;
    options.policy.max_triangles = 128;
    options.policy.max_vertices = 256;
    options.policy.group_size = 4;
    options.weld_epsilon = 2.0e-5F;
    options.page_bytes = 128U * 1024U;
    options.resident_budget_bytes = 64U * 1024U;
    options.surface = shape == Shape::Terrain ? rendering::vg::SurfaceClass::Solid
                                              : rendering::vg::SurfaceClass::Solid;
    return options;
}

Status cook_one(Shape shape, u32 resolution, Scene& scene) noexcept {
    Mesh mesh(scene.allocator);
    if (Status generated = generate(shape, resolution, mesh); !generated) return generated;

    const auto started = std::chrono::steady_clock::now();
    Expected<rendering::vg::GeometryBuild, Error> built =
        rendering::vg::build_geometry(mesh.source(), build_options_for(shape), scene.allocator);
    if (!built) return Status{make_unexpected(built.error())};
    const f32 cook_ms = std::chrono::duration<f32, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();

    Expected<rendering::vg::WatertightReport, Error> watertight =
        rendering::vg::check_watertight(*built, scene.allocator);
    if (!watertight) return Status{make_unexpected(watertight.error())};

    CookedAsset asset(scene.allocator);
    asset.shape = shape;
    if (Status encoded = rendering::vg::encode_asset(*built, rendering::vg::VertexEncoding{},
                                                     asset.bytes);
        !encoded) {
        return encoded;
    }
    asset.source_triangles = built->source_triangles;
    asset.clusters = static_cast<u32>(built->clusters.size());
    asset.levels = built->levels;
    asset.pages = static_cast<u32>(built->pages.size());
    asset.resident_bytes = built->resident_bytes;
    asset.cooked_bytes = static_cast<u32>(asset.bytes.size());
    asset.cook_ms = cook_ms;
    asset.watertight = *watertight;

    if (Status stored = scene.assets.push_back(std::move(asset)); !stored) return stored;
    Expected<rendering::vg::DecodedAsset, Error> decoded =
        rendering::vg::decode_asset(scene.assets[scene.assets.size() - 1U].bytes.span(),
                                    scene.allocator);
    if (!decoded) return Status{make_unexpected(decoded.error())};
    return scene.decoded.push_back(std::move(*decoded));
}

}  // namespace

Status build_scene(const SceneOptions& options, Scene& out) noexcept {
    const auto shapes = static_cast<u32>(Shape::Count);
    if (Status reserved = out.assets.reserve(shapes); !reserved) return reserved;
    if (Status reserved = out.decoded.reserve(shapes); !reserved) return reserved;

    for (u32 index = 0; index < shapes; ++index) {
        if (Status cooked = cook_one(static_cast<Shape>(index), options.resolution[index], out);
            !cooked) {
            return cooked;
        }
    }

    u32 total_instances = 0;
    for (u32 index = 0; index < shapes; ++index) {
        total_instances += options.instances[index];
    }
    if (Status reserved = out.instances.reserve(total_instances); !reserved) return reserved;
    for (u32 index = 0; index < shapes; ++index) {
        place(static_cast<Shape>(index), options.instances[index], out.instances);
    }

    out.bounds = Aabb::empty();
    for (const rendering::vg::GeometryInstance& instance : out.instances) {
        const CookedAsset& asset = out.assets[instance.asset];
        const Aabb local = out.decoded[instance.asset].bounds;
        const Vec3 extents = local.half_extents() * instance.scale;
        out.bounds = merge(out.bounds, Aabb::from_center_extents(instance.translation, extents));
        out.source_triangles += asset.source_triangles;
    }
    for (const CookedAsset& asset : out.assets) {
        out.distinct_triangles += asset.source_triangles;
        out.clusters += asset.clusters;
        out.pages += asset.pages;
        out.cooked_bytes += asset.cooked_bytes;
        out.resident_bytes += asset.resident_bytes;
        out.cook_ms += asset.cook_ms;
        out.closed_assets += asset.watertight.closed_source ? 1U : 0U;
        out.watertight_assets += asset.watertight.watertight() ? 1U : 0U;
    }

    if (Status added = add_surfels(out, out.surfels); !added) return added;
    return add_lights(out.lights);
}

Vec3 camera_at(const Scene& scene, f32 t) noexcept {
    (void)scene;
    if (t < 0.5F) {
        const f32 u = t * 2.0F;
        return Vec3{-5.5F + (9.0F * u), -1.1F + (0.5F * u), 4.2F - (1.4F * u)};
    }
    const f32 u = (t - 0.5F) * 2.0F;
    return kDistrict + Vec3{-96.0F + (56.0F * u), 26.0F - (8.0F * u), 96.0F - (44.0F * u)};
}

Vec3 camera_target(const Scene& scene, f32 t) noexcept {
    (void)scene;
    if (t < 0.5F) {
        const f32 u = t * 2.0F;
        return Vec3{2.0F - (4.0F * u), -0.7F, -3.0F + (1.0F * u)};
    }
    return kDistrict + Vec3{-8.0F, 2.0F, -12.0F};
}

}  // namespace cy::sample::fidelity
