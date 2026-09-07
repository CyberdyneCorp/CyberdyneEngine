#include <cy/rendering/gi/surface_cache.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace cy::rendering::gi {
namespace {

[[nodiscard]] Aabb page_bounds(const SurfacePage& page) noexcept {
    const f32 radius = std::sqrt(std::max(page.area, 1.0e-4F)) * 0.5F;
    const Vec3 extent{radius, radius, radius};
    return Aabb{page.position - extent, page.position + extent};
}

/// How well a card's normal must agree with a hit's before the card may answer for it. A quarter is
/// a 75-degree cone: wide enough that a hit whose normal the field blended at an edge still finds
/// its own surface, and narrow enough that a card facing across the hit cannot.
constexpr f32 kMinimumAlignment = 0.25F;

[[nodiscard]] f32 relative_change(Vec3 before, Vec3 after) noexcept {
    const f32 magnitude = length(before) + length(after);
    if (magnitude <= 1.0e-5F) {
        return 0.0F;
    }
    return length(after - before) / magnitude;
}

}  // namespace

Vec3 direct_radiance(const GiLight& light, Vec3 position, Vec3 normal) noexcept {
    if (light.directional) {
        const Vec3 to_light = -normalized_or(light.direction, Vec3{0.0F, 1.0F, 0.0F});
        const f32 cosine = std::max(0.0F, dot(normal, to_light));
        return light.colour * (light.intensity * cosine);
    }
    const Vec3 offset = light.position - position;
    const f32 distance_squared_metres = std::max(length_squared(offset), 1.0e-6F);
    if (light.range > 0.0F && distance_squared_metres > light.range * light.range) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const Vec3 to_light = offset / std::sqrt(distance_squared_metres);
    const f32 cosine = std::max(0.0F, dot(normal, to_light));
    return light.colour * (light.intensity * cosine / distance_squared_metres);
}

Vec3 shaded_direct(Span<const GiLight> lights, Vec3 position, Vec3 normal,
                   const Occluder* occluder) noexcept {
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (const GiLight& light : lights) {
        const Vec3 contribution = direct_radiance(light, position, normal);
        if (contribution.x <= 0.0F && contribution.y <= 0.0F && contribution.z <= 0.0F) {
            continue;
        }
        if (occluder != nullptr) {
            // A directional light has no position; the shadow ray goes a long way along its
            // reverse direction instead, which is the same test with a different endpoint.
            const Vec3 target =
                light.directional
                    ? position - (normalized_or(light.direction, Vec3{0.0F, 1.0F, 0.0F}) * 1000.0F)
                    : light.position;
            if (occluder->occluded(position + (normal * 1.0e-2F), target)) {
                continue;
            }
        }
        total = total + contribution;
    }
    return total;
}

SurfaceCache::SurfaceCache() noexcept = default;
SurfaceCache::~SurfaceCache() = default;

Expected<u32, Error> SurfaceCache::allocate(const Surfel& surfel) noexcept {
    u32 handle = ~0U;
    if (!free_pages_.empty()) {
        handle = free_pages_.back();
        free_pages_.pop_back();
    } else {
        SurfacePage fresh;
        if (Status pushed = pages_.push_back(fresh); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = proxies_.push_back(0); !pushed) {
            return make_unexpected(pushed.error());
        }
        handle = static_cast<u32>(pages_.size() - 1);
    }

    SurfacePage& page = pages_[handle];
    page = SurfacePage{};
    page.position = surfel.position;
    page.normal = surfel.normal;
    page.albedo = surfel.albedo;
    page.emission = surfel.emission;
    page.roughness = surfel.roughness;
    page.area = surfel.area;
    page.instance_id = surfel.instance_id;
    page.material_id = surfel.material_id;
    page.live = true;
    page.valid = false;

    Expected<u32, Error> proxy = index_.insert(page_bounds(page), handle);
    if (!proxy) {
        page.live = false;
        (void)free_pages_.push_back(handle);
        return make_unexpected(proxy.error());
    }
    proxies_[handle] = proxy.value();
    live_pages_ += 1;
    account();
    return handle;
}

Status SurfaceCache::allocate_from(const GiScene& scene, const Aabb& region) noexcept {
    Status result = ok();
    scene.query_box(region, [&](const Surfel& surfel, u32 /*index*/) {
        if (!result) {
            return;
        }
        Expected<u32, Error> allocated = allocate(surfel);
        if (!allocated) {
            result = make_unexpected(allocated.error());
        }
    });
    return result;
}

void SurfaceCache::release(u32 handle) noexcept {
    if (handle >= pages_.size() || !pages_[handle].live) {
        return;
    }
    (void)index_.remove(proxies_[handle]);
    pages_[handle].live = false;
    pages_[handle].valid = false;
    (void)free_pages_.push_back(handle);
    live_pages_ -= 1;
    account();
}

u32 SurfaceCache::invalidate(const Aabb& region) noexcept {
    u32 count = 0;
    index_.query_aabb(region, [&](u32 /*proxy*/, u64 user_data) {
        const u32 handle = static_cast<u32>(user_data);
        if (handle < pages_.size() && pages_[handle].live &&
            region.contains(pages_[handle].position)) {
            pages_[handle].valid = false;
            pages_[handle].error = 1.0F;
            count += 1;
        }
        return true;
    });
    account();
    return count;
}

u32 SurfaceCache::mark_visible(const Aabb& region, bool visible) noexcept {
    u32 count = 0;
    index_.query_aabb(region, [&](u32 /*proxy*/, u64 user_data) {
        const u32 handle = static_cast<u32>(user_data);
        if (handle < pages_.size() && pages_[handle].live &&
            region.contains(pages_[handle].position)) {
            pages_[handle].visible = visible;
            count += 1;
        }
        return true;
    });
    return count;
}

Vec3 SurfaceCache::outgoing(const SurfacePage& page) noexcept {
    // `direct` is the IRRADIANCE arriving at the card and `accumulated` the indirect already
    // multiplied by the albedo, so the albedo appears once here and once there. Leaving it off the
    // direct term is what makes a red wall light its neighbours white, which is the one thing the
    // colour-bleeding scenario is about.
    //
    // Emission is illumination: an emissive surface lights its surroundings through this term and
    // through no explicitly placed light, which is what `rendering-global-illumination`'s
    // "Emissive surfaces as illumination" requirement asks the cache to carry.
    const Vec3 reflected{page.albedo.x * page.direct.x, page.albedo.y * page.direct.y,
                         page.albedo.z * page.direct.z};
    return page.emission + reflected + page.accumulated;
}

f32 SurfaceCache::priority_of(const SurfacePage& page, u64 frame) noexcept {
    const u64 age = frame > page.last_update_frame ? frame - page.last_update_frame : 0;
    // Visible, then recently invalidated, then high error, then old. The weights are the
    // specification's own order of mention and the magnitudes keep them separable: a visible page
    // outranks any invisible one, an invalid page outranks any valid one below that, and error and
    // age break the remaining ties.
    f32 priority = 0.0F;
    if (page.visible) {
        priority += 1000.0F;
    }
    if (!page.valid) {
        priority += 100.0F;
    }
    priority += page.error * 10.0F;
    priority += std::min(static_cast<f32>(age), 1000.0F) * 0.01F;
    return priority;
}

void SurfaceCache::shade(
    SurfacePage& page,
    const SurfaceUpdateContext&
        context) noexcept {  // NOLINT(readability-convert-member-functions-to-static)
    const Vec3 before = outgoing(page);
    page.direct = shaded_direct(context.lights, page.position, page.normal, context.occluder);
    if (context.indirect != nullptr) {
        // The one line multi-bounce lives in. `gather` reads the radiance cache, which was built
        // from these pages last frame, so each update adds a bounce.
        const Vec3 incoming = context.indirect->gather(page.position, page.normal);
        page.accumulated = Vec3{page.albedo.x * incoming.x, page.albedo.y * incoming.y,
                                page.albedo.z * incoming.z};
    }
    page.last_update_frame = context.frame;
    page.valid = true;
    page.error = relative_change(before, outgoing(page));
}

SurfaceUpdateReport SurfaceCache::service(const SurfaceUpdateContext& context, bool all) noexcept {
    SurfaceUpdateReport report;
    current_frame_ = context.frame;

    Array<u32> queue;
    for (u32 handle = 0; handle < pages_.size(); ++handle) {
        if (pages_[handle].live) {
            (void)queue.push_back(handle);
            if (!pages_[handle].valid) {
                report.pages_invalid += 1;
            }
        }
    }
    report.queue_depth = static_cast<u32>(queue.size());
    if (queue.empty()) {
        diagnostics_.last_update = report;
        return report;
    }

    const u64 frame = context.frame;
    std::ranges::sort(queue, [&](u32 a, u32 b) {
        // The progress guarantee first: a page older than the cap outranks everything, so no valid
        // page is starved indefinitely by higher-priority work.
        const u64 age_a =
            frame > pages_[a].last_update_frame ? frame - pages_[a].last_update_frame : 0;
        const u64 age_b =
            frame > pages_[b].last_update_frame ? frame - pages_[b].last_update_frame : 0;
        const bool starved_a = age_a >= context.max_age_frames;
        const bool starved_b = age_b >= context.max_age_frames;
        if (starved_a != starved_b) {
            return starved_a;
        }
        const f32 priority_a = priority_of(pages_[a], frame);
        const f32 priority_b = priority_of(pages_[b], frame);
        if (priority_a != priority_b) {
            return priority_a > priority_b;
        }
        return a < b;
    });

    const usize limit = all ? queue.size() : std::min<usize>(queue.size(), context.budget);
    f32 error_total = 0.0F;
    for (usize index = 0; index < limit; ++index) {
        shade(pages_[queue[index]], context);
        error_total += pages_[queue[index]].error;
        report.pages_updated += 1;
    }
    for (usize index = limit; index < queue.size(); ++index) {
        const SurfacePage& page = pages_[queue[index]];
        const u64 age = frame > page.last_update_frame ? frame - page.last_update_frame : 0;
        report.oldest_unserviced_age = std::max(report.oldest_unserviced_age, age);
    }
    report.mean_error =
        report.pages_updated == 0 ? 0.0F : error_total / static_cast<f32>(report.pages_updated);

    diagnostics_.last_update = report;
    account();
    return report;
}

SurfaceUpdateReport SurfaceCache::update(const SurfaceUpdateContext& context) noexcept {
    return service(context, false);
}

SurfaceUpdateReport SurfaceCache::update_all(const SurfaceUpdateContext& context) noexcept {
    return service(context, true);
}

bool SurfaceCache::radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                               u32& age_frames) const noexcept {
    diagnostics_.lookups += 1;
    const Vec3 extent{lookup_radius_, lookup_radius_, lookup_radius_};
    const Aabb box{position - extent, position + extent};
    u32 best = ~0U;
    f32 best_score = 1.0e9F;
    const f32 radius_squared = lookup_radius_ * lookup_radius_;
    index_.query_aabb(box, [&](u32 /*proxy*/, u64 user_data) {
        const u32 handle = static_cast<u32>(user_data);
        if (handle >= pages_.size() || !pages_[handle].live || !pages_[handle].valid) {
            return true;
        }
        const SurfacePage& candidate = pages_[handle];
        // Alignment first, distance second — and the order matters at an EDGE, where two cards
        // facing different ways sit at the same point. Picking the nearest and breaking the tie
        // arbitrarily is what the first form did, and it made a hit on a wall next to a ceiling
        // read the ceiling's radiance about a third of the time. Which of two coincident cards a
        // spatial index happens to visit first is not a shading decision.
        const f32 alignment = dot(candidate.normal, normal);
        if (alignment <= kMinimumAlignment) {
            return true;
        }
        const f32 squared = distance_squared(candidate.position, position);
        if (squared > radius_squared) {
            return true;
        }
        const f32 score = squared / alignment;
        if (score < best_score) {
            best_score = score;
            best = handle;
        }
        return true;
    });

    if (best == ~0U) {
        diagnostics_.lookup_misses += 1;
        radiance = Vec3{0.0F, 0.0F, 0.0F};
        age_frames = 0;
        return false;
    }
    const SurfacePage& page = pages_[best];
    radiance = outgoing(page);
    // Staleness is what the tracer turns into a confidence: a page shaded twenty frames ago is a
    // less trustworthy answer than one shaded this frame, and "stale cache is distrusted" is that
    // sentence. `current_frame_` is the last frame an update ran, which is the only clock this
    // subsystem has and is exactly the right one — a lookup between two updates is not staler than
    // the update that preceded it.
    const u64 last = page.last_update_frame;
    age_frames = static_cast<u32>(
        std::min<u64>(current_frame_ > last ? current_frame_ - last : 0, 0xFFFFFFFFULL));
    return true;
}

void SurfaceCache::account() noexcept {
    u32 valid = 0;
    for (const SurfacePage& page : pages_) {
        if (page.live && page.valid) {
            valid += 1;
        }
    }
    diagnostics_.page_count = live_pages_;
    diagnostics_.valid_pages = valid;
    diagnostics_.bytes = static_cast<u64>(pages_.size()) * sizeof(SurfacePage);
}

}  // namespace cy::rendering::gi
