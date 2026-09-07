#include <cy/rendering/virtual_geometry/simplify.h>

#include <cy/core/memory/hash_map.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::vg {

namespace {

/// A symmetric 4x4 quadric, stored as its ten distinct coefficients in the order
/// (xx, xy, xz, xw, yy, yz, yw, zz, zw, ww). Adding two quadrics is adding the coefficients, which
/// is why this is an array rather than a matrix type. Double precision throughout: the quadric is a
/// difference of large numbers near a planar vertex, and in single precision the difference is
/// noise, which turns the collapse order — and with it determinism — into a coin toss.
struct Quadric {
    f64 c[10] = {};

    void add_weighted_plane(Vec3 unit_normal, f32 offset, f64 weight) noexcept {
        const f64 a = static_cast<f64>(unit_normal.x);
        const f64 b = static_cast<f64>(unit_normal.y);
        const f64 d = static_cast<f64>(unit_normal.z);
        const f64 e = static_cast<f64>(offset);
        const f64 terms[10] = {a * a, a * b, a * d, a * e, b * b,
                               b * d, b * e, d * d, d * e, e * e};
        for (u32 index = 0; index < 10; ++index) {
            c[index] += terms[index] * weight;
        }
    }

    void accumulate(const Quadric& other) noexcept {
        for (u32 index = 0; index < 10; ++index) {
            c[index] += other.c[index];
        }
    }

    /// v^T Q v, the sum of squared distances to the accumulated planes. Never negative in exact
    /// arithmetic; clamped because it is a difference of large numbers.
    [[nodiscard]] f64 evaluate(Vec3 v) const noexcept {
        const f64 x = static_cast<f64>(v.x);
        const f64 y = static_cast<f64>(v.y);
        const f64 z = static_cast<f64>(v.z);
        const f64 value = (c[0] * x * x) + (2.0 * c[1] * x * y) + (2.0 * c[2] * x * z) +
                          (2.0 * c[3] * x) + (c[4] * y * y) + (2.0 * c[5] * y * z) +
                          (2.0 * c[6] * y) + (c[7] * z * z) + (2.0 * c[8] * z) + c[9];
        return value > 0.0 ? value : 0.0;
    }
};

struct Triangle {
    u32 v[3] = {};
    bool live = true;

    [[nodiscard]] bool uses(u32 vertex) const noexcept {
        return v[0] == vertex || v[1] == vertex || v[2] == vertex;
    }
};

/// One collapse the pass is considering: move `from` onto `to`.
struct Candidate {
    f64 cost = 0.0;
    u32 from = 0;
    u32 to = 0;
};

/// Ascending cost, then by the pair, so equal costs on a symmetric mesh resolve identically on
/// every run and on every machine. This comparison is task 7.5's determinism at this level.
bool candidate_before(const Candidate& a, const Candidate& b) noexcept {
    if (a.cost != b.cost) {
        return a.cost < b.cost;
    }
    if (a.from != b.from) {
        return a.from < b.from;
    }
    return a.to < b.to;
}

[[nodiscard]] u64 edge_key(u32 a, u32 b) noexcept {
    const u32 lo = a < b ? a : b;
    const u32 hi = a < b ? b : a;
    return (static_cast<u64>(lo) << 32U) | static_cast<u64>(hi);
}

[[nodiscard]] Vec3 face_normal(Vec3 a, Vec3 b, Vec3 c) noexcept {
    return cross(b - a, c - a);
}

/// The whole working set of one simplification, so that the pass helpers below take one argument
/// rather than nine and the top-level function stays readable.
///
/// `incidence` is a CSR of vertex to triangle, rebuilt once per pass. It stays valid for the
/// vertices a pass still touches: a collapse retargets `from`'s triangles onto `to` and marks both
/// touched, so no triangle can GAIN a vertex the pass has yet to look up. Membership is re-tested
/// at use, which makes the list a superset rather than an assumption.
struct Simplifier {
    explicit Simplifier(Allocator& allocator) noexcept
        : triangles(allocator),
          position(allocator),
          locked(allocator),
          quadric(allocator),
          touched(allocator),
          candidates(allocator),
          incidence_start(allocator),
          incidence(allocator),
          seen(allocator),
          star_edges(allocator),
          star_uses(allocator),
          star_before(allocator) {}

    Array<Triangle> triangles;
    Array<Vec3> position;
    Array<bool> locked;
    Array<Quadric> quadric;
    Array<bool> touched;
    Array<Candidate> candidates;
    Array<u32> incidence_start;  // vertex_count + 1 entries
    Array<u32> incidence;
    HashSet<u64> seen;
    /// Scratch for the local manifold check: the star's edges and their post-collapse face counts.
    /// Members rather than locals because the check runs once per candidate per pass and a fresh
    /// allocation there would dominate the simplifier.
    Array<u64> star_edges;
    Array<u32> star_uses;
    /// The star's edges BEFORE the collapse, for the locked-pair rule below.
    Array<u64> star_before;

    u32 vertex_count = 0;
    u32 live_triangles = 0;
    f64 worst_cost = 0.0;

    [[nodiscard]] Span<const u32> incident_to(u32 vertex) const noexcept {
        const u32 first = incidence_start[vertex];
        const u32 last = incidence_start[vertex + 1U];
        return {incidence.data() + first, last - first};
    }

    [[nodiscard]] Status load(const SimplifyInput& input) noexcept;
    [[nodiscard]] Status build_quadrics() noexcept;
    [[nodiscard]] Status rebuild_incidence() noexcept;
    [[nodiscard]] Status gather_candidates() noexcept;
    [[nodiscard]] bool collapse_flips(const Candidate& candidate) const noexcept;
    [[nodiscard]] bool collapse_breaks_topology(const Candidate& candidate) noexcept;
    void apply(const Candidate& candidate) noexcept;
};

Status Simplifier::load(const SimplifyInput& input) noexcept {
    const u32 triangle_count = static_cast<u32>(input.indices.size() / 3);
    if (Status reserved = triangles.reserve(triangle_count); !reserved) {
        return reserved;
    }
    u32 max_vertex = 0;
    for (u32 index = 0; index < triangle_count; ++index) {
        Triangle triangle;
        for (u32 corner = 0; corner < 3; ++corner) {
            triangle.v[corner] = input.indices[(index * 3U) + corner];
            max_vertex = triangle.v[corner] > max_vertex ? triangle.v[corner] : max_vertex;
        }
        if (Status pushed = triangles.push_back(triangle); !pushed) {
            return pushed;
        }
    }
    if (static_cast<usize>(max_vertex) >= input.positions.size()) {
        return fail(ErrorCode::OutOfRange, "simplify_group: an index exceeds the position array");
    }
    vertex_count = max_vertex + 1U;
    live_triangles = triangle_count;

    if (Status resized = position.resize(vertex_count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < vertex_count; ++index) {
        position[index] = input.positions[index];
    }
    if (Status resized = locked.resize(vertex_count); !resized) {
        return resized;
    }
    if (Status resized = touched.resize(vertex_count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < vertex_count; ++index) {
        locked[index] = false;
    }
    for (const u32 vertex : input.locked) {
        if (vertex < vertex_count) {
            locked[vertex] = true;
        }
    }
    return ok();
}

Status Simplifier::build_quadrics() noexcept {
    if (Status resized = quadric.resize(vertex_count); !resized) {
        return resized;
    }
    for (const Triangle& triangle : triangles) {
        const Vec3 a = position[triangle.v[0]];
        const Vec3 b = position[triangle.v[1]];
        const Vec3 c = position[triangle.v[2]];
        const Vec3 normal = face_normal(a, b, c);
        const f32 twice_area = length(normal);
        if (twice_area <= 1.0e-20F) {
            continue;  // a degenerate face has no plane to be at a distance from
        }
        const Vec3 unit = normal * (1.0F / twice_area);
        Quadric plane;
        // Weighted by twice the area, which `cross()` already gives: without it a sliver counts as
        // much as the face beside it and drags its vertices' quadrics off the surface.
        plane.add_weighted_plane(unit, -dot(unit, a), static_cast<f64>(twice_area));
        for (const u32 corner : triangle.v) {
            quadric[corner].accumulate(plane);
        }
    }
    return ok();
}

Status Simplifier::rebuild_incidence() noexcept {
    if (Status resized = incidence_start.resize(vertex_count + 1U); !resized) {
        return resized;
    }
    for (u32 index = 0; index <= vertex_count; ++index) {
        incidence_start[index] = 0;
    }
    for (const Triangle& triangle : triangles) {
        if (!triangle.live) {
            continue;
        }
        for (const u32 vertex : triangle.v) {
            ++incidence_start[vertex + 1U];
        }
    }
    for (u32 index = 0; index < vertex_count; ++index) {
        incidence_start[index + 1U] += incidence_start[index];
    }
    if (Status resized = incidence.resize(incidence_start[vertex_count]); !resized) {
        return resized;
    }
    Array<u32> cursor(incidence.allocator());
    if (Status resized = cursor.resize(vertex_count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < vertex_count; ++index) {
        cursor[index] = incidence_start[index];
    }
    for (u32 index = 0; index < triangles.size(); ++index) {
        if (!triangles[index].live) {
            continue;
        }
        for (const u32 vertex : triangles[index].v) {
            incidence[cursor[vertex]] = index;
            ++cursor[vertex];
        }
    }
    return ok();
}

Status Simplifier::gather_candidates() noexcept {
    candidates.clear();
    seen.clear();
    for (const Triangle& triangle : triangles) {
        if (!triangle.live) {
            continue;
        }
        for (u32 corner = 0; corner < 3; ++corner) {
            const u32 a = triangle.v[corner];
            const u32 b = triangle.v[(corner + 1U) % 3U];
            if (locked[a] && locked[b]) {
                continue;  // a boundary edge of the group: neither end may move
            }
            const u64 key = edge_key(a, b);
            if (seen.contains(key)) {
                continue;
            }
            if (Status inserted = seen.insert(key); !inserted) {
                return inserted;
            }
            // The lock decides the direction: a locked endpoint is the survivor. With neither
            // locked the lower index survives, which is the deterministic choice.
            u32 from = a < b ? b : a;
            if (locked[a]) {
                from = b;
            } else if (locked[b]) {
                from = a;
            }
            const u32 to = from == a ? b : a;
            Quadric merged = quadric[from];
            merged.accumulate(quadric[to]);
            const Candidate candidate{merged.evaluate(position[to]), from, to};
            if (Status pushed = candidates.push_back(candidate); !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(candidates, candidate_before);
    return ok();
}

bool Simplifier::collapse_flips(const Candidate& candidate) const noexcept {
    for (const u32 index : incident_to(candidate.from)) {
        const Triangle& triangle = triangles[index];
        if (!triangle.live || !triangle.uses(candidate.from) || triangle.uses(candidate.to)) {
            continue;  // gone, retargeted away, or degenerate after the collapse and removed
        }
        Vec3 after_corner[3];
        for (u32 corner = 0; corner < 3; ++corner) {
            after_corner[corner] = triangle.v[corner] == candidate.from
                                       ? position[candidate.to]
                                       : position[triangle.v[corner]];
        }
        const Vec3 before =
            face_normal(position[triangle.v[0]], position[triangle.v[1]], position[triangle.v[2]]);
        const Vec3 after = face_normal(after_corner[0], after_corner[1], after_corner[2]);
        // An inverted face is a visible artefact the quadric cannot see, because the quadric is a
        // distance and an inversion preserves distance.
        if (dot(before, after) <= 0.0F) {
            return true;
        }
    }
    return false;
}

// ================================================================================================
// THE LOCAL MANIFOLD CHECK, AND THE FOUR CRACKS IT CLOSED
// ================================================================================================
//
// An edge collapse preserves a manifold only under the LINK CONDITION: the vertices adjacent to
// both endpoints must be exactly the vertices opposite the edge. Where it does not hold, collapsing
// pinches the surface — an edge ends up carrying four faces instead of two — and the cut through
// the hierarchy is no longer a manifold surface. The quadric cannot see it, because a pinch costs
// no distance.
//
// This is not a hypothetical. `check_watertight`'s sweep over 120 cuts of a 1,280-triangle
// icosphere reported four cuts with a four-face edge before this guard existed, every one of them a
// group whose simplification had pinched.
//
// The check is stated as the property rather than as the condition: every edge of the star, AFTER
// the collapse, must carry at most two faces. It is complete — an edge of a retargeted triangle
// that becomes (to, x) can only appear in a triangle that was incident to `from` or to `to`, so the
// local count is the whole count — and it subsumes the link condition rather than restating it.
bool Simplifier::collapse_breaks_topology(const Candidate& candidate) noexcept {
    star_edges.clear();
    star_uses.clear();
    star_before.clear();

    for (const u32 which : {candidate.from, candidate.to}) {
        for (const u32 index : incident_to(which)) {
            const Triangle& triangle = triangles[index];
            if (!triangle.live) {
                continue;
            }
            for (u32 corner = 0; corner < 3; ++corner) {
                const u64 key = edge_key(triangle.v[corner], triangle.v[(corner + 1U) % 3U]);
                bool present = false;
                for (const u64 seen_key : star_before) {
                    present = present || seen_key == key;
                }
                if (!present) {
                    if (Status pushed = star_before.push_back(key); !pushed) {
                        return true;
                    }
                }
            }
        }
    }

    auto count_edge = [this](u32 a, u32 b) noexcept -> Status {
        const u64 key = edge_key(a, b);
        for (usize slot = 0; slot < star_edges.size(); ++slot) {
            if (star_edges[slot] == key) {
                ++star_uses[slot];
                return ok();
            }
        }
        if (Status pushed = star_edges.push_back(key); !pushed) {
            return pushed;
        }
        return star_uses.push_back(1U);
    };

    for (const u32 which : {candidate.from, candidate.to}) {
        for (const u32 index : incident_to(which)) {
            const Triangle& triangle = triangles[index];
            if (!triangle.live) {
                continue;
            }
            if (which == candidate.to && triangle.uses(candidate.from)) {
                continue;  // already counted through `from`
            }
            u32 after[3];
            for (u32 corner = 0; corner < 3; ++corner) {
                after[corner] =
                    triangle.v[corner] == candidate.from ? candidate.to : triangle.v[corner];
            }
            if (after[0] == after[1] || after[1] == after[2] || after[0] == after[2]) {
                continue;  // degenerate after the collapse, and therefore removed
            }
            for (u32 corner = 0; corner < 3; ++corner) {
                if (Status counted = count_edge(after[corner], after[(corner + 1U) % 3U]);
                    !counted) {
                    return true;  // out of memory: refuse rather than risk the topology
                }
            }
        }
    }
    for (const u32 uses : star_uses) {
        if (uses > 2U) {
            return true;
        }
    }

    // ============================================================================================
    // AND NO NEW EDGE MAY JOIN TWO LOCKED VERTICES
    // ============================================================================================
    //
    // The rule above makes each group's patch a manifold. It does not make the UNION of the
    // patches one, and that is a separate failure with the same symptom. Two groups that share a
    // boundary chain are simplified independently; if a collapse inside group A creates a new edge
    // between two of A's boundary vertices, and a collapse inside group B creates the same edge
    // between the same two vertices, that edge carries two faces in each patch and four in the
    // surface. Measured: four of 120 cuts of a 1,280-triangle icosphere, at three different groups
    // meeting one edge, and every one of them survived the per-patch manifold check.
    //
    // A locked vertex is a boundary vertex by definition, so the rule is local and cheap: a
    // collapse may not INVENT an edge between two of them. An edge that was already there is
    // untouched — the boundary chain itself is made of exactly those.
    for (const u64 edge : star_edges) {
        const u32 low = static_cast<u32>(edge >> 32U);
        const u32 high = static_cast<u32>(edge & 0xFFFFFFFFU);
        if (!locked[low] || !locked[high]) {
            continue;
        }
        bool existed = false;
        for (const u64 key : star_before) {
            existed = existed || key == edge;
        }
        if (!existed) {
            return true;
        }
    }
    return false;
}

void Simplifier::apply(const Candidate& candidate) noexcept {
    for (const u32 index : incident_to(candidate.from)) {
        Triangle& triangle = triangles[index];
        if (!triangle.live || !triangle.uses(candidate.from)) {
            continue;
        }
        if (triangle.uses(candidate.to)) {
            triangle.live = false;
            --live_triangles;
            continue;
        }
        for (u32& vertex : triangle.v) {
            if (vertex == candidate.from) {
                vertex = candidate.to;
            }
        }
    }
    quadric[candidate.to].accumulate(quadric[candidate.from]);
    touched[candidate.from] = true;
    touched[candidate.to] = true;
    worst_cost = candidate.cost > worst_cost ? candidate.cost : worst_cost;
}

/// A ceiling on the passes, so a mesh whose candidates never resolve cannot loop. Each pass applies
/// every collapse it can, so the triangle count roughly halves per pass on a well-behaved group and
/// the loop exits on `applied == 0` long before this.
constexpr u32 kMaximumPasses = 64;

}  // namespace

Expected<SimplifyResult, Error> simplify_group(const SimplifyInput& input,
                                               Allocator& allocator) noexcept {
    if (input.indices.size() % 3 != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "simplify_group: index count is not a multiple of 3");
    }
    SimplifyResult result(allocator);
    if (input.indices.empty()) {
        return result;
    }

    Simplifier state(allocator);
    if (Status loaded = state.load(input); !loaded) {
        return make_unexpected(loaded.error());
    }
    if (Status built = state.build_quadrics(); !built) {
        return make_unexpected(built.error());
    }

    for (u32 pass = 0; pass < kMaximumPasses && state.live_triangles > input.target_triangles;
         ++pass) {
        if (Status built = state.rebuild_incidence(); !built) {
            return make_unexpected(built.error());
        }
        if (Status gathered = state.gather_candidates(); !gathered) {
            return make_unexpected(gathered.error());
        }
        if (state.candidates.empty()) {
            break;
        }
        for (u32 index = 0; index < state.vertex_count; ++index) {
            state.touched[index] = false;
        }
        u32 applied = 0;
        for (const Candidate& candidate : state.candidates) {
            if (state.live_triangles <= input.target_triangles) {
                break;
            }
            if (state.touched[candidate.from] || state.touched[candidate.to]) {
                continue;
            }
            if (state.locked[candidate.from]) {
                ++result.refused_locked;
                continue;
            }
            if (state.collapse_breaks_topology(candidate) || state.collapse_flips(candidate)) {
                continue;
            }
            state.apply(candidate);
            ++result.collapses;
            ++applied;
        }
        if (applied == 0) {
            break;
        }
    }

    for (const Triangle& triangle : state.triangles) {
        if (!triangle.live) {
            continue;
        }
        for (const u32 vertex : triangle.v) {
            if (Status pushed = result.indices.push_back(vertex); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    // The quadric is a sum of SQUARED distances; the error a hierarchy composes is a distance.
    result.error = static_cast<f32>(std::sqrt(state.worst_cost));
    return result;
}

}  // namespace cy::rendering::vg
