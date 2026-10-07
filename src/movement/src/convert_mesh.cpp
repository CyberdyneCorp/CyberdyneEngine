// SPDX-License-Identifier: MIT
// The load-time conversion of a baked navigation mesh into a `Fixed` world. Design §7.1 and §9.1.
//
// THIS IS ONE OF THE MODULE'S TWO FLOAT TRANSLATION UNITS (the other is height_field_cook.cpp), and
// both are boundaries: they read the float a bake wrote, convert it once with
// `detmath::from_f32_cooked` — an IEEE-exact operation, so the same everywhere — and never run
// inside a tick. `grep -rn "from_f32_cooked"` lists them, as design §7.3 requires.

#include <cy/core/detmath/convert.h>
#include <cy/movement/nav_mesh.h>

#include <algorithm>

namespace cy::movement {

namespace {

/// The largest magnitude a coordinate may have: 2^28 m, so the sum of a polygon's six corners and
/// every difference a query forms stay inside the Q32.32 range of ±2^31.
constexpr f32 kConvertibleRange = 268435456.0F;

[[nodiscard]] bool convertible(f32 value) noexcept {
    return value > -kConvertibleRange && value < kConvertibleRange;
}

/// The index `ref` was given, by binary search over the references in conversion order — which is
/// ascending bit order, because a reference is (tile slot, salt, polygon) from the high bits down
/// and the conversion walks slots, then polygons, in order.
[[nodiscard]] FixedPolyIndex index_of(Span<const navigation::PolyRef> refs,
                                      navigation::PolyRef ref) noexcept {
    if (!ref.valid()) {
        return kNoPoly;
    }
    const navigation::PolyRef* first = refs.data();
    const navigation::PolyRef* last = refs.data() + refs.size();
    const navigation::PolyRef* found = std::lower_bound(first, last, ref);
    return (found != last && *found == ref) ? static_cast<FixedPolyIndex>(found - first) : kNoPoly;
}

}  // namespace

Expected<NavConversionReport, Error> FixedNavMesh::convert(
    const navigation::NavMesh& source) noexcept {
    polys_.clear();
    corners_.clear();
    corner_heights_.clear();
    neighbours_.clear();
    crossings_.clear();
    blocked_.clear();

    NavConversionReport report;
    Array<navigation::PolyRef> refs(polys_.allocator());
    for (u32 slot = 0; slot < source.tile_capacity(); ++slot) {
        const u32 count = source.tile_poly_count(slot);
        report.tiles += count != 0 ? 1U : 0U;
        for (u32 index = 0; index < count; ++index) {
            if (Status pushed = refs.push_back(source.tile_poly(slot, index)); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    if (refs.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: a Fixed world needs a navigation mesh with at least one polygon");
    }

    for (const navigation::PolyRef ref : refs) {
        const navigation::NavPoly* source_poly = source.poly(ref);
        Vec3 corners[navigation::kMaxPolyVertices];
        const u32 count = source.poly_vertices(ref, corners, navigation::kMaxPolyVertices);
        if (source_poly == nullptr || count < 3) {
            return fail(ErrorCode::InvalidArgument,
                        "movement: a navigation polygon has fewer than three corners");
        }
        FixedNavPoly poly;
        poly.first = static_cast<u32>(corners_.size());
        poly.corner_count = static_cast<u8>(count);
        poly.area = source.effective_area(ref);
        poly.cost = detmath::from_f32_cooked(source_poly->cost);
        poly.source = ref;

        Fixed sum_x;
        Fixed sum_z;
        Fixed sum_y;
        const Span<const navigation::PolyRef> across = source.poly_neighbours(ref);
        for (u32 corner = 0; corner < count; ++corner) {
            const Vec3 at = corners[corner];
            if (!convertible(at.x) || !convertible(at.y) || !convertible(at.z)) {
                return fail(ErrorCode::OutOfRange,
                            "movement: a navigation vertex lies beyond the Fixed range");
            }
            const FixedVec2 plane{detmath::from_f32_cooked(at.x), detmath::from_f32_cooked(at.z)};
            const Fixed height = detmath::from_f32_cooked(at.y);
            const FixedPolyIndex neighbour =
                corner < across.size() ? index_of(refs.span(), across[corner]) : kNoPoly;
            if (!corners_.push_back(plane) || !corner_heights_.push_back(height) ||
                !neighbours_.push_back(neighbour)) {
                return fail(ErrorCode::OutOfMemory, "movement: could not store a converted corner");
            }
            (neighbour == kNoPoly ? report.border_edges : report.internal_edges) += 1;
            sum_x += plane.x;
            sum_z += plane.y;
            sum_y += height;
            poly.min = corner == 0 ? plane
                                   : FixedVec2{std::min(poly.min.x, plane.x),
                                               std::min(poly.min.y, plane.y)};
            poly.max = corner == 0 ? plane
                                   : FixedVec2{std::max(poly.max.x, plane.x),
                                               std::max(poly.max.y, plane.y)};
        }
        const Fixed divisor = Fixed::from_int(static_cast<i32>(count));
        poly.centre = FixedVec2{sum_x / divisor, sum_z / divisor};
        poly.height = sum_y / divisor;
        if (Status pushed = polys_.push_back(poly); !pushed) {
            return make_unexpected(pushed.error());
        }
        report.corners += count;
    }
    report.polys = static_cast<u32>(polys_.size());
    report.links_dropped = source.link_count();
    source_version_ = source.version();

    if (Status sized = blocked_.resize(polys_.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status built = build_buckets(); !built) {
        return make_unexpected(built.error());
    }
    if (Status built = build_crossings(); !built) {
        return make_unexpected(built.error());
    }
    return report;
}

}  // namespace cy::movement
