// Generating a navigation tile from source geometry. See cy/navigation/build.h for the argument;
// this file is the filter every back end shares and the engine-owned rasteriser one of them is.

#include <cy/core/base/assert.h>
#include <cy/navigation/build.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "recast_bridge.h"

namespace cy::navigation {
namespace {

/// A tile larger than this in voxels is refused rather than allocated. Four million cells is a
/// 600 m tile at the default 0.3 m cell size — well past any tiling a project should choose, and
/// small enough that the refusal is a diagnostic rather than an out-of-memory.
constexpr u32 kMaxCells = 4u * 1024u * 1024u;

/// A triangle is tested by its centroid: a volume is a coarse authoring filter, not a clip.
[[nodiscard]] bool covers(const Aabb& volume, Vec3 centre) noexcept {
    return centre.x >= volume.min.x && centre.x <= volume.max.x && centre.y >= volume.min.y &&
           centre.y <= volume.max.y && centre.z >= volume.min.z && centre.z <= volume.max.z;
}

[[nodiscard]] bool inside_all(Span<const Aabb> volumes, Vec3 a, Vec3 b, Vec3 c) noexcept {
    if (volumes.empty()) {
        return true;
    }
    const Vec3 centre = (a + b + c) * (1.0f / 3.0f);
    return std::ranges::any_of(volumes,
                               [centre](const Aabb& volume) { return covers(volume, centre); });
}

[[nodiscard]] bool inside_any(Span<const Aabb> volumes, Vec3 a, Vec3 b, Vec3 c) noexcept {
    const Vec3 centre = (a + b + c) * (1.0f / 3.0f);
    return std::ranges::any_of(volumes,
                               [centre](const Aabb& volume) { return covers(volume, centre); });
}

/// One rasterised surface sample under one cell. Chained per cell, so a cell carries every surface
/// over it rather than only the topmost — which is what makes the headroom test possible.
struct Sample {
    f32 height = 0.0f;
    u32 next = 0xFFFFFFFFu;
    AreaType area = kAreaGround;
    bool walkable = false;
};

struct Heightfield {
    u32 width = 0;
    u32 depth = 0;
    Array<u32> head;  ///< first sample index per cell, or 0xFFFFFFFF
    Array<Sample> samples;
    Array<f32> floor;  ///< the chosen walkable surface, per cell
    Array<u8> area;
    Array<u8> walkable;
    Array<f32> distance;  ///< cells to the nearest hole, for the erosion pass

    explicit Heightfield(Allocator& allocator) noexcept
        : head(allocator),
          samples(allocator),
          floor(allocator),
          area(allocator),
          walkable(allocator),
          distance(allocator) {}
};

/// Barycentric interpolation of `y` at (x, z), or false when (x, z) is outside the triangle.
[[nodiscard]] bool sample_triangle(Vec3 a, Vec3 b, Vec3 c, f32 x, f32 z, f32& y) noexcept {
    const f32 v0x = b.x - a.x;
    const f32 v0z = b.z - a.z;
    const f32 v1x = c.x - a.x;
    const f32 v1z = c.z - a.z;
    const f32 denominator = (v0x * v1z) - (v1x * v0z);
    if (std::fabs(denominator) < 1e-9f) {
        return false;
    }
    const f32 px = x - a.x;
    const f32 pz = z - a.z;
    const f32 u = ((px * v1z) - (v1x * pz)) / denominator;
    const f32 v = ((v0x * pz) - (px * v0z)) / denominator;
    // A closed test on all three barycentric coordinates: a cell centre exactly on a shared edge
    // belongs to both triangles, and taking it twice is harmless where dropping it leaves a hole.
    if (u < -1e-5f || v < -1e-5f || (u + v) > 1.0f + 1e-5f) {
        return false;
    }
    y = a.y + ((b.y - a.y) * u) + ((c.y - a.y) * v);
    return true;
}

[[nodiscard]] Status allocate_field(Heightfield& field, u32 width, u32 depth) noexcept {
    field.width = width;
    field.depth = depth;
    const usize cells = usize{width} * usize{depth};
    if (Status sized = field.head.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = field.floor.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = field.area.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = field.walkable.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = field.distance.resize(cells); !sized) {
        return sized;
    }
    for (usize cell = 0; cell < cells; ++cell) {
        field.head[cell] = 0xFFFFFFFFu;
        field.floor[cell] = 0.0f;
        field.area[cell] = kAreaNull;
        field.walkable[cell] = 0;
        field.distance[cell] = 0.0f;
    }
    return ok();
}

/// Apply `NavBuildParams`' declared filters, once, above the choice of back end.
///
/// `navigation`: "Generation SHALL support filtering by layer, by tag, and by an explicit
/// include/exclude volume." The slope limit is applied here too rather than inside a back end, so
/// that the two back ends agree about which triangles are walkable before either of them starts.
[[nodiscard]] Status filter_geometry(const NavBuildParams& params,
                                     const NavSourceGeometry& geometry, Array<u32>& indices,
                                     Array<AreaType>& areas, NavBuildReport& report) noexcept {
    const f32 cos_limit =
        std::cos(params.agent_max_slope_degrees * (std::numbers::pi_v<f32> / 180.0f));
    const u32 triangle_count = static_cast<u32>(geometry.indices.size() / 3);
    report.triangles_in = triangle_count;

    for (u32 triangle = 0; triangle < triangle_count; ++triangle) {
        const u32 i0 = geometry.indices[(triangle * 3) + 0];
        const u32 i1 = geometry.indices[(triangle * 3) + 1];
        const u32 i2 = geometry.indices[(triangle * 3) + 2];
        if (i0 >= geometry.vertices.size() || i1 >= geometry.vertices.size() ||
            i2 >= geometry.vertices.size()) {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "a triangle names a vertex the geometry has not"});
        }
        const Vec3 a = geometry.vertices[i0];
        const Vec3 b = geometry.vertices[i1];
        const Vec3 c = geometry.vertices[i2];

        const u8 layer = (triangle < geometry.layer.size()) ? geometry.layer[triangle] : 0;
        const u8 tag = (triangle < geometry.tag.size()) ? geometry.tag[triangle] : 0;
        const bool layer_kept = (params.layers & (u64{1} << (layer & 63u))) != 0;
        const bool tag_kept = (params.tags & (u64{1} << (tag & 63u))) != 0;
        const Vec3 normal = cross(b - a, c - a);
        const f32 area_twice = length(normal);
        if (!layer_kept || !tag_kept || !inside_all(params.include_volumes, a, b, c) ||
            inside_any(params.exclude_volumes, a, b, c) || area_twice < 1e-9f) {
            ++report.triangles_filtered;
            continue;
        }

        const bool walkable = (normal.y / area_twice) >= cos_limit;
        if (!walkable) {
            ++report.triangles_steep;
        }
        const AreaType declared =
            (triangle < geometry.area.size()) ? geometry.area[triangle] : kAreaGround;
        if (Status pushed = areas.push_back(walkable ? declared : kAreaNull); !pushed) {
            return pushed;
        }
        for (const u32 index : {i0, i1, i2}) {
            if (Status pushed = indices.push_back(index); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// The filtered triangles, rasterised into `field` as samples. One per cell centre the triangle
/// covers, and every surface is kept — the headroom test in `select_floors` needs the ones an agent
/// cannot stand on as much as the ones it can.
///
/// A CENTRE-SAMPLING RASTERISER, not a conservative one: a surface narrower than a cell can fall
/// between two centres and produce no span. That is the fidelity difference this back end carries
/// against Recast's, and the reason `NavBuildBackend::Recast` is the default wherever it exists.
[[nodiscard]] Status rasterise(Heightfield& field, const NavBuildParams& params,
                               Span<const Vec3> vertices, Span<const u32> indices,
                               Span<const AreaType> areas, const Aabb& bounds) noexcept {
    for (u32 triangle = 0; triangle < areas.size(); ++triangle) {
        const Vec3 a = vertices[indices[(triangle * 3) + 0]];
        const Vec3 b = vertices[indices[(triangle * 3) + 1]];
        const Vec3 c = vertices[indices[(triangle * 3) + 2]];
        const AreaType area = areas[triangle];
        const bool walkable = area != kAreaNull;

        const f32 lo_x = std::fmin(a.x, std::fmin(b.x, c.x));
        const f32 hi_x = std::fmax(a.x, std::fmax(b.x, c.x));
        const f32 lo_z = std::fmin(a.z, std::fmin(b.z, c.z));
        const f32 hi_z = std::fmax(a.z, std::fmax(b.z, c.z));
        const i32 first_x = static_cast<i32>(
            std::floor((lo_x - bounds.min.x - (params.cell_size * 0.5f)) / params.cell_size));
        const i32 last_x = static_cast<i32>(
            std::ceil((hi_x - bounds.min.x - (params.cell_size * 0.5f)) / params.cell_size));
        const i32 first_z = static_cast<i32>(
            std::floor((lo_z - bounds.min.z - (params.cell_size * 0.5f)) / params.cell_size));
        const i32 last_z = static_cast<i32>(
            std::ceil((hi_z - bounds.min.z - (params.cell_size * 0.5f)) / params.cell_size));

        const i32 clamp_z = std::min(last_z, static_cast<i32>(field.depth) - 1);
        const i32 clamp_x = std::min(last_x, static_cast<i32>(field.width) - 1);
        for (i32 z = std::max(first_z, 0); z <= clamp_z; ++z) {
            for (i32 x = std::max(first_x, 0); x <= clamp_x; ++x) {
                const f32 centre_x =
                    bounds.min.x + ((static_cast<f32>(x) + 0.5f) * params.cell_size);
                const f32 centre_z =
                    bounds.min.z + ((static_cast<f32>(z) + 0.5f) * params.cell_size);
                f32 y = 0.0f;
                if (!sample_triangle(a, b, c, centre_x, centre_z, y)) {
                    continue;
                }
                const usize cell = (static_cast<usize>(z) * field.width) + static_cast<usize>(x);
                Sample sample;
                sample.height = y;
                sample.area = area;
                sample.walkable = walkable;
                sample.next = field.head[cell];
                if (Status pushed = field.samples.push_back(sample); !pushed) {
                    return pushed;
                }
                field.head[cell] = static_cast<u32>(field.samples.size() - 1);
            }
        }
    }
    return ok();
}

/// Pick the walkable surface an agent can stand on: the topmost walkable sample with `agent_height`
/// of clear space above it. `navigation` parameterises generation by agent height and this is the
/// only place that value can mean anything.
void select_floors(Heightfield& field, const NavBuildParams& params,
                   NavBuildReport& report) noexcept {
    for (usize cell = 0; cell < field.head.size(); ++cell) {
        f32 best = 0.0f;
        AreaType best_area = kAreaNull;
        bool found = false;
        for (u32 index = field.head[cell]; index != 0xFFFFFFFFu;
             index = field.samples[index].next) {
            const Sample& candidate = field.samples[index];
            if (!candidate.walkable) {
                continue;
            }
            bool clear = true;
            for (u32 above = field.head[cell]; above != 0xFFFFFFFFu && clear;
                 above = field.samples[above].next) {
                const f32 height = field.samples[above].height;
                clear = height <= candidate.height + 1e-3f ||
                        height >= candidate.height + params.agent_height;
            }
            if (!clear) {
                continue;
            }
            if (!found || candidate.height > best) {
                best = candidate.height;
                best_area = candidate.area;
                found = true;
            }
        }
        field.floor[cell] = best;
        field.area[cell] = best_area;
        field.walkable[cell] = found ? 1u : 0u;
        report.spans += found ? 1u : 0u;
    }
}

/// `navigation`: "the walkable surface SHALL be shrunk by that distance from obstacles, so a path's
/// centre line is always traversable".
///
/// A two-pass chamfer distance to the nearest HOLE. Out-of-grid is treated as walkable rather than
/// as a hole, deliberately: a tile boundary is a partition and not an obstacle, and eroding at one
/// would disconnect every pair of adjacent tiles along the seam they are supposed to share.
/// The two-pass chamfer distance transform: every walkable cell's distance, in cells, to the
/// nearest hole. Separated from the thresholding below because it is a self-contained algorithm and
/// because the two passes are mirror images that read much better side by side than inside a
/// function that also decides what to do with the answer.
void chamfer_distance(Heightfield& field) noexcept {
    constexpr f32 kFar = 1e9f;
    constexpr f32 kOrthogonal = 1.0f;
    constexpr f32 kDiagonal = std::numbers::sqrt2_v<f32>;
    const auto width = static_cast<i32>(field.width);
    const auto depth = static_cast<i32>(field.depth);
    const auto at = [&field](i32 x, i32 z) noexcept -> usize {
        return (static_cast<usize>(z) * field.width) + static_cast<usize>(x);
    };
    const auto relax = [&field, at, width, depth](usize here, i32 x, i32 z, f32 weight) noexcept {
        if (x < 0 || z < 0 || x >= width || z >= depth) {
            return;
        }
        field.distance[here] = std::fmin(field.distance[here], field.distance[at(x, z)] + weight);
    };

    for (i32 z = 0; z < depth; ++z) {
        for (i32 x = 0; x < width; ++x) {
            field.distance[at(x, z)] = (field.walkable[at(x, z)] != 0) ? kFar : 0.0f;
        }
    }
    for (i32 z = 0; z < depth; ++z) {
        for (i32 x = 0; x < width; ++x) {
            const usize here = at(x, z);
            relax(here, x, z - 1, kOrthogonal);
            relax(here, x - 1, z - 1, kDiagonal);
            relax(here, x + 1, z - 1, kDiagonal);
            relax(here, x - 1, z, kOrthogonal);
        }
    }
    for (i32 z = depth - 1; z >= 0; --z) {
        for (i32 x = width - 1; x >= 0; --x) {
            const usize here = at(x, z);
            relax(here, x, z + 1, kOrthogonal);
            relax(here, x - 1, z + 1, kDiagonal);
            relax(here, x + 1, z + 1, kDiagonal);
            relax(here, x + 1, z, kOrthogonal);
        }
    }
}

/// `navigation`: "the walkable surface SHALL be shrunk by that distance from obstacles, so a path's
/// centre line is always traversable".
///
/// Out-of-grid is treated as walkable rather than as a hole, deliberately: a tile boundary is a
/// partition and not an obstacle, and eroding at one would disconnect every pair of adjacent tiles
/// along the seam they are supposed to share.
void erode(Heightfield& field, const NavBuildParams& params, NavBuildReport& report) noexcept {
    chamfer_distance(field);
    const f32 radius_in_cells = params.agent_radius / params.cell_size;
    for (usize cell = 0; cell < field.walkable.size(); ++cell) {
        if (field.walkable[cell] != 0 && field.distance[cell] < radius_in_cells) {
            field.walkable[cell] = 0;
            field.area[cell] = kAreaNull;
            --report.spans;
        }
    }
}

/// One vertex of the emitted mesh: a grid corner, at a height shared by every cell around it whose
/// floor is within `agent_max_climb`.
///
/// Clustering by climb is what makes a step an agent can take into shared geometry, and a drop it
/// cannot into a border edge — so `NavMesh::add_tile`'s index comparison finds exactly the
/// adjacency an agent can walk, and nothing else.
struct CornerVertex {
    f32 height = 0.0f;
    u32 index = 0;
};

/// The vertex table of one tile: the corner clusters, and where each corner's run starts.
///
/// A struct so the two passes below can be two functions. Building the clusters and emitting the
/// quads are separate jobs — one is about heights, the other about winding — and the version of
/// this that did both in one function was the most complex in the module by a wide margin.
struct CornerTable {
    Array<CornerVertex> vertices;
    Array<u32> first;
    Array<u32> count;
    u32 width = 0;  ///< corner columns, which is one more than the cell columns

    explicit CornerTable(Allocator& allocator) noexcept
        : vertices(allocator), first(allocator), count(allocator) {}

    [[nodiscard]] usize index_of(u32 cx, u32 cz) const noexcept {
        return (usize{cz} * width) + usize{cx};
    }
};

/// The walkable floors touching corner (cx, cz), ascending.
[[nodiscard]] Status gather_corner_heights(const Heightfield& field, u32 cx, u32 cz,
                                           Array<f32>& heights) noexcept {
    heights.clear();
    for (u32 dz = 0; dz < 2; ++dz) {
        for (u32 dx = 0; dx < 2; ++dx) {
            if (cx + dx == 0 || cz + dz == 0 || cx + dx > field.width || cz + dz > field.depth) {
                continue;
            }
            const usize cell = (usize{cz + dz - 1} * field.width) + usize{cx + dx - 1};
            if (field.walkable[cell] != 0) {
                if (Status pushed = heights.push_back(field.floor[cell]); !pushed) {
                    return pushed;
                }
            }
        }
    }
    // A selection sort over at most four values: cheaper than a call, and stable.
    for (usize i = 0; i < heights.size(); ++i) {
        for (usize j = i + 1; j < heights.size(); ++j) {
            if (heights[j] < heights[i]) {
                const f32 swap = heights[i];
                heights[i] = heights[j];
                heights[j] = swap;
            }
        }
    }
    return ok();
}

/// One vertex per cluster of floors within `agent_max_climb` of each other.
///
/// Clustering by climb is what turns a step an agent can take into shared geometry, and a drop it
/// cannot into a border edge — so `NavMesh::add_tile`'s index comparison finds exactly the
/// adjacency an agent can walk, and nothing else.
[[nodiscard]] Status build_corner_table(const Heightfield& field, const NavBuildParams& params,
                                        const Aabb& bounds, NavTileData& tile,
                                        CornerTable& table) noexcept {
    table.width = field.width + 1;
    const usize corner_count = usize{table.width} * (field.depth + 1);
    if (Status sized = table.first.resize(corner_count); !sized) {
        return sized;
    }
    if (Status sized = table.count.resize(corner_count); !sized) {
        return sized;
    }

    Array<f32> heights(tile.vertices().allocator());
    for (u32 cz = 0; cz <= field.depth; ++cz) {
        for (u32 cx = 0; cx <= field.width; ++cx) {
            if (Status gathered = gather_corner_heights(field, cx, cz, heights); !gathered) {
                return gathered;
            }
            const usize corner = table.index_of(cx, cz);
            table.first[corner] = static_cast<u32>(table.vertices.size());
            table.count[corner] = 0;
            usize begin = 0;
            while (begin < heights.size()) {
                usize end = begin + 1;
                while (end < heights.size() &&
                       heights[end] - heights[end - 1] <= params.agent_max_climb) {
                    ++end;
                }
                f32 sum = 0.0f;
                for (usize i = begin; i < end; ++i) {
                    sum += heights[i];
                }
                CornerVertex vertex;
                vertex.height = sum / static_cast<f32>(end - begin);
                vertex.index = static_cast<u32>(tile.vertices().size());
                if (Status pushed = table.vertices.push_back(vertex); !pushed) {
                    return pushed;
                }
                const Status added = tile.vertices().push_back(
                    Vec3{bounds.min.x + (static_cast<f32>(cx) * params.cell_size), vertex.height,
                         bounds.min.z + (static_cast<f32>(cz) * params.cell_size)});
                if (!added) {
                    return added;
                }
                ++table.count[corner];
                begin = end;
            }
        }
    }
    return ok();
}

/// The vertex of corner (cx, cz) nearest `height`, or `0xFFFFFFFF` when no cluster is within a
/// climb of it.
[[nodiscard]] u32 vertex_for(const CornerTable& table, const NavBuildParams& params, u32 cx, u32 cz,
                             f32 height) noexcept {
    const usize corner = table.index_of(cx, cz);
    u32 best = 0xFFFFFFFFu;
    f32 best_delta = params.agent_max_climb + 1e-3f;
    for (u32 i = 0; i < table.count[corner]; ++i) {
        const CornerVertex& candidate = table.vertices[table.first[corner] + i];
        const f32 delta = std::fabs(candidate.height - height);
        if (delta <= best_delta) {
            best_delta = delta;
            best = candidate.index;
        }
    }
    return best;
}

/// One counter-clockwise quad per walkable cell.
[[nodiscard]] Status emit_cell_quads(const Heightfield& field, const NavBuildParams& params,
                                     const CornerTable& table, NavTileData& tile) noexcept {
    for (u32 z = 0; z < field.depth; ++z) {
        for (u32 x = 0; x < field.width; ++x) {
            const usize cell = (usize{z} * field.width) + usize{x};
            if (field.walkable[cell] == 0) {
                continue;
            }
            const f32 height = field.floor[cell];
            const u32 corner[4] = {vertex_for(table, params, x, z, height),
                                   vertex_for(table, params, x + 1, z, height),
                                   vertex_for(table, params, x + 1, z + 1, height),
                                   vertex_for(table, params, x, z + 1, height)};
            if (corner[0] == 0xFFFFFFFFu || corner[1] == 0xFFFFFFFFu || corner[2] == 0xFFFFFFFFu ||
                corner[3] == 0xFFFFFFFFu) {
                continue;
            }
            NavPoly poly;
            poly.first_corner = static_cast<u32>(tile.corners().size());
            poly.corner_count = 4;
            poly.area = field.area[cell];
            poly.cost = 1.0f;
            if (Status pushed = tile.polys().push_back(poly); !pushed) {
                return pushed;
            }
            for (const u32 index : corner) {
                if (Status pushed = tile.corners().push_back(index); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

[[nodiscard]] Status emit_quads(const Heightfield& field, const NavBuildParams& params,
                                const Aabb& bounds, NavTileData& tile,
                                NavBuildReport& report) noexcept {
    CornerTable table(tile.vertices().allocator());
    if (Status built = build_corner_table(field, params, bounds, tile, table); !built) {
        return built;
    }
    if (Status emitted = emit_cell_quads(field, params, table, tile); !emitted) {
        return emitted;
    }
    report.polys = static_cast<u32>(tile.polys().size());
    report.vertices = static_cast<u32>(tile.vertices().size());
    return ok();
}

[[nodiscard]] Status validate(const NavBuildParams& params, const Aabb& bounds, u32& width,
                              u32& depth) noexcept {
    if (params.cell_size <= 0.0f || params.cell_height <= 0.0f) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "cell size and cell height must be positive"});
    }
    if (params.agent_radius < 0.0f || params.agent_height <= 0.0f) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "the agent radius must not be negative and its height must "
                                     "be positive"});
    }
    if (bounds.is_empty() || bounds.size().x <= 0.0f || bounds.size().z <= 0.0f) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the tile bounds are empty in x or z"});
    }
    const f32 cells_x = std::ceil(bounds.size().x / params.cell_size);
    const f32 cells_z = std::ceil(bounds.size().z / params.cell_size);
    if (cells_x * cells_z > static_cast<f32>(kMaxCells)) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "the tile bounds and cell size ask for more than four million "
                                     "voxel columns; use a larger cell or a smaller tile"});
    }
    width = static_cast<u32>(cells_x);
    depth = static_cast<u32>(cells_z);
    if (width == 0 || depth == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the tile is smaller than one cell"});
    }
    return ok();
}

}  // namespace

bool recast_available() noexcept {
#if defined(CY_NAVIGATION)
    return true;
#else
    return false;
#endif
}

Expected<NavTileData, Error> build_tile(Allocator& allocator, const NavBuildParams& params,
                                        const NavSourceGeometry& geometry, TileCoord coord,
                                        const Aabb& bounds, NavBuildReport& report) noexcept {
    report = NavBuildReport{};
    u32 width = 0;
    u32 depth = 0;
    if (Status checked = validate(params, bounds, width, depth); !checked) {
        return make_unexpected(checked.error());
    }
    if (geometry.indices.size() % 3 != 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the index count is not a multiple of three"});
    }

    Array<u32> indices(allocator);
    Array<AreaType> areas(allocator);
    if (Status filtered = filter_geometry(params, geometry, indices, areas, report); !filtered) {
        return make_unexpected(filtered.error());
    }

    NavBuildBackend backend = params.backend;
    if (backend == NavBuildBackend::Automatic) {
        backend = recast_available() ? NavBuildBackend::Recast : NavBuildBackend::Engine;
    }
    if (backend == NavBuildBackend::Recast) {
        if (!recast_available()) {
            return make_unexpected(
                Error{ErrorCode::Unsupported,
                      "NavBuildBackend::Recast was asked for in a build configured with "
                      "-D CY_NAVIGATION=OFF; use Automatic or Engine, or turn the option on"});
        }
        report.backend = NavBuildBackend::Recast;
        const detail::FilteredGeometry filtered{geometry.vertices, indices.span(), areas.span()};
        return detail::build_tile_recast(allocator, params, filtered, coord, bounds, report);
    }

    Heightfield field(allocator);
    if (Status allocated = allocate_field(field, width, depth); !allocated) {
        return make_unexpected(allocated.error());
    }
    if (Status rasterised =
            rasterise(field, params, geometry.vertices, indices.span(), areas.span(), bounds);
        !rasterised) {
        return make_unexpected(rasterised.error());
    }
    select_floors(field, params, report);
    erode(field, params, report);

    NavTileData tile(allocator);
    tile.coord = coord;
    if (Status emitted = emit_quads(field, params, bounds, tile, report); !emitted) {
        return make_unexpected(emitted.error());
    }
    report.backend = NavBuildBackend::Engine;
    tile.finalise();
    return tile;
}

}  // namespace cy::navigation
