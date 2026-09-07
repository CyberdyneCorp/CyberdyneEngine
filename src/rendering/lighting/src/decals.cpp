#include <cy/rendering/lighting/decals.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] f32 age_of(const DecalInstance& decal, f64 now_seconds) noexcept {
    const f64 age = now_seconds - decal.spawn_time_seconds;
    return age > 0.0 ? static_cast<f32>(age) : 0.0F;
}

}  // namespace

const char* decal_eviction_cause_name(DecalEvictionCause cause) noexcept {
    switch (cause) {
        case DecalEvictionCause::BudgetReached:
            return "budget-reached";
        case DecalEvictionCause::Removed:
            return "removed";
        case DecalEvictionCause::Count:
            break;
    }
    return "unknown";
}

f32 decal_retention_score(const DecalInstance& decal, f64 now_seconds,
                          f32 age_half_life_seconds) noexcept {
    const f32 age = age_of(decal, now_seconds);
    const f32 half_life = age_half_life_seconds > 0.0F ? age_half_life_seconds : 1.0F;
    // A decay rather than a subtraction: a decal spawned an hour ago and one spawned two hours ago
    // are both simply old, and distinguishing them by a number that has grown past f32's useful
    // precision makes eviction depend on rounding.
    const f32 freshness = std::exp2(-age / half_life);
    // Importance dominates, coverage next, freshness last. That order is the requirement's own
    // list read as a priority — "by age, screen coverage, and importance" names the three, and a
    // decal an author marked important should outlive a fresh one nobody can see.
    return (decal.importance * 4.0F) + (decal.screen_coverage * 2.0F) + freshness;
}

f32 decal_angle_fade(const DecalInstance& decal, Vec3 receiver_normal) noexcept {
    const Vec3 axis = normalized_or(decal.axis_z, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 normal = normalized_or(receiver_normal, axis);
    const f32 cosine = math::clamp(dot(normal, axis), -1.0F, 1.0F);
    if (cosine <= 0.0F) {
        return 0.0F;  // facing away from the projector entirely
    }
    const f32 limit = std::cos(math::clamp(decal.fade_angle_radians, 0.0F, 1.5707F));
    if (cosine <= limit) {
        return 0.0F;
    }
    // Smooth from the limit to face-on, so a decal on a curved surface does not show a hard edge
    // where the fade begins.
    const f32 t = (cosine - limit) / math::max(1.0F - limit, 1.0e-4F);
    return t * t * (3.0F - (2.0F * t));
}

f32 decal_distance_fade(const DecalInstance& decal, f32 distance_metres) noexcept {
    const f32 start = decal.fade_start;
    const f32 end = math::max(decal.fade_end, start + 1.0e-4F);
    if (distance_metres <= start) {
        return 1.0F;
    }
    if (distance_metres >= end) {
        return 0.0F;
    }
    const f32 t = 1.0F - ((distance_metres - start) / (end - start));
    return t * t * (3.0F - (2.0F * t));
}

DecalProjection project_decal(const DecalInstance& decal, Vec3 world_position) noexcept {
    const Vec3 offset = world_position - decal.center;
    const Vec3 axis_x = normalized_or(decal.axis_x, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 axis_y = normalized_or(decal.axis_y, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 axis_z = normalized_or(decal.axis_z, Vec3{0.0F, 0.0F, 1.0F});
    const f32 x = dot(offset, axis_x);
    const f32 y = dot(offset, axis_y);
    const f32 z = dot(offset, axis_z);

    DecalProjection projection;
    const Vec3 extent{math::max(decal.half_extent.x, 1.0e-5F),
                      math::max(decal.half_extent.y, 1.0e-5F),
                      math::max(decal.half_extent.z, 1.0e-5F)};
    projection.uv = Vec2{(x / (2.0F * extent.x)) + 0.5F, (y / (2.0F * extent.y)) + 0.5F};
    projection.depth = math::clamp((z + extent.z) / (2.0F * extent.z), 0.0F, 1.0F);
    projection.inside =
        std::fabs(x) <= extent.x && std::fabs(y) <= extent.y && std::fabs(z) <= extent.z;
    return projection;
}

Vec3 blend_decal_normal(Vec3 receiver_normal, Vec3 decal_normal, f32 strength) noexcept {
    const Vec3 base = normalized_or(receiver_normal, Vec3{0.0F, 0.0F, 1.0F});
    const Vec3 detail = normalized_or(decal_normal, Vec3{0.0F, 0.0F, 1.0F});
    const f32 amount = math::clamp(strength, 0.0F, 1.0F);

    // Reoriented normal mapping: rotate the decal's tangent-space normal onto the receiver's frame
    // rather than mixing the two vectors. A lerp of two unit normals flattens the receiver exactly
    // where the decal is strongest, which is the detail the requirement says to preserve.
    // `u`'s x and y are NEGATED and its z is not. That is not a typo and it is the whole of the
    // construction: the published form is written for [0, 1]-encoded normals as
    // `u = d * (-2, -2, 2) + (1, 1, -1)`, which for a unit vector is exactly (-d.x, -d.y, d.z).
    // Writing (d.x, d.y, -d.z) instead — the transcription that looks right — gives -base for a
    // flat detail normal, so every decal inverts the surface it lands on.
    const Vec3 t = base + Vec3{0.0F, 0.0F, 1.0F};
    const Vec3 u = Vec3{-detail.x, -detail.y, detail.z};
    const f32 scale = dot(t, u);
    const Vec3 reoriented = normalized_or(t * scale - u * t.z, base);
    return normalized_or(base + (reoriented - base) * amount, base);
}

void DecalBudget::set_capacity(u32 capacity) noexcept {
    capacity_ = capacity;
}

void DecalBudget::set_age_half_life(f32 seconds) noexcept {
    age_half_life_ = seconds > 0.0F ? seconds : 1.0F;
}

usize DecalBudget::weakest(f64 now_seconds) const noexcept {
    usize chosen = 0;
    f32 lowest = 3.4e38F;
    for (usize index = 0; index < decals_.size(); ++index) {
        const f32 score = decal_retention_score(decals_[index], now_seconds, age_half_life_);
        // Strictly less, so the LOWEST id wins a tie — the tie-break that makes eviction
        // reproducible across two runs of one session.
        if (score < lowest) {
            lowest = score;
            chosen = index;
        }
    }
    return chosen;
}

Status DecalBudget::record(const DecalInstance& decal, f64 now_seconds,
                           DecalEvictionCause cause) noexcept {
    DecalEviction eviction;
    eviction.id = decal.id;
    eviction.cause = cause;
    eviction.score = decal_retention_score(decal, now_seconds, age_half_life_);
    eviction.age_seconds = age_of(decal, now_seconds);
    eviction.importance = decal.importance;
    eviction.screen_coverage = decal.screen_coverage;
    return evictions_.push_back(eviction);
}

Expected<u64, Error> DecalBudget::spawn(const DecalInstance& decal, f64 now_seconds) noexcept {
    if (capacity_ == 0) {
        return fail(ErrorCode::Unavailable,
                    "DecalBudget::spawn: the arbiter has allocated this subsystem no decals");
    }

    DecalInstance placed = decal;
    placed.id = next_id_++;
    placed.spawn_time_seconds = now_seconds;

    if (decals_.size() < capacity_) {
        if (auto stored = decals_.push_back(placed); !stored) {
            return fail(stored.error().code, stored.error().message);
        }
        return placed.id;
    }

    const usize victim = weakest(now_seconds);
    const f32 victim_score = decal_retention_score(decals_[victim], now_seconds, age_half_life_);
    const f32 incoming = decal_retention_score(placed, now_seconds, age_half_life_);
    if (incoming <= victim_score) {
        // The honest answer when a splatter arrives into a wall of persistent damage: the new decal
        // is the least worth keeping and it is the one that goes. Reported like any other eviction,
        // because "nothing appeared" is exactly the symptom somebody will be looking for.
        if (auto reported = record(placed, now_seconds, DecalEvictionCause::BudgetReached);
            !reported) {
            return fail(reported.error().code, reported.error().message);
        }
        return u64{0};
    }

    if (auto reported = record(decals_[victim], now_seconds, DecalEvictionCause::BudgetReached);
        !reported) {
        return fail(reported.error().code, reported.error().message);
    }
    decals_[victim] = placed;
    return placed.id;
}

bool DecalBudget::remove(u64 id) noexcept {
    for (usize index = 0; index < decals_.size(); ++index) {
        if (decals_[index].id != id) {
            continue;
        }
        decals_.erase(index);
        return true;
    }
    return false;
}

void DecalBudget::report_screen_coverage(u64 id, f32 coverage) noexcept {
    for (auto& decal : decals_) {
        if (decal.id == id) {
            decal.screen_coverage = math::clamp(coverage, 0.0F, 1.0F);
            return;
        }
    }
}

u32 DecalBudget::application_order(Span<u32> out) const noexcept {
    const auto count = static_cast<u32>(decals_.size());
    if (out.size() < decals_.size()) {
        return 0;
    }
    for (u32 index = 0; index < count; ++index) {
        out[index] = index;
    }
    // Insertion sort on (sort_order, id). Both are the decal's own, so the order is total and two
    // runs of one frame apply the decals identically — which is what "a later decal can cover an
    // earlier one" needs in order to be a statement about content rather than about luck.
    for (u32 index = 1; index < count; ++index) {
        const u32 candidate = out[index];
        u32 slot = index;
        while (slot > 0) {
            const DecalInstance& previous = decals_[out[slot - 1U]];
            const DecalInstance& current = decals_[candidate];
            const bool after =
                previous.sort_order > current.sort_order ||
                (previous.sort_order == current.sort_order && previous.id > current.id);
            if (!after) {
                break;
            }
            out[slot] = out[slot - 1U];
            --slot;
        }
        out[slot] = candidate;
    }
    return count;
}

}  // namespace cy::rendering
