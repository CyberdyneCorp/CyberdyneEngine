// SPDX-License-Identifier: MIT
// Rasterising UV2 triangles into the atlas, and finding the seams between charts.

#include "internal.h"

#include <cy/core/math/geometry.h>
#include <cy/core/math/matrix.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::lightmap_bake::detail {
namespace {

/// Texel centres a conservative texel may be from its triangle, in texels: half the diagonal, so
/// every texel the triangle touches at all is taken.
constexpr f32 kConservativeReach = 0.7072F;
/// Object-space quantum two seam endpoints are the same point at, in metres.
constexpr f32 kWeldQuantum = 1.0e-4F;
/// The cosine above which two vertex normals are one smooth surface, so a seam between them is a
/// parameterisation seam rather than a hard edge whose two sides are lit differently on purpose.
constexpr f32 kSmoothCosine = 0.98F;

/// Charts are the connected components of a mesh's triangles through shared vertices: the unwrap
/// splits a vertex at every chart boundary, so two triangles that share an index share a chart.
class ChartFinder {
public:
    [[nodiscard]] Status build(const BakeMesh& mesh) noexcept {
        const usize triangles = mesh.indices.size() / 3U;
        if (Status sized = parent_.resize(triangles); !sized) {
            return sized;
        }
        Array<u32> first_owner;
        if (Status sized = first_owner.resize(mesh.positions.size()); !sized) {
            return sized;
        }
        for (u32& owner : first_owner) {
            owner = kNoOwner;
        }
        for (usize triangle = 0; triangle < triangles; ++triangle) {
            parent_[triangle] = static_cast<u32>(triangle);
        }
        for (usize triangle = 0; triangle < triangles; ++triangle) {
            for (u32 corner = 0; corner < 3U; ++corner) {
                const u32 vertex = mesh.indices[(triangle * 3U) + corner];
                if (vertex >= first_owner.size()) {
                    continue;
                }
                if (first_owner[vertex] == kNoOwner) {
                    first_owner[vertex] = static_cast<u32>(triangle);
                } else {
                    unite(first_owner[vertex], static_cast<u32>(triangle));
                }
            }
        }
        // Dense chart numbers in first-triangle order, so a bake is reproducible.
        if (Status sized = chart_.resize(triangles); !sized) {
            return sized;
        }
        Array<u32> numbering;
        if (Status sized = numbering.resize(triangles); !sized) {
            return sized;
        }
        for (u32& number : numbering) {
            number = kNoOwner;
        }
        count_ = 0;
        for (usize triangle = 0; triangle < triangles; ++triangle) {
            const u32 root = find(static_cast<u32>(triangle));
            if (numbering[root] == kNoOwner) {
                numbering[root] = count_++;
            }
            chart_[triangle] = numbering[root];
        }
        return ok();
    }

    [[nodiscard]] u32 chart(usize triangle) const noexcept { return chart_[triangle]; }
    [[nodiscard]] u32 count() const noexcept { return count_; }

private:
    [[nodiscard]] u32 find(u32 node) noexcept {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }
    void unite(u32 a, u32 b) noexcept {
        const u32 ra = find(a);
        const u32 rb = find(b);
        if (ra != rb) {
            parent_[std::max(ra, rb)] = std::min(ra, rb);
        }
    }

    Array<u32> parent_;
    Array<u32> chart_;
    u32 count_ = 0;
};

/// A placed instance's geometry, in the world.
struct Placed {
    const BakeMesh* mesh = nullptr;
    Mat4 transform = Mat4::identity();
    Mat4 normal_transform = Mat4::identity();
    u32 instance = 0;
    u32 address = 0;
};

[[nodiscard]] Vec3 world_normal(const Placed& placed, u32 vertex, Vec3 face) noexcept {
    if (vertex >= placed.mesh->normals.size()) {
        return face;
    }
    return normalized_or(transform_direction(placed.normal_transform, placed.mesh->normals[vertex]),
                         face);
}

/// Barycentric weights of `p` against a 2D triangle, or false when it is degenerate.
[[nodiscard]] bool barycentric(Vec2 p, const Vec2 corners[3], f32 weights[3]) noexcept {
    const f32 area = cross(corners[1] - corners[0], corners[2] - corners[0]);
    if (std::fabs(area) <= 1.0e-12F) {
        return false;
    }
    weights[1] = cross(p - corners[0], corners[2] - corners[0]) / area;
    weights[2] = cross(corners[1] - corners[0], p - corners[0]) / area;
    weights[0] = 1.0F - weights[1] - weights[2];
    return true;
}

struct RasterTriangle {
    Vec2 atlas[3] = {};
    Vec3 world[3] = {};
    Vec3 normals[3] = {};
    u32 chart = 0;
};

void write_texel(Canvas& canvas, const RasterTriangle& triangle, u32 owner, u32 x, u32 y,
                 const f32 weights[3], bool conservative, f32 distance) noexcept {
    TexelSurface& texel = canvas.surfaces[canvas.index(x, y)];
    texel.position = (triangle.world[0] * weights[0]) + (triangle.world[1] * weights[1]) +
                     (triangle.world[2] * weights[2]);
    texel.normal =
        normalized_or((triangle.normals[0] * weights[0]) + (triangle.normals[1] * weights[1]) +
                          (triangle.normals[2] * weights[2]),
                      triangle.normals[0]);
    texel.owner = owner;
    texel.chart = triangle.chart;
    texel.state = TexelState::Surface;
    texel.conservative = conservative;
    texel.distance = distance;
}

/// One triangle into the canvas, clipped to its owner's rectangle `[lo, hi)`.
void rasterise_triangle(Canvas& canvas, const RasterTriangle& triangle, u32 owner, const u32 lo[2],
                        const u32 hi[2]) noexcept {
    const Vec2 low = cwise_min(triangle.atlas[0], cwise_min(triangle.atlas[1], triangle.atlas[2]));
    const Vec2 high = cwise_max(triangle.atlas[0], cwise_max(triangle.atlas[1], triangle.atlas[2]));
    const auto clamp_axis = [](f32 value, u32 from, u32 to) {
        const f32 clamped = std::clamp(value, static_cast<f32>(from), static_cast<f32>(to));
        return static_cast<u32>(clamped);
    };
    const u32 x0 = clamp_axis(std::floor(low.x - 1.0F), lo[0], hi[0]);
    const u32 x1 = clamp_axis(std::ceil(high.x + 1.0F), lo[0], hi[0]);
    const u32 y0 = clamp_axis(std::floor(low.y - 1.0F), lo[1], hi[1]);
    const u32 y1 = clamp_axis(std::ceil(high.y + 1.0F), lo[1], hi[1]);
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const Vec2 centre{static_cast<f32>(x) + 0.5F, static_cast<f32>(y) + 0.5F};
            f32 weights[3] = {};
            if (!barycentric(centre, triangle.atlas, weights)) {
                return;
            }
            TexelSurface& texel = canvas.surfaces[canvas.index(x, y)];
            const bool inside =
                weights[0] >= -1.0e-6F && weights[1] >= -1.0e-6F && weights[2] >= -1.0e-6F;
            if (inside) {
                if (texel.state != TexelState::Surface || texel.conservative) {
                    write_texel(canvas, triangle, owner, x, y, weights, false, 0.0F);
                }
                continue;
            }
            // Grazed: the nearest point of the triangle, if the texel overlaps it at all.
            const Vec3 nearest = geom::closest_point_on_triangle(
                Vec3{centre.x, centre.y, 0.0F}, Vec3{triangle.atlas[0].x, triangle.atlas[0].y, 0},
                Vec3{triangle.atlas[1].x, triangle.atlas[1].y, 0.0F},
                Vec3{triangle.atlas[2].x, triangle.atlas[2].y, 0.0F});
            const Vec2 point{nearest.x, nearest.y};
            const f32 distance = distance_squared(point, centre);
            if (distance > kConservativeReach * kConservativeReach) {
                continue;
            }
            const bool better = texel.state != TexelState::Surface ||
                                (texel.conservative && distance < texel.distance);
            if (!better || !barycentric(point, triangle.atlas, weights)) {
                continue;
            }
            for (f32& weight : weights) {
                weight = std::clamp(weight, 0.0F, 1.0F);
            }
            const f32 sum = weights[0] + weights[1] + weights[2];
            for (f32& weight : weights) {
                weight /= sum > 0.0F ? sum : 1.0F;
            }
            write_texel(canvas, triangle, owner, x, y, weights, true, distance);
        }
    }
}

/// An edge of one triangle, keyed by its welded object-space endpoints.
struct EdgeRecord {
    i32 key[6] = {};
    u32 from = 0;
    u32 to = 0;
    /// True when `from` is the lower endpoint of the key.
    bool forward = true;
};

[[nodiscard]] i32 quantise(f32 value) noexcept {
    return static_cast<i32>(std::lround(value / kWeldQuantum));
}

[[nodiscard]] bool key_less(const i32 a[3], const i32 b[3]) noexcept {
    return std::lexicographical_compare(a, a + 3, b, b + 3);
}

[[nodiscard]] bool same_key(const EdgeRecord& a, const EdgeRecord& b) noexcept {
    return std::equal(a.key, a.key + 6, b.key);
}

[[nodiscard]] bool normals_agree(const BakeMesh& mesh, u32 a, u32 b) noexcept {
    if (a >= mesh.normals.size() || b >= mesh.normals.size()) {
        return true;
    }
    return dot(mesh.normals[a], mesh.normals[b]) >= kSmoothCosine;
}

/// Every seam of one instance, as atlas coordinates on both sides.
[[nodiscard]] Status find_seams(const Placed& placed, u32 page_size, u32 gutter,
                                Array<SeamEdge>& seams) noexcept {
    const BakeMesh& mesh = *placed.mesh;
    if (mesh.uv2.size() < mesh.positions.size()) {
        return ok();
    }
    Array<EdgeRecord> edges;
    if (Status reserved = edges.reserve(mesh.indices.size()); !reserved) {
        return reserved;
    }
    for (usize at = 0; at + 2U < mesh.indices.size(); at += 3U) {
        for (u32 corner = 0; corner < 3U; ++corner) {
            EdgeRecord edge;
            edge.from = mesh.indices[at + corner];
            edge.to = mesh.indices[at + ((corner + 1U) % 3U)];
            if (edge.from >= mesh.positions.size() || edge.to >= mesh.positions.size()) {
                continue;
            }
            const Vec3 p = mesh.positions[edge.from];
            const Vec3 q = mesh.positions[edge.to];
            const i32 kp[3] = {quantise(p.x), quantise(p.y), quantise(p.z)};
            const i32 kq[3] = {quantise(q.x), quantise(q.y), quantise(q.z)};
            edge.forward = !key_less(kq, kp);
            const i32* low = edge.forward ? kp : kq;
            const i32* high = edge.forward ? kq : kp;
            std::copy(low, low + 3, edge.key);
            std::copy(high, high + 3, edge.key + 3);
            if (Status pushed = edges.push_back(edge); !pushed) {
                return pushed;
            }
        }
    }
    std::stable_sort(edges.begin(), edges.end(), [](const EdgeRecord& a, const EdgeRecord& b) {
        return std::lexicographical_compare(a.key, a.key + 6, b.key, b.key + 6);
    });
    for (usize at = 0; at + 1U < edges.size(); ++at) {
        const EdgeRecord& first = edges[at];
        const EdgeRecord& second = edges[at + 1U];
        if (!same_key(first, second)) {
            continue;
        }
        // Both edges' endpoints in key order: low end, then high end.
        const u32 a_low = first.forward ? first.from : first.to;
        const u32 a_high = first.forward ? first.to : first.from;
        const u32 b_low = second.forward ? second.from : second.to;
        const u32 b_high = second.forward ? second.to : second.from;
        const bool shared = a_low == b_low && a_high == b_high;
        const bool split_uv =
            mesh.uv2[a_low] != mesh.uv2[b_low] || mesh.uv2[a_high] != mesh.uv2[b_high];
        if (shared || !split_uv || !normals_agree(mesh, a_low, b_low) ||
            !normals_agree(mesh, a_high, b_high)) {
            continue;
        }
        SeamEdge seam;
        seam.a0 = atlas_coordinate(placed.address, mesh.uv2[a_low], page_size, gutter);
        seam.a1 = atlas_coordinate(placed.address, mesh.uv2[a_high], page_size, gutter);
        seam.b0 = atlas_coordinate(placed.address, mesh.uv2[b_low], page_size, gutter);
        seam.b1 = atlas_coordinate(placed.address, mesh.uv2[b_high], page_size, gutter);
        if (Status pushed = seams.push_back(seam); !pushed) {
            return pushed;
        }
        at += 1U;
    }
    return ok();
}

/// Claim the whole rectangle, gutter included, for the dilation to stay inside.
void claim_rectangle(Canvas& canvas, u32 owner, const u32 lo[2], const u32 hi[2]) noexcept {
    for (u32 y = lo[1]; y < hi[1]; ++y) {
        for (u32 x = lo[0]; x < hi[0]; ++x) {
            canvas.surfaces[canvas.index(x, y)].owner = owner;
        }
    }
}

[[nodiscard]] Status rasterise_instance(const Placed& placed, u32 page_size, u32 gutter,
                                        u32& chart_base, Canvas& canvas) noexcept {
    AtlasPlacement rectangle;
    if (!decode_address(placed.address, rectangle)) {
        return fail(ErrorCode::Internal, "a lightmap address did not decode");
    }
    const u32 block = page_size / kAddressBlocks;
    const u32 lo[2] = {rectangle.block_x * block,
                       (rectangle.page * page_size) + (rectangle.block_y * block)};
    const u32 hi[2] = {lo[0] + (rectangle.block_width * block),
                       lo[1] + (rectangle.block_height * block)};
    claim_rectangle(canvas, placed.instance, lo, hi);

    const BakeMesh& mesh = *placed.mesh;
    ChartFinder charts;
    if (Status built = charts.build(mesh); !built) {
        return built;
    }
    for (usize at = 0; at + 2U < mesh.indices.size(); at += 3U) {
        RasterTriangle triangle;
        bool usable = true;
        for (u32 corner = 0; corner < 3U; ++corner) {
            const u32 vertex = mesh.indices[at + corner];
            if (vertex >= mesh.positions.size() || vertex >= mesh.uv2.size()) {
                usable = false;
                break;
            }
            triangle.atlas[corner] =
                atlas_coordinate(placed.address, mesh.uv2[vertex], page_size, gutter);
            triangle.world[corner] = transform_point(placed.transform, mesh.positions[vertex]);
        }
        if (!usable) {
            continue;
        }
        const Vec3 face = normalized_or(
            cross(triangle.world[1] - triangle.world[0], triangle.world[2] - triangle.world[0]),
            Vec3{0.0F, 1.0F, 0.0F});
        for (u32 corner = 0; corner < 3U; ++corner) {
            triangle.normals[corner] = world_normal(placed, mesh.indices[at + corner], face);
        }
        triangle.chart = chart_base + charts.chart(at / 3U);
        rasterise_triangle(canvas, triangle, placed.instance, lo, hi);
    }
    chart_base += charts.count();
    return ok();
}

}  // namespace

f32 luminance(Vec3 value) noexcept {
    return (0.2126F * value.x) + (0.7152F * value.y) + (0.0722F * value.z);
}

Status rasterise(const LightmapScene& scene, const BakedLightmap& lightmap, Canvas& canvas,
                 Array<SeamEdge>& seams) noexcept {
    canvas.width = lightmap.page_size;
    canvas.height = lightmap.page_size * lightmap.pages;
    const usize texels = usize{canvas.width} * canvas.height;
    if (Status sized = canvas.surfaces.resize(texels); !sized) {
        return sized;
    }
    if (Status sized = canvas.moments.resize(texels); !sized) {
        return sized;
    }
    for (usize index = 0; index < texels; ++index) {
        canvas.surfaces[index] = TexelSurface{};
        canvas.moments[index] = TexelMoments{};
    }
    seams.clear();
    u32 chart_base = 0;
    for (u32 index = 0; index < scene.instances.size(); ++index) {
        const u32 address = lightmap.addresses[index];
        if (address == kNoLightmapAddress) {
            continue;
        }
        const BakeInstance& instance = scene.instances[index];
        Placed placed;
        placed.mesh = &scene.meshes[instance.mesh];
        placed.transform = instance.transform;
        Expected<Mat4, Error> inverted = inverse(instance.transform);
        placed.normal_transform =
            inverted.has_value() ? transpose(inverted.value()) : instance.transform;
        placed.instance = index;
        placed.address = address;
        if (Status drawn = rasterise_instance(placed, lightmap.page_size, lightmap.gutter_texels,
                                              chart_base, canvas);
            !drawn) {
            return drawn;
        }
        if (Status found = find_seams(placed, lightmap.page_size, lightmap.gutter_texels, seams);
            !found) {
            return found;
        }
    }
    return ok();
}

}  // namespace cy::rendering::lightmap_bake::detail
