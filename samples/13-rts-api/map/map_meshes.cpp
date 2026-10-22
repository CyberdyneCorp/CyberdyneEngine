// SPDX-License-Identifier: MIT
// map_meshes.cpp — the map's kit, built from primitives. See map_meshes.h.

#include "map_meshes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace sample::rts::map {
namespace {

using cy::Status;

constexpr f32 kTau = 6.28318531F;

[[nodiscard]] Vec3 add(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] Vec3 sub(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] Vec3 mul(Vec3 a, f32 s) noexcept {
    return Vec3{a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] Vec3 cross3(Vec3 a, Vec3 b) noexcept {
    return Vec3{(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
[[nodiscard]] f32 dot3(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
[[nodiscard]] Vec3 unit(Vec3 a) noexcept {
    const f32 length = std::sqrt(dot3(a, a));
    return length > 1e-12F ? mul(a, 1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
}
[[nodiscard]] Vec3 rotate_yaw(Vec3 v, f32 yaw) noexcept {
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    return Vec3{(c * v.x) + (s * v.z), v.y, (-s * v.x) + (c * v.z)};
}

/// A small deterministic hash: what makes a rock lumpy the same way on every machine.
[[nodiscard]] f32 hash_unit(u32 a, u32 b) noexcept {
    u32 h = (a * 0x9E37'79B1U) ^ (b * 0x85EB'CA77U);
    h ^= h >> 15U;
    h *= 0x2C1B'3C6DU;
    h ^= h >> 12U;
    h *= 0x297A'2D39U;
    h ^= h >> 15U;
    return static_cast<f32>(h & 0x00FF'FFFFU) / static_cast<f32>(0x0100'0000U);
}

/// The texture coordinate a face's point takes: a planar projection along the face's dominant
/// axis, in metres scaled to the repeat a texture is authored for.
[[nodiscard]] Vec2 planar_uv(Vec3 point, Vec3 normal) noexcept {
    constexpr f32 kRepeat = 0.5F;
    const f32 ax = std::fabs(normal.x);
    const f32 ay = std::fabs(normal.y);
    const f32 az = std::fabs(normal.z);
    if (ay >= ax && ay >= az) {
        return Vec2{point.x * kRepeat, point.z * kRepeat};
    }
    if (ax >= az) {
        return Vec2{point.z * kRepeat, -point.y * kRepeat};
    }
    return Vec2{point.x * kRepeat, -point.y * kRepeat};
}

/// Triangles collected per material, and turned into a mesh's parts at the end.
class Builder {
public:
    struct Vertex {
        Vec3 position;
        Vec3 normal;
        Vec2 uv;
        u8 joint = 0;
    };

    explicit Builder(const MapLayout& layout) noexcept : layout_(&layout) {}

    /// Subsequent triangles use material `name`. An unknown name is remembered and reported.
    void use(const char* name) {
        const u32 material = layout_->material(name);
        if (material == kNoMaterial) {
            missing_ = name;
            return;
        }
        current_ = slot_of(material);
    }
    void joint(u8 value) noexcept { joint_ = value; }
    /// Place subsequent primitives: a translation and a yaw within the mesh.
    void place(Vec3 origin, f32 yaw = 0.0F) noexcept {
        origin_ = origin;
        yaw_ = yaw;
    }

    /// A triangle facing away from `inside`: the winding is fixed here, so a primitive cannot be
    /// drawn inside out by the order it listed its corners in.
    void triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 inside) {
        const Vec3 wa = to_mesh(a);
        Vec3 wb = to_mesh(b);
        Vec3 wc = to_mesh(c);
        Vec3 normal = cross3(sub(wb, wa), sub(wc, wa));
        const Vec3 centroid = mul(add(add(wa, wb), wc), 1.0F / 3.0F);
        if (dot3(normal, sub(centroid, to_mesh(inside))) < 0.0F) {
            std::swap(wb, wc);
            normal = mul(normal, -1.0F);
        }
        normal = unit(normal);
        emit(wa, normal);
        emit(wb, normal);
        emit(wc, normal);
    }
    /// The same with each corner's own normal: a smooth surface.
    void smooth_triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 na, Vec3 nb, Vec3 nc) {
        Vec3 wa = to_mesh(a);
        Vec3 wb = to_mesh(b);
        Vec3 wc = to_mesh(c);
        Vec3 ra = rotate_yaw(na, yaw_);
        Vec3 rb = rotate_yaw(nb, yaw_);
        Vec3 rc = rotate_yaw(nc, yaw_);
        const Vec3 face = cross3(sub(wb, wa), sub(wc, wa));
        if (dot3(face, add(add(ra, rb), rc)) < 0.0F) {
            std::swap(wb, wc);
            std::swap(rb, rc);
        }
        emit(wa, unit(ra));
        emit(wb, unit(rb));
        emit(wc, unit(rc));
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 inside) {
        triangle(a, b, c, inside);
        triangle(a, c, d, inside);
    }

    /// An axis-aligned box about `centre`, turned by `yaw` about its own centre.
    void box(Vec3 centre, Vec3 half, f32 yaw = 0.0F) {
        Vec3 corner[8];
        for (u32 index = 0; index < 8U; ++index) {
            const Vec3 local{(index & 1U) != 0U ? half.x : -half.x,
                             (index & 2U) != 0U ? half.y : -half.y,
                             (index & 4U) != 0U ? half.z : -half.z};
            corner[index] = add(centre, rotate_yaw(local, yaw));
        }
        static constexpr u32 kFaces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                             {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
        for (const auto& face : kFaces) {
            quad(corner[face[0]], corner[face[1]], corner[face[2]], corner[face[3]], centre);
        }
    }

    /// A frustum of a cone from `base` up `height`, radius `bottom` to `top`, `sides` facets.
    void cylinder(Vec3 base, f32 bottom, f32 top, f32 height, u32 sides, bool caps = true,
                  f32 phase = 0.0F) {
        const Vec3 inside = add(base, Vec3{0.0F, height * 0.5F, 0.0F});
        const Vec3 apex = add(base, Vec3{0.0F, height, 0.0F});
        for (u32 side = 0; side < sides; ++side) {
            const f32 a0 = phase + (kTau * static_cast<f32>(side) / static_cast<f32>(sides));
            const f32 a1 = phase + (kTau * static_cast<f32>(side + 1U) / static_cast<f32>(sides));
            const Vec3 d0{std::cos(a0), 0.0F, std::sin(a0)};
            const Vec3 d1{std::cos(a1), 0.0F, std::sin(a1)};
            const Vec3 b0 = add(base, mul(d0, bottom));
            const Vec3 b1 = add(base, mul(d1, bottom));
            const Vec3 t0 = add(apex, mul(d0, top));
            const Vec3 t1 = add(apex, mul(d1, top));
            if (top > 1e-4F) {
                quad(b0, b1, t1, t0, inside);
                if (caps) {
                    triangle(apex, t0, t1, inside);
                }
            } else {
                triangle(b0, b1, apex, inside);
            }
            if (caps) {
                triangle(base, b1, b0, inside);
            }
        }
    }

    /// A gable roof over a `2 half_x` by `2 half_z` box whose eaves are at `base`, ridge along x,
    /// `height` above the eaves, overhanging by `eave`.
    void gable(Vec3 base, f32 half_x, f32 half_z, f32 height, f32 eave) {
        const f32 x = half_x + eave;
        const f32 z = half_z + eave;
        const Vec3 inside = add(base, Vec3{0.0F, height * 0.3F, 0.0F});
        const Vec3 ridge0 = add(base, Vec3{-x, height, 0.0F});
        const Vec3 ridge1 = add(base, Vec3{x, height, 0.0F});
        const Vec3 front0 = add(base, Vec3{-x, -eave * 0.4F, z});
        const Vec3 front1 = add(base, Vec3{x, -eave * 0.4F, z});
        const Vec3 back0 = add(base, Vec3{-x, -eave * 0.4F, -z});
        const Vec3 back1 = add(base, Vec3{x, -eave * 0.4F, -z});
        quad(front0, front1, ridge1, ridge0, inside);
        quad(back0, back1, ridge1, ridge0, inside);
        // The roof's underside, so it reads as a slab from below and casts from both faces.
        const Vec3 under = add(base, Vec3{0.0F, height * 2.0F, 0.0F});
        quad(front0, front1, back1, back0, under);
        // Gable ends, flush with the walls.
        triangle(add(base, Vec3{-half_x, 0.0F, half_z}), add(base, Vec3{-half_x, 0.0F, -half_z}),
                 add(base, Vec3{-half_x, height, 0.0F}),
                 add(base, Vec3{0.0F, height * 0.3F, 0.0F}));
        triangle(add(base, Vec3{half_x, 0.0F, half_z}), add(base, Vec3{half_x, 0.0F, -half_z}),
                 add(base, Vec3{half_x, height, 0.0F}), add(base, Vec3{0.0F, height * 0.3F, 0.0F}));
    }

    /// A four-sided pyramid roof over a square, eaves at `base`.
    void pyramid(Vec3 base, f32 half, f32 height) {
        const Vec3 apex = add(base, Vec3{0.0F, height, 0.0F});
        const Vec3 inside = add(base, Vec3{0.0F, height * 0.3F, 0.0F});
        const Vec3 c[4] = {add(base, Vec3{-half, 0.0F, -half}), add(base, Vec3{half, 0.0F, -half}),
                           add(base, Vec3{half, 0.0F, half}), add(base, Vec3{-half, 0.0F, half})};
        for (u32 side = 0; side < 4U; ++side) {
            triangle(c[side], c[(side + 1U) % 4U], apex, inside);
        }
        quad(c[0], c[1], c[2], c[3], apex);
    }

    /// A lumpy sphere: an octahedron subdivided `levels` times, each vertex pushed by a hash.
    /// `smooth` gives it the sphere's normals — a crown of leaves — rather than facets — a rock.
    void blob(Vec3 centre, Vec3 radius, u32 seed, f32 lumps, u32 levels, bool smooth) {
        std::vector<Vec3> triangles = {
            Vec3{1, 0, 0},  Vec3{0, 1, 0},  Vec3{0, 0, 1},  Vec3{0, 0, 1},  Vec3{0, 1, 0},
            Vec3{-1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0},  Vec3{0, 0, -1}, Vec3{0, 0, -1},
            Vec3{0, 1, 0},  Vec3{1, 0, 0},  Vec3{1, 0, 0},  Vec3{0, 0, 1},  Vec3{0, -1, 0},
            Vec3{0, 0, 1},  Vec3{-1, 0, 0}, Vec3{0, -1, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, -1},
            Vec3{0, -1, 0}, Vec3{0, 0, -1}, Vec3{1, 0, 0},  Vec3{0, -1, 0}};
        for (u32 level = 0; level < levels; ++level) {
            std::vector<Vec3> finer;
            finer.reserve(triangles.size() * 4U);
            for (std::size_t at = 0; at + 2 < triangles.size(); at += 3) {
                const Vec3 a = triangles[at];
                const Vec3 b = triangles[at + 1];
                const Vec3 c = triangles[at + 2];
                const Vec3 ab = unit(add(a, b));
                const Vec3 bc = unit(add(b, c));
                const Vec3 ca = unit(add(c, a));
                const Vec3 next[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
                finer.insert(finer.end(), next, next + 12);
            }
            triangles = std::move(finer);
        }
        const auto displaced = [&](Vec3 direction) {
            // Keyed by the direction's quantised components, so a vertex two triangles share
            // moves the same way in both and the surface stays closed.
            const auto key = [](f32 value) {
                return static_cast<u32>(static_cast<std::int32_t>(std::lround(value * 4096.0F)));
            };
            const f32 push =
                1.0F + (lumps * (hash_unit(key(direction.x) ^ (seed * 7919U),
                                           key(direction.y) + (key(direction.z) * 31U)) -
                                 0.5F));
            return add(centre, Vec3{direction.x * radius.x * push, direction.y * radius.y * push,
                                    direction.z * radius.z * push});
        };
        for (std::size_t at = 0; at + 2 < triangles.size(); at += 3) {
            const Vec3 a = displaced(triangles[at]);
            const Vec3 b = displaced(triangles[at + 1]);
            const Vec3 c = displaced(triangles[at + 2]);
            if (smooth) {
                smooth_triangle(a, b, c, triangles[at], triangles[at + 1], triangles[at + 2]);
            } else {
                triangle(a, b, c, centre);
            }
        }
    }

    /// Hand the collected triangles to `library` as mesh `name`, a part a material.
    void finish_mesh(const char* name, MeshLibrary& library) {
        MeshRange mesh;
        mesh.name = name;
        mesh.first_vertex = static_cast<u32>(library.positions.size());
        mesh.first_part = static_cast<u32>(library.parts.size());
        Vec3 low{1e9F, 1e9F, 1e9F};
        Vec3 high{-1e9F, -1e9F, -1e9F};
        u32 local = 0;
        for (const Batch& batch : batches_) {
            if (batch.vertices.empty()) {
                continue;
            }
            MeshPart part;
            part.first_index = static_cast<u32>(library.indices.size());
            part.index_count = static_cast<u32>(batch.vertices.size());
            part.material = batch.material;
            for (const Vertex& vertex : batch.vertices) {
                library.positions.push_back(vertex.position);
                library.normals.push_back(vertex.normal);
                library.uvs.push_back(vertex.uv);
                library.indices.push_back(local++);
                low = Vec3{std::min(low.x, vertex.position.x), std::min(low.y, vertex.position.y),
                           std::min(low.z, vertex.position.z)};
                high =
                    Vec3{std::max(high.x, vertex.position.x), std::max(high.y, vertex.position.y),
                         std::max(high.z, vertex.position.z)};
            }
            library.parts.push_back(part);
        }
        mesh.vertex_count = local;
        mesh.part_count = static_cast<u32>(library.parts.size()) - mesh.first_part;
        mesh.min = local > 0 ? low : Vec3{};
        mesh.max = local > 0 ? high : Vec3{};
        library.meshes.push_back(mesh);
        batches_.clear();
        current_ = 0;
        origin_ = Vec3{};
        yaw_ = 0.0F;
    }

    /// The same, into a skinned mesh.
    void finish_skinned(SkinnedMesh& out) {
        for (const Batch& batch : batches_) {
            if (batch.vertices.empty()) {
                continue;
            }
            MeshPart part;
            part.first_index = static_cast<u32>(out.indices.size());
            part.index_count = static_cast<u32>(batch.vertices.size());
            part.material = batch.material;
            for (const Vertex& vertex : batch.vertices) {
                out.indices.push_back(static_cast<u32>(out.positions.size()));
                out.positions.push_back(vertex.position);
                out.normals.push_back(vertex.normal);
                out.uvs.push_back(vertex.uv);
                out.joints.push_back(vertex.joint);
            }
            out.parts.push_back(part);
        }
        batches_.clear();
    }

    [[nodiscard]] Status status() const noexcept {
        if (missing_ != nullptr) {
            return cy::fail(cy::ErrorCode::NotFound,
                            "a map mesh uses a material the source does not declare");
        }
        return cy::ok();
    }
    [[nodiscard]] const char* missing() const noexcept { return missing_; }

private:
    struct Batch {
        u32 material = 0;
        std::vector<Vertex> vertices;
    };

    [[nodiscard]] Vec3 to_mesh(Vec3 local) const noexcept {
        return add(origin_, rotate_yaw(local, yaw_));
    }
    [[nodiscard]] u32 slot_of(u32 material) {
        for (std::size_t index = 0; index < batches_.size(); ++index) {
            if (batches_[index].material == material) {
                return static_cast<u32>(index);
            }
        }
        batches_.push_back(Batch{material, {}});
        return static_cast<u32>(batches_.size() - 1);
    }
    void emit(Vec3 position, Vec3 normal) {
        if (batches_.empty()) {
            use("plaster");
            if (batches_.empty()) {
                return;
            }
        }
        batches_[current_].vertices.push_back(
            Vertex{position, normal, planar_uv(position, normal), joint_});
    }

    const MapLayout* layout_;
    std::vector<Batch> batches_;
    u32 current_ = 0;
    u8 joint_ = 0;
    Vec3 origin_{0.0F, 0.0F, 0.0F};
    f32 yaw_ = 0.0F;
    const char* missing_ = nullptr;
};

// --- The buildings -------------------------------------------------------------------------------

/// A banner on a pole: a team-coloured cloth beside a wooden staff, standing at `foot`.
void banner_at(Builder& b, Vec3 foot, f32 height) {
    b.use("wood_dark");
    b.cylinder(foot, 0.06F, 0.05F, height, 6);
    b.use("team");
    b.box(add(foot, Vec3{0.32F, height - 0.55F, 0.0F}), Vec3{0.28F, 0.42F, 0.025F});
    b.use("iron");
    b.cylinder(add(foot, Vec3{0.0F, height, 0.0F}), 0.08F, 0.0F, 0.18F, 6);
}

/// The barracks: the GI's 6 x 3 x 4 m box (host/illumination.cpp), as stone walls, a timber
/// band, a gable roof and a door facing the spawn point.
void barracks(Builder& b) {
    b.use("stone_wall");
    b.box(Vec3{0.0F, 0.9F, 0.0F}, Vec3{3.0F, 0.9F, 2.0F});
    b.use("plaster");
    b.box(Vec3{0.0F, 2.15F, 0.0F}, Vec3{2.95F, 0.35F, 1.95F});
    b.use("wood_dark");
    b.box(Vec3{0.0F, 1.82F, 0.0F}, Vec3{3.04F, 0.06F, 2.04F});
    for (const f32 x : {-2.9F, -1.0F, 1.0F, 2.9F}) {
        b.box(Vec3{x, 2.15F, 1.97F}, Vec3{0.08F, 0.38F, 0.04F});
        b.box(Vec3{x, 2.15F, -1.97F}, Vec3{0.08F, 0.38F, 0.04F});
    }
    b.use("roof_wood");
    b.gable(Vec3{0.0F, 2.5F, 0.0F}, 3.0F, 2.0F, 1.5F, 0.35F);
    b.use("wood_dark");
    b.box(Vec3{0.0F, 0.75F, -2.02F}, Vec3{0.55F, 0.75F, 0.05F});
    b.box(Vec3{-1.9F, 1.2F, -2.02F}, Vec3{0.35F, 0.3F, 0.04F});
    b.box(Vec3{1.9F, 1.2F, -2.02F}, Vec3{0.35F, 0.3F, 0.04F});
    banner_at(b, Vec3{3.4F, 0.0F, -2.3F}, 3.6F);
    banner_at(b, Vec3{-3.4F, 0.0F, -2.3F}, 3.6F);
}

/// The keep: a stone hall with a copper roof and a round tower at one corner.
void keep(Builder& b) {
    b.use("stone_wall");
    b.box(Vec3{0.0F, 2.0F, 0.0F}, Vec3{4.0F, 2.0F, 3.2F});
    for (const f32 x : {-3.6F, -1.2F, 1.2F, 3.6F}) {
        b.box(Vec3{x, 4.25F, 3.0F}, Vec3{0.35F, 0.25F, 0.25F});
        b.box(Vec3{x, 4.25F, -3.0F}, Vec3{0.35F, 0.25F, 0.25F});
    }
    b.use("roof_copper");
    b.gable(Vec3{0.0F, 4.0F, 0.0F}, 4.0F, 3.2F, 2.2F, 0.3F);
    b.use("stone_wall");
    b.cylinder(Vec3{3.8F, 0.0F, 3.0F}, 1.5F, 1.35F, 8.0F, 10);
    b.use("roof_copper");
    b.cylinder(Vec3{3.8F, 8.0F, 3.0F}, 1.75F, 0.0F, 2.6F, 10);
    b.use("wood_dark");
    b.box(Vec3{-4.05F, 1.1F, 0.0F}, Vec3{0.06F, 1.1F, 0.8F});
    for (const f32 z : {-2.2F, 2.2F}) {
        b.box(Vec3{-4.05F, 2.6F, z}, Vec3{0.05F, 0.45F, 0.35F});
    }
    banner_at(b, Vec3{3.8F, 10.4F, 3.0F}, 2.2F);
    banner_at(b, Vec3{-4.6F, 0.0F, 2.2F}, 4.0F);
    banner_at(b, Vec3{-4.6F, 0.0F, -2.2F}, 4.0F);
}

void house(Builder& b) {
    b.use("stone_wall");
    b.box(Vec3{0.0F, 0.3F, 0.0F}, Vec3{2.1F, 0.3F, 1.6F});
    b.use("plaster");
    b.box(Vec3{0.0F, 1.45F, 0.0F}, Vec3{2.0F, 0.85F, 1.5F});
    b.use("wood_dark");
    for (const f32 x : {-1.95F, 1.95F}) {
        for (const f32 z : {-1.45F, 1.45F}) {
            b.box(Vec3{x, 1.45F, z}, Vec3{0.08F, 0.9F, 0.08F});
        }
    }
    b.box(Vec3{0.0F, 0.85F, 1.53F}, Vec3{0.45F, 0.85F, 0.04F});
    b.box(Vec3{1.2F, 1.5F, 1.53F}, Vec3{0.3F, 0.3F, 0.03F});
    b.use("thatch");
    b.gable(Vec3{0.0F, 2.3F, 0.0F}, 2.0F, 1.5F, 1.4F, 0.4F);
    b.use("stone_wall");
    b.box(Vec3{-1.3F, 3.4F, -0.6F}, Vec3{0.25F, 0.6F, 0.25F});
}

void well(Builder& b) {
    b.use("stone_wall");
    b.cylinder(Vec3{0.0F, 0.0F, 0.0F}, 0.9F, 0.9F, 0.8F, 10);
    b.use("water");
    b.cylinder(Vec3{0.0F, 0.6F, 0.0F}, 0.7F, 0.7F, 0.1F, 10);
    b.use("wood_dark");
    b.box(Vec3{-0.8F, 1.4F, 0.0F}, Vec3{0.07F, 0.7F, 0.07F});
    b.box(Vec3{0.8F, 1.4F, 0.0F}, Vec3{0.07F, 0.7F, 0.07F});
    b.use("roof_wood");
    b.gable(Vec3{0.0F, 2.1F, 0.0F}, 0.9F, 0.6F, 0.6F, 0.15F);
}

/// Red's watchtower: four legs, a platform with a rail, a roof. The fog of war's 1.6 x 3.6 m box.
void watchtower(Builder& b) {
    b.use("wood_dark");
    for (const f32 x : {-0.65F, 0.65F}) {
        for (const f32 z : {-0.65F, 0.65F}) {
            b.box(Vec3{x, 1.5F, z}, Vec3{0.08F, 1.5F, 0.08F});
        }
    }
    b.box(Vec3{0.0F, 0.9F, 0.0F}, Vec3{0.7F, 0.04F, 0.04F}, 0.0F);
    b.use("wood");
    b.box(Vec3{0.0F, 2.6F, 0.0F}, Vec3{0.85F, 0.06F, 0.85F});
    for (const f32 side : {-0.8F, 0.8F}) {
        b.box(Vec3{side, 2.95F, 0.0F}, Vec3{0.04F, 0.3F, 0.85F});
        b.box(Vec3{0.0F, 2.95F, side}, Vec3{0.85F, 0.3F, 0.04F});
    }
    b.use("thatch");
    b.pyramid(Vec3{0.0F, 3.35F, 0.0F}, 1.0F, 0.9F);
    banner_at(b, Vec3{0.0F, 4.2F, 0.0F}, 1.2F);
}

/// The beacon: the GI's glowing 1.2 x 2 x 1.2 m glass, on a plinth, under an iron cap.
void beacon(Builder& b) {
    b.use("stone_wall");
    b.box(Vec3{0.0F, 0.15F, 0.0F}, Vec3{0.75F, 0.15F, 0.75F});
    b.use("beacon");
    b.box(Vec3{0.0F, 1.05F, 0.0F}, Vec3{0.55F, 0.75F, 0.55F});
    b.use("iron");
    for (const f32 x : {-0.6F, 0.6F}) {
        for (const f32 z : {-0.6F, 0.6F}) {
            b.box(Vec3{x, 1.0F, z}, Vec3{0.05F, 0.85F, 0.05F});
        }
    }
    b.pyramid(Vec3{0.0F, 1.85F, 0.0F}, 0.7F, 0.35F);
}

/// The scout's crate: a metre cube of boards with a darker frame.
void crate(Builder& b) {
    b.use("wood");
    b.box(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.47F, 0.47F, 0.47F});
    b.use("wood_dark");
    for (const f32 a : {-0.47F, 0.47F}) {
        for (const f32 c : {-0.47F, 0.47F}) {
            b.box(Vec3{a, 0.0F, c}, Vec3{0.05F, 0.5F, 0.05F});
            b.box(Vec3{a, c, 0.0F}, Vec3{0.05F, 0.05F, 0.5F});
            b.box(Vec3{0.0F, a, c}, Vec3{0.5F, 0.05F, 0.05F});
        }
    }
}

void tent(Builder& b) {
    b.use("team");
    b.gable(Vec3{0.0F, 0.0F, 0.0F}, 1.3F, 1.0F, 1.5F, 0.05F);
    b.use("wood_dark");
    b.cylinder(Vec3{-1.35F, 0.0F, 0.0F}, 0.04F, 0.04F, 1.7F, 5);
    b.cylinder(Vec3{1.35F, 0.0F, 0.0F}, 0.04F, 0.04F, 1.7F, 5);
}

void fence(Builder& b) {
    b.use("wood");
    b.box(Vec3{-1.0F, 0.5F, 0.0F}, Vec3{0.06F, 0.5F, 0.06F});
    b.box(Vec3{1.0F, 0.5F, 0.0F}, Vec3{0.06F, 0.5F, 0.06F});
    b.box(Vec3{0.0F, 0.7F, 0.0F}, Vec3{1.05F, 0.04F, 0.03F});
    b.box(Vec3{0.0F, 0.35F, 0.0F}, Vec3{1.05F, 0.04F, 0.03F});
}

/// Rows of a crop over the farmland the source paints.
void crops(Builder& b) {
    b.use("crop");
    for (u32 row = 0; row < 7U; ++row) {
        const f32 x = -3.0F + static_cast<f32>(row);
        for (u32 tuft = 0; tuft < 11U; ++tuft) {
            const f32 z = -5.5F + (static_cast<f32>(tuft) * 1.1F);
            const f32 h = 0.35F + (0.2F * hash_unit(row, tuft));
            b.box(Vec3{x, h * 0.5F, z}, Vec3{0.22F, h * 0.5F, 0.45F},
                  0.3F * (hash_unit(tuft, row) - 0.5F));
        }
    }
}

void dock(Builder& b) {
    b.use("wood");
    b.box(Vec3{-3.0F, 0.05F, 0.0F}, Vec3{3.5F, 0.08F, 0.9F});
    b.use("wood_dark");
    for (const f32 x : {-6.2F, -4.0F, -1.8F, 0.4F}) {
        for (const f32 z : {-0.85F, 0.85F}) {
            b.cylinder(Vec3{x, -1.6F, z}, 0.1F, 0.1F, 2.0F, 6);
        }
    }
}

// --- Resources ----------------------------------------------------------------------------------

void gold_crystals(Builder& b, Vec3 at, u32 seed, f32 size) {
    b.use("gold");
    for (u32 index = 0; index < 5U; ++index) {
        const f32 angle = kTau * hash_unit(seed, index);
        const f32 reach = size * (0.2F + (0.6F * hash_unit(index, seed)));
        const Vec3 foot = add(at, Vec3{std::cos(angle) * reach, 0.0F, std::sin(angle) * reach});
        b.cylinder(foot, size * 0.18F, 0.0F, size * (0.6F + hash_unit(seed + index, 3)), 5, true,
                   angle);
    }
}

void gold_mine(Builder& b) {
    b.use("rock");
    b.blob(Vec3{0.0F, 0.6F, 0.0F}, Vec3{2.6F, 1.5F, 2.2F}, 11, 0.35F, 2, false);
    b.blob(Vec3{1.8F, 0.3F, 1.2F}, Vec3{1.3F, 0.9F, 1.2F}, 12, 0.4F, 2, false);
    b.blob(Vec3{-1.6F, 0.2F, 1.4F}, Vec3{1.1F, 0.7F, 1.0F}, 13, 0.4F, 1, false);
    gold_crystals(b, Vec3{0.4F, 1.6F, 0.6F}, 21, 1.1F);
    gold_crystals(b, Vec3{1.8F, 0.8F, 1.8F}, 22, 0.8F);
    gold_crystals(b, Vec3{-1.2F, 0.4F, 2.2F}, 23, 0.7F);
    b.use("wood_dark");
    b.box(Vec3{-0.6F, 0.9F, 2.1F}, Vec3{0.08F, 0.9F, 0.08F});
    b.box(Vec3{0.6F, 0.9F, 2.1F}, Vec3{0.08F, 0.9F, 0.08F});
    b.box(Vec3{0.0F, 1.8F, 2.1F}, Vec3{0.75F, 0.08F, 0.1F});
}

void gold_rock(Builder& b) {
    b.use("rock");
    b.blob(Vec3{0.0F, 0.4F, 0.0F}, Vec3{1.2F, 0.8F, 1.0F}, 31, 0.4F, 2, false);
    gold_crystals(b, Vec3{0.0F, 0.8F, 0.2F}, 32, 0.7F);
}

void rock_pile(Builder& b) {
    b.use("rock");
    b.blob(Vec3{0.0F, 0.5F, 0.0F}, Vec3{1.8F, 1.2F, 1.5F}, 41, 0.35F, 2, false);
    b.blob(Vec3{1.6F, 0.3F, -0.8F}, Vec3{0.9F, 0.6F, 0.8F}, 42, 0.4F, 1, false);
    b.blob(Vec3{-1.2F, 0.25F, 1.1F}, Vec3{0.8F, 0.55F, 0.7F}, 43, 0.4F, 1, false);
    b.use("stone_wall");
    for (u32 index = 0; index < 4U; ++index) {
        b.box(Vec3{-2.4F + (0.5F * static_cast<f32>(index)), 0.18F, -1.8F},
              Vec3{0.22F, 0.18F, 0.3F}, 0.2F * static_cast<f32>(index));
    }
}

void log_pile(Builder& b) {
    b.use("bark");
    for (u32 layer = 0; layer < 3U; ++layer) {
        for (u32 log = 0; log + layer < 4U; ++log) {
            const f32 z =
                (-0.75F + (0.5F * static_cast<f32>(log))) + (0.25F * static_cast<f32>(layer));
            const f32 y = 0.22F + (0.4F * static_cast<f32>(layer));
            b.place(Vec3{-1.6F, y, z}, 0.0F);
            // A log lies along x: a cylinder turned onto its side, faked as a box with bevels.
            b.box(Vec3{1.6F, 0.0F, 0.0F}, Vec3{1.6F, 0.2F, 0.2F});
            b.place(Vec3{0.0F, 0.0F, 0.0F}, 0.0F);
        }
    }
    b.use("wood");
    b.box(Vec3{-1.62F, 0.62F, 0.0F}, Vec3{0.02F, 0.6F, 0.9F});
    b.box(Vec3{1.62F, 0.62F, 0.0F}, Vec3{0.02F, 0.6F, 0.9F});
}

void outcrop(Builder& b) {
    b.use("rock");
    b.blob(Vec3{0.0F, 1.0F, 0.0F}, Vec3{3.2F, 2.2F, 3.0F}, 51, 0.35F, 2, false);
    b.blob(Vec3{2.0F, 0.5F, 2.0F}, Vec3{1.4F, 1.0F, 1.3F}, 52, 0.4F, 2, false);
    b.blob(Vec3{-2.2F, 0.6F, -1.6F}, Vec3{1.5F, 1.3F, 1.4F}, 53, 0.4F, 2, false);
}

// --- Plants -------------------------------------------------------------------------------------

void boulder(Builder& b) {
    b.use("rock");
    b.blob(Vec3{0.0F, 0.3F, 0.0F}, Vec3{0.75F, 0.55F, 0.65F}, 61, 0.45F, 1, false);
}

void tree_pine(Builder& b) {
    b.use("bark");
    b.cylinder(Vec3{0.0F, -0.2F, 0.0F}, 0.17F, 0.12F, 1.6F, 6);
    b.use("leaves_pine");
    b.cylinder(Vec3{0.0F, 0.9F, 0.0F}, 1.25F, 0.0F, 2.1F, 8, true, 0.2F);
    b.cylinder(Vec3{0.0F, 1.9F, 0.0F}, 0.95F, 0.0F, 1.9F, 8, true, 0.6F);
    b.cylinder(Vec3{0.0F, 2.9F, 0.0F}, 0.62F, 0.0F, 1.6F, 7, true, 1.0F);
}

void tree_oak(Builder& b) {
    b.use("bark");
    b.cylinder(Vec3{0.0F, -0.2F, 0.0F}, 0.24F, 0.16F, 2.4F, 7);
    b.box(Vec3{0.35F, 1.9F, 0.0F}, Vec3{0.4F, 0.08F, 0.08F}, 0.4F);
    b.use("leaves_oak");
    b.blob(Vec3{0.0F, 3.0F, 0.0F}, Vec3{1.5F, 1.2F, 1.5F}, 71, 0.25F, 2, true);
    b.blob(Vec3{0.8F, 2.6F, 0.5F}, Vec3{1.0F, 0.85F, 1.0F}, 72, 0.25F, 1, true);
    b.blob(Vec3{-0.7F, 2.5F, -0.6F}, Vec3{1.05F, 0.9F, 1.0F}, 73, 0.25F, 1, true);
}

void bush(Builder& b) {
    b.use("bush");
    b.blob(Vec3{0.0F, 0.35F, 0.0F}, Vec3{0.65F, 0.5F, 0.6F}, 81, 0.3F, 1, true);
    b.blob(Vec3{0.45F, 0.25F, 0.3F}, Vec3{0.45F, 0.35F, 0.45F}, 82, 0.3F, 1, true);
}

/// The lake's surface: a quad a terrain cell over every cell any of whose corners is below the
/// water, flat at the placement's height, in the placement's own frame.
void water_plane(Builder& b, const TerrainData& terrain, f32 level, Vec3 origin) {
    b.use("water");
    if (terrain.edge < 2) {
        return;
    }
    const u32 quads = terrain.edge - 1;
    for (u32 j = 0; j < quads; ++j) {
        for (u32 i = 0; i < quads; ++i) {
            const f32 lowest = std::min({terrain.sample(i, j), terrain.sample(i + 1, j),
                                         terrain.sample(i, j + 1), terrain.sample(i + 1, j + 1)});
            if (lowest >= level) {
                continue;
            }
            const f32 x0 = terrain.origin_x + (static_cast<f32>(i) * terrain.spacing) - origin.x;
            const f32 z0 = terrain.origin_z + (static_cast<f32>(j) * terrain.spacing) - origin.z;
            const f32 x1 = x0 + terrain.spacing;
            const f32 z1 = z0 + terrain.spacing;
            b.quad(Vec3{x0, 0.0F, z0}, Vec3{x1, 0.0F, z0}, Vec3{x1, 0.0F, z1}, Vec3{x0, 0.0F, z1},
                   Vec3{(x0 + x1) * 0.5F, -1.0F, (z0 + z1) * 0.5F});
        }
    }
}

// --- The worker ---------------------------------------------------------------------------------

enum WorkerJoint : u8 { kRoot, kHips, kSpine, kHead, kArmLeft, kArmRight, kLegLeft, kLegRight };

void worker(Builder& b) {
    // Legs, from the hip joints down, each turned whole by its joint.
    for (const auto& [joint, x] : {std::pair{kLegLeft, -0.12F}, std::pair{kLegRight, 0.12F}}) {
        b.joint(joint);
        b.use("trousers");
        b.box(Vec3{x, 0.5F, 0.0F}, Vec3{0.085F, 0.4F, 0.1F});
        b.use("wood_dark");
        b.box(Vec3{x, 0.06F, 0.05F}, Vec3{0.09F, 0.06F, 0.15F});
    }
    // The body: the pelvis on the hips, the chest on the spine.
    b.joint(kHips);
    b.use("trousers");
    b.box(Vec3{0.0F, 0.98F, 0.0F}, Vec3{0.2F, 0.12F, 0.12F});
    b.use("wood_dark");
    b.box(Vec3{0.0F, 1.11F, 0.0F}, Vec3{0.215F, 0.04F, 0.135F});
    b.joint(kSpine);
    b.use("team");
    b.box(Vec3{0.0F, 1.36F, 0.0F}, Vec3{0.21F, 0.22F, 0.13F});
    b.use("cloth");
    b.box(Vec3{0.0F, 1.5F, 0.125F}, Vec3{0.09F, 0.07F, 0.02F});
    // Arms hang from the shoulders, a hand at each end.
    for (const auto& [joint, x] : {std::pair{kArmLeft, -0.28F}, std::pair{kArmRight, 0.28F}}) {
        b.joint(joint);
        b.use("team");
        b.box(Vec3{x, 1.36F, 0.0F}, Vec3{0.065F, 0.24F, 0.07F});
        b.use("skin");
        b.box(Vec3{x, 1.05F, 0.0F}, Vec3{0.055F, 0.07F, 0.06F});
    }
    // A tool in the right hand.
    b.joint(kArmRight);
    b.use("wood");
    b.box(Vec3{0.28F, 1.05F, 0.22F}, Vec3{0.025F, 0.025F, 0.25F});
    b.use("iron");
    b.box(Vec3{0.28F, 1.05F, 0.46F}, Vec3{0.02F, 0.12F, 0.03F});
    // The head, and a straw hat.
    b.joint(kHead);
    b.use("skin");
    b.box(Vec3{0.0F, 1.79F, 0.0F}, Vec3{0.12F, 0.13F, 0.12F});
    b.use("straw");
    b.cylinder(Vec3{0.0F, 1.9F, 0.0F}, 0.24F, 0.0F, 0.2F, 8);
}

using Make = void (*)(Builder&);

struct Recipe {
    const char* name;
    Make make;
};

constexpr Recipe kRecipes[] = {
    {"barracks", &barracks},
    {"keep", &keep},
    {"house", &house},
    {"well", &well},
    {"watchtower", &watchtower},
    {"beacon", &beacon},
    {"crate", &crate},
    {"tent", &tent},
    {"fence", &fence},
    {"crops", &crops},
    {"dock", &dock},
    {"gold_mine", &gold_mine},
    {"gold_rock", &gold_rock},
    {"rock_pile", &rock_pile},
    {"log_pile", &log_pile},
    {"outcrop", &outcrop},
    {"boulder", &boulder},
    {"tree_pine", &tree_pine},
    {"tree_oak", &tree_oak},
    {"bush", &bush},
};

}  // namespace

Status build_mesh_library(const MapLayout& layout, const TerrainData& terrain,
                          const Vec3& water_origin, MeshLibrary& out) {
    out = MeshLibrary{};
    Builder builder(layout);
    for (const char* name : kLibraryMeshes) {
        const std::string wanted = name;
        if (wanted == "banner") {
            banner_at(builder, Vec3{0.0F, 0.0F, 0.0F}, 4.2F);
        } else if (wanted == "water_plane") {
            water_plane(builder, terrain, layout.water_level, water_origin);
        } else {
            for (const Recipe& recipe : kRecipes) {
                if (wanted == recipe.name) {
                    recipe.make(builder);
                }
            }
        }
        builder.finish_mesh(name, out);
        if (Status made = builder.status(); !made) {
            return made;
        }
    }
    return cy::ok();
}

Status build_worker(const MapLayout& layout, SkinnedMesh& out) {
    out = SkinnedMesh{};
    out.joint_count = kWorkerJointCount;
    Builder builder(layout);
    worker(builder);
    builder.finish_skinned(out);
    return builder.status();
}

}  // namespace sample::rts::map
