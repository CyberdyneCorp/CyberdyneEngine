// SPDX-License-Identifier: MIT
// The toolkit's frame: bindings, virtual rows, rich text, what is placed after layout, and the
// built-in theme. See toolkit.h.

#include "toolkit_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::ui {
namespace {

/// What the built-in theme is: a dark, legible default every widget looks finished in.
constexpr std::string_view kDefaultTheme = R"cyss(
/* CyberUI's built-in theme. A game's sheet overrides any of it; `Toolkit::set_theme` replaces it. */
:root {
    --text: #e6e9ef;
    --muted: #8a93a6;
    --surface: #1b1f27;
    --raised: #2b3240;
    --hover: #36405a;
    --sunken: #12151b;
    --line: #3a4252;
    --accent: #3d8bfd;
    --good: #3ccf91;
}
label, rich-text, button-label, toggle-label, checkbox-label, radio-label, option-label,
list-row-label, text-value, dialog-title, tooltip-label { color: var(--text); }
.panel {
    background-color: var(--surface); border-color: var(--line); border-width: 1;
    corner-radius: 6; padding-left: 10; padding-top: 10; padding-right: 10; padding-bottom: 10;
}
row { gap: 6; }
column { gap: 6; }
grid { gap: 6; }
button {
    background-color: var(--raised); border-color: var(--line); border-width: 1; corner-radius: 4;
    padding-left: 10; padding-right: 10; padding-top: 4; padding-bottom: 4; min-height: 24;
    transition-duration: 0.1s; transition-timing-function: ease-out;
}
button:hover { background-color: var(--hover); }
button:active { background-color: #22334f; }
button:disabled { opacity: 0.45; }
.primary { background-color: var(--accent); border-color: var(--accent); }
.primary:hover { background-color: #5a9dff; }
.danger { background-color: #8c2f39; border-color: #b23a48; }
.danger:hover { background-color: #a8384a; }
toggle { gap: 6; min-height: 24; }
toggle-track {
    width: 34; height: 18; corner-radius: 9; background-color: var(--line);
    transition-duration: 0.12s;
}
toggle-track:checked { background-color: var(--accent); }
toggle-knob { corner-radius: 7; background-color: var(--text); }
checkbox { gap: 6; min-height: 24; }
checkbox-box {
    width: 16; height: 16; corner-radius: 3; border-width: 1; border-color: #5a6478;
    background-color: #20252f;
}
checkbox-box:checked { background-color: var(--accent); border-color: var(--accent); }
checkbox-mark { background-color: #ffffff; corner-radius: 1; }
radio-group { gap: 4; }
radio { gap: 6; min-height: 24; }
radio-ring {
    width: 16; height: 16; corner-radius: 8; border-width: 1; border-color: #5a6478;
    background-color: #20252f;
}
radio-ring:checked { border-color: var(--accent); }
radio-dot { background-color: var(--accent); corner-radius: 4; }
slider { height: 24; min-width: 140; }
slider-track { background-color: var(--line); corner-radius: 2; }
slider-fill { background-color: var(--accent); corner-radius: 2; }
slider-thumb {
    background-color: var(--text); corner-radius: 7; border-width: 1; border-color: var(--surface);
}
slider-thumb:hover { background-color: #ffffff; }
progress-bar { height: 10; min-width: 140; background-color: var(--raised); corner-radius: 5; }
progress-fill { background-color: var(--good); corner-radius: 5; }
text-field {
    height: 24; min-width: 180; background-color: var(--sunken); border-width: 1;
    border-color: var(--line); corner-radius: 4; padding-left: 6; padding-right: 6;
}
text-field:focus { border-color: var(--accent); }
text-placeholder { color: #6b7488; }
text-caret { background-color: var(--text); }
text-selection { background-color: #3d8bfd70; }
text-composition { background-color: var(--text); }
dropdown {
    height: 24; min-width: 160; background-color: var(--raised); border-width: 1;
    border-color: var(--line); corner-radius: 4; padding-left: 8; padding-right: 8; gap: 6;
}
dropdown:hover { background-color: var(--hover); }
dropdown-label { color: var(--text); flex-grow: 1; }
dropdown-arrow { color: var(--muted); }
dropdown-popup {
    background-color: #20252f; border-width: 1; border-color: var(--line); corner-radius: 4;
    padding-top: 2; padding-bottom: 2;
}
option { height: 22; padding-left: 8; padding-right: 8; }
option:hover { background-color: var(--hover); }
option:checked { background-color: #2d4a7a; }
list-view {
    background-color: var(--sunken); border-width: 1; border-color: var(--line); corner-radius: 4;
    min-height: 88; min-width: 180;
}
list-row:hover { background-color: var(--raised); }
list-row:checked { background-color: #2d4a7a; }
list-thumb { background-color: #5a647899; corner-radius: 2; }
scroll-thumb { background-color: #5a647899; corner-radius: 2; }
scroll-content { gap: 6; }
scroll-view {
    background-color: var(--sunken); border-width: 1; border-color: var(--line); corner-radius: 4;
    padding-left: 6; padding-top: 4; padding-right: 10; padding-bottom: 4;
}
tab-bar { gap: 2; }
tab {
    background-color: #20252f; corner-radius: 4; padding-left: 10; padding-right: 10;
    padding-top: 4; padding-bottom: 4; min-height: 24;
}
tab:hover { background-color: var(--raised); }
tab:checked { background-color: var(--hover); }
tab-label { color: var(--muted); }
tab-label:checked { color: var(--text); }
tab-pages { padding-top: 8; }
dialog-backdrop { background-color: #00000099; }
dialog {
    background-color: var(--surface); border-width: 1; border-color: var(--line); corner-radius: 8;
    padding-left: 16; padding-top: 14; padding-right: 16; padding-bottom: 14; gap: 10;
    min-width: 280;
}
dialog-body { gap: 8; }
focus-ring { border-width: 2; border-color: #ffd34d; corner-radius: 6; }
tooltip {
    background-color: #0d0f13; border-width: 1; border-color: var(--line); corner-radius: 3;
    padding-left: 6; padding-top: 3; padding-right: 6; padding-bottom: 3;
}
)cyss";

[[nodiscard]] u32 hex_digit(char byte) noexcept {
    if (byte >= '0' && byte <= '9') {
        return static_cast<u32>(byte - '0');
    }
    if (byte >= 'a' && byte <= 'f') {
        return static_cast<u32>(byte - 'a') + 10U;
    }
    if (byte >= 'A' && byte <= 'F') {
        return static_cast<u32>(byte - 'A') + 10U;
    }
    return 16U;
}

/// `#rgb`, `#rrggbb` or `#rrggbbaa`, premultiplied, as `.cyss` reads them.
[[nodiscard]] bool parse_markup_colour(std::string_view text, u32& out) noexcept {
    if (text.empty() || text[0] != '#') {
        return false;
    }
    const std::string_view digits = text.substr(1);
    u32 channels[4] = {0, 0, 0, 255};
    const usize width = (digits.size() == 3 || digits.size() == 4) ? 1U : 2U;
    if (digits.size() != 3 && digits.size() != 4 && digits.size() != 6 && digits.size() != 8) {
        return false;
    }
    for (usize index = 0; index * width < digits.size(); ++index) {
        u32 value = 0;
        for (usize nibble = 0; nibble < width; ++nibble) {
            const u32 digit = hex_digit(digits[(index * width) + nibble]);
            if (digit > 15U) {
                return false;
            }
            value = (value * 16U) + digit;
        }
        channels[index] = (width == 1U) ? (value * 17U) : value;
    }
    for (u32 index = 0; index < 3U; ++index) {
        channels[index] = ((channels[index] * channels[3]) + 127U) / 255U;
    }
    out = (channels[3] << 24U) | (channels[0] << 16U) | (channels[1] << 8U) | channels[2];
    return true;
}

[[nodiscard]] bool parse_number(std::string_view text, f32& out) noexcept {
    if (text.empty()) {
        return false;
    }
    f32 value = 0.0F;
    f32 scale = 0.0F;
    for (const char byte : text) {
        if (byte == '.' && scale == 0.0F) {
            scale = 0.1F;
        } else if (byte >= '0' && byte <= '9') {
            if (scale == 0.0F) {
                value = (value * 10.0F) + static_cast<f32>(byte - '0');
            } else {
                value += static_cast<f32>(byte - '0') * scale;
                scale *= 0.1F;
            }
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

/// One run of rich text: text in one size with colour spans, or an image.
struct RichRun {
    bool image = false;
    usize begin = 0;
    usize end = 0;
    f32 size = 0.0F;
    u16 page = 0;
    Rect uv;
    usize first_span = 0;
    usize end_span = 0;
};

}  // namespace

std::string_view default_theme_source() noexcept {
    return kDefaultTheme;
}

// --- Rich text -----------------------------------------------------------------------------------

Status Toolkit::build_rich_text(Widget& widget) noexcept {
    // Rebuilt whole: rich text is set, not edited, and a run per change of size or image is a
    // handful of elements.
    const WidgetId id = widget.element;
    Array<ElementId> old(*allocator_);
    if (Status listed = store_.children_of(id, old); !listed) {
        return listed;
    }
    for (const ElementId element : old.span()) {
        forget(element);
        if (Status destroyed = store_.destroy(element); !destroyed) {
            return destroyed;
        }
    }
    Widget* rich = record(id);
    rich->spans.clear();
    // The markup is the widget's caption, whose bytes no element made below can move.
    const std::string_view markup(rich->caption.data(), rich->caption.size());

    // PARSE: text accumulates into `plain`; a colour tag closes the span being written and opens
    // one in the new colour (or in the colour outside it, on `[/color]`), so spans never overlap
    // and text outside every span is the run's own colour.
    Array<char> plain(*allocator_);
    Array<RichRun> runs(*allocator_);
    Array<u32> colours(*allocator_);
    Array<f32> sizes(*allocator_);
    RichRun current;
    bool span_open = false;
    const auto run_offset = [&]() noexcept {
        return static_cast<u32>(plain.size() - current.begin);
    };
    const auto open_span = [&]() noexcept -> Status {
        span_open = false;
        if (colours.empty()) {
            return ok();
        }
        ColourSpan span;
        span.begin = run_offset();
        span.end = span.begin;
        span.colour = colours[colours.size() - 1];
        span_open = true;
        return record(id)->spans.push_back(span);
    };
    const auto close_run = [&]() noexcept -> Status {
        current.end = plain.size();
        current.end_span = record(id)->spans.size();
        if (current.end > current.begin) {
            if (Status pushed = runs.push_back(current); !pushed) {
                return pushed;
            }
        }
        current = RichRun{};
        current.begin = plain.size();
        current.size = sizes.empty() ? 0.0F : sizes[sizes.size() - 1];
        current.first_span = record(id)->spans.size();
        // The colour in force carries into the next run.
        return open_span();
    };
    const auto add_byte = [&](char byte) noexcept -> Status {
        if (Status kept = plain.push_back(byte); !kept) {
            return kept;
        }
        if (span_open) {
            Widget* owner = record(id);
            owner->spans[owner->spans.size() - 1].end = run_offset();
        }
        return ok();
    };
    for (usize cursor = 0; cursor < markup.size();) {
        const char byte = markup[cursor];
        if (byte == '[' && cursor + 1 < markup.size() && markup[cursor + 1] == '[') {
            if (Status kept = add_byte('['); !kept) {
                return kept;
            }
            cursor += 2;
            continue;
        }
        const usize close = (byte == '[') ? markup.find(']', cursor) : std::string_view::npos;
        if (close == std::string_view::npos) {
            if (Status kept = add_byte(byte); !kept) {
                return kept;
            }
            ++cursor;
            continue;
        }
        const std::string_view tag = markup.substr(cursor + 1, close - cursor - 1);
        cursor = close + 1;
        Status handled = ok();
        if (tag.starts_with("color=")) {
            u32 colour = 0;
            if (parse_markup_colour(tag.substr(6), colour)) {
                handled = colours.push_back(colour);
                if (handled) {
                    handled = open_span();
                }
            }
        } else if (tag == "/color") {
            if (!colours.empty()) {
                colours.pop_back();
            }
            handled = open_span();
        } else if (tag.starts_with("size=")) {
            f32 size = 0.0F;
            if (parse_number(tag.substr(5), size)) {
                handled = sizes.push_back(size);
                if (handled) {
                    handled = close_run();
                }
            }
        } else if (tag == "/size") {
            if (!sizes.empty()) {
                sizes.pop_back();
            }
            handled = close_run();
        } else if (tag.starts_with("img=")) {
            handled = close_run();
            RichRun image;
            image.image = true;
            image.size = current.size;
            f32 fields[5] = {0.0F, 0.0F, 0.0F, 1.0F, 1.0F};
            std::string_view rest = tag.substr(4);
            for (f32& field : fields) {
                const usize comma = rest.find(',');
                (void)parse_number(rest.substr(0, comma), field);
                rest =
                    (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
            }
            image.page = static_cast<u16>(fields[0]);
            image.uv = Rect{fields[1], fields[2], fields[3], fields[4]};
            if (handled) {
                handled = runs.push_back(image);
            }
        }
        if (!handled) {
            return handled;
        }
    }
    if (Status closed = close_run(); !closed) {
        return closed;
    }

    // ONE ELEMENT A RUN: a label for text, carrying its colour spans; an image for an image.
    const u32 slot = part_record(id)->widget;
    for (const RichRun& run : runs.span()) {
        Expected<ElementId, Error> made =
            make_element(id, run.image ? "rich-image" : "rich-run", slot);
        if (!made) {
            return make_unexpected(made.error());
        }
        Part* part = part_record(*made);
        if (run.image) {
            part->base_paint.material = material_index(BuiltinMaterial::Image);
            part->base_paint.atlas = run.page;
            part->base_paint.uv = run.uv;
            part->base_paint.background = 0xFFFFFFFFU;
            const f32 edge = run.size > 0.0F ? run.size : 13.0F;
            part->base_layout.preferred = Vec2{edge, edge};
            continue;
        }
        part->has_text = true;
        part->text_style.size = run.size;
        part->base_text_size = run.size;
        // The run's spans: those opened inside it, before the next run's began.
        const auto first = static_cast<u32>(run.first_span);
        const auto count = static_cast<u32>(run.end_span - run.first_span);
        part->first_span = first;
        part->span_count = count;
        if (Status set = set_element_text(
                *made, std::string_view(plain.data() + run.begin, run.end - run.begin));
            !set) {
            return set;
        }
        if (count > 0) {
            if (Status coloured =
                    text_.set_colours(*made, record(id)->spans.span().subspan(first, count));
                !coloured) {
                return coloured;
            }
        }
    }
    store_.mark(id, Dirty::Measure | Dirty::Arrange);
    return ok();
}

// --- Lists ---------------------------------------------------------------------------------------

f32 Toolkit::max_scroll(const Widget& widget) const noexcept {
    const LayoutOutput* port = store_.layout_output(widget.parts[0]);
    const f32 height = port != nullptr ? port->rect.height : 0.0F;
    f32 total = 0.0F;
    if (widget.kind == WidgetKind::ListView) {
        total = static_cast<f32>(item_count(widget.element)) * widget.row_height;
    } else {
        const LayoutOutput* content = store_.layout_output(widget.content);
        total = content != nullptr ? content->desired.y : 0.0F;
    }
    return std::max(total - height, 0.0F);
}

Status Toolkit::realise_list(Widget& widget, u32& realised) noexcept {
    const WidgetId list_id = widget.element;
    const LayoutOutput* port = store_.layout_output(widget.parts[0]);
    VirtualList list;
    list.item_count = item_count(list_id);
    list.item_height = widget.row_height;
    list.scroll = widget.scroll;
    list.viewport_height = port != nullptr ? port->rect.height : 0.0F;
    // THE ARITHMETIC IS `virtual_range`'s: which rows can be seen, plus two either side. Nothing
    // here walks the items, so ten thousand of them cost what ten do.
    const VirtualRange range = virtual_range(list);
    const u32 needed = range.count();

    while (record(list_id)->rows.size() < needed) {
        WidgetDesc row;
        row.kind = WidgetKind::ListRow;
        Expected<WidgetId, Error> made = create(list_id, row);
        if (!made) {
            return make_unexpected(made.error());
        }
        Widget* row_widget = record(*made);
        row_widget->owner = list_id;
        row_widget->index = kNone;
        if (Status pushed = record(list_id)->rows.push_back(*made); !pushed) {
            return pushed;
        }
    }
    Widget* owner = record(list_id);
    const bool all = owner->rows_dirty;
    owner->rows_dirty = false;
    const f32 height = owner->row_height;
    Array<char> caption(*allocator_);
    for (usize slot = 0; slot < owner->rows.size(); ++slot) {
        Widget* row = record(record(list_id)->rows[slot]);
        if (row == nullptr) {
            continue;
        }
        const bool used = slot < needed;
        const u32 index = used ? range.first + static_cast<u32>(slot) : kNone;
        const ElementFlags flags = store_.flags(row->element);
        if (has_flag(flags, ElementFlags::Collapsed) == used) {
            (void)store_.set_flags(row->element, used ? without_flag(flags, ElementFlags::Collapsed)
                                                      : (flags | ElementFlags::Collapsed));
        }
        if (!used || (index == row->index && !all)) {
            continue;
        }
        // A ROW TAKES A NEW ITEM: its place, its text and its selected state. Its place is an
        // arrange, its text a repaint of a label its fixed-height row does not resize.
        row->index = index;
        row->checked = index == record(list_id)->selected;
        Part* part = part_record(row->element);
        const f32 top = static_cast<f32>(index) * height;
        anchor(part->base_layout, Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{0.0F, top},
               Vec2{0.0F, top + height});
        mark_style(row->element);
        caption.clear();
        if (Status read = item_text(list_id, index, caption); !read) {
            return read;
        }
        row = record(record(list_id)->rows[slot]);
        const ElementId label = row->parts[0];
        if (Status set = set_element_text(label, std::string_view(caption.data(), caption.size()));
            !set) {
            return set;
        }
        Part* label_part = part_record(label);
        const f32 line = text_.measure("Ag", label_part->text_style).y;
        anchor(label_part->base_layout, Vec2{0.0F, 0.5F}, Vec2{0.0F, 0.5F},
               Vec2{6.0F, -line * 0.5F}, Vec2{6.0F, line * 0.5F});
        mark_style(label);
        if (Status refreshed = update_parts(*row); !refreshed) {
            return refreshed;
        }
        ++realised;
    }
    owner = record(list_id);
    return place_thumb(owner->parts[2], list.viewport_height, range.total_height, owner->scroll);
}

namespace {

/// A scroll bar's thumb: where the window is in the whole, as long as the window is of the whole.
/// Returns whether it shows.
[[nodiscard]] bool thumb_layout(f32 window, f32 total, f32 scroll, LayoutInput& layout) noexcept {
    if (!(total > window && window > 0.0F)) {
        return false;
    }
    const f32 length = std::max(16.0F, window * window / total);
    const f32 top = (scroll / (total - window)) * (window - length);
    anchor(layout, Vec2{1.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{-5.0F, top + 1.0F},
           Vec2{-2.0F, top + length - 1.0F});
    return true;
}

}  // namespace

Status Toolkit::place_thumb(ElementId thumb, f32 window, f32 total, f32 scroll) noexcept {
    Part* part = part_record(thumb);
    if (part == nullptr) {
        return ok();
    }
    LayoutInput next = part->base_layout;
    const bool shows = thumb_layout(window, total, scroll, next);
    const ElementFlags flags = store_.flags(thumb);
    if (has_flag(flags, ElementFlags::Visible) != shows) {
        if (Status set =
                store_.set_flags(thumb, shows ? (flags | ElementFlags::Visible)
                                              : without_flag(flags, ElementFlags::Visible));
            !set) {
            return set;
        }
    }
    if (shows && std::memcmp(&next, &part->base_layout, sizeof(LayoutInput)) != 0) {
        part->base_layout = next;
        mark_style(thumb);
    }
    return ok();
}

Status Toolkit::realise_lists(u32& realised) noexcept {
    for (usize index = 0; index < widgets_.size(); ++index) {
        const Widget& widget = widgets_[index];
        if (!widget.alive || has_flag(store_.flags(widget.element), ElementFlags::Collapsed)) {
            continue;
        }
        if (widget.kind == WidgetKind::ScrollView) {
            // A scroll view's thumb, from its viewport and what its content asked for.
            const LayoutOutput* port = store_.layout_output(widget.parts[0]);
            const LayoutOutput* content = store_.layout_output(widget.content);
            if (port != nullptr && content != nullptr) {
                if (Status placed = place_thumb(widget.parts[2], port->rect.height,
                                                content->desired.y, widget.scroll);
                    !placed) {
                    return placed;
                }
            }
            continue;
        }
        if (widget.kind != WidgetKind::ListView) {
            continue;
        }
        const WidgetId list = widget.element;
        if (Status done = realise_list(*record(list), realised); !done) {
            return done;
        }
    }
    return ok();
}

// --- After layout --------------------------------------------------------------------------------

namespace {

/// Place an overlay element at a rect in the document, by degenerate-free anchors at the origin.
[[nodiscard]] bool place_at(LayoutInput& layout, const Rect& rect) noexcept {
    LayoutInput next = layout;
    next.anchor_min = Vec2{0.0F, 0.0F};
    next.anchor_max = Vec2{0.0F, 0.0F};
    next.offset_min = Vec2{rect.x, rect.y};
    next.offset_max = Vec2{rect.right(), rect.bottom()};
    if (std::memcmp(&next, &layout, sizeof(LayoutInput)) == 0) {
        return false;
    }
    layout = next;
    return true;
}

void show(ElementStore& store, ElementId element, bool on) noexcept {
    const ElementFlags flags = store.flags(element);
    if (has_flag(flags, ElementFlags::Visible) != on) {
        (void)store.set_flags(element, on ? (flags | ElementFlags::Visible)
                                          : without_flag(flags, ElementFlags::Visible));
    }
}

[[nodiscard]] bool drawn(const ElementStore& store, ElementId element) noexcept {
    for (ElementId walk = element; walk.is_valid();) {
        const ElementFlags flags = store.flags(walk);
        if (!has_flag(flags, ElementFlags::Visible) || has_flag(flags, ElementFlags::Collapsed)) {
            return false;
        }
        const Hierarchy* node = store.hierarchy(walk);
        walk = node != nullptr ? node->parent : kNoElement;
    }
    return true;
}

}  // namespace

Status Toolkit::place_overlays(f32 seconds) noexcept {
    // THE RING AND THE TOOLTIP ARE DRAWN LAST, over a dialog or a drop-down opened after them:
    // each is moved to the end of the overlay whenever something was put after it.
    if (const Hierarchy* overlay = store_.hierarchy(overlay_);
        overlay != nullptr && overlay->last_child != tooltip_) {
        for (const ElementId floating : {focus_ring_, tooltip_}) {
            if (Status moved = store_.reparent(floating, overlay_); !moved) {
                return moved;
            }
        }
    }
    // THE FOCUS RING: around the focused widget, two units out, only when focus came from a
    // keyboard or a gamepad.
    const WidgetId focus = focused();
    const bool ring = focus.is_valid() && focus_visible_ && drawn(store_, focus);
    if (ring) {
        const Rect rect = store_.layout_output(focus)->rect;
        Part* part = part_record(focus_ring_);
        if (place_at(part->base_layout,
                     Rect{rect.x - 3.0F, rect.y - 3.0F, rect.width + 6.0F, rect.height + 6.0F})) {
            mark_style(focus_ring_);
        }
    }
    show(store_, focus_ring_, ring);

    // THE CARET BLINKS while its field has focus; any edit restarts the period.
    caret_seconds_ += seconds;
    if (Widget* field = record(focus); field != nullptr && field->kind == WidgetKind::TextField) {
        const f32 period = settings_.caret_blink;
        const bool on = period <= 0.0F || std::fmod(caret_seconds_, period) < period * 0.5F;
        show(store_, field->parts[4], on);
    }

    // AN OPEN DROP-DOWN'S LIST follows the drop-down, should the drop-down move.
    if (popup_.is_valid() && store_.alive(popup_owner_)) {
        const Rect owner = store_.layout_output(popup_owner_)->rect;
        Part* part = part_record(popup_);
        if (place_at(part->base_layout, Rect{owner.x, owner.bottom() + 2.0F, owner.width, 0.0F})) {
            mark_style(popup_);
        }
    }

    // THE TOOLTIP: after the pointer has rested on a widget that has one, beside the pointer.
    const Widget* hovered = record(hovered_);
    const bool wants = hovered != nullptr && !hovered->tooltip.empty() && !pointer_down_;
    if (wants) {
        hover_seconds_ += seconds;
    }
    const bool tooltip = wants && hover_seconds_ >= settings_.tooltip_delay;
    if (tooltip && tooltip_target_ != hovered_) {
        tooltip_target_ = hovered_;
        if (Status set = set_element_text(
                tooltip_label_, std::string_view(hovered->tooltip.data(), hovered->tooltip.size()));
            !set) {
            return set;
        }
        Part* part = part_record(tooltip_);
        // Degenerate on both axes: the tooltip's own desired size, at the pointer.
        LayoutInput next = part->base_layout;
        next.anchor_min = Vec2{0.0F, 0.0F};
        next.anchor_max = Vec2{0.0F, 0.0F};
        next.offset_min = Vec2{pointer_position_.x + 12.0F, pointer_position_.y + 16.0F};
        next.offset_max = next.offset_min;
        part->base_layout = next;
        mark_style(tooltip_);
    }
    if (!tooltip) {
        tooltip_target_ = kNoElement;
    }
    show(store_, tooltip_, tooltip);
    return ok();
}

// --- Bindings ------------------------------------------------------------------------------------

Status Toolkit::bind(WidgetId widget, WidgetProperty property, BindingSource& source,
                     BindingMode mode) noexcept {
    if (record(widget) == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (property >= WidgetProperty::Count) {
        return fail(ErrorCode::InvalidArgument, "toolkit: no such widget property");
    }
    BoundWidget binding;
    binding.widget = widget;
    binding.property = property;
    binding.source = &source;
    binding.mode = mode;
    return bindings_.push_back(binding);
}

void Toolkit::unbind(WidgetId widget) noexcept {
    for (usize index = bindings_.size(); index > 0; --index) {
        if (bindings_[index - 1].widget == widget) {
            bindings_.remove_unordered(index - 1);
        }
    }
}

namespace {

[[nodiscard]] BoundValue value_of(const BoundValue& base, f32 number) noexcept {
    BoundValue value = base;
    value.kind = BoundValue::Kind::Number;
    value.number = number;
    return value;
}

[[nodiscard]] BoundValue truth_of(bool on) noexcept {
    BoundValue value;
    value.kind = BoundValue::Kind::Boolean;
    value.boolean = on;
    return value;
}

}  // namespace

Status Toolkit::write_back(Widget& widget, WidgetProperty property) noexcept {
    BoundValue value;
    switch (property) {
        case WidgetProperty::Value:
            value = value_of(value, widget.value);
            break;
        case WidgetProperty::Checked:
            value = truth_of(widget.checked);
            break;
        case WidgetProperty::Selected:
            value = value_of(value, static_cast<f32>(widget.selected));
            break;
        case WidgetProperty::Text: {
            value.kind = BoundValue::Kind::Text;
            const std::string_view text =
                widget.kind == WidgetKind::TextField
                    ? widget.editor.text()
                    : std::string_view(widget.caption.data(), widget.caption.size());
            value.text = Name::intern(text);
            break;
        }
        default:
            return ok();
    }
    for (BoundWidget& binding : bindings_.span()) {
        if (binding.widget != widget.element || binding.property != property ||
            binding.mode != BindingMode::TwoWay) {
            continue;
        }
        if (Status written = binding.source->write(value); !written) {
            return written;
        }
        // What was written is what the source now holds: reading it back is not a change.
        binding.last = value;
        binding.primed = true;
    }
    return ok();
}

Status Toolkit::update_bindings(u32& applied) noexcept {
    for (usize index = 0; index < bindings_.size(); ++index) {
        BoundWidget& binding = bindings_[index];
        Widget* widget = record(binding.widget);
        if (widget == nullptr || binding.source == nullptr) {
            continue;
        }
        if (binding.primed && !binding.pending && !binding.source->changed()) {
            continue;
        }
        // BEING EDITED: a text field with focus, a slider in the hand. The source's change waits.
        const bool editing =
            (widget->kind == WidgetKind::TextField && interaction_.focus() == widget->element) ||
            (widget->kind == WidgetKind::Slider && pressed_ == widget->element && dragging_);
        if (editing) {
            binding.pending = true;
            continue;
        }
        const BoundValue value = binding.source->read();
        binding.pending = false;
        if (binding.primed && value == binding.last) {
            continue;
        }
        binding.last = value;
        binding.primed = true;
        const WidgetId id = binding.widget;
        const WidgetProperty property = binding.property;
        Status set = ok();
        switch (property) {
            case WidgetProperty::Value:
                set = set_value(id, value.number);
                break;
            case WidgetProperty::Checked:
                set = set_checked(id, value.boolean);
                break;
            case WidgetProperty::Text:
                set = set_text(id, value.text.text());
                break;
            case WidgetProperty::Selected:
                set = set_selected(id, static_cast<u32>(std::max(value.number, 0.0F)));
                break;
            case WidgetProperty::Visible:
                set = set_visible(id, value.boolean);
                break;
            case WidgetProperty::Enabled:
                set = set_enabled(id, value.boolean);
                break;
            case WidgetProperty::Count:
                break;
        }
        if (!set) {
            return set;
        }
        ++applied;
    }
    return ok();
}

// --- The frame -----------------------------------------------------------------------------------

Status Toolkit::update(f32 seconds, const ScaleSettings& settings, Vec2 viewport,
                       ToolkitReport* report) noexcept {
    if (!root_.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "toolkit: not started");
    }
    ToolkitReport local;
    if (Status bound = update_bindings(local.bindings_applied); !bound) {
        return bound;
    }
    for (const Track& track : tracks_.span()) {
        local.animations += track.transition ? 0U : 1U;
        local.transitions += track.transition ? 1U : 0U;
    }
    if (Status advanced = advance_tracks(seconds); !advanced) {
        return advanced;
    }
    sync_states();
    const auto styled = [this, &local]() noexcept -> Status {
        const u32 before = style_frame_;
        (void)before;
        const usize pending = restyle_.size();
        if (Status restyled = restyle(); !restyled) {
            return restyled;
        }
        local.restyled += static_cast<u32>(pending);
        return ok();
    };
    if (Status restyled = styled(); !restyled) {
        return restyled;
    }
    if (Status laid = layout(store_, settings, viewport, &text_, local.layout); !laid) {
        return laid;
    }
    // AFTER LAYOUT: rows the lists can now see, and what is placed around laid-out widgets. Each
    // dirties a handful of elements, which a second, small layout pass settles in this frame.
    if (Status rows = realise_lists(local.rows_realised); !rows) {
        return rows;
    }
    if (Status placed = place_overlays(seconds); !placed) {
        return placed;
    }
    sync_states();
    if (!restyle_.empty() || store_.any_dirty()) {
        if (Status restyled = styled(); !restyled) {
            return restyled;
        }
        LayoutReport second;
        if (Status laid = layout(store_, settings, viewport, &text_, second); !laid) {
            return laid;
        }
        local.layout.measured += second.measured;
        local.layout.arranged += second.arranged;
        local.layout.skipped += second.skipped;
        local.layout.deepest = std::max(local.layout.deepest, second.deepest);
    }
    if (report != nullptr) {
        *report = local;
    }
    return ok();
}

}  // namespace cy::ui
