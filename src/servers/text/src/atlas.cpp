#include <cy/servers/text/atlas.h>

#include <cstring>

namespace cy::text {
namespace {

[[nodiscard]] bool is_power_of_two(u32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] i32 min_of(i32 a, i32 b) noexcept {
    return a < b ? a : b;
}

[[nodiscard]] i32 max_of(i32 a, i32 b) noexcept {
    return a > b ? a : b;
}

}  // namespace

Status GlyphAtlas::start(const GlyphAtlasConfig& config) noexcept {
    if (!is_power_of_two(config.initial_extent) || !is_power_of_two(config.maximum_extent)) {
        // A power of two so that growth is a doubling and the repack is a straight halving of the
        // occupancy. A non-power-of-two atlas grows by an awkward factor and packs no better.
        return fail(ErrorCode::InvalidArgument, "an atlas extent that is not a power of two");
    }
    if (config.initial_extent > config.maximum_extent) {
        return fail(ErrorCode::InvalidArgument, "an atlas that starts larger than its own maximum");
    }
    config_ = config;
    entries_.clear();
    recently_evicted_.clear();
    clock_ = 0;
    diagnostics_ = {};
    dirty_ = {};
    return repack(config.initial_extent);
}

void GlyphAtlas::stop() noexcept {
    entries_.clear();
    recently_evicted_.clear();
    pixels_.clear();
    packer_.reset(IVec2{0, 0});
    extent_ = 0;
    dirty_ = {};
}

usize GlyphAtlas::find_index(const GlyphKey& key) const noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].key == key) {
            return index;
        }
    }
    return entries_.size();
}

const GlyphSlot* GlyphAtlas::find(const GlyphKey& key) noexcept {
    const usize index = find_index(key);
    if (index == entries_.size()) {
        return nullptr;
    }
    // Touching here rather than in the caller is what makes "least recently USED" true: a glyph a
    // frame looked up is a glyph that frame is about to draw.
    entries_[index].used_at = ++clock_;
    return &entries_[index].slot;
}

void GlyphAtlas::mark_dirty(const IRect& rect) noexcept {
    if (rect.is_empty()) {
        return;
    }
    if (dirty_.is_empty()) {
        dirty_ = rect;
        return;
    }
    const IVec2 low{min_of(dirty_.position.x, rect.position.x),
                    min_of(dirty_.position.y, rect.position.y)};
    const IVec2 high{max_of(dirty_.max().x, rect.max().x), max_of(dirty_.max().y, rect.max().y)};
    dirty_ = IRect{low, IVec2{high.x - low.x, high.y - low.y}};
}

void GlyphAtlas::clear_dirty() noexcept {
    dirty_ = IRect{};
}

Status GlyphAtlas::repack(u32 extent) noexcept {
    Array<u8> pixels;
    if (Status resized = pixels.resize(static_cast<usize>(extent) * extent); !resized) {
        return resized;
    }
    std::memset(pixels.data(), 0, pixels.size());

    geom::AtlasPacker packer(IVec2{static_cast<i32>(extent), static_cast<i32>(extent)});

    // Largest first. The packer's own documentation says insertion order matters and that packing
    // large rectangles first is markedly better; a repack is the one moment the order is ours to
    // choose, so it is chosen.
    Array<u32> order;
    if (Status reserved = order.reserve(entries_.size()); !reserved) {
        return reserved;
    }
    for (usize index = 0; index < entries_.size(); ++index) {
        if (Status pushed = order.push_back(static_cast<u32>(index)); !pushed) {
            return pushed;
        }
    }
    for (usize outer = 1; outer < order.size(); ++outer) {
        const u32 held = order[outer];
        const i64 held_area = entries_[held].slot.rect.area();
        usize inner = outer;
        while (inner > 0 && entries_[order[inner - 1]].slot.rect.area() < held_area) {
            order[inner] = order[inner - 1];
            --inner;
        }
        order[inner] = held;
    }

    const i32 padding = static_cast<i32>(config_.padding);
    for (const u32 index : order) {
        Entry& entry = entries_[index];
        const IVec2 wanted{entry.slot.rect.size.x + padding, entry.slot.rect.size.y + padding};
        Expected<IRect, Error> placed = packer.pack(wanted);
        if (!placed) {
            // The caller decides what to do — grow again, or evict — because only the caller knows
            // whether there is room to grow.
            return make_unexpected(placed.error());
        }
        const IRect destination{placed.value().position,
                                IVec2{entry.slot.rect.size.x, entry.slot.rect.size.y}};
        for (i32 row = 0; row < destination.size.y; ++row) {
            const u8* source = pixels_.data() +
                               (static_cast<usize>(entry.slot.rect.position.y + row) * extent_) +
                               static_cast<usize>(entry.slot.rect.position.x);
            u8* target = pixels.data() +
                         (static_cast<usize>(destination.position.y + row) * extent) +
                         static_cast<usize>(destination.position.x);
            std::memcpy(target, source, static_cast<usize>(destination.size.x));
        }
        entry.slot.rect = destination;
    }

    pixels_ = std::move(pixels);
    packer_ = std::move(packer);
    extent_ = extent;
    // Everything moved, so everything is dirty. An uploader that was tracking a small region has to
    // be told the whole texture changed, and saying so here is cheaper than being wrong.
    dirty_ = IRect{IVec2{0, 0}, IVec2{static_cast<i32>(extent), static_cast<i32>(extent)}};
    return ok();
}

void GlyphAtlas::evict(usize count) noexcept {
    for (usize removed = 0; removed < count && !entries_.empty(); ++removed) {
        usize oldest = 0;
        for (usize index = 1; index < entries_.size(); ++index) {
            if (entries_[index].used_at < entries_[oldest].used_at) {
                oldest = index;
            }
        }
        if (recently_evicted_.size() >= kThrashMemory) {
            // A ring by rotation rather than a real ring buffer: the array is 256 entries and this
            // happens once per eviction, which is far below anything worth a second data structure.
            for (usize index = 1; index < recently_evicted_.size(); ++index) {
                recently_evicted_[index - 1] = recently_evicted_[index];
            }
            recently_evicted_.pop_back();
        }
        (void)recently_evicted_.push_back(entries_[oldest].key);
        entries_[oldest] = entries_[entries_.size() - 1];
        entries_.pop_back();
        ++diagnostics_.glyphs_evicted;
    }
}

Expected<const GlyphSlot*, Error> GlyphAtlas::insert(const GlyphKey& key,
                                                     const GlyphMetrics& metrics,
                                                     Span<const u8> coverage) noexcept {
    if (!is_running()) {
        return fail(ErrorCode::Unavailable, "the glyph atlas has not been started");
    }
    if (coverage.size() != static_cast<usize>(metrics.width) * metrics.height) {
        return fail(ErrorCode::InvalidArgument,
                    "a glyph whose coverage does not fill its own metrics");
    }
    const auto padding = static_cast<i32>(config_.padding);
    const IVec2 wanted{static_cast<i32>(metrics.width) + padding,
                       static_cast<i32>(metrics.height) + padding};
    const auto ceiling = static_cast<i32>(config_.maximum_extent);
    if (wanted.x > ceiling || wanted.y > ceiling) {
        // Enlarging would not help: the glyph is larger than any atlas this configuration allows.
        return fail(ErrorCode::OutOfRange, "a glyph larger than the atlas's maximum extent");
    }

    for (const GlyphKey& evicted : recently_evicted_) {
        if (evicted == key) {
            // Re-rasterising something that was thrown away since it was last used. This number and
            // not occupancy is what says the atlas is too small; see the header.
            ++diagnostics_.thrashes;
            break;
        }
    }

    Expected<IRect, Error> placed = packer_.pack(wanted);
    while (!placed) {
        if (extent_ * 2 <= config_.maximum_extent) {
            if (Status grown = repack(extent_ * 2); !grown) {
                return make_unexpected(grown.error());
            }
            ++diagnostics_.atlas_growths;
        } else if (!entries_.empty()) {
            // At the ceiling: make room by evicting, and repack, because the packer's skyline does
            // not reclaim a hole in the middle of itself.
            evict((entries_.size() / 4) + 1);
            if (Status compacted = repack(extent_); !compacted) {
                return make_unexpected(compacted.error());
            }
        } else {
            return fail(ErrorCode::OutOfMemory, "the glyph atlas is full and cannot grow");
        }
        placed = packer_.pack(wanted);
    }

    const IRect rect{placed.value().position,
                     IVec2{static_cast<i32>(metrics.width), static_cast<i32>(metrics.height)}};
    for (u32 row = 0; row < metrics.height; ++row) {
        u8* target = pixels_.data() +
                     (static_cast<usize>(rect.position.y + static_cast<i32>(row)) * extent_) +
                     static_cast<usize>(rect.position.x);
        std::memcpy(target, coverage.data() + (static_cast<usize>(row) * metrics.width),
                    metrics.width);
    }
    mark_dirty(rect);

    Entry entry;
    entry.key = key;
    entry.slot.rect = rect;
    entry.slot.metrics = metrics;
    entry.used_at = ++clock_;
    if (Status pushed = entries_.push_back(entry); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++diagnostics_.glyphs_rasterised;
    return &entries_[entries_.size() - 1].slot;
}

Span<const u8> GlyphAtlas::pixels() const noexcept {
    return {pixels_.data(), pixels_.size()};
}

f32 GlyphAtlas::occupancy() const noexcept {
    return packer_.occupancy();
}

void GlyphAtlas::reset_diagnostics() noexcept {
    diagnostics_ = {};
    recently_evicted_.clear();
}

}  // namespace cy::text
