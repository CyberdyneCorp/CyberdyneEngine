// The query path: the heightfield's lattice arithmetic, the mesh representation, and the merge that
// makes a column one answer. M10 task 2.1.

#include <cy/terrain/surface.h>

#include <cmath>
#include <numbers>
#include <utility>

namespace cy::terrain {
namespace {

[[nodiscard]] f32 clamp_f32(f32 value, f32 low, f32 high) noexcept {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

[[nodiscard]] u32 clamp_index(i64 value, u32 high) noexcept {
    if (value < 0) {
        return 0;
    }
    return (std::cmp_greater(value, high)) ? high : static_cast<u32>(value);
}

/// Where a position sits inside a tile, in sample units.
struct LatticePosition {
    u32 i = 0;
    u32 j = 0;
    f32 fraction_x = 0.0F;
    f32 fraction_z = 0.0F;
};

[[nodiscard]] LatticePosition lattice_of(const TileLayout& layout, const TileCoord& coord, f64 x,
                                         f64 z) noexcept {
    const TerrainBounds bounds = tile_bounds(layout, coord);
    const f64 spacing = layout.tile_size(coord.level) / static_cast<f64>(kTileQuads);
    const f64 local_x = (x - bounds.min_x) / spacing;
    const f64 local_z = (z - bounds.min_z) / spacing;
    LatticePosition position;
    position.i = clamp_index(static_cast<i64>(std::floor(local_x)), kTileQuads - 1);
    position.j = clamp_index(static_cast<i64>(std::floor(local_z)), kTileQuads - 1);
    position.fraction_x =
        clamp_f32(static_cast<f32>(local_x - static_cast<f64>(position.i)), 0.0F, 1.0F);
    position.fraction_z =
        clamp_f32(static_cast<f32>(local_z - static_cast<f64>(position.j)), 0.0F, 1.0F);
    return position;
}

/// Sort a column highest first. Insertion sort over at most `kMaxColumnSurfaces` entries: the array
/// is tiny and bounded, and a stable order is what makes two machines report the same column.
void sort_descending(Span<SurfaceSample> column, u32 count) noexcept {
    for (u32 a = 1; a < count; ++a) {
        SurfaceSample key = column[a];
        u32 b = a;
        while (b > 0 && column[b - 1].height < key.height) {
            column[b] = column[b - 1];
            --b;
        }
        column[b] = key;
    }
}

}  // namespace

const char* representation_name(Representation representation) noexcept {
    switch (representation) {
        case Representation::Heightfield:
            return "heightfield";
        case Representation::Mesh:
            return "mesh";
        case Representation::SignedDistanceField:
            return "signed-distance-field";
    }
    return "unknown";
}

// --- The heightfield
// ------------------------------------------------------------------------------

f32 HeightfieldSource::sample_height(const TileCoord& coord, u32 i, u32 j) const noexcept {
    const TerrainTile* tile = store_->find(coord);
    if (tile == nullptr) {
        return 0.0F;
    }
    const f32 cooked = tile_height(store_->layout(), *tile, i, j);
    // `cooked terrain + terrain delta = current terrain`, in the one place the addition happens.
    return (delta_ == nullptr) ? cooked : cooked + delta_->height_delta(coord, i, j);
}

u32 HeightfieldSource::column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept {
    if (out.empty()) {
        return 0;
    }
    const TileLayout& layout = store_->layout();
    const TerrainTile* tile = store_->finest_at(x, z, 0);
    if (tile == nullptr) {
        return 0;
    }
    const TileCoord coord = tile->coord;
    const LatticePosition position = lattice_of(layout, coord, x, z);

    SurfaceSample sample;
    sample.level = coord.level;
    sample.sample_metres = layout.sample_metres(coord.level);
    sample.from = Representation::Heightfield;

    const bool cooked_hole = tile->hole(position.i, position.j);
    const bool runtime_hole =
        (delta_ != nullptr) && delta_->hole_delta(coord, position.i, position.j);
    if (cooked_hole || runtime_hole) {
        // "WHEN a hole is authored, THEN rendering, collision, navigation, and height queries SHALL
        // ALL AGREE that there is no surface there." A hole is reported and is not a surface.
        sample.hole = true;
        sample.resolved = false;
        out[0] = sample;
        return 1;
    }

    const f32 h00 = sample_height(coord, position.i, position.j);
    const f32 h10 = sample_height(coord, position.i + 1, position.j);
    const f32 h01 = sample_height(coord, position.i, position.j + 1);
    const f32 h11 = sample_height(coord, position.i + 1, position.j + 1);
    const f32 fx = position.fraction_x;
    const f32 fz = position.fraction_z;
    sample.height = (((h00 * (1.0F - fx)) + (h10 * fx)) * (1.0F - fz)) +
                    (((h01 * (1.0F - fx)) + (h11 * fx)) * fz);

    // The normal from the quad's own differences rather than from a neighbourhood: the surface a
    // consumer stands on is this quad's bilinear patch, and a normal averaged over a wider stencil
    // is the normal of a surface nothing renders or collides against.
    const f32 spacing = layout.sample_metres(coord.level);
    const f32 dx = (((h10 - h00) * (1.0F - fz)) + ((h11 - h01) * fz)) / spacing;
    const f32 dz = (((h01 - h00) * (1.0F - fx)) + ((h11 - h10) * fx)) / spacing;
    Vec3 normal{-dx, 1.0F, -dz};
    const f32 length = std::sqrt((normal.x * normal.x) + 1.0F + (normal.z * normal.z));
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
    sample.normal = normal;
    sample.slope_degrees =
        std::acos(clamp_f32(normal.y, -1.0F, 1.0F)) * (180.0F / std::numbers::pi_v<float>);
    // Discrete Laplacian over the quad's corners, in metres per metre squared. Positive on a ridge.
    sample.curvature = (h00 + h11 - h10 - h01) / (spacing * spacing);

    const u32 texel_i = (position.i < kTileTexels) ? position.i : kTileTexels - 1;
    const u32 texel_j = (position.j < kTileTexels) ? position.j : kTileTexels - 1;
    sample.layer = tile->texel(texel_i, texel_j).dominant();
    sample.biome = tile->biome[(texel_j * kTileTexels) + texel_i];
    sample.resolved = true;
    out[0] = sample;
    return 1;
}

// --- The mesh representation
// ----------------------------------------------------------------

MeshSource::MeshSource(Allocator& allocator) noexcept : triangles_(allocator) {}

Status MeshSource::add_triangle(const WorldTriangle& triangle) noexcept {
    return triangles_.push_back(triangle);
}

u32 MeshSource::column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept {
    u32 written = 0;
    for (const WorldTriangle& triangle : triangles_.span()) {
        // Barycentric coordinates of (x, z) in the triangle's horizontal projection. A triangle
        // whose projection is degenerate covers no column and is skipped rather than dividing.
        const f64 x1 = triangle.a.x;
        const f64 z1 = triangle.a.z;
        const f64 x2 = triangle.b.x;
        const f64 z2 = triangle.b.z;
        const f64 x3 = triangle.c.x;
        const f64 z3 = triangle.c.z;
        const f64 area = ((z2 - z3) * (x1 - x3)) + ((x3 - x2) * (z1 - z3));
        if (area > -1.0e-12 && area < 1.0e-12) {
            continue;
        }
        const f64 u = (((z2 - z3) * (x - x3)) + ((x3 - x2) * (z - z3))) / area;
        const f64 v = (((z3 - z1) * (x - x3)) + ((x1 - x3) * (z - z3))) / area;
        const f64 w = 1.0 - u - v;
        if (u < 0.0 || v < 0.0 || w < 0.0) {
            continue;
        }
        if (written >= out.size()) {
            // Truncated rather than grown: a query allocates nothing. See `kMaxColumnSurfaces`.
            break;
        }

        SurfaceSample sample;
        sample.height = static_cast<f32>((u * static_cast<f64>(triangle.height_a)) +
                                         (v * static_cast<f64>(triangle.height_b)) +
                                         (w * static_cast<f64>(triangle.height_c)));
        const Vec3 edge1{static_cast<f32>(x2 - x1), triangle.height_b - triangle.height_a,
                         static_cast<f32>(z2 - z1)};
        const Vec3 edge2{static_cast<f32>(x3 - x1), triangle.height_c - triangle.height_a,
                         static_cast<f32>(z3 - z1)};
        Vec3 normal{(edge1.y * edge2.z) - (edge1.z * edge2.y),
                    (edge1.z * edge2.x) - (edge1.x * edge2.z),
                    (edge1.x * edge2.y) - (edge1.y * edge2.x)};
        const f32 length =
            std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
        if (length > 0.0F) {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        }
        // Face the sky, so a downward-wound triangle reports the surface a walker stands on.
        if (normal.y < 0.0F) {
            normal.x = -normal.x;
            normal.y = -normal.y;
            normal.z = -normal.z;
        }
        sample.normal = normal;
        sample.slope_degrees =
            std::acos(clamp_f32(normal.y, -1.0F, 1.0F)) * (180.0F / std::numbers::pi_v<float>);
        sample.layer = triangle.layer;
        sample.biome = triangle.biome;
        sample.resolved = true;
        sample.from = Representation::Mesh;
        out[written] = sample;
        ++written;
    }
    return written;
}

// --- The query
// ------------------------------------------------------------------------------------

TerrainQuery::TerrainQuery(Allocator& allocator) noexcept : sources_(allocator) {}

Status TerrainQuery::add_source(const SurfaceSource& source) noexcept {
    return sources_.push_back(&source);
}

u32 TerrainQuery::column(f64 x, f64 z, Span<SurfaceSample> out) const noexcept {
    u32 written = 0;
    for (const SurfaceSource* source : sources_.span()) {
        if (written >= out.size()) {
            break;
        }
        const Span<SurfaceSample> remaining = out.subspan(written);
        written += source->column(x, z, remaining);
    }
    sort_descending(out, written);
    return written;
}

SurfaceSample TerrainQuery::sample(f64 x, f64 z) const noexcept {
    SurfaceSample column_buffer[kMaxColumnSurfaces];
    const u32 count = column(x, z, Span<SurfaceSample>(column_buffer, kMaxColumnSurfaces));
    if (count == 0) {
        return SurfaceSample{};
    }
    // The topmost RESOLVED surface. A hole at the top of a column is reported by `below()`, which
    // is the call that has to distinguish "no ground here" from "ground lower down".
    for (u32 index = 0; index < count; ++index) {
        if (column_buffer[index].resolved) {
            return column_buffer[index];
        }
    }
    return column_buffer[0];
}

SurfaceSample TerrainQuery::below(f64 x, f64 z, f32 from_height) const noexcept {
    SurfaceSample column_buffer[kMaxColumnSurfaces];
    const u32 count = column(x, z, Span<SurfaceSample>(column_buffer, kMaxColumnSurfaces));
    for (u32 index = 0; index < count; ++index) {
        if (column_buffer[index].resolved && column_buffer[index].height <= from_height) {
            return column_buffer[index];
        }
    }
    SurfaceSample missing;
    missing.hole = (count > 0) && column_buffer[count - 1].hole;
    return missing;
}

SurfaceSample TerrainQuery::above(f64 x, f64 z, f32 from_height) const noexcept {
    SurfaceSample column_buffer[kMaxColumnSurfaces];
    const u32 count = column(x, z, Span<SurfaceSample>(column_buffer, kMaxColumnSurfaces));
    SurfaceSample found;
    for (u32 index = 0; index < count; ++index) {
        if (column_buffer[index].resolved && column_buffer[index].height >= from_height) {
            found = column_buffer[index];
        }
    }
    return found;
}

RayHit TerrainQuery::raycast(f64 origin_x, f32 origin_height, f64 origin_z, Vec3 direction,
                             f32 max_distance) const noexcept {
    RayHit result;
    const f32 length = std::sqrt((direction.x * direction.x) + (direction.y * direction.y) +
                                 (direction.z * direction.z));
    if (length <= 0.0F || max_distance <= 0.0F) {
        return result;
    }
    const Vec3 step{direction.x / length, direction.y / length, direction.z / length};

    // A FIXED step and a FIXED number of bisections, so that two machines marching the same ray
    // take the same samples. A step derived from wall time or from an adaptive tolerance would make
    // a gameplay query a function of the machine it ran on.
    constexpr u32 kMarchSteps = 256;
    constexpr u32 kBisections = 16;
    const f32 march = max_distance / static_cast<f32>(kMarchSteps);

    f32 previous_distance = 0.0F;
    SurfaceSample previous = sample(origin_x, origin_z);
    f32 previous_gap = origin_height - previous.height;
    for (u32 index = 1; index <= kMarchSteps; ++index) {
        const f32 distance = static_cast<f32>(index) * march;
        const f64 x = origin_x + (static_cast<f64>(step.x) * static_cast<f64>(distance));
        const f64 z = origin_z + (static_cast<f64>(step.z) * static_cast<f64>(distance));
        const f32 height = origin_height + (step.y * distance);
        const SurfaceSample here = sample(x, z);
        const f32 gap = height - here.height;
        if (here.resolved && previous.resolved && previous_gap > 0.0F && gap <= 0.0F) {
            f32 low = previous_distance;
            f32 high = distance;
            for (u32 refine = 0; refine < kBisections; ++refine) {
                const f32 middle = (low + high) * 0.5F;
                const f64 mx = origin_x + (static_cast<f64>(step.x) * static_cast<f64>(middle));
                const f64 mz = origin_z + (static_cast<f64>(step.z) * static_cast<f64>(middle));
                const SurfaceSample probe = sample(mx, mz);
                if (origin_height + (step.y * middle) - probe.height > 0.0F) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            result.distance = high;
            result.surface = sample(origin_x + (static_cast<f64>(step.x) * static_cast<f64>(high)),
                                    origin_z + (static_cast<f64>(step.z) * static_cast<f64>(high)));
            result.hit = true;
            return result;
        }
        previous = here;
        previous_gap = gap;
        previous_distance = distance;
    }
    return result;
}

Status TerrainQuery::sample_many(Span<const TerrainPoint> positions,
                                 Span<SurfaceSample> out) const noexcept {
    if (out.size() < positions.size()) {
        return fail(ErrorCode::BufferTooSmall,
                    "terrain: sample_many needs one output per position");
    }
    // One traversal of the source list for the whole batch. The sources are read once into a local
    // span so the per-position loop does no indirection through the array's bookkeeping.
    const Span<const SurfaceSource* const> sources = sources_.span();
    for (usize index = 0; index < positions.size(); ++index) {
        SurfaceSample column_buffer[kMaxColumnSurfaces];
        u32 written = 0;
        for (const SurfaceSource* source : sources) {
            if (written >= kMaxColumnSurfaces) {
                break;
            }
            written += source->column(
                positions[index].x, positions[index].z,
                Span<SurfaceSample>(column_buffer + written, kMaxColumnSurfaces - written));
        }
        sort_descending(Span<SurfaceSample>(column_buffer, kMaxColumnSurfaces), written);
        out[index] = SurfaceSample{};
        for (u32 entry = 0; entry < written; ++entry) {
            if (column_buffer[entry].resolved) {
                out[index] = column_buffer[entry];
                break;
            }
        }
        if (written > 0 && !out[index].resolved) {
            out[index] = column_buffer[0];
        }
    }
    return ok();
}

}  // namespace cy::terrain
