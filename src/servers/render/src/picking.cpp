// Engine-side picking against the draw list a view produced. See cy/servers/render/picking.h.

#include <cy/servers/render/picking.h>

#include <cy/core/math/geometry.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/scalar.h>

#include <algorithm>

namespace cy::render {
namespace {

/// Unproject a point of the presented frame into world space.
///
/// `depth` is in the engine's REVERSED-Z convention: 1 is the near plane, 0 is infinitely far. That
/// is why the ray below is built from depths 1 and 0.5 rather than from 0 and 1 — with the engine's
/// default infinite far plane, depth 0 is at infinity and the subtraction that would give a
/// direction is a subtraction of infinities.
[[nodiscard]] bool unproject(const Mat4& inverse_view_projection, f32 ndc_x, f32 ndc_y, f32 depth,
                             Vec3& out) noexcept {
    const Vec4 clip{ndc_x, ndc_y, depth, 1.0F};
    const Vec4 world = inverse_view_projection * clip;
    if (math::nearly_zero(world.w)) {
        return false;
    }
    const f32 inverse_w = 1.0F / world.w;
    out = Vec3{world.x * inverse_w, world.y * inverse_w, world.z * inverse_w};
    return true;
}

/// The viewport rect a view draws into, with a zero extent replaced by one pixel.
///
/// A zeroed `ViewportRect` is what a default-constructed `View` carries, and dividing by its width
/// would produce an infinity that then propagates into every candidate's distance. One pixel is not
/// a meaningful viewport, but it produces a ray rather than a NaN, and a caller that forgot to size
/// its view sees a pick that misses rather than a pick that poisons an ordering.
[[nodiscard]] ViewportRect sized_viewport(const View& view) noexcept {
    ViewportRect rect = view.desc.viewport;
    rect.width = (rect.width == 0) ? 1U : rect.width;
    rect.height = (rect.height == 0) ? 1U : rect.height;
    return rect;
}

[[nodiscard]] Sphere world_sphere_of(const GpuInstance& record) noexcept {
    return Sphere{Vec3{record.bounds_center[0], record.bounds_center[1], record.bounds_center[2]},
                  record.bounds_radius};
}

[[nodiscard]] bool is_excluded(Span<const u64> excluded, u64 stable_id) noexcept {
    return std::ranges::find(excluded, stable_id) != excluded.end();
}

/// Whether a draw is one this filter accepts at all, before any geometry is tested.
[[nodiscard]] bool accepted(const DrawItem& item, const PickFilter& filter,
                            bool transparent) noexcept {
    if (transparent && !filter.include_transparent) {
        return false;
    }
    return !is_excluded(filter.excluded, item.stable_id);
}

/// The record a draw item refers to, or null when the slot is out of range.
///
/// Out of range means the caller paired a draw list with a different scene's records, which is a
/// programmer error the pick refuses to guess about — skipping is what keeps it from reading a
/// neighbouring instance's bounds and reporting a confident wrong answer.
[[nodiscard]] const GpuInstance* record_for(Span<const GpuInstance> records,
                                            const DrawItem& item) noexcept {
    return (item.instance_slot < records.size()) ? &records[item.instance_slot] : nullptr;
}

/// The total order candidates are reported in. See the header: two candidates may share a distance
/// and a centrality, and no two may share a stable identity, so this is total.
[[nodiscard]] bool candidate_precedes(const PickCandidate& a, const PickCandidate& b) noexcept {
    if (a.distance != b.distance) {
        return a.distance < b.distance;
    }
    if (a.centrality != b.centrality) {
        return a.centrality < b.centrality;
    }
    if (a.stable_id != b.stable_id) {
        return a.stable_id < b.stable_id;
    }
    return a.surface < b.surface;
}

/// Order, collapse the several draws one instance produced into one candidate, and apply the cap.
///
/// One instance with three material slots is three draws (sort.h) with identical bounds, so they
/// sort adjacently under `candidate_precedes` and the collapse is a scan rather than a set. The
/// surviving entry is the lowest surface index, which is a property of the instance rather than of
/// which draw the loop happened to reach first.
void finalise(Array<PickCandidate>& out, u32 max_candidates) noexcept {
    std::ranges::sort(out, candidate_precedes);

    usize kept = 0;
    for (usize index = 0; index < out.size(); ++index) {
        if (kept > 0 && out[kept - 1].stable_id == out[index].stable_id) {
            continue;
        }
        out[kept] = out[index];
        ++kept;
    }
    while (out.size() > kept) {
        out.pop_back();
    }

    if (max_candidates != 0) {
        while (out.size() > static_cast<usize>(max_candidates)) {
            out.pop_back();
        }
    }
}

/// The candidates an area test accepts, shared by the rectangle and the lasso.
///
/// `inside` is given the projected pixel of a record's bounding centre and answers whether it is in
/// the region. Both callers are otherwise identical, and a second copy of this loop is where the
/// rectangle and the lasso would drift apart in what they consider visible.
template <class InsideFn>
[[nodiscard]] Status collect_area(Span<const GpuInstance> records, const View& view,
                                  Span<const DrawItem> drawn, const PickFilter& filter,
                                  InsideFn inside, Array<PickCandidate>& out) noexcept {
    out.clear();
    for (const DrawItem& item : drawn) {
        const bool transparent = sort_key_layer(item.key) == SortLayer::Transparent;
        if (!accepted(item, filter, transparent)) {
            continue;
        }
        const GpuInstance* record = record_for(records, item);
        if (record == nullptr) {
            continue;
        }
        Vec2 pixel{0.0F, 0.0F};
        if (!project_to_pixel(view, world_sphere_of(*record).center, pixel) || !inside(pixel)) {
            continue;
        }
        PickCandidate candidate;
        candidate.stable_id = item.stable_id;
        candidate.instance_slot = item.instance_slot;
        candidate.surface = item.surface;
        candidate.transparent = transparent;
        if (Status pushed = out.push_back(candidate); !pushed) {
            return pushed;
        }
    }
    finalise(out, filter.max_candidates);
    return ok();
}

}  // namespace

Ray ray_through_pixel(const View& view, f32 pixel_x, f32 pixel_y) noexcept {
    const ViewportRect rect = sized_viewport(view);
    // Pixel coordinates are top-left origin, normalised device coordinates are bottom-left origin.
    // This subtraction is the ONE place the flip happens, so a caller never has to know it exists —
    // and `project_to_pixel` below is its exact inverse, which is what a test asserts.
    const f32 across = (pixel_x - static_cast<f32>(rect.x)) / static_cast<f32>(rect.width);
    const f32 down = (pixel_y - static_cast<f32>(rect.y)) / static_cast<f32>(rect.height);
    const f32 ndc_x = (across * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (down * 2.0F);

    // The fallback, used when the view-projection cannot be inverted: the camera's own axis. A
    // degenerate projection has no ray through a pixel, and a caller with a broken view should get
    // a pick that finds what is straight ahead rather than a NaN that finds everything.
    const Ray fallback{view.desc.camera.translation, view.desc.camera.forward()};

    const Expected<Mat4, Error> inverted = inverse(view.view_projection);
    if (!inverted) {
        return fallback;
    }
    Vec3 near_point{0.0F, 0.0F, 0.0F};
    Vec3 mid_point{0.0F, 0.0F, 0.0F};
    if (!unproject(inverted.value(), ndc_x, ndc_y, 1.0F, near_point) ||
        !unproject(inverted.value(), ndc_x, ndc_y, 0.5F, mid_point)) {
        return fallback;
    }
    const Vec3 direction = mid_point - near_point;
    if (length_squared(direction) <= math::kSmallLength) {
        return fallback;
    }
    return Ray{near_point, normalize(direction)};
}

bool project_to_pixel(const View& view, Vec3 world, Vec2& out_pixel) noexcept {
    // Behind the camera is decided in VIEW space rather than from the clip w, because an
    // orthographic projection's w is 1 everywhere and a point behind the eye would project onto the
    // screen as confidently as one in front of it. The camera looks down its local −Z, so a visible
    // point has a negative view-space z.
    const Vec4 view_space = view.view_matrix * Vec4{world.x, world.y, world.z, 1.0F};
    if (view_space.z >= 0.0F) {
        return false;
    }
    const Vec4 clip = view.projection_matrix * view_space;
    if (math::nearly_zero(clip.w)) {
        return false;
    }
    const f32 inverse_w = 1.0F / clip.w;
    const ViewportRect rect = sized_viewport(view);
    // The exact inverse of `ray_through_pixel`'s flip, written in the same two steps so the two can
    // be read against each other.
    const f32 across = ((clip.x * inverse_w) * 0.5F) + 0.5F;
    const f32 down = 0.5F - ((clip.y * inverse_w) * 0.5F);
    out_pixel = Vec2{static_cast<f32>(rect.x) + (across * static_cast<f32>(rect.width)),
                     static_cast<f32>(rect.y) + (down * static_cast<f32>(rect.height))};
    return true;
}

Status pick_ray(Span<const GpuInstance> records, Span<const DrawItem> drawn, const Ray& ray,
                const PickFilter& filter, Array<PickCandidate>& out) noexcept {
    out.clear();
    const Vec3 direction = normalized_or(ray.direction, Vec3{0.0F, 0.0F, -1.0F});

    for (const DrawItem& item : drawn) {
        const bool transparent = sort_key_layer(item.key) == SortLayer::Transparent;
        if (!accepted(item, filter, transparent)) {
            continue;
        }
        const GpuInstance* record = record_for(records, item);
        if (record == nullptr) {
            continue;
        }
        const Sphere bounds = world_sphere_of(*record);
        f32 distance = 0.0F;
        if (!geom::ray_sphere(Ray{ray.origin, direction}, bounds, math::kInfinity, distance)) {
            continue;
        }

        // How near the ray's line passes the centre, as a fraction of the radius. The tie-break
        // that makes a click between two concentric objects prefer the one under the cursor rather
        // than the one that happens to be nearer by a micron.
        const Vec3 to_center = bounds.center - ray.origin;
        const Vec3 closest = ray.origin + direction * dot(to_center, direction);
        const f32 radius = (bounds.radius > math::kEpsilon) ? bounds.radius : 1.0F;
        const f32 centrality = math::min(length(bounds.center - closest) / radius, 1.0F);

        PickCandidate candidate;
        candidate.stable_id = item.stable_id;
        candidate.instance_slot = item.instance_slot;
        candidate.surface = item.surface;
        candidate.distance = distance;
        candidate.centrality = centrality;
        candidate.transparent = transparent;
        if (Status pushed = out.push_back(candidate); !pushed) {
            return pushed;
        }
    }

    finalise(out, filter.max_candidates);
    return ok();
}

Status pick_rect(Span<const GpuInstance> records, const View& view, Span<const DrawItem> drawn,
                 const PickRect& rect, const PickFilter& filter,
                 Array<PickCandidate>& out) noexcept {
    return collect_area(
        records, view, drawn, filter,
        [&rect](Vec2 pixel) noexcept { return rect.contains(pixel.x, pixel.y); }, out);
}

Status pick_polygon(Span<const GpuInstance> records, const View& view, Span<const DrawItem> drawn,
                    Span<const Vec2> vertices, const PickFilter& filter,
                    Array<PickCandidate>& out) noexcept {
    if (vertices.size() < 3) {
        out.clear();
        return fail(ErrorCode::InvalidArgument, "a lasso needs at least three vertices");
    }
    return collect_area(
        records, view, drawn, filter,
        [&vertices](Vec2 pixel) noexcept {
            return geom::point_in_polygon(pixel, vertices.data(), vertices.size());
        },
        out);
}

u64 cycle_candidate(Span<const PickCandidate> candidates, u32 cycle) noexcept {
    if (candidates.empty()) {
        return 0;
    }
    return candidates[static_cast<usize>(cycle) % candidates.size()].stable_id;
}

}  // namespace cy::render
