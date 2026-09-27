// SPDX-License-Identifier: MIT
#include <cy/rendering/selection/highlight.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::selection {
namespace {

[[nodiscard]] bool unit_interval(f32 value) noexcept {
    return value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] f32 width_of(HighlightKind kind, const OutlineSettings& settings) noexcept {
    return static_cast<f32>(kind == HighlightKind::Hovered ? settings.hovered_width
                                                           : settings.selected_width);
}

[[nodiscard]] f32 channel(u32 packed, u32 shift) noexcept {
    return static_cast<f32>((packed >> shift) & 0xFFU) / 255.0F;
}

[[nodiscard]] usize index_of(const OutlineInputs& inputs, i32 x, i32 y) noexcept {
    return (static_cast<usize>(y) * inputs.width) + static_cast<usize>(x);
}

/// `hidden` in selection_outline.slang: the scene is nearer than the marked surface — reversed-Z,
/// so a larger depth — by more than the tolerance allows.
[[nodiscard]] bool hidden(const OutlineInputs& inputs, const OutlineConstants& constants, i32 x,
                          i32 y) noexcept {
    const usize index = index_of(inputs, x, y);
    const f32 marked = inputs.mask_depth[index];
    const f32 scene = inputs.scene_depth[index];
    return scene > marked + (marked * constants.depth_tolerance);
}

struct Nearest {
    u32 slot = 0;
    i32 distance_squared = 0;
    i32 x = 0;
    i32 y = 0;
};

/// The nearest marked pixel whose style reaches `(x, y)`: the scan `cyOutlineComposite` makes, in
/// its order, so a tie goes to the same pixel on both sides.
[[nodiscard]] Nearest nearest_marked(const OutlineInputs& inputs,
                                     Span<const GpuOutlineStyle> styles,
                                     const OutlineConstants& constants, i32 x, i32 y) noexcept {
    const auto radius = static_cast<i32>(constants.radius);
    const auto columns = static_cast<i32>(inputs.width);
    const auto rows = static_cast<i32>(inputs.height);
    Nearest best;
    best.distance_squared = (radius * radius) + 1;
    for (i32 dy = -radius; dy <= radius; ++dy) {
        for (i32 dx = -radius; dx <= radius; ++dx) {
            const i32 distance_squared = (dx * dx) + (dy * dy);
            const i32 qx = x + dx;
            const i32 qy = y + dy;
            if (distance_squared >= best.distance_squared || qx < 0 || qy < 0 || qx >= columns ||
                qy >= rows) {
                continue;
            }
            const u32 slot = inputs.mask[index_of(inputs, qx, qy)];
            if (slot == 0 || slot > styles.size()) {
                continue;
            }
            const f32 width = styles[slot - 1U].width;
            if (static_cast<f32>(distance_squared) > width * width) {
                continue;
            }
            best = Nearest{slot, distance_squared, qx, qy};
        }
    }
    return best;
}

/// The opacity a style has at a distance from its silhouette.
[[nodiscard]] f32 falloff(const GpuOutlineStyle& style, i32 distance_squared) noexcept {
    f32 alpha = style.strength;
    if (style.kind == static_cast<u32>(HighlightKind::Hovered)) {
        const f32 distance = std::sqrt(static_cast<f32>(distance_squared));
        const f32 t = std::clamp((style.width + 1.0F - distance) / style.width, 0.0F, 1.0F);
        alpha *= t * t;
    }
    return alpha * channel(style.colour, 24U);
}

[[nodiscard]] bool dash_gap(const OutlineConstants& constants, i32 x, i32 y) noexcept {
    if (constants.dash == 0) {
        return false;
    }
    return (((static_cast<u32>(x) + static_cast<u32>(y)) / constants.dash) & 1U) != 0;
}

[[nodiscard]] OutlineDecision inside(const OutlineInputs& inputs,
                                     Span<const GpuOutlineStyle> styles,
                                     const OutlineConstants& constants, u32 slot, i32 x,
                                     i32 y) noexcept {
    const bool show = (constants.flags & kOutlineShowOccluded) != 0;
    if (!show || constants.occluded_fill <= 0.0F || slot > styles.size() ||
        !hidden(inputs, constants, x, y)) {
        return {};
    }
    const f32 alpha = constants.occluded_fill * channel(styles[slot - 1U].colour, 24U);
    if (alpha <= 0.0F) {
        return {};
    }
    return OutlineDecision{OutlineTexel::OccludedFill, static_cast<u8>(slot), alpha};
}

}  // namespace

Status validate(const OutlineSettings& settings) noexcept {
    if (settings.selected_width == 0 || settings.selected_width > kMaxOutlineWidth ||
        settings.hovered_width == 0 || settings.hovered_width > kMaxOutlineWidth) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: a width is 1 to kMaxOutlineWidth pixels");
    }
    if (!unit_interval(settings.hovered_strength) || !unit_interval(settings.occluded_alpha) ||
        !unit_interval(settings.occluded_fill)) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: a strength, an alpha or a fill is in [0, 1]");
    }
    if (!(settings.depth_tolerance >= 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: the depth tolerance is not negative");
    }
    return ok();
}

HighlightSet::HighlightSet(Allocator& allocator) noexcept : marks_(allocator), styles_(allocator) {}

usize HighlightSet::lower_bound(u64 identity) const noexcept {
    usize low = 0;
    usize high = marks_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2U);
        if (marks_[middle].identity < identity) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

u32 HighlightSet::find_style(HighlightKind kind, HighlightColour colour) const noexcept {
    for (usize index = 0; index < styles_.size(); ++index) {
        if (styles_[index].kind == kind && styles_[index].colour == colour) {
            return static_cast<u32>(index) + 1U;
        }
    }
    return 0;
}

Status HighlightSet::compact() noexcept {
    usize kept = 0;
    for (usize index = 0; index < styles_.size(); ++index) {
        const u32 slot = static_cast<u32>(index) + 1U;
        bool used = false;
        for (const HighlightMark& mark : marks_) {
            used = used || mark.slot == slot;
        }
        if (!used) {
            continue;
        }
        styles_[kept] = styles_[index];
        for (HighlightMark& mark : marks_) {
            if (mark.slot == slot) {
                mark.slot = static_cast<u8>(kept + 1U);
            }
        }
        ++kept;
    }
    return styles_.resize(kept);
}

Expected<u32, Error> HighlightSet::acquire_style(HighlightKind kind,
                                                 HighlightColour colour) noexcept {
    if (const u32 found = find_style(kind, colour); found != 0) {
        return found;
    }
    if (styles_.size() >= kMaxHighlightStyles) {
        if (Status compacted = compact(); !compacted) {
            return make_unexpected(compacted.error());
        }
    }
    if (styles_.size() >= kMaxHighlightStyles) {
        return fail(ErrorCode::OutOfRange,
                    "selection outlines: 255 distinct kinds and colours are already in use");
    }
    if (Status pushed = styles_.push_back(Style{kind, colour}); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(styles_.size());
}

Status HighlightSet::mark(u64 identity, HighlightKind kind, HighlightColour colour) noexcept {
    if (kind != HighlightKind::Selected && kind != HighlightKind::Hovered) {
        return fail(ErrorCode::InvalidArgument, "selection outlines: an unknown highlight kind");
    }
    Expected<u32, Error> slot = acquire_style(kind, colour);
    if (!slot.has_value()) {
        return make_unexpected(slot.error());
    }
    const HighlightMark mark{identity, kind, colour, static_cast<u8>(*slot)};
    const usize at = lower_bound(identity);
    if (at < marks_.size() && marks_[at].identity == identity) {
        marks_[at] = mark;
        return ok();
    }
    if (Status pushed = marks_.push_back(mark); !pushed) {
        return pushed;
    }
    for (usize index = marks_.size() - 1U; index > at; --index) {
        marks_[index] = marks_[index - 1U];
    }
    marks_[at] = mark;
    return ok();
}

bool HighlightSet::unmark(u64 identity) noexcept {
    const usize at = lower_bound(identity);
    if (at >= marks_.size() || marks_[at].identity != identity) {
        return false;
    }
    for (usize index = at; index + 1U < marks_.size(); ++index) {
        marks_[index] = marks_[index + 1U];
    }
    marks_.pop_back();
    return true;
}

void HighlightSet::clear(HighlightKind kind) noexcept {
    usize kept = 0;
    for (const HighlightMark& mark : marks_) {
        if (mark.kind != kind) {
            marks_[kept++] = mark;
        }
    }
    while (marks_.size() > kept) {
        marks_.pop_back();
    }
}

void HighlightSet::clear() noexcept {
    marks_.clear();
    styles_.clear();
}

const HighlightMark* HighlightSet::find(u64 identity) const noexcept {
    const usize at = lower_bound(identity);
    if (at < marks_.size() && marks_[at].identity == identity) {
        return &marks_[at];
    }
    return nullptr;
}

u32 HighlightSet::slot_of(u64 identity) const noexcept {
    const HighlightMark* mark = find(identity);
    return mark != nullptr ? mark->slot : 0U;
}

GpuOutlineStyle HighlightSet::style(u32 slot, const OutlineSettings& settings) const noexcept {
    if (slot == 0 || slot > styles_.size()) {
        return {};
    }
    const Style& entry = styles_[slot - 1U];
    GpuOutlineStyle style;
    style.colour = entry.colour.packed();
    style.kind = static_cast<u32>(entry.kind);
    style.width = width_of(entry.kind, settings);
    style.strength = entry.kind == HighlightKind::Hovered ? settings.hovered_strength : 1.0F;
    return style;
}

void HighlightSet::write_styles(const OutlineSettings& settings,
                                Span<GpuOutlineStyle> out) const noexcept {
    for (usize index = 0; index < styles_.size() && index < out.size(); ++index) {
        out[index] = style(static_cast<u32>(index) + 1U, settings);
    }
}

u32 HighlightSet::radius(const OutlineSettings& settings) const noexcept {
    u32 widest = 0;
    for (const HighlightMark& mark : marks_) {
        const u32 width =
            mark.kind == HighlightKind::Hovered ? settings.hovered_width : settings.selected_width;
        widest = width > widest ? width : widest;
    }
    return widest > kMaxOutlineWidth ? kMaxOutlineWidth : widest;
}

OutlineConstants make_outline_constants(const HighlightSet& highlights,
                                        const OutlineSettings& settings, u32 width,
                                        u32 height) noexcept {
    OutlineConstants constants;
    constants.extent[0] = width;
    constants.extent[1] = height;
    constants.radius = highlights.radius(settings);
    constants.dash = settings.occluded_dash;
    constants.occluded_alpha = settings.occluded_alpha;
    constants.occluded_fill = settings.occluded_fill;
    constants.depth_tolerance = settings.depth_tolerance;
    constants.flags = settings.show_occluded ? kOutlineShowOccluded : 0U;
    return constants;
}

OutlineDecision outline_reference_at(const OutlineInputs& inputs,
                                     Span<const GpuOutlineStyle> styles,
                                     const OutlineConstants& constants, u32 x, u32 y) noexcept {
    const auto px = static_cast<i32>(x);
    const auto py = static_cast<i32>(y);
    const u32 here = inputs.mask[index_of(inputs, px, py)];
    if (here != 0) {
        return inside(inputs, styles, constants, here, px, py);
    }
    const Nearest nearest = nearest_marked(inputs, styles, constants, px, py);
    if (nearest.slot == 0) {
        return {};
    }
    const GpuOutlineStyle& style = styles[nearest.slot - 1U];
    f32 alpha = falloff(style, nearest.distance_squared);
    OutlineTexel texel = OutlineTexel::Outline;
    if (hidden(inputs, constants, nearest.x, nearest.y)) {
        if ((constants.flags & kOutlineShowOccluded) == 0 || dash_gap(constants, px, py)) {
            return {};
        }
        alpha *= constants.occluded_alpha;
        texel = OutlineTexel::OccludedOutline;
    }
    if (alpha <= 0.0F) {
        return {};
    }
    return OutlineDecision{texel, static_cast<u8>(nearest.slot), alpha};
}

Status outline_reference(const OutlineInputs& inputs, Span<const GpuOutlineStyle> styles,
                         const OutlineConstants& constants, Span<OutlineDecision> out) noexcept {
    const usize pixels = static_cast<usize>(inputs.width) * inputs.height;
    if (pixels == 0 || inputs.mask.size() != pixels || inputs.mask_depth.size() != pixels ||
        inputs.scene_depth.size() != pixels || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "selection outlines: the reference's inputs are not one image of the extent");
    }
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            out[(static_cast<usize>(y) * inputs.width) + x] =
                outline_reference_at(inputs, styles, constants, x, y);
        }
    }
    return ok();
}

u32 blend_outline(u32 destination, u32 colour, f32 alpha) noexcept {
    u32 result = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        // The alpha channel blends the source alpha itself; the colour channels blend the
        // premultiplied colour — `One, OneMinusSourceAlpha` on both, as the pipeline declares.
        const f32 source = shift == 24U ? alpha : channel(colour, shift) * alpha;
        const f32 blended = source + (channel(destination, shift) * (1.0F - alpha));
        const f32 clamped = std::clamp(blended, 0.0F, 1.0F);
        result |= static_cast<u32>(std::lround(clamped * 255.0F)) << shift;
    }
    return result;
}

}  // namespace cy::rendering::selection
