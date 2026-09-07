#include <cy/rendering/shadows/pages.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <ranges>

namespace cy::rendering {

const char* update_class_name(UpdateClass update_class) noexcept {
    switch (update_class) {
        case UpdateClass::Critical:
            return "Critical";
        case UpdateClass::Dynamic:
            return "Dynamic";
        case UpdateClass::Normal:
            return "Normal";
        case UpdateClass::Background:
            return "Background";
        case UpdateClass::Count:
            break;
    }
    return "Unknown";
}

u32 max_stale_frames(UpdateClass update_class) noexcept {
    switch (update_class) {
        case UpdateClass::Critical:
            return 0;
        case UpdateClass::Dynamic:
            return 2;
        case UpdateClass::Normal:
            return 8;
        case UpdateClass::Background:
            return 30;
        case UpdateClass::Count:
            break;
    }
    return 0;
}

PageRequestSet::PageRequestSet(Allocator& allocator) noexcept
    : raw_(allocator), unique_(allocator) {}

Status PageRequestSet::initialize(u32 capacity) noexcept {
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "page request set: capacity must be non-zero");
    }
    if (Status reserved = raw_.reserve(capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = unique_.reserve(capacity); !reserved) {
        return reserved;
    }
    capacity_ = capacity;
    begin_frame();
    return ok();
}

void PageRequestSet::begin_frame() noexcept {
    raw_.clear();
    unique_.clear();
    marks_ = 0;
    overflow_ = 0;
    compacted_ = false;
}

void PageRequestSet::mark(Span<const ShadowAddressSpace> levels,
                          const ReceiverSample& sample) noexcept {
    // Finest first: take the first level whose texel is no finer than the pixel asked for. A pixel
    // that wants finer than level 0 can have gets level 0, which is the correct answer — the light
    // has no more resolution to give and inventing one would be a promise the page table cannot
    // keep.
    const usize count = levels.size();
    for (usize index = 0; index < count; ++index) {
        const ShadowAddressSpace& space = levels[index];
        const ShadowAddress address = address_of(space, sample.world_position);
        if (!address.inside) {
            continue;
        }
        const f32 distance = length(sample.world_position - space.position);
        const f32 texel = shadow_texel_world_size(space, distance);
        if (texel >= sample.texel_world_size || index + 1 == count) {
            mark_page(address.page, sample.importance);
            return;
        }
    }
}

void PageRequestSet::mark_page(VirtualPage page, f32 importance) noexcept {
    ++marks_;
    if (raw_.size() >= capacity_) {
        ++overflow_;
        return;
    }
    PageRequest request;
    request.page = page;
    request.marks = 1;
    request.importance = importance;
    if (Status pushed = raw_.push_back(request); !pushed) {
        ++overflow_;
        return;
    }
    compacted_ = false;
}

Status PageRequestSet::compact() noexcept {
    if (compacted_) {
        return ok();
    }
    unique_.clear();
    Span<PageRequest> raw = raw_.span();
    // The sort key is the packed page id and nothing else, so two runs over the same marks produce
    // the same list in the same order whatever order the marks arrived in. See the header: a set
    // that reordered itself would make a shadow budget irreproducible.
    std::ranges::sort(raw, [](const PageRequest& a, const PageRequest& b) noexcept {
        return a.page.pack() < b.page.pack();
    });
    for (const PageRequest& request : raw) {
        if (!unique_.empty() && unique_.back().page.pack() == request.page.pack()) {
            PageRequest& merged = unique_.back();
            merged.marks += request.marks;
            merged.importance = math::max(merged.importance, request.importance);
            continue;
        }
        if (Status pushed = unique_.push_back(request); !pushed) {
            return pushed;
        }
    }
    compacted_ = true;
    return ok();
}

f32 staleness_priority(const StalePage& page) noexcept {
    if (page.update_class == UpdateClass::Critical) {
        // Above everything, and rising with age so that two critical pages still order sensibly.
        return kCriticalPriority + static_cast<f32>(page.age);
    }
    const u32 tolerance = math::max(max_stale_frames(page.update_class), 1U);
    const f32 overdue = static_cast<f32>(page.age) / static_cast<f32>(tolerance);
    // Importance and motion both make staleness visible; the product with `overdue` is what makes
    // this a "where would staleness show" ordering rather than an age ordering with extra steps.
    return overdue * (0.25F + (0.75F * page.importance)) * (1.0F + page.motion);
}

PageSelection select_pages_to_render(Span<StalePage> pages, f32 allocation_ms, f32 default_cost_ms,
                                     VirtualPage* out, u32 out_capacity) noexcept {
    PageSelection selection;
    std::ranges::sort(pages, [](const StalePage& a, const StalePage& b) noexcept {
        const f32 pa = staleness_priority(a);
        const f32 pb = staleness_priority(b);
        // The packed id breaks the tie, so the selection is deterministic when two pages are
        // equally urgent — which they routinely are on the frame after a cut.
        return pa != pb ? pa > pb : a.page.pack() < b.page.pack();
    });

    if (out == nullptr) {
        return selection;
    }
    for (const StalePage& page : pages) {
        const f32 cost = page.last_cost_ms > 0.0F ? page.last_cost_ms : default_cost_ms;
        const bool critical = page.update_class == UpdateClass::Critical;
        const bool affordable = selection.spend_ms + cost <= allocation_ms;
        if (!critical && !affordable) {
            ++selection.deferred;
            continue;
        }
        if (selection.selected >= out_capacity) {
            ++selection.deferred;
            continue;
        }
        if (critical && !affordable) {
            selection.overspend_ms += math::max(selection.spend_ms + cost - allocation_ms, 0.0F);
        }
        out[selection.selected] = page.page;
        ++selection.selected;
        selection.spend_ms += cost;
    }
    return selection;
}

}  // namespace cy::rendering
