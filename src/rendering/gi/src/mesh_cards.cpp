// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/mesh_cards.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
};

[[nodiscard]] Triangle triangle_at(Span<const Vec3> positions, Span<const u32> indices,
                                   usize first) noexcept {
    return Triangle{positions[indices[first]], positions[indices[first + 1]],
                    positions[indices[first + 2]]};
}

/// The point of a triangle nearest `p` (Ericson, "Real-Time Collision Detection", 5.1.5).
[[nodiscard]] Vec3 closest_on_triangle(Vec3 p, const Triangle& t) noexcept {
    const Vec3 ab = t.b - t.a;
    const Vec3 ac = t.c - t.a;
    const Vec3 ap = p - t.a;
    const f32 d1 = dot(ab, ap);
    const f32 d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return t.a;
    }
    const Vec3 bp = p - t.b;
    const f32 d3 = dot(ab, bp);
    const f32 d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return t.b;
    }
    const f32 vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return t.a + (ab * (d1 / (d1 - d3)));
    }
    const Vec3 cp = p - t.c;
    const f32 d5 = dot(ab, cp);
    const f32 d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return t.c;
    }
    const f32 vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return t.a + (ac * (d2 / (d2 - d6)));
    }
    const f32 va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        return t.b + ((t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }
    const f32 denominator = 1.0F / (va + vb + vc);
    return t.a + (ab * (vb * denominator)) + (ac * (vc * denominator));
}

/// Whether a ray from `origin` along `direction` crosses the triangle (Moller-Trumbore).
[[nodiscard]] bool crosses(Vec3 origin, Vec3 direction, const Triangle& t) noexcept {
    const Vec3 ab = t.b - t.a;
    const Vec3 ac = t.c - t.a;
    const Vec3 p = cross(direction, ac);
    const f32 determinant = dot(ab, p);
    if (std::abs(determinant) < 1.0e-12F) {
        return false;
    }
    const f32 inverse = 1.0F / determinant;
    const Vec3 s = origin - t.a;
    const f32 u = dot(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const Vec3 q = cross(s, ab);
    const f32 v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    return dot(ac, q) * inverse > 0.0F;
}

/// A direction no axis-aligned mesh edge lies along, so a parity ray does not graze one.
constexpr Vec3 kParityDirection{0.5719F, 0.6181F, 0.5390F};

/// How many pieces a cell's edge is cut into, at the least: enough that a cell's card is the mean
/// of several pieces of each triangle crossing it rather than of one centroid.
constexpr f32 kPiecesPerCell = 3.0F;

/// One piece of a cut triangle: where it is, which way it faces, how much surface it is, and the
/// card it pools into.
struct Piece {
    u64 key = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    f32 area = 0.0F;
};

/// The facing a normal pools under: its dominant axis, signed — six, so the two faces of a thin
/// wall in one cell are two cards and not one that faces neither way.
[[nodiscard]] u64 facing_of(Vec3 normal) noexcept {
    const f32 ax = std::abs(normal.x);
    const f32 ay = std::abs(normal.y);
    const f32 az = std::abs(normal.z);
    if (ax >= ay && ax >= az) {
        return normal.x >= 0.0F ? 0U : 1U;
    }
    if (ay >= az) {
        return normal.y >= 0.0F ? 2U : 3U;
    }
    return normal.z >= 0.0F ? 4U : 5U;
}

/// The pool a piece at `position` facing `facing` belongs to: its cell's coordinates, 19 bits
/// each about a bias, and the facing.
[[nodiscard]] u64 pool_key(Vec3 position, f32 spacing, u64 facing) noexcept {
    constexpr i64 kBias = 1 << 18;
    const auto cell = [spacing](f32 value) noexcept {
        const i64 index = static_cast<i64>(std::floor(value / spacing)) + kBias;
        return static_cast<u64>(std::clamp<i64>(index, 0, (2 * kBias) - 1));
    };
    return (cell(position.x) << 41U) | (cell(position.y) << 22U) | (cell(position.z) << 3U) |
           facing;
}

/// Cut `world` into pieces no longer than a third of the spacing, and keep those `settings`'
/// region admits.
[[nodiscard]] Status cut_triangle(const Triangle& world, const MeshCardSettings& settings,
                                  Array<Piece>& pieces) noexcept {
    const Vec3 winding = cross(world.b - world.a, world.c - world.a);
    const f32 twice_area = length(winding);
    if (!(twice_area > 1.0e-10F)) {
        return ok();
    }
    const Vec3 normal = winding / twice_area;
    const u64 facing = facing_of(normal);
    const f32 longest =
        std::max({length(world.b - world.a), length(world.c - world.b), length(world.a - world.c)});
    const u32 steps = std::max(
        1U, static_cast<u32>(std::ceil(longest * kPiecesPerCell / settings.spacing_metres)));
    const f32 area = 0.5F * twice_area / static_cast<f32>(steps * steps);
    const f32 inverse_steps = 1.0F / static_cast<f32>(steps);
    // The centroids of the subdivision's triangles: per cell (a, b), the upward one and — but on
    // the diagonal — the downward one.
    for (u32 a = 0; a < steps; ++a) {
        for (u32 b = 0; a + b < steps; ++b) {
            const u32 count = a + b + 1U < steps ? 2U : 1U;
            for (u32 which = 0; which < count; ++which) {
                const f32 offset = which == 0U ? (1.0F / 3.0F) : (2.0F / 3.0F);
                const f32 fa = (static_cast<f32>(a) + offset) * inverse_steps;
                const f32 fb = (static_cast<f32>(b) + offset) * inverse_steps;
                const Vec3 position =
                    world.a + ((world.b - world.a) * fa) + ((world.c - world.a) * fb);
                if (settings.clip_to_region && !settings.region.contains(position)) {
                    continue;
                }
                Piece piece;
                piece.key = pool_key(position, settings.spacing_metres, facing);
                piece.position = position;
                piece.normal = normal;
                piece.area = area;
                if (Status pushed = pieces.push_back(piece); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

}  // namespace

Status append_mesh_cards(Span<const Vec3> positions, Span<const u32> indices, const Mat4& transform,
                         const MeshCardSettings& settings, Array<Surfel>& out) noexcept {
    if (indices.size() % 3 != 0 || !(settings.spacing_metres > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "append_mesh_cards: whole triangles and a positive spacing");
    }
    for (const u32 index : indices) {
        if (index >= positions.size()) {
            return fail(ErrorCode::OutOfRange, "append_mesh_cards: an index past the positions");
        }
    }
    Array<Piece> pieces(out.allocator());
    for (usize first = 0; first < indices.size(); first += 3) {
        const Triangle local = triangle_at(positions, indices, first);
        const Triangle world{transform_point(transform, local.a),
                             transform_point(transform, local.b),
                             transform_point(transform, local.c)};
        if (Status cut = cut_triangle(world, settings, pieces); !cut) {
            return cut;
        }
    }
    std::ranges::sort(pieces,
                      [](const Piece& a, const Piece& b) noexcept { return a.key < b.key; });
    for (usize begin = 0; begin < pieces.size();) {
        usize end = begin;
        Vec3 weighted_position{0.0F, 0.0F, 0.0F};
        Vec3 weighted_normal{0.0F, 0.0F, 0.0F};
        f32 area = 0.0F;
        for (; end < pieces.size() && pieces[end].key == pieces[begin].key; ++end) {
            weighted_position = weighted_position + (pieces[end].position * pieces[end].area);
            weighted_normal = weighted_normal + (pieces[end].normal * pieces[end].area);
            area += pieces[end].area;
        }
        const f32 normal_length = length(weighted_normal);
        if (area > 0.0F && normal_length > 0.0F) {
            Surfel surfel;
            surfel.position = weighted_position / area;
            surfel.normal = weighted_normal / normal_length;
            surfel.albedo = settings.albedo;
            surfel.emission = settings.emission;
            surfel.roughness = settings.roughness;
            surfel.area = area;
            surfel.instance_id = settings.instance_id;
            surfel.material_id = settings.material_id;
            if (Status pushed = out.push_back(surfel); !pushed) {
                return pushed;
            }
            if (settings.two_sided) {
                surfel.normal = -surfel.normal;
                surfel.emission = Vec3{0.0F, 0.0F, 0.0F};
                if (Status pushed = out.push_back(surfel); !pushed) {
                    return pushed;
                }
            }
        }
        begin = end;
    }
    return ok();
}

Status MeshDistanceField::build(Span<const Vec3> positions, Span<const u32> indices,
                                const MeshFieldSettings& settings) noexcept {
    if (positions.empty() || indices.empty() || indices.size() % 3 != 0 || settings.dimension < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "MeshDistanceField::build: a mesh of whole triangles and a grid of two or "
                    "more samples an axis");
    }
    for (const u32 index : indices) {
        if (index >= positions.size()) {
            return fail(ErrorCode::OutOfRange,
                        "MeshDistanceField::build: an index past the positions");
        }
    }
    Vec3 lo = positions[0];
    Vec3 hi = positions[0];
    for (const Vec3 p : positions) {
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const Vec3 margin{settings.margin_metres, settings.margin_metres, settings.margin_metres};
    bounds_ = Aabb::from_min_max(lo - margin, hi + margin);
    dimension_ = settings.dimension;
    const usize samples = static_cast<usize>(dimension_) * dimension_ * dimension_;
    if (Status sized = distances_.resize(samples); !sized) {
        return sized;
    }
    const Vec3 size = bounds_.size();
    const f32 last = static_cast<f32>(dimension_ - 1U);
    const usize triangles = indices.size() / 3;
    for (u32 z = 0; z < dimension_; ++z) {
        for (u32 y = 0; y < dimension_; ++y) {
            for (u32 x = 0; x < dimension_; ++x) {
                const Vec3 point{bounds_.min.x + (size.x * static_cast<f32>(x) / last),
                                 bounds_.min.y + (size.y * static_cast<f32>(y) / last),
                                 bounds_.min.z + (size.z * static_cast<f32>(z) / last)};
                f32 nearest = 1.0e30F;
                u32 crossings = 0;
                for (usize triangle = 0; triangle < triangles; ++triangle) {
                    const Triangle t = triangle_at(positions, indices, triangle * 3U);
                    nearest = std::min(nearest, length(point - closest_on_triangle(point, t)));
                    crossings += crosses(point, kParityDirection, t) ? 1U : 0U;
                }
                // An odd number of crossings: the point is inside a closed mesh, or below an open
                // one the ray rises through.
                distances_[(((static_cast<usize>(z) * dimension_) + y) * dimension_) + x] =
                    (crossings & 1U) != 0U ? -nearest : nearest;
            }
        }
    }
    return ok();
}

AssetDistanceField MeshDistanceField::asset() const noexcept {
    AssetDistanceField field;
    field.dimension = dimension_;
    field.bounds = bounds_;
    field.distances = distances_.span();
    return field;
}

}  // namespace cy::rendering::gi
