// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/card_lighting.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <ranges>

namespace cy::rendering::gi {
namespace {

/// The basis a shadow map's texels are laid out in. The same construction `hemisphere_direction`
/// uses for its tangent, so the one degenerate case — a light straight along y — picks x.
void light_basis(Vec3 direction, Vec3& right, Vec3& up) noexcept {
    const Vec3 reference =
        std::abs(direction.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    right = normalized_or(cross(direction, reference), Vec3{1.0F, 0.0F, 0.0F});
    up = cross(right, direction);
}

[[nodiscard]] u32 bucket_count_for(u32 cards) noexcept {
    u32 count = 64;
    while (count < cards * 2U) {
        count <<= 1U;
    }
    return count;
}

}  // namespace

// --- ShadowMap -----------------------------------------------------------------------------------

Status ShadowMap::configure(const ShadowMapSettings& settings) noexcept {
    if (settings.resolution == 0 || settings.half_extent_metres <= 0.0F ||
        settings.depth_range_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "ShadowMap::configure: the resolution, half extent and depth range must be "
                    "positive");
    }
    settings_ = settings;
    direction_ = normalized_or(settings.direction, Vec3{0.0F, -1.0F, 0.0F});
    light_basis(direction_, right_, up_);
    if (Status sized =
            depths_.resize(static_cast<usize>(settings.resolution) * settings.resolution);
        !sized) {
        return sized;
    }
    for (f32& depth : depths_) {
        depth = settings.depth_range_metres;
    }
    return ok();
}

void ShadowMap::capture(const DistanceField& field) noexcept {
    const u32 resolution = settings_.resolution;
    const f32 extent = settings_.half_extent_metres;
    const Vec3 near_plane = settings_.centre - (direction_ * (settings_.depth_range_metres * 0.5F));
    for (u32 row = 0; row < resolution; ++row) {
        for (u32 column = 0; column < resolution; ++column) {
            const f32 u =
                ((((static_cast<f32>(column) + 0.5F) / static_cast<f32>(resolution)) * 2.0F) -
                 1.0F) *
                extent;
            const f32 v =
                ((((static_cast<f32>(row) + 0.5F) / static_cast<f32>(resolution)) * 2.0F) - 1.0F) *
                extent;
            Ray ray;
            ray.origin = near_plane + (right_ * u) + (up_ * v);
            ray.direction = direction_;
            const SphereTraceHit hit = field.sphere_trace(ray, settings_.depth_range_metres);
            depths_[(static_cast<usize>(row) * resolution) + column] =
                hit.hit ? hit.t : settings_.depth_range_metres;
        }
    }
}

Status ShadowMap::assign(Span<const f32> depths) noexcept {
    if (depths.size() != depths_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "ShadowMap::assign: the depth count is not resolution^2");
    }
    for (usize index = 0; index < depths.size(); ++index) {
        depths_[index] = depths[index];
    }
    return ok();
}

bool ShadowMap::covers(Vec3 toward_light) const noexcept {
    return !depths_.empty() && dot(toward_light, -direction_) > kShadowMapAlignment;
}

bool ShadowMap::shadowed(Vec3 point) const noexcept {
    if (depths_.empty()) {
        return false;
    }
    const Vec3 relative = point - settings_.centre;
    const f32 extent = settings_.half_extent_metres;
    const f32 u = dot(relative, right_);
    const f32 v = dot(relative, up_);
    const f32 depth = dot(relative, direction_) + (settings_.depth_range_metres * 0.5F);
    if (std::abs(u) >= extent || std::abs(v) >= extent || depth < 0.0F ||
        depth > settings_.depth_range_metres) {
        return false;
    }
    const auto texel = [this, extent](f32 coordinate) {
        const auto index = static_cast<i32>(std::floor((((coordinate / extent) * 0.5F) + 0.5F) *
                                                       static_cast<f32>(settings_.resolution)));
        return static_cast<u32>(std::clamp(index, 0, static_cast<i32>(settings_.resolution) - 1));
    };
    const f32 stored = depths_[(static_cast<usize>(texel(v)) * settings_.resolution) + texel(u)];
    return stored < depth - settings_.bias_metres;
}

bool ShadowMapOccluder::occluded(Vec3 from, Vec3 to) const noexcept {
    const Vec3 offset = to - from;
    const f32 span = length(offset);
    if (span <= 1.0e-4F) {
        return false;
    }
    if (map_ != nullptr && map_->covers(offset / span)) {
        return map_->shadowed(from);
    }
    return fallback_ != nullptr && fallback_->occluded(from, to);
}

// --- CardGrid ------------------------------------------------------------------------------------

Status CardGrid::build(Span<const SurfacePage> pages, f32 cell_metres) noexcept {
    cell_ = std::max(cell_metres, 1.0e-3F);
    u32 live = 0;
    for (const SurfacePage& page : pages) {
        live += page.live ? 1U : 0U;
    }
    const u32 buckets = bucket_count_for(live);
    if (Status sized = ranges_.resize(static_cast<usize>(buckets) * 2U); !sized) {
        return sized;
    }
    for (u32& word : ranges_) {
        word = 0;
    }
    const auto bucket_of_page = [this, buckets](const SurfacePage& page) {
        return bucket_of(cell_of(page.position.x, cell_), cell_of(page.position.y, cell_),
                         cell_of(page.position.z, cell_), buckets);
    };
    // A counting sort: sizes, then offsets, then a second pass that places each handle. Handles
    // land in a bucket in ascending order, which is what makes the grid's bytes a function of the
    // pages alone.
    for (const SurfacePage& page : pages) {
        if (page.live) {
            ranges_[(bucket_of_page(page) * 2U) + 1U] += 1U;
        }
    }
    u32 running = 0;
    for (usize bucket = 0; bucket < buckets; ++bucket) {
        ranges_[bucket * 2U] = running;
        running += ranges_[(bucket * 2U) + 1U];
    }
    if (Status sized = items_.resize(running); !sized) {
        return sized;
    }
    Array<u32> cursor;
    if (Status sized = cursor.resize(buckets); !sized) {
        return sized;
    }
    for (usize bucket = 0; bucket < buckets; ++bucket) {
        cursor[bucket] = ranges_[bucket * 2U];
    }
    for (u32 handle = 0; handle < pages.size(); ++handle) {
        if (pages[handle].live) {
            const u32 bucket = bucket_of_page(pages[handle]);
            items_[cursor[bucket]] = handle;
            cursor[bucket] += 1U;
        }
    }
    return ok();
}

// --- CardNeighbourGrid ---------------------------------------------------------------------------

namespace {

/// One (cell, card) pair of a neighbour grid, before it is grouped by bucket.
struct NeighbourEntry {
    i32 cell[3] = {0, 0, 0};
    u32 handle = 0;
    u32 bucket = 0;
};

/// The distance from `value` to the cell `[index, index + 1) * cell` along one axis.
[[nodiscard]] f32 cell_gap(f32 value, i32 index, f32 cell) noexcept {
    return std::max({(static_cast<f32>(index) * cell) - value,
                     value - (static_cast<f32>(index + 1) * cell), 0.0F});
}

/// An entry for every cell of `cell` metres the radius `cell` about `p` reaches into — with a
/// slack, so a point the exact test admits is never in a cell whose list a rounding left the card
/// out of.
[[nodiscard]] Status list_card_cells(Vec3 p, u32 handle, f32 cell,
                                     Array<NeighbourEntry>& entries) noexcept {
    const f32 reach = (cell * cell * 1.001F) + 1.0e-6F;
    const i32 lo[3] = {CardGrid::cell_of(p.x - cell, cell), CardGrid::cell_of(p.y - cell, cell),
                       CardGrid::cell_of(p.z - cell, cell)};
    const i32 hi[3] = {CardGrid::cell_of(p.x + cell, cell), CardGrid::cell_of(p.y + cell, cell),
                       CardGrid::cell_of(p.z + cell, cell)};
    for (i32 z = lo[2]; z <= hi[2]; ++z) {
        for (i32 y = lo[1]; y <= hi[1]; ++y) {
            for (i32 x = lo[0]; x <= hi[0]; ++x) {
                const f32 dx = cell_gap(p.x, x, cell);
                const f32 dy = cell_gap(p.y, y, cell);
                const f32 dz = cell_gap(p.z, z, cell);
                if ((dx * dx) + (dy * dy) + (dz * dz) > reach) {
                    continue;
                }
                if (Status pushed = entries.push_back(NeighbourEntry{{x, y, z}, handle, 0});
                    !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

/// How many distinct cells `entries`, sorted by cell, name.
[[nodiscard]] u32 distinct_cells(Span<const NeighbourEntry> entries) noexcept {
    u32 cells = 0;
    for (usize index = 0; index < entries.size(); ++index) {
        const bool fresh = index == 0 || entries[index].cell[0] != entries[index - 1].cell[0] ||
                           entries[index].cell[1] != entries[index - 1].cell[1] ||
                           entries[index].cell[2] != entries[index - 1].cell[2];
        cells += fresh ? 1U : 0U;
    }
    return cells;
}

}  // namespace

Status CardNeighbourGrid::build(Span<const SurfacePage> pages, f32 radius,
                                u32 max_buckets) noexcept {
    cell_ = std::max(radius, 1.0e-3F);
    Array<NeighbourEntry> entries;
    for (u32 handle = 0; handle < pages.size(); ++handle) {
        if (!pages[handle].live) {
            continue;
        }
        if (Status listed = list_card_cells(pages[handle].position, handle, cell_, entries);
            !listed) {
            return listed;
        }
    }
    std::ranges::sort(entries, [](const NeighbourEntry& a, const NeighbourEntry& b) noexcept {
        if (a.cell[2] != b.cell[2]) {
            return a.cell[2] < b.cell[2];
        }
        if (a.cell[1] != b.cell[1]) {
            return a.cell[1] < b.cell[1];
        }
        if (a.cell[0] != b.cell[0]) {
            return a.cell[0] < b.cell[0];
        }
        return a.handle < b.handle;
    });
    // Twice the occupied cells, as `CardGrid` sizes for its cards, within what the caller holds.
    const u32 cells = distinct_cells(entries.span());
    u32 buckets = 64;
    while (buckets < cells * 2U && buckets < max_buckets) {
        buckets <<= 1U;
    }
    if (Status sized = ranges_.resize(static_cast<usize>(buckets) * 2U); !sized) {
        return sized;
    }
    for (u32& word : ranges_) {
        word = 0;
    }
    // A counting sort by bucket, as `CardGrid::build`'s: sizes, offsets, then each entry placed.
    for (NeighbourEntry& entry : entries) {
        entry.bucket = CardGrid::bucket_of(entry.cell[0], entry.cell[1], entry.cell[2], buckets);
        ranges_[(static_cast<usize>(entry.bucket) * 2U) + 1U] += 1U;
    }
    u32 running = 0;
    for (usize bucket = 0; bucket < buckets; ++bucket) {
        ranges_[bucket * 2U] = running;
        running += ranges_[(bucket * 2U) + 1U];
    }
    Array<u32> cursor;
    if (Status sized = handles_.resize(running); !sized) {
        return sized;
    }
    if (Status sized = positions_.resize(running); !sized) {
        return sized;
    }
    if (Status sized = cursor.resize(buckets); !sized) {
        return sized;
    }
    for (usize bucket = 0; bucket < buckets; ++bucket) {
        cursor[bucket] = ranges_[bucket * 2U];
    }
    for (const NeighbourEntry& entry : entries) {
        const u32 slot = cursor[entry.bucket];
        handles_[slot] = entry.handle;
        positions_[slot] = pages[entry.handle].position;
        cursor[entry.bucket] = slot + 1U;
    }
    return ok();
}

// --- CardSnapshot --------------------------------------------------------------------------------

Status CardSnapshot::capture(const SurfaceCache& cache, u64 frame) noexcept {
    const Span<const SurfacePage> pages = cache.pages();
    if (Status sized = samples_.resize(pages.size()); !sized) {
        return sized;
    }
    for (usize handle = 0; handle < pages.size(); ++handle) {
        const SurfacePage& page = pages[handle];
        CardSample& sample = samples_[handle];
        sample.position = page.position;
        sample.normal = page.normal;
        sample.outgoing = SurfaceCache::outgoing(page);
        sample.last_update_frame = page.last_update_frame;
        sample.valid = page.live && page.valid;
    }
    radius_ = cache.lookup_radius();
    frame_ = frame;
    return grid_.build(pages, radius_);
}

u32 CardSnapshot::find(Vec3 position, Vec3 normal) const noexcept {
    u32 best = ~0U;
    f32 best_score = 1.0e9F;
    f32 best_alignment = 0.0F;
    const f32 radius_squared = radius_ * radius_;
    grid_.visit(position, radius_, [&](u32 handle) {
        const CardSample& candidate = samples_[handle];
        if (!candidate.valid) {
            return;
        }
        const f32 alignment = dot(candidate.normal, normal);
        if (alignment <= kCardLookupAlignment) {
            return;
        }
        const f32 squared = distance_squared(candidate.position, position);
        if (squared > radius_squared) {
            return;
        }
        const f32 score = squared / alignment;
        if (card_lookup_prefers(score, alignment, handle, best_score, best_alignment, best)) {
            best_score = score;
            best_alignment = alignment;
            best = handle;
        }
    });
    return best;
}

bool CardSnapshot::radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                               u32& age_frames) const noexcept {
    const u32 best = find(position, normal);
    if (best == ~0U) {
        radiance = Vec3{0.0F, 0.0F, 0.0F};
        age_frames = 0;
        return false;
    }
    radiance = samples_[best].outgoing;
    const u64 last = samples_[best].last_update_frame;
    age_frames = static_cast<u32>(std::min<u64>(frame_ > last ? frame_ - last : 0, 0xFFFFFFFFULL));
    return true;
}

// --- CardGather ----------------------------------------------------------------------------------

Vec3 CardGather::gather(Vec3 position, Vec3 normal) const noexcept {
    if (field_ == nullptr || settings_.rays == 0) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    // Clear the surface the ray leaves, as `SoftwareTracer::occluded` does and for its reason —
    // and lift the origin one voxel off it first. A card sits ON its surface, so a ray leaving it
    // at a grazing angle is, at `t_min`, at a height of `t_min * cos` above it; for the sequence's
    // last direction with 8 rays that is exactly the hit threshold of half a voxel, and whether the
    // ray "hits" the card's own surface came down to the last bit of a sine. Lifted, the grazing
    // ray starts a voxel clear and the surface it leaves can no longer answer it.
    const f32 voxel = field_->voxel_size();
    const f32 bias = voxel * 2.0F;
    const Vec3 origin = position + (normal * voxel);
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < settings_.rays; ++index) {
        Ray ray;
        ray.origin = origin;
        ray.direction = hemisphere_direction(normal, index, settings_.rays);
        const SphereTraceHit hit = field_->sphere_trace(ray, settings_.max_distance_metres, bias);
        if (!hit.hit) {
            total = total + settings_.sky.radiance(ray.direction);
            continue;
        }
        Vec3 cached{0.0F, 0.0F, 0.0F};
        u32 age = 0;
        if (cards_ != nullptr && cards_->radiance_at(hit.position, hit.normal, cached, age)) {
            total = total + cached;
        }
    }
    return total / static_cast<f32>(settings_.rays);
}

}  // namespace cy::rendering::gi
