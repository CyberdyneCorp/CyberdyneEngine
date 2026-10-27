// SPDX-License-Identifier: MIT
// Bowyer-Watson over at most `kMaxBlendSamples` points. Cook time only: nothing here runs in a
// frame, and a runtime that loads a cooked program does not link this file.
//
// Double precision and a relative tolerance on the in-circle test, because the spaces an author
// draws are GRIDS — a centre and eight directions, a row of speeds — and a grid's quads are
// co-circular: all four corners lie on one circle. An exact test would then call the fourth corner
// inside or outside by rounding, and the triangulation would depend on the last bit of a position.
// With the tolerance a co-circular point is outside, so each such quad is split by the diagonal the
// insertion order reached first, which is a function of the authored order alone.

#include "blend_triangulation.h"

#include <cmath>

namespace cy::graph::pose {
namespace {

constexpr u32 kSuper = 3;
constexpr u32 kMaxPoints = kMaxBlendSamples + kSuper;
// Each insertion adds at most two triangles net; the super triangle is the first.
constexpr u32 kMaxTriangles = (2 * kMaxPoints) + 1;
constexpr u32 kMaxEdges = 3 * kMaxTriangles;
constexpr double kInCircleTolerance = 1e-9;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Triangle {
    u32 v[3] = {0, 0, 0};
    double cx = 0.0;
    double cy = 0.0;
    double radius2 = 0.0;
};

struct Edge {
    u32 a = 0;
    u32 b = 0;
};

[[nodiscard]] double cross(const Point& a, const Point& b, const Point& c) noexcept {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

/// A triangle with its circumcircle, counter-clockwise. False for a degenerate one.
[[nodiscard]] bool make_triangle(const Point* points, u32 a, u32 b, u32 c, Triangle& out) noexcept {
    const double orientation = cross(points[a], points[b], points[c]);
    if (orientation == 0.0) {
        return false;
    }
    if (orientation < 0.0) {
        const u32 swap = b;
        b = c;
        c = swap;
    }
    const Point& pa = points[a];
    const Point& pb = points[b];
    const Point& pc = points[c];
    const double d =
        2.0 * ((pa.x * (pb.y - pc.y)) + (pb.x * (pc.y - pa.y)) + (pc.x * (pa.y - pb.y)));
    const double a2 = (pa.x * pa.x) + (pa.y * pa.y);
    const double b2 = (pb.x * pb.x) + (pb.y * pb.y);
    const double c2 = (pc.x * pc.x) + (pc.y * pc.y);
    out.v[0] = a;
    out.v[1] = b;
    out.v[2] = c;
    out.cx = ((a2 * (pb.y - pc.y)) + (b2 * (pc.y - pa.y)) + (c2 * (pa.y - pb.y))) / d;
    out.cy = ((a2 * (pc.x - pb.x)) + (b2 * (pa.x - pc.x)) + (c2 * (pb.x - pa.x))) / d;
    out.radius2 = ((pa.x - out.cx) * (pa.x - out.cx)) + ((pa.y - out.cy) * (pa.y - out.cy));
    return true;
}

[[nodiscard]] bool inside_circumcircle(const Triangle& triangle, const Point& p) noexcept {
    const double dx = p.x - triangle.cx;
    const double dy = p.y - triangle.cy;
    return ((dx * dx) + (dy * dy)) < triangle.radius2 * (1.0 - kInCircleTolerance);
}

[[nodiscard]] bool same_edge(const Edge& a, const Edge& b) noexcept {
    return (a.a == b.a && a.b == b.b) || (a.a == b.b && a.b == b.a);
}

struct Mesh {
    Point points[kMaxPoints];
    Triangle triangles[kMaxTriangles];
    u32 triangle_count = 0;

    /// Insert point `p`: remove every triangle whose circumcircle holds it and fan the hole's
    /// boundary to it.
    [[nodiscard]] bool insert(u32 p) noexcept {
        Edge edges[kMaxEdges];
        u32 edge_count = 0;
        if (!carve(p, edges, edge_count)) {
            return false;
        }
        for (u32 index = 0; index < edge_count; ++index) {
            if (!boundary(edges, edge_count, index)) {
                continue;
            }
            Triangle triangle;
            if (!make_triangle(points, edges[index].a, edges[index].b, p, triangle)) {
                continue;
            }
            if (triangle_count == kMaxTriangles) {
                return false;
            }
            triangles[triangle_count++] = triangle;
        }
        return true;
    }

    /// Remove the triangles whose circumcircle holds `p`, collecting their edges.
    [[nodiscard]] bool carve(u32 p, Edge (&edges)[kMaxEdges], u32& edge_count) noexcept {
        u32 kept = 0;
        for (u32 index = 0; index < triangle_count; ++index) {
            const Triangle& triangle = triangles[index];
            if (!inside_circumcircle(triangle, points[p])) {
                triangles[kept++] = triangle;
                continue;
            }
            if (edge_count + 3 > kMaxEdges) {
                return false;
            }
            for (u32 side = 0; side < 3; ++side) {
                edges[edge_count++] = Edge{triangle.v[side], triangle.v[(side + 1) % 3]};
            }
        }
        triangle_count = kept;
        return true;
    }

    /// An edge of the hole's boundary: one no other removed triangle shared.
    [[nodiscard]] static bool boundary(const Edge (&edges)[kMaxEdges], u32 edge_count,
                                       u32 index) noexcept {
        for (u32 other = 0; other < edge_count; ++other) {
            if (other != index && same_edge(edges[index], edges[other])) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace

Status triangulate_blend_space(Span<const BlendSample> samples,
                               Array<BlendTriangle>& out) noexcept {
    const auto count = static_cast<u32>(samples.size());
    if (count < 3 || count > kMaxBlendSamples) {
        return fail(ErrorCode::NotFound,
                    "a two-dimensional blend space needs three to sixteen samples");
    }
    Mesh mesh;
    const auto widen = [](f32 value) noexcept { return static_cast<double>(value); };
    double low_x = widen(samples[0].x);
    double high_x = low_x;
    double low_y = widen(samples[0].y);
    double high_y = low_y;
    for (u32 index = 0; index < count; ++index) {
        mesh.points[index] = Point{widen(samples[index].x), widen(samples[index].y)};
        low_x = std::fmin(low_x, mesh.points[index].x);
        high_x = std::fmax(high_x, mesh.points[index].x);
        low_y = std::fmin(low_y, mesh.points[index].y);
        high_y = std::fmax(high_y, mesh.points[index].y);
    }
    // A super triangle far outside every sample, removed with everything touching it at the end.
    const double span = std::fmax(std::fmax(high_x - low_x, high_y - low_y), 1.0) * 64.0;
    const double mid_x = 0.5 * (low_x + high_x);
    const double mid_y = 0.5 * (low_y + high_y);
    mesh.points[count] = Point{mid_x - (2.0 * span), mid_y - span};
    mesh.points[count + 1] = Point{mid_x + (2.0 * span), mid_y - span};
    mesh.points[count + 2] = Point{mid_x, mid_y + (2.0 * span)};
    if (!make_triangle(mesh.points, count, count + 1, count + 2, mesh.triangles[0])) {
        return fail(ErrorCode::InvalidArgument, "the blend space's samples are not finite");
    }
    mesh.triangle_count = 1;
    for (u32 index = 0; index < count; ++index) {
        if (!mesh.insert(index)) {
            return fail(ErrorCode::OutOfRange, "the blend space's triangulation overflowed");
        }
    }

    const usize first = out.size();
    for (u32 index = 0; index < mesh.triangle_count; ++index) {
        const Triangle& triangle = mesh.triangles[index];
        if (triangle.v[0] >= count || triangle.v[1] >= count || triangle.v[2] >= count) {
            continue;
        }
        BlendTriangle stored;
        for (u32 vertex = 0; vertex < 3; ++vertex) {
            stored.sample[vertex] = static_cast<u16>(triangle.v[vertex]);
        }
        if (Status pushed = out.push_back(stored); !pushed) {
            return pushed;
        }
    }
    if (out.size() == first) {
        return fail(ErrorCode::NotFound,
                    "a two-dimensional blend space needs samples that are not all on one line");
    }
    return ok();
}

}  // namespace cy::graph::pose
