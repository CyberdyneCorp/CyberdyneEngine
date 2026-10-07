// SPDX-License-Identifier: MIT
// Queries on a converted `Fixed` navigation mesh. Design §9.1.
//
// Integer arithmetic only. Every decision — inside or outside, which edge is nearer, which polygon
// a point on a shared edge belongs to — is made on an exact Q64.64 product or square, and every tie
// is broken by an index, so two peers that converted the same bytes answer every query alike.

#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/shapes.h>
#include <cy/core/memory/hash.h>
#include <cy/movement/nav_mesh.h>

namespace cy::movement {

namespace {

/// The squared distance from `point` to the box [min, max], exactly: zero inside.
[[nodiscard]] WideFixed box_distance_squared(FixedVec2 min, FixedVec2 max,
                                             FixedVec2 point) noexcept {
    const auto axis = [](Fixed low, Fixed high, Fixed value) noexcept {
        if (value < low) {
            return low - value;
        }
        if (value > high) {
            return value - high;
        }
        return Fixed::zero();
    };
    const FixedVec2 outside{axis(min.x, max.x, point.x), axis(min.y, max.y, point.y)};
    return detmath::length_squared(outside);
}

[[nodiscard]] bool within_box(const FixedNavPoly& poly, FixedVec2 point) noexcept {
    return point.x >= poly.min.x && point.x <= poly.max.x && point.y >= poly.min.y &&
           point.y <= poly.max.y;
}

}  // namespace

FixedNavMesh::FixedNavMesh(Allocator& allocator) noexcept
    : polys_(allocator),
      corners_(allocator),
      corner_heights_(allocator),
      neighbours_(allocator),
      crossings_(allocator),
      blocked_(allocator),
      bucket_starts_(allocator),
      bucket_polys_(allocator) {}

Status FixedNavMesh::check_source(const navigation::NavMesh& source) const noexcept {
    if (source.version() != source_version_) {
        return fail(ErrorCode::PermissionDenied,
                    "movement: the navigation mesh was rebuilt after it was converted; a runtime "
                    "rebuild is float work on each peer and is refused as authoritative input in "
                    "a Fixed world");
    }
    return ok();
}

void FixedNavMesh::set_blocked(FixedPolyIndex index, bool blocked) noexcept {
    if (index < blocked_.size()) {
        blocked_[index] = blocked ? u8{1} : u8{0};
    }
}

i64 FixedNavMesh::bucket_x(Fixed x) const noexcept {
    return (x.raw >> (Fixed::kFractionBits + kBucketShift)) - bucket_origin_x_;
}

i64 FixedNavMesh::bucket_z(Fixed z) const noexcept {
    return (z.raw >> (Fixed::kFractionBits + kBucketShift)) - bucket_origin_z_;
}

Status FixedNavMesh::build_buckets() noexcept {
    min_ = polys_[0].min;
    max_ = polys_[0].max;
    for (const FixedNavPoly& poly : polys_) {
        min_ = FixedVec2{poly.min.x < min_.x ? poly.min.x : min_.x,
                         poly.min.y < min_.y ? poly.min.y : min_.y};
        max_ = FixedVec2{poly.max.x > max_.x ? poly.max.x : max_.x,
                         poly.max.y > max_.y ? poly.max.y : max_.y};
    }
    bucket_origin_x_ = 0;
    bucket_origin_z_ = 0;
    bucket_origin_x_ = bucket_x(min_.x);
    bucket_origin_z_ = bucket_z(min_.y);
    buckets_x_ = static_cast<u32>(bucket_x(max_.x) + 1);
    buckets_z_ = static_cast<u32>(bucket_z(max_.y) + 1);

    // Counting sort into buckets, polygons in index order within each, so `locate()` meets them in
    // the order its tie-break needs.
    bucket_starts_.clear();
    if (Status sized = bucket_starts_.resize((usize{buckets_x_} * buckets_z_) + 1); !sized) {
        return sized;
    }
    for (int pass = 0; pass < 2; ++pass) {
        Array<u32> fill(bucket_starts_.allocator());
        if (pass == 1) {
            u32 running = 0;
            for (u32& start : bucket_starts_) {
                const u32 count = start;
                start = running;
                running += count;
            }
            bucket_polys_.clear();
            if (Status sized = bucket_polys_.resize(running); !sized) {
                return sized;
            }
            if (Status copied = fill.append(bucket_starts_.span()); !copied) {
                return copied;
            }
        }
        for (FixedPolyIndex index = 0; index < poly_count(); ++index) {
            const FixedNavPoly& poly = polys_[index];
            for (i64 z = bucket_z(poly.min.y); z <= bucket_z(poly.max.y); ++z) {
                for (i64 x = bucket_x(poly.min.x); x <= bucket_x(poly.max.x); ++x) {
                    const auto bucket = static_cast<usize>((z * buckets_x_) + x);
                    if (pass == 0) {
                        ++bucket_starts_[bucket];
                    } else {
                        bucket_polys_[fill[bucket]++] = index;
                    }
                }
            }
        }
    }
    return ok();
}

Status FixedNavMesh::build_crossings() noexcept {
    crossings_.clear();
    if (Status sized = crossings_.resize(neighbours_.size()); !sized) {
        return sized;
    }
    for (const FixedNavPoly& poly : polys_) {
        for (u32 edge = 0; edge < poly.corner_count; ++edge) {
            const FixedPolyIndex across = neighbours_[poly.first + edge];
            crossings_[poly.first + edge] =
                across == kNoPoly ? Fixed::zero()
                                  : detmath::distance(poly.centre, polys_[across].centre);
        }
    }
    return ok();
}

bool FixedNavMesh::contains(FixedPolyIndex index, FixedVec2 point) const noexcept {
    const Span<const FixedVec2> corners = corners_of(index);
    bool any_positive = false;
    bool any_negative = false;
    for (usize i = 0; i < corners.size(); ++i) {
        const FixedVec2 a = corners[i];
        const FixedVec2 b = corners[(i + 1) % corners.size()];
        const WideFixed side = detmath::cross(b - a, point - a);
        any_positive = any_positive || (!side.negative() && side != WideFixed{});
        any_negative = any_negative || side.negative();
    }
    // Convex: inside when no edge has the point strictly on one side and another edge strictly on
    // the other. Independent of the winding, which the converted mesh does not normalise.
    return !(any_positive && any_negative);
}

FixedPolyIndex FixedNavMesh::locate(FixedVec2 point) const noexcept {
    if (polys_.empty()) {
        return kNoPoly;
    }
    const i64 x = bucket_x(point.x);
    const i64 z = bucket_z(point.y);
    if (x < 0 || z < 0 || x >= i64{buckets_x_} || z >= i64{buckets_z_}) {
        return kNoPoly;
    }
    const auto bucket = static_cast<usize>((z * buckets_x_) + x);
    for (u32 slot = bucket_starts_[bucket]; slot < bucket_starts_[bucket + 1]; ++slot) {
        const FixedPolyIndex candidate = bucket_polys_[slot];
        if (within_box(polys_[candidate], point) && contains(candidate, point)) {
            return candidate;
        }
    }
    return kNoPoly;
}

FixedVec2 FixedNavMesh::closest_point(FixedPolyIndex index, FixedVec2 point) const noexcept {
    if (contains(index, point)) {
        return point;
    }
    const Span<const FixedVec2> corners = corners_of(index);
    FixedVec2 best = corners[0];
    WideFixed best_distance;
    for (usize i = 0; i < corners.size(); ++i) {
        const FixedVec2 on_edge =
            detmath::closest_point_on_segment(corners[i], corners[(i + 1) % corners.size()], point);
        const WideFixed distance = detmath::distance_squared(on_edge, point);
        if (i == 0 || distance < best_distance) {
            best = on_edge;
            best_distance = distance;
        }
    }
    return best;
}

FixedPolyIndex FixedNavMesh::nearest(FixedVec2 point, FixedVec2& on_mesh) const noexcept {
    const FixedPolyIndex inside = locate(point);
    if (inside != kNoPoly) {
        on_mesh = point;
        return inside;
    }
    FixedPolyIndex best = kNoPoly;
    WideFixed best_distance;
    for (FixedPolyIndex index = 0; index < poly_count(); ++index) {
        const FixedNavPoly& poly = polys_[index];
        if (best != kNoPoly && !(box_distance_squared(poly.min, poly.max, point) < best_distance)) {
            continue;
        }
        const FixedVec2 candidate = closest_point(index, point);
        const WideFixed distance = detmath::distance_squared(candidate, point);
        if (best == kNoPoly || distance < best_distance) {
            best = index;
            best_distance = distance;
            on_mesh = candidate;
        }
    }
    return best;
}

FixedVec2 FixedNavMesh::clamp_move(FixedPolyIndex& from, FixedVec2 desired) const noexcept {
    if (from >= poly_count()) {
        FixedVec2 on_mesh = desired;
        from = nearest(desired, on_mesh);
        return on_mesh;
    }
    if (contains(from, desired)) {
        return desired;
    }
    const Span<const FixedPolyIndex> ring = neighbours_of(from);
    FixedPolyIndex entered = kNoPoly;
    for (const FixedPolyIndex neighbour : ring) {
        if (neighbour != kNoPoly && neighbour < entered && contains(neighbour, desired)) {
            entered = neighbour;
        }
    }
    if (entered != kNoPoly) {
        from = entered;
        return desired;
    }
    // Off the ring: the nearest boundary point of `from` and its neighbours, `from` first, then the
    // neighbours in edge order, a later candidate winning only when strictly nearer.
    FixedVec2 best = closest_point(from, desired);
    WideFixed best_distance = detmath::distance_squared(best, desired);
    FixedPolyIndex best_poly = from;
    for (const FixedPolyIndex neighbour : ring) {
        if (neighbour == kNoPoly) {
            continue;
        }
        const FixedVec2 candidate = closest_point(neighbour, desired);
        const WideFixed distance = detmath::distance_squared(candidate, desired);
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
            best_poly = neighbour;
        }
    }
    from = best_poly;
    return best;
}

bool FixedNavMesh::portal(FixedPolyIndex from, FixedPolyIndex to, FixedVec2& a,
                          FixedVec2& b) const noexcept {
    const Span<const FixedVec2> corners = corners_of(from);
    const Span<const FixedPolyIndex> across = neighbours_of(from);
    for (usize i = 0; i < across.size(); ++i) {
        if (across[i] == to) {
            a = corners[i];
            b = corners[(i + 1) % corners.size()];
            return true;
        }
    }
    return false;
}

u64 FixedNavMesh::digest() const noexcept {
    u64 fold = hash_combine(0xF1'3ED0'0A5ULL, polys_.size());
    for (const FixedNavPoly& poly : polys_) {
        fold = hash_combine(fold, (u64{poly.corner_count} << 8U) | poly.area);
        fold = hash_combine(fold, static_cast<u64>(poly.cost.raw));
        fold = hash_combine(fold, static_cast<u64>(poly.height.raw));
    }
    for (usize i = 0; i < corners_.size(); ++i) {
        fold = hash_combine(fold, static_cast<u64>(corners_[i].x.raw));
        fold = hash_combine(fold, static_cast<u64>(corners_[i].y.raw));
        fold = hash_combine(fold, static_cast<u64>(corner_heights_[i].raw));
        fold = hash_combine(fold, neighbours_[i]);
    }
    return fold;
}

}  // namespace cy::movement
