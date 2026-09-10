// Camera volumes and the grid that finds them. M8.b task 7.3.

#include <cy/camera/volume.h>

#include <algorithm>
#include <cmath>

namespace cy::camera {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// The influence of one volume at a point: one inside the bounds, ramping to zero across the blend
/// distance outside them, zero beyond. The distance is the point's distance to the box, which is
/// the only formulation that behaves at a corner.
[[nodiscard]] f32 influence_of(const CameraVolume& volume, Vec3 point) noexcept {
    const Vec3 lo = volume.bounds.min;
    const Vec3 hi = volume.bounds.max;
    // The distance from the point to the box, one axis at a time: zero inside the slab, and how
    // far outside it otherwise. Written as a `max` pair rather than a chain of conditionals because
    // that is what it is, and the corner case — outside on two axes — falls out of the length.
    const auto axis_distance = [](f32 value, f32 low, f32 high) noexcept {
        return std::max({low - value, value - high, 0.0F});
    };
    const Vec3 outside{axis_distance(point.x, lo.x, hi.x), axis_distance(point.y, lo.y, hi.y),
                       axis_distance(point.z, lo.z, hi.z)};
    const f32 distance = length(outside);
    if (distance <= 0.0F) {
        return 1.0F;
    }
    if (volume.blend_distance <= 0.0F || distance >= volume.blend_distance) {
        return 0.0F;
    }
    return 1.0F - (distance / volume.blend_distance);
}

[[nodiscard]] f32 mix(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

}  // namespace

VolumeBlend blend_volumes(Span<const CameraVolume> candidates, Vec3 point,
                          Array<VolumeInfluence>& influences) noexcept {
    influences.clear();
    VolumeBlend out;

    // THE DECLARED RULE, and it is the one post-process volumes use: priority selects, weight
    // blends. The highest priority with any influence owns each field it overrides; volumes at that
    // same priority blend among themselves by weight.
    i32 top_priority = 0;
    bool have_priority = false;
    for (const CameraVolume& volume : candidates) {
        const f32 weight = influence_of(volume, point);
        if (weight <= 0.0F) {
            continue;
        }
        if (!have_priority || volume.priority > top_priority) {
            top_priority = volume.priority;
            have_priority = true;
        }
    }
    if (!have_priority) {
        return out;
    }

    f32 total = 0.0F;
    for (const CameraVolume& volume : candidates) {
        const f32 weight = influence_of(volume, point);
        if (weight <= 0.0F) {
            continue;
        }
        VolumeInfluence influence;
        influence.name = volume.name;
        influence.weight = weight;
        influence.priority = volume.priority;
        // Recorded even when it loses the priority contest: "the active volumes and their weights"
        // is a diagnostic, and a volume the camera is inside that changes nothing is exactly what a
        // developer is looking for when the camera is not doing what they expected.
        if (Status pushed = influences.push_back(influence); !pushed) {
            break;
        }
        if (volume.priority != top_priority) {
            continue;
        }
        const f32 blend = (total <= 0.0F) ? 1.0F : (weight / (total + weight));
        if (volume.settings.overrides_fov) {
            out.settings.fov_y_radians =
                out.settings.overrides_fov
                    ? mix(out.settings.fov_y_radians, volume.settings.fov_y_radians, blend)
                    : volume.settings.fov_y_radians;
            out.settings.overrides_fov = true;
        }
        if (volume.settings.overrides_smoothing) {
            out.settings.smoothing_half_life = out.settings.overrides_smoothing
                                                   ? mix(out.settings.smoothing_half_life,
                                                         volume.settings.smoothing_half_life, blend)
                                                   : volume.settings.smoothing_half_life;
            out.settings.overrides_smoothing = true;
        }
        if (volume.settings.overrides_distance) {
            out.settings.distance =
                out.settings.overrides_distance
                    ? mix(out.settings.distance, volume.settings.distance, blend)
                    : volume.settings.distance;
            out.settings.overrides_distance = true;
        }
        if (volume.settings.overrides_collision) {
            // A POLICY IS NOT A NUMBER. Responses and tag masks do not interpolate, so the
            // highest-weight contributor at the top priority takes them whole.
            if (!out.settings.overrides_collision || weight > total) {
                out.settings.collision = volume.settings.collision;
            }
            out.settings.overrides_collision = true;
        }
        if (volume.settings.overrides_composition) {
            if (!out.settings.overrides_composition || weight > total) {
                out.settings.composition = volume.settings.composition;
            }
            out.settings.overrides_composition = true;
        }
        out.settings.post_process_weight =
            out.settings.post_process_weight + (volume.settings.post_process_weight * weight);
        total += weight;
        ++out.contributors;
    }
    out.total_weight = clampf(total, 0.0F, 1.0F);
    return out;
}

VolumeSet::VolumeSet(Allocator& allocator, f32 cell_size) noexcept
    : volumes_(allocator),
      buckets_(allocator),
      cell_size_((cell_size > 0.0F) ? cell_size : 32.0F) {}

Status VolumeSet::add(const CameraVolume& volume) noexcept {
    const u32 index = static_cast<u32>(volumes_.size());
    if (Status pushed = volumes_.push_back(volume); !pushed) {
        return pushed;
    }
    // Every grid cell the grown bounds touch gets a bucket entry, so a query visits one cell and
    // finds every volume that could possibly influence it. A very large volume produces many
    // entries, which is the cost this shape pays and the reason `cell_size` is a parameter.
    const f32 grow = (volume.blend_distance > 0.0F) ? volume.blend_distance : 0.0F;
    const Vec3 lo = volume.bounds.min - Vec3{grow, grow, grow};
    const Vec3 hi = volume.bounds.max + Vec3{grow, grow, grow};
    const auto cell_of = [this](f32 value) noexcept {
        return static_cast<i32>(std::floor(value / cell_size_));
    };
    for (i32 x = cell_of(lo.x); x <= cell_of(hi.x); ++x) {
        for (i32 y = cell_of(lo.y); y <= cell_of(hi.y); ++y) {
            for (i32 z = cell_of(lo.z); z <= cell_of(hi.z); ++z) {
                if (Status pushed = buckets_.push_back(Bucket{cell_key(x, y, z), index}); !pushed) {
                    return pushed;
                }
                sorted_ = false;
            }
        }
    }
    return ok();
}

void VolumeSet::clear() noexcept {
    volumes_.clear();
    buckets_.clear();
    tested_ = 0;
    sorted_ = true;
}

Status VolumeSet::query(Vec3 point, Array<CameraVolume>& out) noexcept {
    out.clear();
    tested_ = 0;
    const auto cell_of = [this](f32 value) noexcept {
        return static_cast<i32>(std::floor(value / cell_size_));
    };
    const u64 key = cell_key(cell_of(point.x), cell_of(point.y), cell_of(point.z));
    if (!sorted_) {
        // Sorted once after the volumes are declared, not per query: a level's camera volumes are
        // authored data and change when a cell streams in, which is where the cost belongs.
        Span<Bucket> span = buckets_.span();
        std::ranges::sort(span,
                          [](const Bucket& a, const Bucket& b) noexcept { return a.key < b.key; });
        sorted_ = true;
    }
    // BINARY SEARCH, not a scan: "Volumes SHALL be found through the world's spatial index rather
    // than by testing every volume each frame", and `tested()` reports what the query actually
    // compared so the claim is a measurement.
    Span<const Bucket> span = buckets_.span();
    usize low = 0;
    usize high = span.size();
    while (low < high) {
        const usize mid = low + ((high - low) / 2);
        if (span[mid].key < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    for (usize index = low; index < span.size() && span[index].key == key; ++index) {
        ++tested_;
        const CameraVolume& volume = volumes_[span[index].volume];
        if (influence_of(volume, point) <= 0.0F) {
            continue;
        }
        if (Status pushed = out.push_back(volume); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::camera
