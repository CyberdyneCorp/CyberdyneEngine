#include <cy/import/mesh.h>

#include <cy/core/math/geometry.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace cy::import {
namespace {

/// A quantised position, for the hash the welder and the seam detector both use.
///
/// Snapping to a lattice rather than comparing pairwise is what makes both O(n). The cost, stated
/// once here for both callers: it is not transitive — two vertices within tolerance can land in
/// different cells — so a welder built on it merges *almost* everything it should. That is the
/// standard trade, `cy::geom::weld_vertices` makes it too, and the alternative is O(n²) on a
/// million-vertex import.
struct Lattice {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;

    friend bool operator==(const Lattice& a, const Lattice& b) noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

struct LatticeHash {
    [[nodiscard]] usize operator()(const Lattice& cell) const noexcept {
        u64 mixed = static_cast<u64>(cell.x) * 0x9e3779b97f4a7c15ULL;
        mixed ^= static_cast<u64>(cell.y) * 0xc2b2ae3d27d4eb4fULL;
        mixed ^= static_cast<u64>(cell.z) * 0x165667b19e3779f9ULL;
        mixed ^= mixed >> 29U;
        return static_cast<usize>(mixed);
    }
};

[[nodiscard]] Lattice cell_of(Vec3 position, f32 tolerance) noexcept {
    const f32 scale = tolerance > 0.0f ? 1.0f / tolerance : 1.0f;
    return Lattice{static_cast<i64>(std::llround(position.x * scale)),
                   static_cast<i64>(std::llround(position.y * scale)),
                   static_cast<i64>(std::llround(position.z * scale))};
}

[[nodiscard]] Vec3 triangle_normal(Vec3 a, Vec3 b, Vec3 c) noexcept {
    return cross(b - a, c - a);
}

/// A plane's quadric, `K_p` in Garland and Heckbert's notation, as its ten distinct coefficients.
struct Quadric {
    f64 a2 = 0.0;
    f64 ab = 0.0;
    f64 ac = 0.0;
    f64 ad = 0.0;
    f64 b2 = 0.0;
    f64 bc = 0.0;
    f64 bd = 0.0;
    f64 c2 = 0.0;
    f64 cd = 0.0;
    f64 d2 = 0.0;

    void add(const Quadric& other) noexcept {
        a2 += other.a2;
        ab += other.ab;
        ac += other.ac;
        ad += other.ad;
        b2 += other.b2;
        bc += other.bc;
        bd += other.bd;
        c2 += other.c2;
        cd += other.cd;
        d2 += other.d2;
    }

    /// `v^T Q v` — the sum of squared distances to the planes this quadric accumulated.
    [[nodiscard]] f64 evaluate(Vec3 v) const noexcept {
        const f64 x = v.x;
        const f64 y = v.y;
        const f64 z = v.z;
        return (a2 * x * x) + (2.0 * ab * x * y) + (2.0 * ac * x * z) + (2.0 * ad * x) +
               (b2 * y * y) + (2.0 * bc * y * z) + (2.0 * bd * y) + (c2 * z * z) + (2.0 * cd * z) +
               d2;
    }

    [[nodiscard]] static Quadric from_plane(Vec3 normal, f32 offset, f64 weight) noexcept {
        const f64 a = normal.x;
        const f64 b = normal.y;
        const f64 c = normal.z;
        const f64 d = offset;
        Quadric q;
        q.a2 = weight * a * a;
        q.ab = weight * a * b;
        q.ac = weight * a * c;
        q.ad = weight * a * d;
        q.b2 = weight * b * b;
        q.bc = weight * b * c;
        q.bd = weight * b * d;
        q.c2 = weight * c * c;
        q.cd = weight * c * d;
        q.d2 = weight * d * d;
        return q;
    }
};

/// One candidate collapse, in the heap.
struct Collapse {
    u32 from = 0;
    u32 to = 0;
    f64 cost = 0.0;
    Vec3 target{};
    /// The version the two endpoints had when this was computed. A collapse whose endpoints have
    /// moved since is stale and is discarded when it surfaces, which is what makes lazy
    /// invalidation correct without touching the heap on every collapse.
    u32 from_version = 0;
    u32 to_version = 0;
    /// A stable tie-break, so two runs produce the same mesh. See the note on determinism in
    /// mesh.h.
    u32 serial = 0;
};

/// An undirected edge between two position groups, ordered so that a pair has one key.
struct EdgeKey {
    u32 low = 0;
    u32 high = 0;

    friend bool operator==(const EdgeKey& a, const EdgeKey& b) noexcept {
        return a.low == b.low && a.high == b.high;
    }
};

struct EdgeKeyHash {
    [[nodiscard]] usize operator()(const EdgeKey& key) const noexcept {
        return static_cast<usize>((static_cast<u64>(key.low) << 32U) ^ key.high);
    }
};

struct CollapseOrder {
    [[nodiscard]] bool operator()(const Collapse& a, const Collapse& b) const noexcept {
        if (a.cost != b.cost) {
            return a.cost > b.cost;  // a min-heap out of std::priority_queue's max-heap
        }
        return a.serial > b.serial;
    }
};

}  // namespace

// --- MeshData ------------------------------------------------------------------------------------

Status MeshData::validate() const noexcept {
    const usize count = positions.size();
    if (count == 0 && indices.empty()) {
        return ok();
    }
    if (!normals.empty() && normals.size() != count) {
        return fail(ErrorCode::InvalidArgument, "the mesh has a normal count unlike its vertices");
    }
    if (!uvs.empty() && uvs.size() != count) {
        return fail(ErrorCode::InvalidArgument, "the mesh has a uv count unlike its vertices");
    }
    if (!uv2.empty() && uv2.size() != count) {
        return fail(ErrorCode::InvalidArgument, "the mesh has a uv2 count unlike its vertices");
    }
    if (!tangents.empty() && tangents.size() != count) {
        return fail(ErrorCode::InvalidArgument, "the mesh has a tangent count unlike its vertices");
    }
    if (indices.size() % 3 != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "the mesh's index count is not a multiple of three");
    }
    for (const u32 index : indices) {
        if (index >= count) {
            return fail(ErrorCode::OutOfRange, "the mesh has an index past its last vertex");
        }
    }
    usize covered = 0;
    for (const MeshSection& section : sections) {
        if (section.first_index != covered) {
            return fail(ErrorCode::InvalidArgument,
                        "the mesh's sections do not tile its index list in order");
        }
        if (section.index_count % 3 != 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a mesh section's index count is not a multiple of three");
        }
        covered += section.index_count;
    }
    if (!sections.empty() && covered != indices.size()) {
        return fail(ErrorCode::InvalidArgument, "the mesh's sections do not cover its index list");
    }
    return ok();
}

Aabb MeshData::bounds() const noexcept {
    Aabb box = Aabb::empty();
    for (const Vec3& position : positions) {
        box.min = cwise_min(box.min, position);
        box.max = cwise_max(box.max, position);
    }
    return box;
}

void MeshData::clear() noexcept {
    positions.clear();
    normals.clear();
    uvs.clear();
    uv2.clear();
    tangents.clear();
    indices.clear();
    sections.clear();
}

// --- Welding -------------------------------------------------------------------------------------

Expected<usize, Error> weld(MeshData& mesh, const WeldOptions& options) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    const usize count = mesh.vertex_count();
    if (count == 0) {
        return usize{0};
    }

    const f32 cos_limit = std::cos(math::radians(options.normal_tolerance_degrees));
    std::vector<u32> remap(count, 0);
    /// The output slot a kept vertex landed in, so a merge finds its representative's slot without
    /// searching the kept list — which would make welding quadratic in the number of vertices.
    std::vector<u32> slot_of(count, 0);
    std::vector<u32> unique;
    unique.reserve(count);
    std::unordered_map<Lattice, std::vector<u32>, LatticeHash> buckets;
    buckets.reserve(count);

    for (usize index = 0; index < count; ++index) {
        const Lattice cell = cell_of(mesh.positions[index], options.position_tolerance);
        std::vector<u32>& bucket = buckets[cell];
        u32 found = 0xFFFFFFFFU;
        for (const u32 candidate : bucket) {
            if (length(mesh.positions[index] - mesh.positions[candidate]) >
                options.position_tolerance) {
                continue;
            }
            if (!mesh.normals.empty() &&
                dot(mesh.normals[index], mesh.normals[candidate]) < cos_limit) {
                // A hard edge. Two vertices in the same place with different normals are two
                // vertices, and merging them is how a cube comes out looking like a ball.
                continue;
            }
            if (!mesh.uvs.empty() &&
                length(mesh.uvs[index] - mesh.uvs[candidate]) > options.uv_tolerance) {
                continue;  // a texture seam
            }
            found = candidate;
            break;
        }
        if (found == 0xFFFFFFFFU) {
            slot_of[index] = static_cast<u32>(unique.size());
            remap[index] = slot_of[index];
            unique.push_back(static_cast<u32>(index));
            bucket.push_back(static_cast<u32>(index));
        } else {
            // The FIRST vertex of a group wins, rather than an average. That makes welding
            // idempotent, which a pipeline that re-runs steps depends on.
            remap[index] = slot_of[found];
        }
    }

    if (unique.size() == count) {
        return usize{0};
    }

    MeshData welded;
    const auto gather = [&](const Array<Vec3>& source,
                            Array<Vec3>& destination) noexcept -> Status {
        if (source.empty()) {
            return ok();
        }
        if (Status reserved = destination.reserve(unique.size()); !reserved) {
            return reserved;
        }
        for (const u32 original : unique) {
            if (Status pushed = destination.push_back(source[original]); !pushed) {
                return pushed;
            }
        }
        return ok();
    };

    if (Status gathered = gather(mesh.positions, welded.positions); !gathered) {
        return make_unexpected(gathered.error());
    }
    if (Status gathered = gather(mesh.normals, welded.normals); !gathered) {
        return make_unexpected(gathered.error());
    }
    for (const u32 original : unique) {
        if (!mesh.uvs.empty()) {
            if (Status pushed = welded.uvs.push_back(mesh.uvs[original]); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        if (!mesh.uv2.empty()) {
            if (Status pushed = welded.uv2.push_back(mesh.uv2[original]); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        if (!mesh.tangents.empty()) {
            if (Status pushed = welded.tangents.push_back(mesh.tangents[original]); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    if (Status reserved = welded.indices.reserve(mesh.indices.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (const u32 index : mesh.indices) {
        if (Status pushed = welded.indices.push_back(remap[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    welded.sections = std::move(mesh.sections);

    const usize removed = count - unique.size();
    mesh = std::move(welded);
    return removed;
}

// --- Normals -------------------------------------------------------------------------------------

Status generate_normals(MeshData& mesh, f32 smoothing_angle_degrees) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    if (mesh.indices.empty()) {
        return ok();
    }
    const usize triangles = mesh.triangle_count();

    // Face normals first: every per-vertex normal is a weighted sum of these, and the smoothing
    // decision is made between pairs of them.
    std::vector<Vec3> face_normals(triangles, Vec3{});
    std::vector<f32> face_areas(triangles, 0.0f);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        const Vec3 a = mesh.positions[mesh.indices[(triangle * 3) + 0]];
        const Vec3 b = mesh.positions[mesh.indices[(triangle * 3) + 1]];
        const Vec3 c = mesh.positions[mesh.indices[(triangle * 3) + 2]];
        const Vec3 unnormalised = triangle_normal(a, b, c);
        const f32 magnitude = length(unnormalised);
        face_areas[triangle] = magnitude * 0.5f;
        face_normals[triangle] = magnitude > 0.0f ? unnormalised * (1.0f / magnitude) : Vec3{};
    }

    // Which triangles touch each vertex.
    std::vector<std::vector<u32>> incident(mesh.vertex_count());
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            incident[mesh.indices[(triangle * 3) + corner]].push_back(static_cast<u32>(triangle));
        }
    }

    const f32 cos_limit = std::cos(math::radians(smoothing_angle_degrees));

    // A vertex whose incident faces disagree by more than the smoothing angle becomes several
    // vertices, one per smoothing group. Splitting rather than averaging is the whole point: an
    // averaged normal across a hard edge is the shading artefact the smoothing angle exists to
    // prevent.
    MeshData built;
    if (Status reserved = built.positions.reserve(mesh.vertex_count()); !reserved) {
        return reserved;
    }
    std::vector<u32> new_indices(mesh.indices.size(), 0);
    /// For each vertex, the (representative face normal, emitted index) pairs already created.
    std::vector<std::vector<std::pair<Vec3, u32>>> groups(mesh.vertex_count());

    for (usize triangle = 0; triangle < triangles; ++triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 vertex = mesh.indices[(triangle * 3) + corner];
            const Vec3 face = face_normals[triangle];
            u32 emitted = 0xFFFFFFFFU;
            for (const auto& [representative, slot] : groups[vertex]) {
                if (dot(representative, face) >= cos_limit) {
                    emitted = slot;
                    break;
                }
            }
            if (emitted == 0xFFFFFFFFU) {
                emitted = static_cast<u32>(built.positions.size());
                if (Status pushed = built.positions.push_back(mesh.positions[vertex]); !pushed) {
                    return pushed;
                }
                if (!mesh.uvs.empty()) {
                    if (Status pushed = built.uvs.push_back(mesh.uvs[vertex]); !pushed) {
                        return pushed;
                    }
                }
                if (!mesh.uv2.empty()) {
                    if (Status pushed = built.uv2.push_back(mesh.uv2[vertex]); !pushed) {
                        return pushed;
                    }
                }
                // Accumulate the group's normal over the faces that join it, area-weighted.
                Vec3 accumulated{};
                for (const u32 other : incident[vertex]) {
                    if (dot(face_normals[other], face) >= cos_limit) {
                        accumulated = accumulated + (face_normals[other] * face_areas[other]);
                    }
                }
                const f32 magnitude = length(accumulated);
                if (Status pushed = built.normals.push_back(
                        magnitude > 0.0f ? accumulated * (1.0f / magnitude) : face);
                    !pushed) {
                    return pushed;
                }
                groups[vertex].emplace_back(face, emitted);
            }
            new_indices[(triangle * 3) + corner] = emitted;
        }
    }

    if (Status reserved = built.indices.reserve(new_indices.size()); !reserved) {
        return reserved;
    }
    for (const u32 index : new_indices) {
        if (Status pushed = built.indices.push_back(index); !pushed) {
            return pushed;
        }
    }
    built.sections = std::move(mesh.sections);
    // Tangents are dropped rather than remapped: they were computed against the old vertex set and
    // a split vertex has no tangent of its own. `generate_tangents` is the step that follows, and
    // carrying a stale basis forward would be a normal map that is wrong only on hard edges.
    mesh = std::move(built);
    return ok();
}

Status generate_tangents(MeshData& mesh) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    if (mesh.normals.empty() || mesh.uvs.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "tangents need normals and texture coordinates; generate normals first");
    }
    if (Status resized = mesh.tangents.resize(mesh.vertex_count()); !resized) {
        return resized;
    }
    return geom::generate_tangents(mesh.positions.data(), mesh.normals.data(), mesh.uvs.data(),
                                   mesh.vertex_count(), mesh.indices.data(), mesh.indices.size(),
                                   mesh.tangents.data());
}

// --- Cache and fetch -----------------------------------------------------------------------------

namespace {

/// Forsyth's score for a vertex, from its position in a simulated FIFO cache and how many triangles
/// it still has to contribute to.
///
/// The constants are his and are not tuned here: they were fitted against measured hardware, and
/// re-deriving them would be work with no result. A vertex not in the cache scores nothing from its
/// position; one at the very front scores less than one a few slots back, because the front one is
/// about to be used again anyway.
constexpr usize kCacheSize = 32;
constexpr f32 kCacheDecayPower = 1.5f;
constexpr f32 kLastTriangleScore = 0.75f;
constexpr f32 kValenceBoostScale = 2.0f;
constexpr f32 kValenceBoostPower = -0.5f;

[[nodiscard]] f32 vertex_score(i32 cache_position, u32 remaining_triangles) noexcept {
    if (remaining_triangles == 0) {
        return -1.0f;
    }
    f32 score = 0.0f;
    if (cache_position >= 0) {
        if (cache_position < 3) {
            score = kLastTriangleScore;
        } else {
            const f32 scaler = 1.0f / static_cast<f32>(kCacheSize - 3);
            score = 1.0f - (static_cast<f32>(cache_position - 3) * scaler);
            score = std::pow(score, kCacheDecayPower);
        }
    }
    score +=
        kValenceBoostScale * std::pow(static_cast<f32>(remaining_triangles), kValenceBoostPower);
    return score;
}

}  // namespace

Status optimise_vertex_cache(MeshData& mesh) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    const usize triangles = mesh.triangle_count();
    if (triangles < 2) {
        return ok();
    }

    const usize vertices = mesh.vertex_count();
    std::vector<std::vector<u32>> incident(vertices);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            incident[mesh.indices[(triangle * 3) + corner]].push_back(static_cast<u32>(triangle));
        }
    }

    std::vector<u32> remaining(vertices, 0);
    for (usize vertex = 0; vertex < vertices; ++vertex) {
        remaining[vertex] = static_cast<u32>(incident[vertex].size());
    }
    std::vector<i32> cache_position(vertices, -1);
    std::vector<f32> score(vertices, 0.0f);
    for (usize vertex = 0; vertex < vertices; ++vertex) {
        score[vertex] = vertex_score(-1, remaining[vertex]);
    }
    std::vector<f32> triangle_score(triangles, 0.0f);
    std::vector<bool> emitted(triangles, false);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        triangle_score[triangle] = score[mesh.indices[(triangle * 3) + 0]] +
                                   score[mesh.indices[(triangle * 3) + 1]] +
                                   score[mesh.indices[(triangle * 3) + 2]];
    }

    // The simulated post-transform cache, as a fixed array rather than a vector. A vector with an
    // insert-at-front would say the same thing and trips a false `-Wnull-dereference` inside
    // libstdc++ under -O2, which this tree compiles as an error; a fixed array is also the faster
    // of the two for thirty-two entries.
    u32 cache[kCacheSize] = {};
    usize cache_count = 0;
    std::vector<u32> order;
    order.reserve(triangles);

    // A full scan for the best triangle each step is O(n²) and is what Forsyth's paper replaces
    // with a "recently used" shortlist. The shortlist is kept here: only triangles touching a
    // vertex that just entered the cache can have improved, so the scan is over those, and the full
    // scan is the fallback for the first triangle and for the case where every candidate is already
    // emitted.
    u32 best = 0xFFFFFFFFU;
    for (usize step = 0; step < triangles; ++step) {
        if (best == 0xFFFFFFFFU) {
            f32 best_score = -1.0f;
            for (usize triangle = 0; triangle < triangles; ++triangle) {
                if (!emitted[triangle] && triangle_score[triangle] > best_score) {
                    best_score = triangle_score[triangle];
                    best = static_cast<u32>(triangle);
                }
            }
        }
        if (best == 0xFFFFFFFFU) {
            break;
        }

        emitted[best] = true;
        order.push_back(best);

        for (usize corner = 0; corner < 3; ++corner) {
            const u32 vertex = mesh.indices[(static_cast<usize>(best) * 3) + corner];
            remaining[vertex] = remaining[vertex] > 0 ? remaining[vertex] - 1 : 0;

            // Move the vertex to the front of the simulated cache, evicting the tail when it is
            // full.
            usize slot = cache_count;
            for (usize probe = 0; probe < cache_count; ++probe) {
                if (cache[probe] == vertex) {
                    slot = probe;
                    break;
                }
            }
            if (slot == cache_count) {
                if (cache_count < kCacheSize) {
                    ++cache_count;
                    slot = cache_count - 1;
                } else {
                    slot = kCacheSize - 1;
                    const u32 evicted = cache[slot];
                    cache_position[evicted] = -1;
                    score[evicted] = vertex_score(-1, remaining[evicted]);
                }
            }
            for (usize shift = slot; shift > 0; --shift) {
                cache[shift] = cache[shift - 1];
            }
            cache[0] = vertex;
        }
        for (usize slot = 0; slot < cache_count; ++slot) {
            cache_position[cache[slot]] = static_cast<i32>(slot);
            score[cache[slot]] = vertex_score(static_cast<i32>(slot), remaining[cache[slot]]);
        }

        best = 0xFFFFFFFFU;
        f32 best_score = -1.0f;
        for (usize slot = 0; slot < cache_count; ++slot) {
            const u32 vertex = cache[slot];
            for (const u32 triangle : incident[vertex]) {
                if (emitted[triangle]) {
                    continue;
                }
                triangle_score[triangle] = score[mesh.indices[(triangle * 3) + 0]] +
                                           score[mesh.indices[(triangle * 3) + 1]] +
                                           score[mesh.indices[(triangle * 3) + 2]];
                if (triangle_score[triangle] > best_score) {
                    best_score = triangle_score[triangle];
                    best = triangle;
                }
            }
        }
    }

    // Rebuild the index list, and the sections with it. A section is a run of triangles, so the
    // permutation is applied within each section rather than across the whole mesh — reordering
    // across a material boundary would put one material's triangles inside another's draw.
    Array<u32> reordered;
    if (Status reserved = reordered.reserve(mesh.indices.size()); !reserved) {
        return reserved;
    }
    if (mesh.sections.empty()) {
        for (const u32 triangle : order) {
            for (usize corner = 0; corner < 3; ++corner) {
                if (Status pushed = reordered.push_back(
                        mesh.indices[(static_cast<usize>(triangle) * 3) + corner]);
                    !pushed) {
                    return pushed;
                }
            }
        }
    } else {
        for (const MeshSection& section : mesh.sections) {
            const u32 first = section.first_index / 3;
            const u32 last = first + (section.index_count / 3);
            for (const u32 triangle : order) {
                if (triangle < first || triangle >= last) {
                    continue;
                }
                for (usize corner = 0; corner < 3; ++corner) {
                    if (Status pushed = reordered.push_back(
                            mesh.indices[(static_cast<usize>(triangle) * 3) + corner]);
                        !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }
    mesh.indices = std::move(reordered);
    return ok();
}

Status optimise_vertex_fetch(MeshData& mesh) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    const usize vertices = mesh.vertex_count();
    if (vertices == 0) {
        return ok();
    }

    std::vector<u32> remap(vertices, 0xFFFFFFFFU);
    std::vector<u32> order;
    order.reserve(vertices);
    for (const u32 index : mesh.indices) {
        if (remap[index] == 0xFFFFFFFFU) {
            remap[index] = static_cast<u32>(order.size());
            order.push_back(index);
        }
    }
    // A vertex no triangle references is dropped here rather than carried. That is a real reduction
    // on imported content — an exporter that wrote every control point of a subdivision cage leaves
    // plenty — and it is safe precisely because nothing references it.
    if (order.size() == vertices) {
        bool already_ordered = true;
        for (usize vertex = 0; vertex < vertices && already_ordered; ++vertex) {
            already_ordered = remap[vertex] == static_cast<u32>(vertex);
        }
        if (already_ordered) {
            return ok();
        }
    }

    MeshData built;
    for (const u32 original : order) {
        if (Status pushed = built.positions.push_back(mesh.positions[original]); !pushed) {
            return pushed;
        }
        if (!mesh.normals.empty()) {
            if (Status pushed = built.normals.push_back(mesh.normals[original]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.uvs.empty()) {
            if (Status pushed = built.uvs.push_back(mesh.uvs[original]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.uv2.empty()) {
            if (Status pushed = built.uv2.push_back(mesh.uv2[original]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.tangents.empty()) {
            if (Status pushed = built.tangents.push_back(mesh.tangents[original]); !pushed) {
                return pushed;
            }
        }
    }
    if (Status reserved = built.indices.reserve(mesh.indices.size()); !reserved) {
        return reserved;
    }
    for (const u32 index : mesh.indices) {
        if (Status pushed = built.indices.push_back(remap[index]); !pushed) {
            return pushed;
        }
    }
    built.sections = std::move(mesh.sections);
    mesh = std::move(built);
    return ok();
}

// --- Simplification ------------------------------------------------------------------------------

Expected<SimplifyReport, Error> simplify(MeshData& mesh, const SimplifyOptions& options) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (options.target_ratio <= 0.0f || options.target_ratio > 1.0f) {
        return fail(ErrorCode::InvalidArgument, "a target ratio is in (0, 1]");
    }

    SimplifyReport report;
    report.triangles_before = mesh.triangle_count();
    report.vertices_before = mesh.vertex_count();
    report.triangles_after = report.triangles_before;
    report.vertices_after = report.vertices_before;
    if (report.triangles_before < 4) {
        return report;
    }

    const usize vertices = mesh.vertex_count();
    const usize triangles = mesh.triangle_count();
    const auto target =
        static_cast<usize>(std::lround(static_cast<f32>(triangles) * options.target_ratio));

    // POSITION GROUPS. Two vertices at the same place with different UVs are one point of geometry
    // and two vertices of the mesh, and a collapse must move both or neither — otherwise a seam
    // tears open. Grouping by exact position is right here because the importer has already welded.
    std::vector<u32> group_of(vertices, 0);
    std::vector<std::vector<u32>> group_members;
    {
        std::unordered_map<Lattice, u32, LatticeHash> lookup;
        lookup.reserve(vertices);
        for (usize vertex = 0; vertex < vertices; ++vertex) {
            const Lattice cell = cell_of(mesh.positions[vertex], 1.0e-6f);
            const auto found = lookup.find(cell);
            if (found == lookup.end()) {
                const auto group = static_cast<u32>(group_members.size());
                lookup.emplace(cell, group);
                group_members.emplace_back();
                group_members.back().push_back(static_cast<u32>(vertex));
                group_of[vertex] = group;
            } else {
                group_of[vertex] = found->second;
                group_members[found->second].push_back(static_cast<u32>(vertex));
            }
        }
    }
    const usize groups = group_members.size();

    std::vector<Vec3> group_position(groups, Vec3{});
    for (usize group = 0; group < groups; ++group) {
        group_position[group] = mesh.positions[group_members[group][0]];
    }

    // QUADRICS, one per group, area-weighted over the faces that touch it.
    std::vector<Quadric> quadrics(groups);
    std::vector<u32> face_groups(triangles * 3, 0);
    std::vector<bool> face_alive(triangles, true);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            face_groups[(triangle * 3) + corner] = group_of[mesh.indices[(triangle * 3) + corner]];
        }
        const Vec3 a = group_position[face_groups[(triangle * 3) + 0]];
        const Vec3 b = group_position[face_groups[(triangle * 3) + 1]];
        const Vec3 c = group_position[face_groups[(triangle * 3) + 2]];
        const Vec3 unnormalised = triangle_normal(a, b, c);
        const f32 magnitude = length(unnormalised);
        if (magnitude <= 0.0f) {
            face_alive[triangle] = false;
            continue;
        }
        const Vec3 normal = unnormalised * (1.0f / magnitude);
        const Quadric plane =
            Quadric::from_plane(normal, -dot(normal, a), static_cast<f64>(magnitude) * 0.5);
        for (usize corner = 0; corner < 3; ++corner) {
            quadrics[face_groups[(triangle * 3) + corner]].add(plane);
        }
    }

    // BOUNDARY AND SEAM CONSTRAINTS. An edge used by one face is a geometric boundary; an edge
    // whose two endpoints belong to more than one attribute vertex is an attribute seam. Both get a
    // plane through the edge and perpendicular to the face, weighted heavily, which is what stops a
    // collapse from moving them and lets a collapse ALONG them proceed.
    std::unordered_map<EdgeKey, u32, EdgeKeyHash> edge_uses;
    edge_uses.reserve(triangles * 3);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        if (!face_alive[triangle]) {
            continue;
        }
        for (usize corner = 0; corner < 3; ++corner) {
            u32 first = face_groups[(triangle * 3) + corner];
            u32 second = face_groups[(triangle * 3) + ((corner + 1) % 3)];
            if (first > second) {
                std::swap(first, second);
            }
            ++edge_uses[EdgeKey{first, second}];
        }
    }

    if (options.preserve_boundary) {
        constexpr f64 kBoundaryWeight = 1000.0;
        for (usize triangle = 0; triangle < triangles; ++triangle) {
            if (!face_alive[triangle]) {
                continue;
            }
            const Vec3 a = group_position[face_groups[(triangle * 3) + 0]];
            const Vec3 b = group_position[face_groups[(triangle * 3) + 1]];
            const Vec3 c = group_position[face_groups[(triangle * 3) + 2]];
            const Vec3 face_normal = normalized_or(triangle_normal(a, b, c), Vec3{0, 1, 0});
            for (usize corner = 0; corner < 3; ++corner) {
                const u32 from = face_groups[(triangle * 3) + corner];
                const u32 to = face_groups[(triangle * 3) + ((corner + 1) % 3)];
                const EdgeKey key{std::min(from, to), std::max(from, to)};
                if (edge_uses[key] != 1) {
                    continue;
                }
                const Vec3 along = group_position[to] - group_position[from];
                const Vec3 constraint = normalized_or(cross(along, face_normal), face_normal);
                const Quadric plane = Quadric::from_plane(
                    constraint, -dot(constraint, group_position[from]), kBoundaryWeight);
                quadrics[from].add(plane);
                quadrics[to].add(plane);
            }
        }
    }

    // A group that carries several attribute vertices is a seam point. Locking it — rather than
    // adding a plane — is the conservative reading of "preserved or collapsed only along the seam",
    // and it is the one that cannot tear a UV chart. The cost is that a seam-heavy mesh simplifies
    // less than it could; the alternative costs a texture that slides.
    std::vector<bool> locked(groups, false);
    if (options.preserve_seams) {
        for (usize group = 0; group < groups; ++group) {
            locked[group] = group_members[group].size() > 1;
        }
    }

    // Which faces touch each group, so a collapse costs its own neighbourhood rather than a scan of
    // the whole mesh. Without this the flip check and the face update are each O(triangles) per
    // collapse, which is O(n²) overall and turns a two-second import into a two-minute one.
    std::vector<std::vector<u32>> group_faces(groups);
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        if (!face_alive[triangle]) {
            continue;
        }
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 group = face_groups[(triangle * 3) + corner];
            if (group_faces[group].empty() || group_faces[group].back() != triangle) {
                group_faces[group].push_back(static_cast<u32>(triangle));
            }
        }
    }

    std::vector<u32> version(groups, 0);
    std::vector<bool> group_alive(groups, true);
    std::vector<u32> collapsed_to(groups, 0);
    for (usize group = 0; group < groups; ++group) {
        collapsed_to[group] = static_cast<u32>(group);
    }

    const auto resolve = [&](u32 group) noexcept -> u32 {
        while (collapsed_to[group] != group) {
            group = collapsed_to[group];
        }
        return group;
    };

    const auto cost_of = [&](u32 from, u32 to, Vec3& out_target) noexcept -> f64 {
        Quadric combined = quadrics[from];
        combined.add(quadrics[to]);
        // The best of three candidates rather than the exact minimiser. Solving the 3x3 system is
        // better when it is well conditioned and produces a point far outside the mesh when it is
        // not, which on real content is often; the endpoints and the midpoint are always sane and
        // are what a simplifier ships with when it must never produce a spike.
        const Vec3 candidates[3] = {group_position[from], group_position[to],
                                    (group_position[from] + group_position[to]) * 0.5f};
        f64 best = combined.evaluate(candidates[0]);
        out_target = candidates[0];
        for (usize index = 1; index < 3; ++index) {
            const f64 value = combined.evaluate(candidates[index]);
            if (value < best) {
                best = value;
                out_target = candidates[index];
            }
        }
        return best < 0.0 ? 0.0 : best;
    };

    std::vector<Collapse> heap;
    heap.reserve(edge_uses.size());
    u32 serial = 0;
    for (const auto& [key, uses] : edge_uses) {
        (void)uses;
        Collapse candidate;
        candidate.from = key.low;
        candidate.to = key.high;
        candidate.serial = serial++;
        candidate.cost = cost_of(key.low, key.high, candidate.target);
        heap.push_back(candidate);
    }
    // Sorted once, then maintained as a heap. Sorting first rather than pushing one at a time makes
    // the initial order deterministic even though `edge_uses` iterates in an unspecified order —
    // the serial numbers are assigned in that order, so the sort below is what fixes it.
    std::ranges::sort(heap, [](const Collapse& a, const Collapse& b) noexcept {
        if (a.from != b.from) {
            return a.from < b.from;
        }
        return a.to < b.to;
    });
    for (usize index = 0; index < heap.size(); ++index) {
        heap[index].serial = static_cast<u32>(index);
    }
    std::ranges::make_heap(heap, CollapseOrder{});

    usize live_triangles = 0;
    for (const bool alive : face_alive) {
        live_triangles += alive ? 1U : 0U;
    }

    while (live_triangles > target && !heap.empty()) {
        std::ranges::pop_heap(heap, CollapseOrder{});
        const Collapse candidate = heap.back();
        heap.pop_back();

        if (!group_alive[candidate.from] || !group_alive[candidate.to]) {
            continue;
        }
        if (candidate.from_version != version[candidate.from] ||
            candidate.to_version != version[candidate.to]) {
            // Stale: an endpoint has moved since this cost was computed. Recompute and re-insert
            // rather than discard, which is what keeps the heap complete without touching every
            // entry on each collapse.
            Collapse refreshed = candidate;
            refreshed.from_version = version[candidate.from];
            refreshed.to_version = version[candidate.to];
            refreshed.cost = cost_of(candidate.from, candidate.to, refreshed.target);
            heap.push_back(refreshed);
            std::ranges::push_heap(heap, CollapseOrder{});
            continue;
        }
        // Both ends seam points, joined by an edge of the original mesh: a collapse ALONG the seam,
        // which the requirement permits. Exactly one end locked is a collapse ACROSS it, which
        // tears the chart, and is refused.
        if (locked[candidate.from] != locked[candidate.to]) {
            continue;
        }
        if (options.error_bound > 0.0f && candidate.cost > static_cast<f64>(options.error_bound)) {
            report.bounded = true;
            break;
        }

        // Refuse a collapse that would flip a triangle. A flipped face is a hole in the silhouette
        // and is far more visible than the geometric error the quadric is measuring.
        bool flips = false;
        for (usize pass = 0; pass < 2 && !flips; ++pass) {
            const std::vector<u32>& faces =
                pass == 0 ? group_faces[candidate.from] : group_faces[candidate.to];
            for (const u32 triangle : faces) {
                if (!face_alive[triangle]) {
                    continue;
                }
                const u32 corners[3] = {face_groups[(triangle * 3) + 0],
                                        face_groups[(triangle * 3) + 1],
                                        face_groups[(triangle * 3) + 2]};
                usize touching = 0;
                for (const u32 corner : corners) {
                    touching += corner == candidate.from || corner == candidate.to ? 1U : 0U;
                }
                if (touching == 0) {
                    continue;
                }
                if (touching >= 2) {
                    continue;  // this face is removed by the collapse rather than moved
                }
                Vec3 moved[3];
                for (usize corner = 0; corner < 3; ++corner) {
                    moved[corner] =
                        corners[corner] == candidate.from || corners[corner] == candidate.to
                            ? candidate.target
                            : group_position[corners[corner]];
                }
                const Vec3 before =
                    triangle_normal(group_position[corners[0]], group_position[corners[1]],
                                    group_position[corners[2]]);
                const Vec3 after = triangle_normal(moved[0], moved[1], moved[2]);
                if (dot(before, after) <= 0.0f) {
                    flips = true;
                    break;
                }
            }
        }
        if (flips) {
            continue;
        }

        // Apply.
        group_position[candidate.to] = candidate.target;
        quadrics[candidate.to].add(quadrics[candidate.from]);
        group_alive[candidate.from] = false;
        collapsed_to[candidate.from] = candidate.to;
        locked[candidate.to] = locked[candidate.to] || locked[candidate.from];
        ++version[candidate.to];
        report.error = std::max(report.error, static_cast<f32>(candidate.cost));

        for (const u32 triangle : group_faces[candidate.from]) {
            if (!face_alive[triangle]) {
                continue;
            }
            for (usize corner = 0; corner < 3; ++corner) {
                u32& slot = face_groups[(static_cast<usize>(triangle) * 3) + corner];
                if (slot == candidate.from) {
                    slot = candidate.to;
                }
            }
            const u32 a = face_groups[(triangle * 3) + 0];
            const u32 b = face_groups[(triangle * 3) + 1];
            const u32 c = face_groups[(triangle * 3) + 2];
            if (a == b || b == c || a == c) {
                face_alive[triangle] = false;
                --live_triangles;
            }
        }
        // The surviving group inherits the removed one's faces. Duplicates are harmless — every
        // loop above skips a dead face and re-checks the corners it reads — and de-duplicating
        // would cost more than the occasional repeat visit.
        group_faces[candidate.to].insert(group_faces[candidate.to].end(),
                                         group_faces[candidate.from].begin(),
                                         group_faces[candidate.from].end());
        group_faces[candidate.from].clear();
    }

    if (live_triangles == report.triangles_before) {
        return report;
    }

    // REBUILD. Attribute vertices follow their group: a vertex whose group survived keeps its own
    // attributes and takes the group's new position, and a vertex whose group was collapsed is
    // replaced by ONE representative of the group it merged into. Picking a representative rather
    // than merging attributes is what keeps a UV chart intact — averaging two charts' coordinates
    // is exactly the distortion the seam preservation exists to avoid.
    std::vector<u32> representative(groups, 0xFFFFFFFFU);
    MeshData built;
    std::vector<u32> vertex_remap(vertices, 0xFFFFFFFFU);
    const auto emit = [&](u32 vertex) noexcept -> Status {
        if (Status pushed = built.positions.push_back(group_position[resolve(group_of[vertex])]);
            !pushed) {
            return pushed;
        }
        if (!mesh.normals.empty()) {
            if (Status pushed = built.normals.push_back(mesh.normals[vertex]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.uvs.empty()) {
            if (Status pushed = built.uvs.push_back(mesh.uvs[vertex]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.uv2.empty()) {
            if (Status pushed = built.uv2.push_back(mesh.uv2[vertex]); !pushed) {
                return pushed;
            }
        }
        if (!mesh.tangents.empty()) {
            if (Status pushed = built.tangents.push_back(mesh.tangents[vertex]); !pushed) {
                return pushed;
            }
        }
        return ok();
    };

    for (usize triangle = 0; triangle < triangles; ++triangle) {
        if (!face_alive[triangle]) {
            continue;
        }
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 vertex = mesh.indices[(triangle * 3) + corner];
            const u32 group = resolve(group_of[vertex]);
            u32 emitted = 0xFFFFFFFFU;
            if (group == group_of[vertex]) {
                if (vertex_remap[vertex] == 0xFFFFFFFFU) {
                    vertex_remap[vertex] = static_cast<u32>(built.positions.size());
                    if (Status pushed = emit(vertex); !pushed) {
                        return make_unexpected(pushed.error());
                    }
                }
                emitted = vertex_remap[vertex];
            } else {
                if (representative[group] == 0xFFFFFFFFU) {
                    const u32 stand_in = group_members[group][0];
                    representative[group] = static_cast<u32>(built.positions.size());
                    if (Status pushed = emit(stand_in); !pushed) {
                        return make_unexpected(pushed.error());
                    }
                }
                emitted = representative[group];
            }
            if (Status pushed = built.indices.push_back(emitted); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    // Sections are dropped rather than guessed: a collapse can remove every triangle of a section,
    // and a section list that still claimed a range would fail `validate`. The importer that had
    // sections rebuilds them per LOD from the material each triangle carried, which it knows and
    // this function does not.
    mesh = std::move(built);
    report.triangles_after = mesh.triangle_count();
    report.vertices_after = mesh.vertex_count();
    return report;
}

Status convex_hull(const MeshData& mesh, MeshData& out) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    const usize count = mesh.vertex_count();
    if (count < 4) {
        return fail(ErrorCode::InvalidArgument, "a convex hull needs at least four points");
    }
    Array<u32> hull_indices;
    if (Status resized = hull_indices.resize(count * 6); !resized) {
        return resized;
    }
    Expected<usize, Error> produced = geom::convex_hull_3d(
        mesh.positions.data(), count, 1.0e-5f, hull_indices.data(), hull_indices.size());
    if (!produced) {
        return make_unexpected(produced.error());
    }

    out.clear();
    // The hull references the ORIGINAL point cloud, so the output carries every source position and
    // only the referenced ones survive `optimise_vertex_fetch`. Compacting here would mean a second
    // remap for no benefit: a collision hull is run through the fetch optimiser like any other
    // mesh.
    if (Status appended = out.positions.append(Span<const Vec3>(mesh.positions.data(), count));
        !appended) {
        return appended;
    }
    if (Status appended =
            out.indices.append(Span<const u32>(hull_indices.data(), produced.value()));
        !appended) {
        return appended;
    }
    return optimise_vertex_fetch(out);
}

}  // namespace cy::import
