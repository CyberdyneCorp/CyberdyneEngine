#include <cy/rendering/lighting/shadow_atlas.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {
namespace {

[[nodiscard]] bool is_power_of_two(u32 value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0U;
}

}  // namespace

ShadowAtlas::ShadowAtlas(Allocator& allocator) noexcept : cells_(allocator), entries_(allocator) {}

Status ShadowAtlas::initialize(const ShadowAtlasConfig& config) noexcept {
    if (!is_power_of_two(config.size) || !is_power_of_two(config.min_tile) ||
        !is_power_of_two(config.max_tile)) {
        return fail(ErrorCode::InvalidArgument,
                    "shadow atlas: size, min_tile and max_tile must be powers of two");
    }
    if (config.min_tile > config.max_tile || config.max_tile > config.size) {
        return fail(ErrorCode::InvalidArgument,
                    "shadow atlas: min_tile <= max_tile <= size is required");
    }
    config_ = config;
    grid_ = config.size / config.min_tile;
    if (Status sized = cells_.resize(static_cast<usize>(grid_) * grid_); !sized) {
        return sized;
    }
    for (u8& cell : cells_.span()) {
        cell = 0;
    }
    entries_.clear();
    stats_ = ShadowAtlasStatistics{};
    stats_.cells = grid_ * grid_;
    return ok();
}

void ShadowAtlas::begin_frame(u64 frame_index) noexcept {
    frame_ = frame_index;
    stats_.cache_hits = 0;
    stats_.renders = 0;
    stats_.evictions = 0;
    stats_.shortfall = 0;
    stats_.resizes = 0;
}

u32 ShadowAtlas::tile_size_for(f32 screen_coverage) const noexcept {
    if (config_.max_tile == 0) {
        return 0;
    }
    const f32 reference = math::max(config_.coverage_for_max_tile, 1e-6F);
    f32 size = static_cast<f32>(config_.max_tile);
    f32 coverage = math::max(screen_coverage, 0.0F);
    // Halve the tile for each halving of coverage below the reference. A loop rather than a log2 so
    // the answer is exactly a power of two with no rounding to argue about.
    while (size > static_cast<f32>(config_.min_tile) && coverage < reference) {
        size *= 0.5F;
        coverage *= 2.0F;
    }
    return math::clamp(static_cast<u32>(size), config_.min_tile, config_.max_tile);
}

/// The coverage at which a tile of this size is the right one, before any hysteresis. The inverse
/// of `tile_size_for`: each halving of the tile corresponds to a halving of coverage.
f32 ShadowAtlas::coverage_for_size(u32 size) const noexcept {
    if (config_.max_tile == 0) {
        return 0.0F;
    }
    return config_.coverage_for_max_tile *
           (static_cast<f32>(size) / static_cast<f32>(config_.max_tile));
}

u32 ShadowAtlas::resolve_size(const Entry* existing, f32 coverage) const noexcept {
    const u32 wanted = tile_size_for(coverage);
    if (existing == nullptr || existing->tile.size == 0 || wanted == existing->tile.size) {
        return wanted;
    }

    // THE HYSTERESIS BAND IS IN COVERAGE, NOT IN TILE SIZE, and that distinction is the whole
    // point. Tile sizes are powers of two, so two different sizes always differ by a factor of at
    // least two: a band expressed in them is either zero or wider than any real change, and a light
    // on a boundary resizes — and therefore re-renders — every frame regardless. Expressed in
    // coverage, the band is a genuine dead zone around the size already held.
    const u32 held = existing->tile.size;
    const f32 lower = coverage_for_size(held) * (1.0F - config_.size_hysteresis);
    const f32 upper = held < config_.max_tile
                          ? coverage_for_size(held * 2U) * (1.0F + config_.size_hysteresis)
                          : math::kInfinity;
    return (coverage >= lower && coverage < upper) ? held : wanted;
}

usize ShadowAtlas::find_entry(u64 light_id) const noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].light_id == light_id) {
            return index;
        }
    }
    return entries_.size();
}

void ShadowAtlas::occupy(u32 cell_x, u32 cell_y, u32 cells, bool used) noexcept {
    for (u32 y = 0; y < cells; ++y) {
        for (u32 x = 0; x < cells; ++x) {
            cells_[static_cast<usize>((cell_y + y) * grid_) + (cell_x + x)] = used ? 1U : 0U;
        }
    }
}

bool ShadowAtlas::square_is_free(u32 cell_x, u32 cell_y, u32 cells) const noexcept {
    for (u32 y = 0; y < cells; ++y) {
        for (u32 x = 0; x < cells; ++x) {
            if (cells_[static_cast<usize>((cell_y + y) * grid_) + (cell_x + x)] != 0U) {
                return false;
            }
        }
    }
    return true;
}

bool ShadowAtlas::find_free_square(u32 cells, u32& out_x, u32& out_y) const noexcept {
    if (cells == 0 || cells > grid_) {
        return false;
    }
    // Aligned placement: a tile of 2^k cells starts at a multiple of 2^k. That is what keeps the
    // grid from fragmenting into unusable diagonal gaps, and it is the one rule a quadtree would
    // also enforce.
    for (u32 y = 0; y + cells <= grid_; y += cells) {
        for (u32 x = 0; x + cells <= grid_; x += cells) {
            if (square_is_free(x, y, cells)) {
                out_x = x;
                out_y = y;
                return true;
            }
        }
    }
    return false;
}

bool ShadowAtlas::evict_for(u32 cells) noexcept {
    usize victim = entries_.size();
    u64 oldest = ~0ULL;
    for (usize index = 0; index < entries_.size(); ++index) {
        const Entry& entry = entries_[index];
        if (entry.cells < cells) {
            continue;
        }
        // The retention period: a tile used within it is protected, however old it is relative to
        // the others. Without this, two lights competing for one tile evict each other every frame.
        if (frame_ < entry.last_used_frame + config_.retention_frames) {
            continue;
        }
        if (entry.last_used_frame < oldest) {
            oldest = entry.last_used_frame;
            victim = index;
        }
    }
    if (victim == entries_.size()) {
        return false;
    }
    occupy(entries_[victim].cell_x, entries_[victim].cell_y, entries_[victim].cells, false);
    entries_.remove_unordered(victim);
    ++stats_.evictions;
    return true;
}

Expected<ShadowAssignment, Error> ShadowAtlas::request(const ShadowRequest& request) noexcept {
    if (grid_ == 0) {
        return fail(ErrorCode::Unavailable, "shadow atlas: initialize() was not called");
    }

    const usize existing_index = find_entry(request.light_id);
    Entry* existing = existing_index < entries_.size() ? &entries_[existing_index] : nullptr;
    const u32 size = resolve_size(existing, request.screen_coverage);
    const u32 cells = math::max(size / config_.min_tile, 1U);

    // The cache hit: same light, same size, and neither the light nor its casters have changed.
    if (existing != nullptr && existing->tile.size == size && existing->rendered &&
        existing->light_version == request.light_version &&
        existing->caster_version == request.caster_version) {
        existing->last_used_frame = frame_;
        ++stats_.cache_hits;
        ShadowAssignment assignment;
        assignment.tile = existing->tile;
        assignment.needs_render = false;
        assignment.slot = (existing->cell_y * grid_) + existing->cell_x;
        return assignment;
    }

    // A resize is a re-render, so the old placement goes back first — otherwise the atlas holds a
    // tile of the wrong size for a light that is about to get another one.
    if (existing != nullptr) {
        if (existing->tile.size != size) {
            ++stats_.resizes;
        }
        occupy(existing->cell_x, existing->cell_y, existing->cells, false);
        entries_.remove_unordered(existing_index);
        existing = nullptr;
    }

    u32 cell_x = 0;
    u32 cell_y = 0;
    if (!find_free_square(cells, cell_x, cell_y)) {
        // Requests arrive in descending importance, so a request that reaches here is less
        // important than everything already placed THIS frame — which is why the victim is chosen
        // by age past the retention period rather than by comparing importances. The ordering the
        // caller guarantees is what makes that sound; `request.importance` is carried for the
        // report rather than for this decision.
        if (!evict_for(cells) || !find_free_square(cells, cell_x, cell_y)) {
            ++stats_.shortfall;
            return fail(ErrorCode::Unavailable,
                        "shadow atlas: no tile available; this light renders unshadowed");
        }
    }

    Entry entry;
    entry.light_id = request.light_id;
    entry.cell_x = cell_x;
    entry.cell_y = cell_y;
    entry.cells = cells;
    entry.tile = ShadowTile{cell_x * config_.min_tile, cell_y * config_.min_tile, size};
    entry.last_used_frame = frame_;
    entry.light_version = request.light_version;
    entry.caster_version = request.caster_version;
    entry.rendered = true;
    occupy(cell_x, cell_y, cells, true);
    if (Status pushed = entries_.push_back(entry); !pushed) {
        occupy(cell_x, cell_y, cells, false);
        return make_unexpected(pushed.error());
    }

    ++stats_.renders;
    ShadowAssignment assignment;
    assignment.tile = entry.tile;
    assignment.needs_render = true;
    // THE SLOT IS THE TILE'S CELL, NOT THE ENTRY'S INDEX. `remove_unordered` moves entries around,
    // so an index would name a different light after the next eviction — and
    // `GpuLight::shadow_slot` is read by a shader a frame later. The cell is stable for as long as
    // the tile is held.
    assignment.slot = (cell_y * grid_) + cell_x;
    return assignment;
}

void ShadowAtlas::release(u64 light_id) noexcept {
    const usize index = find_entry(light_id);
    if (index >= entries_.size()) {
        return;
    }
    occupy(entries_[index].cell_x, entries_[index].cell_y, entries_[index].cells, false);
    entries_.remove_unordered(index);
}

ShadowTile ShadowAtlas::tile_of(u64 light_id) const noexcept {
    const usize index = find_entry(light_id);
    return index < entries_.size() ? entries_[index].tile : ShadowTile{};
}

ShadowAtlasStatistics ShadowAtlas::statistics() const noexcept {
    ShadowAtlasStatistics stats = stats_;
    stats.tiles_live = static_cast<u32>(entries_.size());
    u32 used = 0;
    for (const u8 cell : cells_.span()) {
        used += cell != 0U ? 1U : 0U;
    }
    stats.cells_used = used;
    return stats;
}

void ShadowAtlas::reset() noexcept {
    for (u8& cell : cells_.span()) {
        cell = 0;
    }
    entries_.clear();
    stats_ = ShadowAtlasStatistics{};
    stats_.cells = grid_ * grid_;
}

}  // namespace cy::rendering
