// The two-pass layout: measure bottom-up, arrange top-down, over three models. M8.b task 9.1.

#include <cy/ui/layout.h>

#include <algorithm>
#include <cmath>

namespace cy::ui {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

[[nodiscard]] bool takes_part_in_layout(const ElementStore& store, ElementId element) noexcept {
    const ElementFlags flags = store.flags(element);
    return !has_flag(flags, ElementFlags::Collapsed);
}

[[nodiscard]] bool is_row(FlexDirection direction) noexcept {
    return direction == FlexDirection::Row || direction == FlexDirection::RowReverse;
}

[[nodiscard]] bool is_reversed(FlexDirection direction) noexcept {
    return direction == FlexDirection::RowReverse || direction == FlexDirection::ColumnReverse;
}

/// The size an element wants, after its own minimum, maximum and aspect ratio have had their say.
[[nodiscard]] Vec2 constrain(const LayoutInput& input, Vec2 size) noexcept {
    Vec2 result{clampf(size.x, input.minimum.x, input.maximum.x),
                clampf(size.y, input.minimum.y, input.maximum.y)};
    if (input.aspect_ratio > 0.0F) {
        // The aspect constraint is applied LAST and by shrinking, never by growing: growing would
        // let an element exceed the maximum it just had applied.
        const f32 from_width = result.x / input.aspect_ratio;
        if (from_width <= result.y) {
            result.y = from_width;
        } else {
            result.x = result.y * input.aspect_ratio;
        }
    }
    return result;
}

struct Cursor {
    ElementStore* store = nullptr;
    ContentMeasurer* measurer = nullptr;
    LayoutReport* report = nullptr;
};

[[nodiscard]] Status measure_element(Cursor& cursor, ElementId element, Vec2 available) noexcept;

/// Measure a flex container's children and combine them into the container's desired size.
[[nodiscard]] Status measure_flex(Cursor& cursor, ElementId element, const LayoutInput& input,
                                  Vec2 inner, Vec2& content) noexcept {
    ElementStore& store = *cursor.store;
    const bool row = is_row(input.direction);
    f32 main = 0.0F;
    f32 cross = 0.0F;
    u32 counted = 0;
    for (ElementId child = store.hierarchy(element)->first_child; child.is_valid();) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        const ElementId next = node->next_sibling;
        if (!takes_part_in_layout(store, child)) {
            ++cursor.report->skipped;
            child = next;
            continue;
        }
        if (Status measured = measure_element(cursor, child, inner); !measured) {
            return measured;
        }
        const LayoutOutput* out = store.layout_output(child);
        const LayoutInput* child_input = store.layout_input(child);
        const f32 child_main = (row ? out->desired.x + child_input->margin.horizontal()
                                    : out->desired.y + child_input->margin.vertical());
        const f32 child_cross = (row ? out->desired.y + child_input->margin.vertical()
                                     : out->desired.x + child_input->margin.horizontal());
        main += child_main;
        cross = (child_cross > cross) ? child_cross : cross;
        ++counted;
        child = next;
    }
    if (counted > 1) {
        main += input.gap * static_cast<f32>(counted - 1);
    }
    content = row ? Vec2{main, cross} : Vec2{cross, main};
    return ok();
}

/// Measure a grid container: the widest cell decides a column, the tallest decides a row.
[[nodiscard]] Status measure_grid(Cursor& cursor, ElementId element, const LayoutInput& input,
                                  Vec2 inner, Vec2& content) noexcept {
    ElementStore& store = *cursor.store;
    constexpr usize kMaxTracks = 64;
    f32 column_width[kMaxTracks] = {};
    f32 row_height[kMaxTracks] = {};
    const u16 columns = (input.grid_columns == 0) ? 1U : input.grid_columns;
    u16 rows_used = 0;

    u16 auto_column = 0;
    u16 auto_row = 0;
    for (ElementId child = store.hierarchy(element)->first_child; child.is_valid();) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        const ElementId next = node->next_sibling;
        if (!takes_part_in_layout(store, child)) {
            ++cursor.report->skipped;
            child = next;
            continue;
        }
        if (Status measured = measure_element(cursor, child, inner); !measured) {
            return measured;
        }
        LayoutInput* child_input = store.layout_input(child);
        // IMPLICIT PLACEMENT: a child with no declared cell takes the next one, wrapping at the
        // column count. That is CSS's auto-placement in the subset this module documents.
        u16 column = child_input->grid_column;
        u16 row = child_input->grid_row;
        if (column == 0 && row == 0) {
            column = auto_column;
            row = auto_row;
            auto_column = static_cast<u16>(auto_column + child_input->grid_column_span);
            if (auto_column >= columns) {
                auto_column = 0;
                ++auto_row;
            }
        }
        if (column >= kMaxTracks || row >= kMaxTracks) {
            child = next;
            continue;
        }
        const LayoutOutput* out = store.layout_output(child);
        const f32 width =
            (out->desired.x + child_input->margin.horizontal()) /
            static_cast<f32>((child_input->grid_column_span == 0) ? 1U
                                                                  : child_input->grid_column_span);
        const f32 height =
            (out->desired.y + child_input->margin.vertical()) /
            static_cast<f32>((child_input->grid_row_span == 0) ? 1U : child_input->grid_row_span);
        const u16 column_span =
            std::min<u16>(child_input->grid_column_span, static_cast<u16>(kMaxTracks - column));
        for (u16 span = 0; span < column_span; ++span) {
            column_width[column + span] = std::max(column_width[column + span], width);
        }
        const u16 row_span =
            std::min<u16>(child_input->grid_row_span, static_cast<u16>(kMaxTracks - row));
        for (u16 span = 0; span < row_span; ++span) {
            row_height[row + span] = std::max(row_height[row + span], height);
            rows_used = std::max(rows_used, static_cast<u16>(row + span + 1U));
        }
        child = next;
    }

    f32 width = 0.0F;
    for (u16 column = 0; column < columns && column < kMaxTracks; ++column) {
        width += column_width[column];
    }
    f32 height = 0.0F;
    for (u16 row = 0; row < rows_used && row < kMaxTracks; ++row) {
        height += row_height[row];
    }
    if (columns > 1) {
        width += input.gap * static_cast<f32>(columns - 1);
    }
    if (rows_used > 1) {
        height += input.gap * static_cast<f32>(rows_used - 1);
    }
    content = Vec2{width, height};
    return ok();
}

Status measure_element(Cursor& cursor, ElementId element, Vec2 available) noexcept {
    ElementStore& store = *cursor.store;
    const LayoutInput* input_ptr = store.layout_input(element);
    LayoutOutput* output = store.layout_output(element);
    if (input_ptr == nullptr || output == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const LayoutInput input = *input_ptr;
    ++cursor.report->measured;
    const Hierarchy* node = store.hierarchy(element);
    if (node != nullptr && node->depth > cursor.report->deepest) {
        cursor.report->deepest = node->depth;
    }

    const Vec2 inner{(available.x < 0.0F)
                         ? -1.0F
                         : (available.x - input.padding.horizontal() - input.margin.horizontal()),
                     (available.y < 0.0F)
                         ? -1.0F
                         : (available.y - input.padding.vertical() - input.margin.vertical())};

    Vec2 content{0.0F, 0.0F};
    if (node != nullptr && node->child_count > 0) {
        switch (input.model) {
            case LayoutModel::Flex:
                if (Status measured = measure_flex(cursor, element, input, inner, content);
                    !measured) {
                    return measured;
                }
                break;
            case LayoutModel::Grid:
                if (Status measured = measure_grid(cursor, element, input, inner, content);
                    !measured) {
                    return measured;
                }
                break;
            case LayoutModel::Absolute:
                // An absolute container's children do not contribute to its desired size: they are
                // positioned against its rect, which is the point of the model.
                for (ElementId child = node->first_child; child.is_valid();) {
                    const Hierarchy* child_node = store.hierarchy(child);
                    if (child_node == nullptr) {
                        break;
                    }
                    const ElementId next = child_node->next_sibling;
                    if (takes_part_in_layout(store, child)) {
                        if (Status measured = measure_element(cursor, child, inner); !measured) {
                            return measured;
                        }
                    }
                    child = next;
                }
                break;
        }
    } else if (cursor.measurer != nullptr) {
        // A LEAF ASKS ITS CONTENT. A label's desired size is the text server's answer, and this
        // module does not know what a font is.
        content = cursor.measurer->measure_content(element, inner);
    }

    Vec2 desired = content;
    desired.x += input.padding.horizontal();
    desired.y += input.padding.vertical();
    if (input.preferred.x >= 0.0F) {
        desired.x = input.preferred.x;
    }
    if (input.preferred.y >= 0.0F) {
        desired.y = input.preferred.y;
    }
    output->desired = constrain(input, desired);
    store.clear_dirty(element, Dirty::Measure);
    return ok();
}

/// Place one child inside the space a container gave it, honouring alignment and margins.
[[nodiscard]] Rect place(const LayoutInput& input, Align align, const Rect& slot,
                         Vec2 desired) noexcept {
    Rect rect;
    const f32 available_width = slot.width - input.margin.horizontal();
    const f32 available_height = slot.height - input.margin.vertical();
    f32 width =
        (desired.x > available_width && align == Align::Stretch) ? available_width : desired.x;
    f32 height = desired.y;
    if (align == Align::Stretch) {
        width = available_width;
        height = available_height;
    }
    const Vec2 constrained = constrain(input, Vec2{width, height});
    width = constrained.x;
    height = constrained.y;

    rect.x = slot.x + input.margin.left;
    rect.y = slot.y + input.margin.top;
    switch (align) {
        case Align::Centre:
            rect.x += (available_width - width) * 0.5F;
            rect.y += (available_height - height) * 0.5F;
            break;
        case Align::End:
            rect.x += available_width - width;
            rect.y += available_height - height;
            break;
        case Align::Start:
        case Align::Stretch:
            break;
    }
    rect.width = width;
    rect.height = height;
    return rect;
}

[[nodiscard]] Status arrange_element(Cursor& cursor, ElementId element, const Rect& rect,
                                     const Rect& clip) noexcept;

/// Distribute a flex container's main axis: grow into surplus, shrink out of deficit.
[[nodiscard]] Status arrange_flex(Cursor& cursor, ElementId element, const LayoutInput& input,
                                  const Rect& content, const Rect& clip) noexcept {
    ElementStore& store = *cursor.store;
    const bool row = is_row(input.direction);
    const f32 main_available = row ? content.width : content.height;

    constexpr usize kMaxChildren = 512;
    ElementId items[kMaxChildren];
    f32 base[kMaxChildren];
    usize count = 0;
    f32 total_base = 0.0F;
    f32 total_grow = 0.0F;
    f32 total_shrink = 0.0F;

    for (ElementId child = store.hierarchy(element)->first_child;
         child.is_valid() && count < kMaxChildren;) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        const ElementId next = node->next_sibling;
        if (!takes_part_in_layout(store, child)) {
            child = next;
            continue;
        }
        const LayoutInput* child_input = store.layout_input(child);
        const LayoutOutput* child_output = store.layout_output(child);
        // A negative basis is CSS's `auto`: the child's measured size along the main axis.
        const f32 measured = row ? child_output->desired.x : child_output->desired.y;
        const f32 basis = (child_input->flex_basis >= 0.0F) ? child_input->flex_basis : measured;
        const f32 margins = row ? child_input->margin.horizontal() : child_input->margin.vertical();
        items[count] = child;
        base[count] = basis + margins;
        total_base += base[count];
        total_grow += child_input->flex_grow;
        total_shrink += child_input->flex_shrink;
        ++count;
        child = next;
    }
    if (count == 0) {
        return ok();
    }
    const f32 gaps = input.gap * static_cast<f32>(count - 1);
    const f32 surplus = main_available - total_base - gaps;

    // GROW AND SHRINK, respecting each child's maximum: "WHEN a flex row has more width than its
    // children's minimum THEN surplus SHALL be distributed by flex grow factors, respecting each
    // child's maximum."
    for (usize index = 0; index < count; ++index) {
        const LayoutInput* child_input = store.layout_input(items[index]);
        if (surplus > 0.0F && total_grow > 0.0F) {
            base[index] += surplus * (child_input->flex_grow / total_grow);
        } else if (surplus < 0.0F && total_shrink > 0.0F) {
            base[index] += surplus * (child_input->flex_shrink / total_shrink);
        }
        const f32 limit = row ? child_input->maximum.x : child_input->maximum.y;
        const f32 floor_value = row ? child_input->minimum.x : child_input->minimum.y;
        const f32 margins = row ? child_input->margin.horizontal() : child_input->margin.vertical();
        base[index] = clampf(base[index], floor_value + margins, limit + margins);
    }

    f32 used = gaps;
    for (usize index = 0; index < count; ++index) {
        used += base[index];
    }
    const f32 free_space = main_available - used;

    // JUSTIFY: where the leftover goes. The CSS names, and the CSS behaviour within the subset.
    f32 cursor_main = row ? content.x : content.y;
    f32 spacing = input.gap;
    switch (input.justify) {
        case Justify::Start:
            break;
        case Justify::Centre:
            cursor_main += free_space * 0.5F;
            break;
        case Justify::End:
            cursor_main += free_space;
            break;
        case Justify::SpaceBetween:
            spacing += (count > 1) ? (free_space / static_cast<f32>(count - 1)) : 0.0F;
            break;
        case Justify::SpaceAround: {
            const f32 share = free_space / static_cast<f32>(count);
            cursor_main += share * 0.5F;
            spacing += share;
            break;
        }
        case Justify::SpaceEvenly: {
            const f32 share = free_space / static_cast<f32>(count + 1);
            cursor_main += share;
            spacing += share;
            break;
        }
    }

    for (usize order = 0; order < count; ++order) {
        const usize index = is_reversed(input.direction) ? (count - 1 - order) : order;
        const ElementId child = items[index];
        const LayoutInput* child_input = store.layout_input(child);
        const LayoutOutput* child_output = store.layout_output(child);
        const Align align =
            (child_input->self_align == Align::Stretch) ? input.align : child_input->self_align;

        Rect slot;
        if (row) {
            slot.x = cursor_main;
            slot.y = content.y;
            slot.width = base[index];
            slot.height = content.height;
        } else {
            slot.x = content.x;
            slot.y = cursor_main;
            slot.width = content.width;
            slot.height = base[index];
        }
        Vec2 desired = child_output->desired;
        if (row) {
            desired.x = base[index] - child_input->margin.horizontal();
            if (align == Align::Stretch) {
                desired.y = content.height - child_input->margin.vertical();
            }
        } else {
            desired.y = base[index] - child_input->margin.vertical();
            if (align == Align::Stretch) {
                desired.x = content.width - child_input->margin.horizontal();
            }
        }
        const Rect rect = place(*child_input, align, slot, desired);
        if (Status arranged = arrange_element(cursor, child, rect, clip); !arranged) {
            return arranged;
        }
        cursor_main += base[index] + spacing;
    }
    return ok();
}

[[nodiscard]] Status arrange_grid(Cursor& cursor, ElementId element, const LayoutInput& input,
                                  const Rect& content, const Rect& clip) noexcept {
    ElementStore& store = *cursor.store;
    const u16 columns = (input.grid_columns == 0) ? 1U : input.grid_columns;
    u16 rows = 0;
    for (ElementId child = store.hierarchy(element)->first_child; child.is_valid();) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        rows = static_cast<u16>(rows + 1U);
        child = node->next_sibling;
    }
    rows = static_cast<u16>((rows + columns - 1U) / columns);
    if (rows == 0) {
        return ok();
    }
    const f32 cell_width =
        (content.width - (input.gap * static_cast<f32>(columns - 1))) / static_cast<f32>(columns);
    const f32 cell_height =
        (content.height - (input.gap * static_cast<f32>(rows - 1))) / static_cast<f32>(rows);

    u16 column = 0;
    u16 row = 0;
    for (ElementId child = store.hierarchy(element)->first_child; child.is_valid();) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        const ElementId next = node->next_sibling;
        if (!takes_part_in_layout(store, child)) {
            child = next;
            continue;
        }
        const LayoutInput* child_input = store.layout_input(child);
        const u16 column_span =
            (child_input->grid_column_span == 0) ? 1U : child_input->grid_column_span;
        const u16 row_span = (child_input->grid_row_span == 0) ? 1U : child_input->grid_row_span;
        const u16 placed_column = (child_input->grid_column != 0 || child_input->grid_row != 0)
                                      ? child_input->grid_column
                                      : column;
        const u16 placed_row = (child_input->grid_column != 0 || child_input->grid_row != 0)
                                   ? child_input->grid_row
                                   : row;

        Rect slot;
        slot.x = content.x + (static_cast<f32>(placed_column) * (cell_width + input.gap));
        slot.y = content.y + (static_cast<f32>(placed_row) * (cell_height + input.gap));
        slot.width = (cell_width * static_cast<f32>(column_span)) +
                     (input.gap * static_cast<f32>(column_span - 1));
        slot.height = (cell_height * static_cast<f32>(row_span)) +
                      (input.gap * static_cast<f32>(row_span - 1));

        const Align align =
            (child_input->self_align == Align::Stretch) ? input.align : child_input->self_align;
        const Rect rect = place(*child_input, align, slot,
                                Vec2{slot.width - child_input->margin.horizontal(),
                                     slot.height - child_input->margin.vertical()});
        if (Status arranged = arrange_element(cursor, child, rect, clip); !arranged) {
            return arranged;
        }
        column = static_cast<u16>(column + column_span);
        if (column >= columns) {
            column = 0;
            ++row;
        }
        child = next;
    }
    return ok();
}

[[nodiscard]] Status arrange_absolute(Cursor& cursor, ElementId element, const Rect& content,
                                      const Rect& clip) noexcept {
    ElementStore& store = *cursor.store;
    for (ElementId child = store.hierarchy(element)->first_child; child.is_valid();) {
        const Hierarchy* node = store.hierarchy(child);
        if (node == nullptr) {
            break;
        }
        const ElementId next = node->next_sibling;
        if (!takes_part_in_layout(store, child)) {
            child = next;
            continue;
        }
        const LayoutInput* child_input = store.layout_input(child);
        const LayoutOutput* child_output = store.layout_output(child);

        // ANCHORS ARE FRACTIONS OF THE PARENT, so a minimap anchored to the bottom-right stays
        // there across a resize and an aspect change — which is the scenario, and the reason
        // anchors are not pixels.
        Rect rect;
        rect.x =
            content.x + (content.width * child_input->anchor_min.x) + child_input->offset_min.x;
        rect.y =
            content.y + (content.height * child_input->anchor_min.y) + child_input->offset_min.y;
        const f32 far_x =
            content.x + (content.width * child_input->anchor_max.x) + child_input->offset_max.x;
        const f32 far_y =
            content.y + (content.height * child_input->anchor_max.y) + child_input->offset_max.y;
        rect.width = far_x - rect.x;
        rect.height = far_y - rect.y;
        // A degenerate anchor pair — both anchors equal, no offsets — means "use the desired size
        // at this point", which is how a fixed-size element is anchored to a corner.
        if (rect.width <= 0.0F) {
            rect.width = child_output->desired.x;
        }
        if (rect.height <= 0.0F) {
            rect.height = child_output->desired.y;
        }
        const Vec2 constrained = constrain(*child_input, Vec2{rect.width, rect.height});
        rect.width = constrained.x;
        rect.height = constrained.y;

        if (Status arranged = arrange_element(cursor, child, rect, clip); !arranged) {
            return arranged;
        }
        child = next;
    }
    return ok();
}

Status arrange_element(Cursor& cursor, ElementId element, const Rect& rect,
                       const Rect& clip) noexcept {
    ElementStore& store = *cursor.store;
    const LayoutInput* input_ptr = store.layout_input(element);
    LayoutOutput* output = store.layout_output(element);
    if (input_ptr == nullptr || output == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const LayoutInput input = *input_ptr;
    ++cursor.report->arranged;

    output->rect = rect;
    // THE CLIP IS INTERSECTED DOWN THE TREE. A scroll view narrows it for its subtree and nothing
    // below can widen it again, which is what makes clip culling correct rather than approximate.
    output->clip =
        has_flag(store.flags(element), ElementFlags::ClipsChildren) ? rect.intersected(clip) : clip;

    Rect content = rect;
    content.x += input.padding.left - output->scroll.x;
    content.y += input.padding.top - output->scroll.y;
    content.width -= input.padding.horizontal();
    content.height -= input.padding.vertical();

    const Hierarchy* node = store.hierarchy(element);
    if (node != nullptr && node->child_count > 0) {
        switch (input.model) {
            case LayoutModel::Flex:
                if (Status arranged = arrange_flex(cursor, element, input, content, output->clip);
                    !arranged) {
                    return arranged;
                }
                break;
            case LayoutModel::Grid:
                if (Status arranged = arrange_grid(cursor, element, input, content, output->clip);
                    !arranged) {
                    return arranged;
                }
                break;
            case LayoutModel::Absolute:
                if (Status arranged = arrange_absolute(cursor, element, content, output->clip);
                    !arranged) {
                    return arranged;
                }
                break;
        }
    }
    store.clear_dirty(element, Dirty::Arrange);
    return ok();
}

}  // namespace

f32 resolve_scale(const ScaleSettings& settings, Vec2 output) noexcept {
    const f32 reference_x = (settings.reference.x > 0.0F) ? settings.reference.x : 1.0F;
    const f32 reference_y = (settings.reference.y > 0.0F) ? settings.reference.y : 1.0F;
    const f32 by_width = output.x / reference_x;
    const f32 by_height = output.y / reference_y;

    f32 scale = 1.0F;
    switch (settings.mode) {
        case ScaleMode::FixedPixel:
            scale = 1.0F;
            break;
        case ScaleMode::ScaleWithWidth:
            scale = by_width;
            break;
        case ScaleMode::ScaleWithHeight:
            scale = by_height;
            break;
        case ScaleMode::ScaleWithSmaller:
            scale = (by_width < by_height) ? by_width : by_height;
            break;
        case ScaleMode::ScaleWithLarger:
            scale = (by_width > by_height) ? by_width : by_height;
            break;
        case ScaleMode::Match: {
            const f32 match = clampf(settings.match, 0.0F, 1.0F);
            scale = (by_width * (1.0F - match)) + (by_height * match);
            break;
        }
    }
    // THE DPI SCALE AND THE PLAYER'S OWN SCALE ARE MULTIPLIED IN ONE PLACE. A caller that applied
    // either a second time would double it, which is the "settings applied once" rule this module
    // keeps the same way `camera-system` does.
    return scale * ((settings.dpi_scale > 0.0F) ? settings.dpi_scale : 1.0F) *
           ((settings.user_scale > 0.0F) ? settings.user_scale : 1.0F);
}

Status measure_subtree(ElementStore& store, ElementId root, Vec2 available,
                       ContentMeasurer* measurer, LayoutReport& report) noexcept {
    Cursor cursor;
    cursor.store = &store;
    cursor.measurer = measurer;
    cursor.report = &report;
    return measure_element(cursor, root, available);
}

Status arrange_subtree(ElementStore& store, ElementId root, const Rect& rect, const Rect& clip,
                       LayoutReport& report) noexcept {
    Cursor cursor;
    cursor.store = &store;
    cursor.measurer = nullptr;
    cursor.report = &report;
    return arrange_element(cursor, root, rect, clip);
}

Status layout(ElementStore& store, const ScaleSettings& settings, Vec2 viewport,
              ContentMeasurer* measurer, LayoutReport& report) noexcept {
    report = LayoutReport{};
    const f32 scale = resolve_scale(settings, viewport);
    const Vec2 available{(scale > 0.0F) ? (viewport.x / scale) : viewport.x,
                         (scale > 0.0F) ? (viewport.y / scale) : viewport.y};

    // ONLY WHAT IS DIRTY. "WHEN no UI state changes in a frame THEN no layout or paint work SHALL
    // be performed": with nothing dirty both collections come back empty and this function does two
    // scans of the dirty bits and returns. `LayoutReport::measured` is zero, which is what the
    // "idle UI costs nothing" case reads.
    Array<ElementId> pending(store.allocator());
    if (Status collected = store.collect_dirty(Dirty::Measure, pending); !collected) {
        return collected;
    }

    Cursor cursor;
    cursor.store = &store;
    cursor.measurer = measurer;
    cursor.report = &report;

    for (const ElementId element : pending.span()) {
        // The list is in depth order, so an ancestor that is also dirty has already re-measured
        // this element's subtree and cleared its bit. Re-measuring it again would be correct and
        // would cost the thing this pass exists to avoid.
        if (!has_dirty(store.dirty(element), Dirty::Measure)) {
            continue;
        }
        const Hierarchy* node = store.hierarchy(element);
        Vec2 space = available;
        if (node != nullptr && node->parent.is_valid()) {
            const LayoutOutput* parent = store.layout_output(node->parent);
            if (parent != nullptr) {
                space = Vec2{parent->rect.width, parent->rect.height};
            }
        }
        if (Status measured = measure_element(cursor, element, space); !measured) {
            return measured;
        }
    }

    if (Status collected = store.collect_dirty(Dirty::Arrange, pending); !collected) {
        return collected;
    }
    for (const ElementId element : pending.span()) {
        if (!has_dirty(store.dirty(element), Dirty::Arrange)) {
            continue;
        }
        const Hierarchy* node = store.hierarchy(element);
        const LayoutOutput* output = store.layout_output(element);
        if (node == nullptr || output == nullptr) {
            continue;
        }
        Rect rect;
        Rect clip;
        if (node->parent.is_valid()) {
            // A dirty element under a clean parent keeps the rect its parent gave it: only its own
            // subtree is re-arranged, which is "only the dirty subtree and its size-affecting
            // ancestors".
            rect = output->rect;
            const LayoutOutput* parent = store.layout_output(node->parent);
            clip = (parent != nullptr) ? parent->clip : Rect{0.0F, 0.0F, available.x, available.y};
        } else {
            rect = Rect{0.0F, 0.0F, available.x, available.y};
            clip = rect;
        }
        if (Status arranged = arrange_element(cursor, element, rect, clip); !arranged) {
            return arranged;
        }
    }
    return ok();
}

}  // namespace cy::ui
