// SPDX-License-Identifier: MIT
// The toolkit's private records, shared by its four sources. Not installed.
#pragma once

#include <cy/ui/toolkit/toolkit.h>

namespace cy::ui {

/// A layout input for a container of `model`, in the direction a flex one runs.
[[nodiscard]] inline LayoutInput container(LayoutModel model,
                                           FlexDirection direction = FlexDirection::Row) noexcept {
    LayoutInput input;
    input.model = model;
    input.direction = direction;
    return input;
}

/// Placed by anchors in an absolute parent.
inline void anchor(LayoutInput& input, Vec2 anchor_min, Vec2 anchor_max, Vec2 offset_min,
                   Vec2 offset_max) noexcept {
    input.anchor_min = anchor_min;
    input.anchor_max = anchor_max;
    input.offset_min = offset_min;
    input.offset_max = offset_max;
}

/// What a transition or an animation drives on an element, as a bit in a mask.
enum class Channel : u8 {
    Opacity = 0,
    TranslateX,
    TranslateY,
    Scale,
    Background,
    Border,
    /// The text's colour, `color` in a sheet.
    Text,
    Count,
};

[[nodiscard]] constexpr u8 channel_bit(Channel channel) noexcept {
    return static_cast<u8>(1U << static_cast<u32>(channel));
}

[[nodiscard]] constexpr bool is_colour_channel(Channel channel) noexcept {
    return channel == Channel::Background || channel == Channel::Border ||
           channel == Channel::Text;
}

/// The values a mask of channels carries: one slot per channel.
struct ChannelValues {
    f32 numbers[static_cast<u32>(Channel::Count)] = {};
    u32 colours[static_cast<u32>(Channel::Count)] = {};
    u8 mask = 0;

    void set(Channel channel, f32 number, u32 colour) noexcept {
        numbers[static_cast<u32>(channel)] = number;
        colours[static_cast<u32>(channel)] = colour;
        mask = static_cast<u8>(mask | channel_bit(channel));
    }
    void clear(Channel channel) noexcept {
        mask = static_cast<u8>(mask & ~channel_bit(channel));
    }
    [[nodiscard]] bool has(Channel channel) const noexcept {
        return (mask & channel_bit(channel)) != 0U;
    }
};

/// Every element the toolkit made: what code and the document said (the base), what the cascade
/// made of it (the styled), and what transitions and animations lay over that.
struct Toolkit::Part {
    ElementId element;
    /// The widget slot the element belongs to.
    u32 widget = kNone;
    LayoutInput base_layout;
    PaintData base_paint;
    /// The cascade's last answer, before transitions and animations.
    PaintData styled;
    u32 styled_text = 0xFFFFFFFFU;
    f32 styled_font_size = 0.0F;
    bool styled_once = false;
    /// Whether the element carries text in the painter, and the style it was last set with.
    bool has_text = false;
    TextStyle text_style;
    /// The text's size when no `font-size` reaches it: a rich text run's own `[size=N]`.
    f32 base_text_size = 0.0F;
    /// What the element's children inherit: `color` and `font-size`, when anything set them.
    bool inherits_colour = false;
    u32 colour = 0xFFFFFFFFU;
    bool inherits_size = false;
    f32 font_size = 0.0F;
    /// The element's transition, from its cascade.
    f32 transition_duration = 0.0F;
    f32 transition_delay = 0.0F;
    Easing transition_easing = Easing::Ease;
    ChannelValues transition;
    ChannelValues animated;
    /// A container blocks the pointer only while it draws a background: an invisible row lets
    /// clicks through to the world, a visible panel does not.
    bool hit_follows_background = false;
    /// The frame the element was last restyled in, so one restyle covers it once.
    u32 restyled_frame = 0;
    /// `display: none` from the cascade, which collapses the element.
    bool styled_hidden = false;
    /// A rich text run's colour spans: `Widget::spans[first_span, first_span + span_count)`,
    /// applied again whenever the run's text is set, because setting text clears them.
    u32 first_span = 0;
    u32 span_count = 0;
};

/// One running transition or animation.
struct Toolkit::Track {
    ElementId element;
    Channel channel = Channel::Opacity;
    /// A transition between two cascaded values, which ends by giving the property back to the
    /// cascade; an animation leaves its last value standing.
    bool transition = false;
    bool decorative = false;
    f32 elapsed = 0.0F;
    f32 delay = 0.0F;
    f32 duration = 0.0F;
    Easing easing = Easing::Linear;
    f32 from = 0.0F;
    f32 to = 0.0F;
    u32 from_colour = 0;
    u32 to_colour = 0;
};

/// One binding of a widget's property to a source.
struct Toolkit::BoundWidget {
    WidgetId widget;
    WidgetProperty property = WidgetProperty::Value;
    BindingSource* source = nullptr;
    BindingMode mode = BindingMode::OneWay;
    BoundValue last;
    bool primed = false;
    /// A change the source made while the player was editing, applied when the editing ends.
    bool pending = false;
};

/// One widget: its elements and its state.
struct Toolkit::Widget {
    explicit Widget(Allocator& allocator) noexcept
        : inline_style(allocator),
          strings(allocator),
          item_ends(allocator),
          editor(allocator),
          caption(allocator),
          tooltip(allocator),
          rows(allocator),
          spans(allocator) {}

    ElementId element;
    /// Where children are attached: the root for most, a scroll view's content, a dialog's body,
    /// the tabs' pages.
    ElementId content;
    ElementId parts[kMaxParts];
    u32 part_count = 0;
    WidgetId parent;
    /// The widget a made one belongs to: a radio's group, a tab's tabs, an option's drop-down, a
    /// row's list.
    WidgetId owner;
    WidgetKind kind = WidgetKind::Panel;
    bool alive = false;
    Name id;
    Name classes[kMaxClasses];
    u32 class_count = 0;
    Array<StyleDeclaration> inline_style;

    f32 value = 0.0F;
    f32 minimum = 0.0F;
    f32 maximum = 1.0F;
    f32 step = 0.0F;
    u32 selected = 0;
    /// A made widget's place in its owner: a radio's, a tab's, an option's or a row's item.
    u32 index = 0;
    bool checked = false;
    bool open = false;

    /// Items, packed: `strings[item_ends[i - 1], item_ends[i])`.
    Array<char> strings;
    Array<u32> item_ends;
    const ListSource* source = nullptr;

    TextEditor editor;
    Array<char> caption;
    Array<char> tooltip;
    f32 scroll = 0.0F;
    f32 row_height = 22.0F;
    /// A list's pooled rows (widgets), and whether their items must be asked for again.
    Array<WidgetId> rows;
    bool rows_dirty = true;
    /// The text field's horizontal scroll, so the caret stays inside it.
    f32 text_scroll = 0.0F;
    /// A rich text's colour spans, by run.
    Array<ColourSpan> spans;
    /// The store flags the cascade last saw, so a hover or a press restyles the widget once.
    ElementFlags seen_flags = ElementFlags::None;
};

/// The id-less, class-less part of a part's style subject, filled from the store and its widget.
[[nodiscard]] PseudoState states_of(ElementFlags flags) noexcept;

}  // namespace cy::ui
