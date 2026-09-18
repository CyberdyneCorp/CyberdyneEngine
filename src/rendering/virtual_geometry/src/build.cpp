#include <cy/rendering/virtual_geometry/build.h>

#include <cy/core/memory/hash_map.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/simplify.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::vg {

namespace {

// ================================================================================================
// THE WELDED INDEX SPACE
// ================================================================================================
//
// Every topological decision in this file — adjacency, group boundaries, the locked set, the
// watertightness check — is made over WELDED vertex indices, and none over the source mesh's own.
// The reason is the requirement rather than tidiness: an imported mesh routinely stores one corner
// three times, once per adjacent face, because the attributes differ. Two clusters that split such
// a corner would have no vertex in common, so the boundary between them would be empty, so nothing
// would be locked, so simplification would move the shared corner in one group and not the other.
// That is a crack, and it would appear only on imported meshes and never on a mesh a test wrote by
// hand.
//
// Welding is by quantised position and by nothing else. Attributes are carried from the first
// source vertex that mapped to a welded index; `weld_epsilon` is a topology tolerance, not a
// simplification one.
//
// ================================================================================================
// A LEVEL IS A LIST OF CLUSTER INDICES, NOT A RANGE
// ================================================================================================
//
// A cluster that ends up in a group of one, or in a group simplification made no progress on, is
// still part of the surface and must appear at the next level. Promoting it by COPYING would put a
// cluster in the asset that can never be selected — its own upper and lower tests would be the same
// number — and would do it once per level. So a level is an index list and promotion is leaving the
// index in the next list. Every cluster in `clusters` is therefore reachable and selectable.

/// A map from a quantised lattice cell to a dense identifier.
///
/// IT EXISTS BECAUSE THE OBVIOUS HASH IS WRONG HERE, and the failure is quiet. Mixing three
/// coordinates with `x*A ^ y*B ^ z*C` cancels on symmetric geometry: measured on a 162-vertex
/// icosphere, that hash produced 114 distinct identifiers — a quarter of the mesh's vertices
/// silently merged with another. In the weld that splits a shared corner into two, which unlocks a
/// group boundary and puts a crack in the surface; in the watertightness check it merges two edges
/// and reports a closed cut that is not one. Both failures look like a simplifier defect.
///
/// So the mix is sequential and avalanching, and a hit is CONFIRMED against the stored cell before
/// it is believed. A genuine 64-bit collision re-probes rather than merging, so the structure is
/// correct rather than merely unlikely to be wrong.
class Lattice {
public:
    explicit Lattice(Allocator& allocator) noexcept : map_(allocator), cells_(allocator) {}

    struct Cell {
        i64 x = 0;
        i64 y = 0;
        i64 z = 0;

        [[nodiscard]] bool operator==(const Cell& other) const noexcept {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct Lookup {
        u32 id = 0;
        bool fresh = false;
    };

    /// The identifier for `cell`, allocating one if this is the first time it has been seen.
    [[nodiscard]] Expected<Lookup, Error> intern(const Cell& cell) noexcept {
        u64 probe = hash(cell);
        for (u32 attempt = 0; attempt < 64; ++attempt) {
            const u32* found = map_.find(probe);
            if (found == nullptr) {
                const u32 fresh = static_cast<u32>(cells_.size());
                if (Status pushed = cells_.push_back(cell); !pushed) {
                    return make_unexpected(pushed.error());
                }
                Expected<u32*, Error> inserted = map_.insert(probe, fresh);
                if (!inserted) {
                    return make_unexpected(inserted.error());
                }
                return Lookup{fresh, true};
            }
            if (cells_[*found] == cell) {
                return Lookup{*found, false};
            }
            probe = mix(probe + 0x9E3779B97F4A7C15ULL);
        }
        return fail(ErrorCode::Internal,
                    "Lattice: 64 consecutive hash collisions, which is not a state this structure "
                    "can reach with a working mix");
    }

    [[nodiscard]] usize size() const noexcept { return cells_.size(); }

private:
    [[nodiscard]] static u64 mix(u64 value) noexcept {
        value ^= value >> 33U;
        value *= 0xFF51AFD7ED558CCDULL;
        value ^= value >> 33U;
        value *= 0xC4CEB9FE1A85EC53ULL;
        value ^= value >> 33U;
        return value;
    }

    [[nodiscard]] static u64 hash(const Cell& cell) noexcept {
        u64 value = 0xCBF29CE484222325ULL;
        for (const i64 coordinate : {cell.x, cell.y, cell.z}) {
            value ^= static_cast<u64>(coordinate);
            value *= 0x100000001B3ULL;
        }
        return mix(value);
    }

    HashMap<u64, u32> map_;
    Array<Cell> cells_;
};

struct WeldedTriangle {
    u32 v[3] = {};
    u32 material = 0;
};

/// One cluster while it is being built: triangles in the welded index space. It becomes a `Cluster`
/// at the end, when the output layout is known.
struct BuildCluster {
    explicit BuildCluster(Allocator& allocator) noexcept : triangles(allocator) {}

    BuildCluster(const BuildCluster&) = delete;
    BuildCluster& operator=(const BuildCluster&) = delete;
    BuildCluster(BuildCluster&&) noexcept = default;
    BuildCluster& operator=(BuildCluster&&) noexcept = default;

    Array<u32> triangles;  // 3 welded indices each
    u32 material = 0;
    u8 level = 0;
    f32 lod_error = 0.0F;
    ErrorSphere lod_sphere;
    f32 parent_error = kRootError;
    ErrorSphere parent_sphere;
    u32 group = kInvalidGroup;       // the group this cluster was simplified INTO
    u32 from_group = kInvalidGroup;  // the group that produced it
};

/// A 30-bit Morton code from a point normalised into the unit cube. Ten bits per axis is a lattice
/// of 1024 cells per side: finer than any cluster and coarse enough to be exact in f32.
[[nodiscard]] u32 morton_code(Vec3 unit) noexcept {
    auto spread = [](f32 value) noexcept -> u32 {
        const f32 scaled = std::clamp(value * 1023.0F, 0.0F, 1023.0F);
        u32 bits = static_cast<u32>(scaled);
        bits = (bits | (bits << 16U)) & 0x030000FFU;
        bits = (bits | (bits << 8U)) & 0x0300F00FU;
        bits = (bits | (bits << 4U)) & 0x030C30C3U;
        bits = (bits | (bits << 2U)) & 0x09249249U;
        return bits;
    };
    return spread(unit.x) | (spread(unit.y) << 1U) | (spread(unit.z) << 2U);
}

[[nodiscard]] u64 edge_key(u32 a, u32 b) noexcept {
    const u32 lo = a < b ? a : b;
    const u32 hi = a < b ? b : a;
    return (static_cast<u64>(lo) << 32U) | static_cast<u64>(hi);
}

/// A sphere containing every input sphere. Not minimal — a minimal enclosing sphere is Welzl's
/// randomised algorithm — but containment is the whole of what the monotonicity invariant needs,
/// and this is deterministic, which Welzl's is not without care.
[[nodiscard]] ErrorSphere enclose(Span<const ErrorSphere> spheres) noexcept {
    ErrorSphere result;
    if (spheres.empty()) {
        return result;
    }
    Vec3 sum{0.0F, 0.0F, 0.0F};
    for (const ErrorSphere& sphere : spheres) {
        sum = sum + sphere.center;
    }
    result.center = sum * (1.0F / static_cast<f32>(spheres.size()));
    for (const ErrorSphere& sphere : spheres) {
        const Vec3 delta = sphere.center - result.center;
        const f32 reach = std::sqrt(dot(delta, delta)) + sphere.radius;
        result.radius = reach > result.radius ? reach : result.radius;
    }
    return result;
}

[[nodiscard]] ErrorSphere sphere_of_points(Span<const Vec3> points) noexcept {
    ErrorSphere result;
    if (points.empty()) {
        return result;
    }
    Vec3 sum{0.0F, 0.0F, 0.0F};
    for (const Vec3 point : points) {
        sum = sum + point;
    }
    result.center = sum * (1.0F / static_cast<f32>(points.size()));
    for (const Vec3 point : points) {
        const Vec3 delta = point - result.center;
        const f32 reach = std::sqrt(dot(delta, delta));
        result.radius = reach > result.radius ? reach : result.radius;
    }
    return result;
}

/// The smallest amount by which a parent's error must exceed its children's. Equal errors make the
/// two selection tests mutually exclusive and put a hole in the surface at exactly that threshold;
/// see the argument in cluster.h.
[[nodiscard]] f32 strictly_above(f32 value) noexcept {
    const f32 relative = value * 1.0e-3F;
    const f32 step = relative > 1.0e-7F ? relative : 1.0e-7F;
    return value + step;
}

/// Sort key for the spatial ordering. Material first, because a cluster is single-material by
/// construction; then the Morton code, which is what makes a run of triangles a compact region;
/// then the triangle's own index, so equal codes order identically on every run.
struct SortKey {
    u32 material = 0;
    u32 morton = 0;
    u32 triangle = 0;
};

bool sort_key_before(const SortKey& a, const SortKey& b) noexcept {
    if (a.material != b.material) {
        return a.material < b.material;
    }
    if (a.morton != b.morton) {
        return a.morton < b.morton;
    }
    return a.triangle < b.triangle;
}

/// The state one build carries between its stages.
struct Builder {
    explicit Builder(Allocator& allocator) noexcept
        : arena(allocator),
          welded_positions(allocator),
          welded_normals(allocator),
          welded_uvs(allocator),
          triangles(allocator),
          clusters(allocator),
          groups(allocator),
          group_members(allocator),
          group_children(allocator) {}

    Allocator& arena;
    Array<Vec3> welded_positions;
    Array<Vec3> welded_normals;
    Array<Vec2> welded_uvs;
    Array<WeldedTriangle> triangles;
    Array<BuildCluster> clusters;
    Array<Group> groups;
    /// Build-space cluster indices, parallel to `groups`.
    Array<Array<u32>> group_members;
    Array<Array<u32>> group_children;
    Aabb bounds;
    BuildOptions options;
    bool has_normals = false;
    bool has_uvs = false;

    [[nodiscard]] Status weld(const SourceMesh& mesh) noexcept;
    [[nodiscard]] Status partition(Span<const u32> triangle_ids, u8 level, f32 lod_error,
                                   const ErrorSphere& lod_sphere, u32 from_group,
                                   Array<u32>& produced) noexcept;
    [[nodiscard]] Status build_levels() noexcept;
    [[nodiscard]] Status group_level(Span<const u32> level, Array<Array<u32>>& out) noexcept;
    [[nodiscard]] Status locked_set(Span<const u32> level, Span<const u32> members,
                                    Array<u32>& out) noexcept;
    [[nodiscard]] Status simplify_one_group(Span<const u32> level, Span<const u32> members,
                                            u8 level_index, Array<u32>& next) noexcept;
    [[nodiscard]] ErrorSphere cluster_sphere(const BuildCluster& cluster) noexcept;
};

Status Builder::weld(const SourceMesh& mesh) noexcept {
    // f64 throughout the lattice: a weld epsilon of 1e-5 over a mesh at kilometre scale is 1e8
    // lattice cells, which f32 cannot index without collapsing distinct cells onto one.
    const f64 scale =
        1.0 / static_cast<f64>(options.weld_epsilon > 0.0F ? options.weld_epsilon : 1.0e-6F);
    Lattice lattice(arena);
    Array<u32> remap(arena);
    if (Status resized = remap.resize(mesh.positions.size()); !resized) {
        return resized;
    }

    for (usize index = 0; index < mesh.positions.size(); ++index) {
        const Vec3 position = mesh.positions[index];
        const Lattice::Cell cell{std::lround(static_cast<f64>(position.x) * scale),
                                 std::lround(static_cast<f64>(position.y) * scale),
                                 std::lround(static_cast<f64>(position.z) * scale)};
        Expected<Lattice::Lookup, Error> welded = lattice.intern(cell);
        if (!welded) {
            return make_unexpected(welded.error());
        }
        if (welded->fresh) {
            if (Status pushed = welded_positions.push_back(position); !pushed) {
                return pushed;
            }
            if (!mesh.normals.empty()) {
                if (Status pushed = welded_normals.push_back(mesh.normals[index]); !pushed) {
                    return pushed;
                }
            }
            if (!mesh.uvs.empty()) {
                if (Status pushed = welded_uvs.push_back(mesh.uvs[index]); !pushed) {
                    return pushed;
                }
            }
        }
        remap[index] = welded->id;
    }

    const usize triangle_count = mesh.indices.size() / 3;
    if (Status reserved = triangles.reserve(triangle_count); !reserved) {
        return reserved;
    }
    for (usize index = 0; index < triangle_count; ++index) {
        WeldedTriangle triangle;
        for (u32 corner = 0; corner < 3; ++corner) {
            triangle.v[corner] = remap[mesh.indices[(index * 3) + corner]];
        }
        // A triangle two of whose corners welded together has no area and no plane. Dropping it
        // here keeps every later stage able to assume a face has a normal.
        if (triangle.v[0] == triangle.v[1] || triangle.v[1] == triangle.v[2] ||
            triangle.v[0] == triangle.v[2]) {
            continue;
        }
        triangle.material = mesh.triangle_materials.empty() ? 0U : mesh.triangle_materials[index];
        if (Status pushed = triangles.push_back(triangle); !pushed) {
            return pushed;
        }
    }

    bounds = Aabb{};
    for (const Vec3 position : welded_positions) {
        bounds.grow(position);
    }
    has_normals = !welded_normals.empty();
    has_uvs = !welded_uvs.empty();
    return ok();
}

ErrorSphere Builder::cluster_sphere(const BuildCluster& cluster) noexcept {
    Array<Vec3> points(arena);
    for (const u32 vertex : cluster.triangles) {
        if (Status pushed = points.push_back(welded_positions[vertex]); !pushed) {
            return ErrorSphere{};
        }
    }
    return sphere_of_points(points.span());
}

Status Builder::partition(Span<const u32> triangle_ids, u8 level, f32 lod_error,
                          const ErrorSphere& lod_sphere, u32 from_group,
                          Array<u32>& produced) noexcept {
    Array<SortKey> order(arena);
    if (Status reserved = order.reserve(triangle_ids.size()); !reserved) {
        return reserved;
    }
    const Vec3 extent = bounds.size();
    const Vec3 inverse{extent.x > 0.0F ? 1.0F / extent.x : 0.0F,
                       extent.y > 0.0F ? 1.0F / extent.y : 0.0F,
                       extent.z > 0.0F ? 1.0F / extent.z : 0.0F};
    for (const u32 id : triangle_ids) {
        const WeldedTriangle& triangle = triangles[id];
        const Vec3 centroid = (welded_positions[triangle.v[0]] + welded_positions[triangle.v[1]] +
                               welded_positions[triangle.v[2]]) *
                              (1.0F / 3.0F);
        const Vec3 unit{(centroid.x - bounds.min.x) * inverse.x,
                        (centroid.y - bounds.min.y) * inverse.y,
                        (centroid.z - bounds.min.z) * inverse.z};
        if (Status pushed = order.push_back(SortKey{triangle.material, morton_code(unit), id});
            !pushed) {
            return pushed;
        }
    }
    std::ranges::sort(order, sort_key_before);

    HashSet<u32> in_cluster(arena);
    BuildCluster current(arena);
    bool open = false;

    auto close = [&]() noexcept -> Status {
        if (!open) {
            return ok();
        }
        current.level = level;
        current.lod_error = lod_error;
        current.from_group = from_group;
        // Level 0: the geometry is the source and deviates from it by nothing, so the sphere the
        // error is projected through is the cluster's own. Above level 0 the sphere is the
        // producing group's, and every cluster it produced shares it.
        current.lod_sphere = from_group == kInvalidGroup ? cluster_sphere(current) : lod_sphere;
        if (Status pushed = produced.push_back(static_cast<u32>(clusters.size())); !pushed) {
            return pushed;
        }
        if (Status pushed = clusters.push_back(std::move(current)); !pushed) {
            return pushed;
        }
        current = BuildCluster(arena);
        in_cluster.clear();
        open = false;
        return ok();
    };

    for (const SortKey& key : order) {
        const WeldedTriangle& triangle = triangles[key.triangle];
        u32 fresh = 0;
        for (const u32 vertex : triangle.v) {
            if (!in_cluster.contains(vertex)) {
                ++fresh;
            }
        }
        const u32 have = static_cast<u32>(current.triangles.size() / 3);
        const bool full = have >= options.policy.max_triangles ||
                          (in_cluster.size() + fresh) > options.policy.max_vertices;
        if (open && (full || current.material != triangle.material)) {
            if (Status closed = close(); !closed) {
                return closed;
            }
        }
        current.material = triangle.material;
        open = true;
        for (const u32 vertex : triangle.v) {
            if (Status inserted = in_cluster.insert(vertex); !inserted) {
                return inserted;
            }
            if (Status pushed = current.triangles.push_back(vertex); !pushed) {
                return pushed;
            }
        }
    }
    return close();
}

/// Group the clusters of one level. `virtual-geometry`: "neighbouring clusters are collected into
/// **groups**", and "Group membership SHALL be re-partitioned between levels rather than nested
/// rigidly, so that boundaries do not accumulate across the hierarchy".
///
/// Re-partitioning falls out of the method rather than being a step: adjacency is recomputed from
/// the level's own clusters, which are a fresh spatial partition of a freshly simplified surface,
/// so a group at level L+1 bears no relation to one at level L beyond geometry. The suite asserts
/// it by comparing the partitions.
Status Builder::group_level(Span<const u32> level, Array<Array<u32>>& out) noexcept {
    const u32 count = static_cast<u32>(level.size());
    HashMap<u64, u32> edge_owner(arena);
    HashMap<u64, u32> shared(arena);  // (a << 32 | b) -> shared edge count, a < b

    for (u32 index = 0; index < count; ++index) {
        const BuildCluster& cluster = clusters[level[index]];
        for (usize corner = 0; corner < cluster.triangles.size(); corner += 3) {
            for (u32 side = 0; side < 3; ++side) {
                const u64 key = edge_key(cluster.triangles[corner + side],
                                         cluster.triangles[corner + ((side + 1U) % 3U)]);
                u32* owner = edge_owner.find(key);
                if (owner == nullptr) {
                    if (Expected<u32*, Error> inserted = edge_owner.insert(key, index); !inserted) {
                        return make_unexpected(inserted.error());
                    }
                    continue;
                }
                if (*owner == index) {
                    continue;
                }
                const u32 lo = *owner < index ? *owner : index;
                const u32 hi = *owner < index ? index : *owner;
                const u64 pair = (static_cast<u64>(lo) << 32U) | static_cast<u64>(hi);
                if (u32* weight = shared.find(pair); weight != nullptr) {
                    ++*weight;
                } else if (Expected<u32*, Error> inserted = shared.insert(pair, 1U); !inserted) {
                    return make_unexpected(inserted.error());
                }
            }
        }
    }

    Array<Array<u32>> neighbours(arena);
    Array<Array<u32>> weights(arena);
    for (u32 index = 0; index < count; ++index) {
        if (Status pushed = neighbours.push_back(Array<u32>(arena)); !pushed) {
            return pushed;
        }
        if (Status pushed = weights.push_back(Array<u32>(arena)); !pushed) {
            return pushed;
        }
    }
    for (const auto& entry : shared) {
        const u32 lo = static_cast<u32>(entry.key >> 32U);
        const u32 hi = static_cast<u32>(entry.key & 0xFFFFFFFFU);
        if (Status pushed = neighbours[lo].push_back(hi); !pushed) {
            return pushed;
        }
        if (Status pushed = weights[lo].push_back(entry.value); !pushed) {
            return pushed;
        }
        if (Status pushed = neighbours[hi].push_back(lo); !pushed) {
            return pushed;
        }
        if (Status pushed = weights[hi].push_back(entry.value); !pushed) {
            return pushed;
        }
    }

    Array<bool> taken(arena);
    if (Status resized = taken.resize(count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < count; ++index) {
        taken[index] = false;
    }

    for (u32 seed = 0; seed < count; ++seed) {
        if (taken[seed]) {
            continue;
        }
        Array<u32> group(arena);
        if (Status pushed = group.push_back(seed); !pushed) {
            return pushed;
        }
        taken[seed] = true;
        while (group.size() < options.policy.group_size) {
            // The adjacent ungrouped cluster with the most shared edges, ties broken by the lower
            // index. The comparison is order-independent, so the hash map's layout does not reach
            // the result — which is what task 7.5's determinism needs of this stage.
            u32 best = kInvalidCluster;
            u32 best_weight = 0;
            for (const u32 member : group) {
                for (usize slot = 0; slot < neighbours[member].size(); ++slot) {
                    const u32 candidate = neighbours[member][slot];
                    if (taken[candidate]) {
                        continue;
                    }
                    const u32 weight = weights[member][slot];
                    if (weight > best_weight || (weight == best_weight && candidate < best)) {
                        best = candidate;
                        best_weight = weight;
                    }
                }
            }
            if (best == kInvalidCluster) {
                break;  // an isolated component: the group is what it is
            }
            taken[best] = true;
            if (Status pushed = group.push_back(best); !pushed) {
                return pushed;
            }
        }
        std::ranges::sort(group);
        if (Status pushed = out.push_back(std::move(group)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The locked set for one group: every welded vertex the group shares with the rest of this level,
/// plus every vertex on an open edge of the level's surface.
///
/// THIS IS THE WHOLE CRACK-FREE MECHANISM. A vertex here is not moved and not removed, so the
/// group's boundary comes out of simplification identical to the way it went in, and the group's
/// neighbours — simplified separately, possibly to a different level — join it exactly.
Status Builder::locked_set(Span<const u32> level, Span<const u32> members,
                           Array<u32>& out) noexcept {
    HashSet<u32> in_group(arena);
    for (const u32 member : members) {
        if (Status inserted = in_group.insert(member); !inserted) {
            return inserted;
        }
    }

    HashSet<u32> group_vertices(arena);
    HashSet<u32> locked(arena);
    HashMap<u64, u32> edge_uses(arena);

    for (u32 index = 0; index < level.size(); ++index) {
        const BuildCluster& cluster = clusters[level[index]];
        for (usize corner = 0; corner < cluster.triangles.size(); corner += 3) {
            for (u32 side = 0; side < 3; ++side) {
                const u64 key = edge_key(cluster.triangles[corner + side],
                                         cluster.triangles[corner + ((side + 1U) % 3U)]);
                if (u32* uses = edge_uses.find(key); uses != nullptr) {
                    ++*uses;
                } else if (Expected<u32*, Error> inserted = edge_uses.insert(key, 1U); !inserted) {
                    return make_unexpected(inserted.error());
                }
            }
        }
        if (!in_group.contains(index)) {
            continue;
        }
        for (const u32 vertex : cluster.triangles) {
            if (Status inserted = group_vertices.insert(vertex); !inserted) {
                return inserted;
            }
        }
    }

    for (u32 index = 0; index < level.size(); ++index) {
        if (in_group.contains(index)) {
            continue;
        }
        for (const u32 vertex : clusters[level[index]].triangles) {
            if (group_vertices.contains(vertex)) {
                if (Status inserted = locked.insert(vertex); !inserted) {
                    return inserted;
                }
            }
        }
    }
    // An open edge of the level's surface is the mesh's own border. Locking it keeps a sheet from
    // eroding inwards level by level: not a crack against a neighbour, but a visible defect.
    for (const auto& entry : edge_uses) {
        if (entry.value != 1U) {
            continue;
        }
        const u32 ends[2] = {static_cast<u32>(entry.key >> 32U),
                             static_cast<u32>(entry.key & 0xFFFFFFFFU)};
        for (const u32 end : ends) {
            if (group_vertices.contains(end)) {
                if (Status inserted = locked.insert(end); !inserted) {
                    return inserted;
                }
            }
        }
    }

    for (const auto& entry : locked) {
        if (Status pushed = out.push_back(entry.key); !pushed) {
            return pushed;
        }
    }
    // Sorted, because `SimplifyInput::locked` says so and because nothing downstream should depend
    // on a hash table's layout.
    std::ranges::sort(out);
    return ok();
}

Status Builder::simplify_one_group(Span<const u32> level, Span<const u32> members, u8 level_index,
                                   Array<u32>& next) noexcept {
    Array<u32> group_triangles(arena);
    Array<ErrorSphere> member_spheres(arena);
    f32 child_error = 0.0F;
    u32 triangle_total = 0;
    for (const u32 member : members) {
        const BuildCluster& cluster = clusters[level[member]];
        if (Status appended = group_triangles.append(cluster.triangles.span()); !appended) {
            return appended;
        }
        if (Status pushed = member_spheres.push_back(cluster.lod_sphere); !pushed) {
            return pushed;
        }
        child_error = cluster.lod_error > child_error ? cluster.lod_error : child_error;
        triangle_total += static_cast<u32>(cluster.triangles.size() / 3);
    }

    Array<u32> locked(arena);
    if (Status computed = locked_set(level, members, locked); !computed) {
        return computed;
    }

    SimplifyInput input;
    input.positions = welded_positions.span();
    input.indices = group_triangles.span();
    input.locked = locked.span();
    input.target_triangles =
        static_cast<u32>(static_cast<f32>(triangle_total) * options.policy.simplify_ratio);
    Expected<SimplifyResult, Error> simplified = simplify_group(input, arena);
    if (!simplified) {
        return make_unexpected(simplified.error());
    }
    const u32 produced_triangles = static_cast<u32>(simplified->indices.size() / 3);
    if (produced_triangles == 0 || produced_triangles >= triangle_total) {
        // No progress. The members are promoted unchanged, which the caller does by leaving their
        // indices in `next`.
        for (const u32 member : members) {
            if (Status pushed = next.push_back(level[member]); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    const u32 group_index = static_cast<u32>(groups.size());
    Group record;
    record.level = level_index;
    record.member_count = static_cast<u32>(members.size());
    record.sphere = enclose(member_spheres.span());
    record.error = simplified->error > child_error ? simplified->error : child_error;
    if (!(record.error > child_error)) {
        record.error = strictly_above(child_error);
    }
    record.locked_vertices = static_cast<u32>(locked.size());
    record.outside_clusters = static_cast<u32>(level.size() - members.size());

    Array<u32> member_indices(arena);
    for (const u32 member : members) {
        BuildCluster& cluster = clusters[level[member]];
        cluster.parent_error = record.error;
        cluster.parent_sphere = record.sphere;
        cluster.group = group_index;
        if (Status pushed = member_indices.push_back(level[member]); !pushed) {
            return pushed;
        }
    }

    // The simplified triangles enter `triangles` so that the next level's adjacency and locking see
    // them exactly the way this level's saw the source.
    Array<u32> produced_ids(arena);
    const u32 material = clusters[level[members[0]]].material;
    for (usize corner = 0; corner < simplified->indices.size(); corner += 3) {
        WeldedTriangle triangle;
        triangle.v[0] = simplified->indices[corner];
        triangle.v[1] = simplified->indices[corner + 1];
        triangle.v[2] = simplified->indices[corner + 2];
        triangle.material = material;
        if (Status pushed = produced_ids.push_back(static_cast<u32>(triangles.size())); !pushed) {
            return pushed;
        }
        if (Status pushed = triangles.push_back(triangle); !pushed) {
            return pushed;
        }
    }

    Array<u32> children(arena);
    if (Status partitioned = partition(produced_ids.span(), static_cast<u8>(level_index + 1U),
                                       record.error, record.sphere, group_index, children);
        !partitioned) {
        return partitioned;
    }
    record.child_count = static_cast<u32>(children.size());
    for (const u32 child : children) {
        if (Status pushed = next.push_back(child); !pushed) {
            return pushed;
        }
    }
    if (Status pushed = groups.push_back(record); !pushed) {
        return pushed;
    }
    if (Status pushed = group_members.push_back(std::move(member_indices)); !pushed) {
        return pushed;
    }
    return group_children.push_back(std::move(children));
}

Status Builder::build_levels() noexcept {
    Array<u32> level(arena);
    for (u32 index = 0; index < clusters.size(); ++index) {
        if (Status pushed = level.push_back(index); !pushed) {
            return pushed;
        }
    }

    for (u8 level_index = 0; level_index + 1U < options.policy.max_levels; ++level_index) {
        if (level.size() <= options.policy.root_cluster_limit) {
            break;
        }
        Array<Array<u32>> members(arena);
        if (Status grouped = group_level(level.span(), members); !grouped) {
            return grouped;
        }
        Array<u32> next(arena);
        const usize groups_before = groups.size();
        for (const Array<u32>& group : members) {
            if (group.size() < 2) {
                // A group of one cannot be simplified below itself without unlocking its boundary.
                // It is promoted by keeping its index, which costs nothing and adds no cluster.
                for (const u32 member : group) {
                    if (Status pushed = next.push_back(level[member]); !pushed) {
                        return pushed;
                    }
                }
                continue;
            }
            if (Status simplified =
                    simplify_one_group(level.span(), group.span(), level_index, next);
                !simplified) {
                return simplified;
            }
        }
        if (groups.size() == groups_before) {
            break;  // nothing simplified anywhere: the hierarchy is as deep as this mesh allows
        }
        level = std::move(next);
        if (level.empty()) {
            break;
        }
    }
    return ok();
}

/// Copy the built clusters into the output layout, coarsest level first.
struct Finaliser {
    const Builder& builder;
    GeometryBuild& out;
    Array<u32> order;
    Array<u32> output_of;

    Finaliser(const Builder& source, GeometryBuild& target, Allocator& allocator) noexcept
        : builder(source), out(target), order(allocator), output_of(allocator) {}

    [[nodiscard]] Status order_clusters() noexcept;
    [[nodiscard]] Status emit_clusters() noexcept;
    [[nodiscard]] Status emit_hierarchy() noexcept;
};

Status Finaliser::order_clusters() noexcept {
    if (Status reserved = order.reserve(builder.clusters.size()); !reserved) {
        return reserved;
    }
    for (u32 index = 0; index < builder.clusters.size(); ++index) {
        if (Status pushed = order.push_back(index); !pushed) {
            return pushed;
        }
    }
    const Array<BuildCluster>& source = builder.clusters;
    // Coarsest first: the always-resident root region is then a PREFIX of the asset rather than a
    // gather, which is what lets the root be "the first N pages" in the page table and in the
    // streaming request path.
    std::ranges::sort(order, [&source](u32 a, u32 b) noexcept {
        if (source[a].level != source[b].level) {
            return source[a].level > source[b].level;
        }
        return a < b;
    });
    if (Status resized = output_of.resize(builder.clusters.size()); !resized) {
        return resized;
    }
    for (u32 slot = 0; slot < order.size(); ++slot) {
        output_of[order[slot]] = slot;
    }
    return ok();
}

Status Finaliser::emit_clusters() noexcept {
    HashMap<u32, u32> local(out.positions.allocator());
    for (const u32 source_index : order) {
        const BuildCluster& source = builder.clusters[source_index];
        Cluster cluster;
        cluster.level = source.level;
        cluster.material = source.material;
        cluster.lod_error = source.lod_error;
        cluster.lod_sphere = source.lod_sphere;
        cluster.parent_error = source.parent_error;
        cluster.parent_sphere = source.parent_sphere;
        cluster.group = source.group;
        cluster.first_vertex = static_cast<u32>(out.positions.size());
        cluster.first_index = static_cast<u32>(out.indices.size());

        local.clear();
        Aabb box;
        Vec2 uv_low{3.4e38F, 3.4e38F};
        Vec2 uv_high{-3.4e38F, -3.4e38F};
        for (const u32 vertex : source.triangles) {
            u32 slot = 0;
            if (const u32* found = local.find(vertex); found != nullptr) {
                slot = *found;
            } else {
                slot = static_cast<u32>(out.positions.size()) - cluster.first_vertex;
                Expected<u32*, Error> inserted = local.insert(vertex, slot);
                if (!inserted) {
                    return make_unexpected(inserted.error());
                }
                if (Status pushed = out.positions.push_back(builder.welded_positions[vertex]);
                    !pushed) {
                    return pushed;
                }
                if (builder.has_normals) {
                    if (Status pushed = out.normals.push_back(builder.welded_normals[vertex]);
                        !pushed) {
                        return pushed;
                    }
                }
                if (builder.has_uvs) {
                    const Vec2 uv = builder.welded_uvs[vertex];
                    if (Status pushed = out.uvs.push_back(uv); !pushed) {
                        return pushed;
                    }
                    uv_low = cwise_min(uv_low, uv);
                    uv_high = cwise_max(uv_high, uv);
                }
                box.grow(builder.welded_positions[vertex]);
            }
            if (Status pushed = out.indices.push_back(slot); !pushed) {
                return pushed;
            }
        }
        cluster.vertex_count = static_cast<u32>(out.positions.size()) - cluster.first_vertex;
        cluster.index_count = static_cast<u32>(out.indices.size()) - cluster.first_index;
        cluster.bounds = box;
        if (builder.has_uvs) {
            cluster.uv_min = uv_low;
            cluster.uv_max = uv_high;
        }

        // The normal cone, from FACE normals: the test it feeds is "can this cluster show a front
        // face", and a vertex normal is an average that has already lost that.
        Vec3 axis_sum{0.0F, 0.0F, 0.0F};
        for (usize corner = 0; corner < source.triangles.size(); corner += 3) {
            const Vec3 a = builder.welded_positions[source.triangles[corner]];
            const Vec3 b = builder.welded_positions[source.triangles[corner + 1]];
            const Vec3 c = builder.welded_positions[source.triangles[corner + 2]];
            const Vec3 face = cross(b - a, c - a);
            const f32 area = length(face);
            if (area > 1.0e-20F) {
                axis_sum = axis_sum + (face * (1.0F / area));
            }
        }
        const f32 axis_length = length(axis_sum);
        if (axis_length > 1.0e-6F) {
            cluster.cone.axis = axis_sum * (1.0F / axis_length);
            f32 min_cosine = 1.0F;
            for (usize corner = 0; corner < source.triangles.size(); corner += 3) {
                const Vec3 a = builder.welded_positions[source.triangles[corner]];
                const Vec3 b = builder.welded_positions[source.triangles[corner + 1]];
                const Vec3 c = builder.welded_positions[source.triangles[corner + 2]];
                const Vec3 face = cross(b - a, c - a);
                const f32 area = length(face);
                if (area <= 1.0e-20F) {
                    continue;
                }
                const f32 cosine = dot(cluster.cone.axis, face * (1.0F / area));
                min_cosine = cosine < min_cosine ? cosine : min_cosine;
            }
            // At or below zero the cone spans a hemisphere or more and can never reject. Saying so
            // is what keeps a wide cluster from being culled wrongly.
            cluster.cone.cos_angle = min_cosine > 0.0F ? min_cosine : -1.0F;
        }
        if (Status pushed = out.clusters.push_back(cluster); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status Finaliser::emit_hierarchy() noexcept {
    for (u32 index = 0; index < builder.groups.size(); ++index) {
        Group record = builder.groups[index];
        record.first_member = static_cast<u32>(out.group_members.size());
        record.first_child = static_cast<u32>(out.group_children.size());
        for (const u32 member : builder.group_members[index]) {
            if (Status pushed = out.group_members.push_back(output_of[member]); !pushed) {
                return pushed;
            }
        }
        for (const u32 child : builder.group_children[index]) {
            if (Status pushed = out.group_children.push_back(output_of[child]); !pushed) {
                return pushed;
            }
        }
        if (Status pushed = out.groups.push_back(record); !pushed) {
            return pushed;
        }
    }
    // A cluster's children are the members of the group that produced it. Recorded per cluster so
    // that GPU traversal descends without a second indirection through the group table; the ranges
    // of the clusters produced by one group are shared rather than duplicated.
    Array<u32> group_child_range_first(out.cluster_children.allocator());
    if (Status resized = group_child_range_first.resize(out.groups.size()); !resized) {
        return resized;
    }
    for (u32 index = 0; index < out.groups.size(); ++index) {
        group_child_range_first[index] = static_cast<u32>(out.cluster_children.size());
        const Group& group = out.groups[index];
        for (u32 slot = 0; slot < group.member_count; ++slot) {
            if (Status pushed =
                    out.cluster_children.push_back(out.group_members[group.first_member + slot]);
                !pushed) {
                return pushed;
            }
        }
    }
    for (u32 index = 0; index < out.clusters.size(); ++index) {
        const u32 from = builder.clusters[order[index]].from_group;
        if (from == kInvalidGroup) {
            out.clusters[index].first_child = 0;
            out.clusters[index].child_count = 0;
            continue;
        }
        out.clusters[index].first_child = group_child_range_first[from];
        out.clusters[index].child_count = out.groups[from].member_count;
    }
    return ok();
}

}  // namespace

GeometryBuild::GeometryBuild(Allocator& allocator) noexcept
    : positions(allocator),
      normals(allocator),
      uvs(allocator),
      indices(allocator),
      clusters(allocator),
      groups(allocator),
      group_members(allocator),
      group_children(allocator),
      cluster_children(allocator),
      pages(allocator) {}

Expected<GeometryBuild, Error> build_geometry(const SourceMesh& mesh, const BuildOptions& options,
                                              Allocator& allocator) noexcept {
    if (Status valid = options.policy.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (mesh.positions.empty() || mesh.indices.empty()) {
        return fail(ErrorCode::InvalidArgument, "build_geometry: the source mesh is empty");
    }
    if (mesh.indices.size() % 3 != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "build_geometry: the index count is not a multiple of 3");
    }
    if (!mesh.triangle_materials.empty() &&
        mesh.triangle_materials.size() != mesh.indices.size() / 3) {
        return fail(ErrorCode::InvalidArgument,
                    "build_geometry: triangle_materials must have one entry per triangle");
    }

    Builder builder(allocator);
    builder.options = options;
    if (Status welded = builder.weld(mesh); !welded) {
        return make_unexpected(welded.error());
    }
    if (builder.triangles.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "build_geometry: every triangle was degenerate after welding");
    }

    Array<u32> all(allocator);
    if (Status resized = all.resize(builder.triangles.size()); !resized) {
        return make_unexpected(resized.error());
    }
    for (u32 index = 0; index < all.size(); ++index) {
        all[index] = index;
    }
    Array<u32> level_zero(allocator);
    if (Status partitioned =
            builder.partition(all.span(), 0, 0.0F, ErrorSphere{}, kInvalidGroup, level_zero);
        !partitioned) {
        return make_unexpected(partitioned.error());
    }
    if (Status built = builder.build_levels(); !built) {
        return make_unexpected(built.error());
    }

    GeometryBuild out(allocator);
    out.bounds = builder.bounds;
    out.policy = options.policy;
    out.page_bytes = options.page_bytes;
    out.resident_budget_bytes = options.resident_budget_bytes;
    out.deformation = options.deformation;
    out.surface = options.surface;
    out.tangents = options.tangents;
    out.source_triangles = static_cast<u32>(mesh.indices.size() / 3);

    Finaliser finaliser(builder, out, allocator);
    if (Status ordered = finaliser.order_clusters(); !ordered) {
        return make_unexpected(ordered.error());
    }
    if (Status emitted = finaliser.emit_clusters(); !emitted) {
        return make_unexpected(emitted.error());
    }
    if (Status emitted = finaliser.emit_hierarchy(); !emitted) {
        return make_unexpected(emitted.error());
    }

    u32 max_level = 0;
    for (const Cluster& cluster : out.clusters) {
        max_level = cluster.level > max_level ? cluster.level : max_level;
    }
    out.levels = max_level + 1U;
    out.cluster_metadata_bytes = kClusterMetadataBytes * static_cast<u32>(out.clusters.size());
    out.suitability_warning = options.surface == SurfaceClass::Foliage ||
                              options.surface == SurfaceClass::Hair ||
                              options.surface == SurfaceClass::Thin;
    if (out.suitability_warning) {
        out.suitability_reason =
            "aggregate and thin surfaces simplify and occlude poorly; the hierarchy is built with "
            "a class-appropriate policy but the achievable error is worse than for a solid surface";
    }
    out.level_limit_reached = out.levels >= options.policy.max_levels;
    return out;
}

// ================================================================================================
// THE WATERTIGHTNESS CHECK
// ================================================================================================
//
// `virtual-geometry` — "Watertightness is tested": "WHEN an asset is cooked THEN an automated check
// SHALL verify that adjacent clusters across levels share consistent boundaries."
//
// It is written as an assertion about the SURFACE rather than about the bookkeeping. A cut of the
// DAG is taken at a sweep of camera distances and thresholds, its triangles are gathered, and every
// edge is counted: a closed source mesh must produce a closed cut, and an edge used once is
// literally the hole. Checking the bookkeeping instead — "the members' parent error equals the
// group's" — would pass on a build whose simplifier had moved a locked vertex, which is the one
// defect this check exists to catch.

namespace {

/// Positions are cluster-local and a shared corner is stored once per cluster, so edges are keyed
/// on quantised POSITION pairs rather than on index pairs. A tenth of a millimetre at metre scale:
/// four orders of magnitude coarser than f32 and four finer than any weld epsilon a cook would use.
class PositionIndex {
public:
    explicit PositionIndex(Allocator& allocator) noexcept : lattice_(allocator) {}

    [[nodiscard]] Expected<u32, Error> id(const Vec3& position) noexcept {
        const Lattice::Cell cell{std::lround(static_cast<f64>(position.x) * 1.0e4),
                                 std::lround(static_cast<f64>(position.y) * 1.0e4),
                                 std::lround(static_cast<f64>(position.z) * 1.0e4)};
        Expected<Lattice::Lookup, Error> found = lattice_.intern(cell);
        if (!found) {
            return make_unexpected(found.error());
        }
        return found->id;
    }

private:
    Lattice lattice_;
};

/// The edges of the selected clusters that are NOT used exactly twice — the boundary of the patch,
/// and the holes in it if it was supposed to be closed. Sorted, so two patches' boundaries compare
/// as arrays.
///
/// `selected` is one byte per cluster rather than a callback: this runs once per group and once per
/// swept threshold, and a predicate that had to search a member list would make the check quadratic
/// in the cluster count on the cook path.
[[nodiscard]] Status boundary_edges(const GeometryBuild& build, Span<const u8> selected,
                                    PositionIndex& index, Array<u64>& out) noexcept {
    HashMap<u64, u32> edge_uses(out.allocator());
    for (u32 cluster_index = 0; cluster_index < build.clusters.size(); ++cluster_index) {
        if (selected[cluster_index] == 0) {
            continue;
        }
        const Cluster& cluster = build.clusters[cluster_index];
        for (u32 corner = 0; corner < cluster.index_count; corner += 3) {
            u32 id[3] = {};
            for (u32 side = 0; side < 3; ++side) {
                const u32 local = build.indices[cluster.first_index + corner + side];
                Expected<u32, Error> resolved =
                    index.id(build.positions[cluster.first_vertex + local]);
                if (!resolved) {
                    return make_unexpected(resolved.error());
                }
                id[side] = *resolved;
            }
            for (u32 side = 0; side < 3; ++side) {
                const u32 a = id[side];
                const u32 b = id[(side + 1U) % 3U];
                const u64 key =
                    (static_cast<u64>(a < b ? a : b) << 32U) | static_cast<u64>(a < b ? b : a);
                if (u32* uses = edge_uses.find(key); uses != nullptr) {
                    ++*uses;
                } else if (Expected<u32*, Error> inserted = edge_uses.insert(key, 1U); !inserted) {
                    return make_unexpected(inserted.error());
                }
            }
        }
    }
    for (const auto& entry : edge_uses) {
        if (entry.value != 2U) {
            if (Status pushed = out.push_back(entry.key); !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(out);
    return ok();
}

[[nodiscard]] Status check_group_boundaries(const GeometryBuild& build, Allocator& allocator,
                                            WatertightReport& report) noexcept {
    Array<u8> members(allocator);
    Array<u8> children(allocator);
    if (Status resized = members.resize(build.clusters.size()); !resized) {
        return resized;
    }
    if (Status resized = children.resize(build.clusters.size()); !resized) {
        return resized;
    }
    for (const Group& group : build.groups) {
        for (u32 index = 0; index < build.clusters.size(); ++index) {
            members[index] = 0;
            children[index] = 0;
        }
        for (u32 slot = 0; slot < group.member_count; ++slot) {
            members[build.group_members[group.first_member + slot]] = 1;
        }
        for (u32 slot = 0; slot < group.child_count; ++slot) {
            children[build.group_children[group.first_child + slot]] = 1;
        }
        // One index for both patches, so an edge that appears in each is the same number.
        PositionIndex index(allocator);
        Array<u64> before(allocator);
        Array<u64> after(allocator);
        if (Status computed = boundary_edges(build, members.span(), index, before); !computed) {
            return computed;
        }
        if (Status computed = boundary_edges(build, children.span(), index, after); !computed) {
            return computed;
        }
        // THE INVARIANT: simplification held the group's boundary vertices fixed, so the patch it
        // produced presents the same boundary edges to its neighbours as the patch it replaced.
        if (before.size() != after.size()) {
            ++report.boundary_mismatches;
            continue;
        }
        for (usize slot = 0; slot < before.size(); ++slot) {
            if (before[slot] != after[slot]) {
                ++report.boundary_mismatches;
                break;
            }
        }
    }
    return ok();
}

}  // namespace

Expected<WatertightReport, Error> check_watertight(const GeometryBuild& build,
                                                   Allocator& allocator) noexcept {
    WatertightReport report;

    // --- Monotonicity
    // -----------------------------------------------------------------------------
    //
    // A parent's error strictly above its children's, and a parent sphere containing theirs. Either
    // failing puts a hole in the surface at some threshold; see the argument in cluster.h.
    for (const Cluster& cluster : build.clusters) {
        if (cluster.parent_error >= kRootError) {
            continue;
        }
        if (!(cluster.parent_error > cluster.lod_error)) {
            ++report.monotonicity_violations;
            continue;
        }
        if (!cluster.parent_sphere.contains(cluster.lod_sphere)) {
            ++report.monotonicity_violations;
        }
    }

    if (Status checked = check_group_boundaries(build, allocator, report); !checked) {
        return make_unexpected(checked.error());
    }

    // --- The source, and then the cuts
    // ------------------------------------------------------------
    Array<u8> selected(allocator);
    if (Status resized = selected.resize(build.clusters.size()); !resized) {
        return make_unexpected(resized.error());
    }
    {
        for (u32 index = 0; index < build.clusters.size(); ++index) {
            selected[index] = build.clusters[index].level == 0 ? 1U : 0U;
        }
        PositionIndex index(allocator);
        Array<u64> open(allocator);
        if (Status computed = boundary_edges(build, selected.span(), index, open); !computed) {
            return make_unexpected(computed.error());
        }
        report.closed_source = open.empty();
    }
    if (!report.closed_source) {
        // Not applicable rather than a false pass: an open mesh has open edges at every level and
        // the count says nothing about cracks.
        return report;
    }

    ProjectionView view;
    view.viewport_height = 1080.0F;
    const Vec3 center = build.bounds.center();
    const f32 radius = length(build.bounds.size()) * 0.5F;
    for (u32 step = 0; step < 24; ++step) {
        const f32 distance = radius * (1.0F + (static_cast<f32>(step) * 0.75F));
        view.camera_position = center + Vec3{0.0F, 0.0F, distance};
        for (u32 exponent = 0; exponent < 5; ++exponent) {
            const f32 threshold = 0.25F * static_cast<f32>(1U << exponent);
            ++report.thresholds_tested;
            for (u32 index = 0; index < build.clusters.size(); ++index) {
                selected[index] =
                    cluster_selected(build.clusters[index], view, threshold) ? 1U : 0U;
            }
            PositionIndex index(allocator);
            Array<u64> open(allocator);
            if (Status computed = boundary_edges(build, selected.span(), index, open); !computed) {
                return make_unexpected(computed.error());
            }
            if (!open.empty()) {
                ++report.open_cuts;
            }
        }
    }
    return report;
}

}  // namespace cy::rendering::vg
