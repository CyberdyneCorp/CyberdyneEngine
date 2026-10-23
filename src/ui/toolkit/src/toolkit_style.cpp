// SPDX-License-Identifier: MIT
// The toolkit's styles: the cascade per element, transitions between cascaded values, and the
// animations a game starts. See toolkit.h.

#include "toolkit_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::ui {
namespace {

/// The deepest an element's ancestors are walked for a selector's descendant combinators.
constexpr u32 kMaxDepth = 24;

[[nodiscard]] bool same_paint(const PaintData& a, const PaintData& b) noexcept {
    return std::memcmp(&a, &b, sizeof(PaintData)) == 0;
}

[[nodiscard]] bool same_layout(const LayoutInput& a, const LayoutInput& b) noexcept {
    return std::memcmp(&a, &b, sizeof(LayoutInput)) == 0;
}

[[nodiscard]] bool same_text(const TextStyle& a, const TextStyle& b) noexcept {
    return std::memcmp(&a, &b, sizeof(TextStyle)) == 0;
}

/// A channel's number in a paint, or its colour.
[[nodiscard]] f32 number_of(const PaintData& paint, Channel channel) noexcept {
    switch (channel) {
        case Channel::Opacity:
            return paint.opacity;
        case Channel::TranslateX:
            return paint.translate.x;
        case Channel::TranslateY:
            return paint.translate.y;
        case Channel::Scale:
            return paint.scale;
        default:
            return 0.0F;
    }
}

[[nodiscard]] u32 colour_of(const PaintData& paint, Channel channel) noexcept {
    switch (channel) {
        case Channel::Background:
            return paint.background;
        case Channel::Border:
            return paint.border_colour;
        default:
            return 0U;
    }
}

/// Lay a mask of channel values over a paint.
void overlay(const ChannelValues& values, PaintData& paint) noexcept {
    const auto number = [&values](Channel channel) noexcept {
        return values.numbers[static_cast<u32>(channel)];
    };
    const auto colour = [&values](Channel channel) noexcept {
        return values.colours[static_cast<u32>(channel)];
    };
    if (values.has(Channel::Opacity)) {
        paint.opacity = number(Channel::Opacity);
    }
    if (values.has(Channel::TranslateX)) {
        paint.translate.x = number(Channel::TranslateX);
    }
    if (values.has(Channel::TranslateY)) {
        paint.translate.y = number(Channel::TranslateY);
    }
    if (values.has(Channel::Scale)) {
        paint.scale = number(Channel::Scale);
    }
    if (values.has(Channel::Background)) {
        paint.background = colour(Channel::Background);
    }
    if (values.has(Channel::Border)) {
        paint.border_colour = colour(Channel::Border);
    }
}

[[nodiscard]] Channel channel_of(AnimatedProperty property) noexcept {
    switch (property) {
        case AnimatedProperty::Opacity:
            return Channel::Opacity;
        case AnimatedProperty::TranslateX:
            return Channel::TranslateX;
        case AnimatedProperty::TranslateY:
            return Channel::TranslateY;
        case AnimatedProperty::Scale:
            return Channel::Scale;
        case AnimatedProperty::Background:
        case AnimatedProperty::Count:
            break;
    }
    return Channel::Background;
}

}  // namespace

// --- Sheets --------------------------------------------------------------------------------------

Status Toolkit::set_stylesheet(std::string_view source) noexcept {
    StyleSheet parsed(*allocator_);
    if (Status ok_parse = parse_stylesheet(source, parsed); !ok_parse) {
        return ok_parse;
    }
    sheet_ = std::move(parsed);
    restyle_all_ = true;
    return ok();
}

Status Toolkit::set_theme(std::string_view source) noexcept {
    StyleSheet parsed(*allocator_);
    if (Status ok_parse =
            parse_stylesheet(source.empty() ? default_theme_source() : source, parsed);
        !ok_parse) {
        return ok_parse;
    }
    theme_sheet_ = std::move(parsed);
    theme_.name = Name::intern(source.empty() ? "default" : "custom");
    theme_.sheet = &theme_sheet_;
    restyle_all_ = true;
    return ok();
}

// --- State ---------------------------------------------------------------------------------------

void Toolkit::sync_states() noexcept {
    // A HOVER OR A PRESS RESTYLES ITS WIDGET, ONCE: `Interaction` writes the store's flags, and the
    // cascade reads them through the widget, so a change in the flags it reads is what dirties.
    constexpr ElementFlags kRead = ElementFlags::Hovered | ElementFlags::Pressed |
                                   ElementFlags::Focused | ElementFlags::Disabled |
                                   ElementFlags::Checked;
    for (Widget& widget : widgets_.span()) {
        if (!widget.alive) {
            continue;
        }
        const ElementFlags seen = store_.flags(widget.element) & kRead;
        if (seen != widget.seen_flags) {
            widget.seen_flags = seen;
            mark_widget_style(widget);
        }
    }
}

// --- The cascade ---------------------------------------------------------------------------------

Status Toolkit::restyle_text(ElementId element, const TextStyle& style) noexcept {
    Part* part = part_record(element);
    if (part == nullptr) {
        return ok();
    }
    const std::string_view current = text_.text_of(element);
    scratch_.clear();
    if (Status copied = scratch_.append(Span<const char>(current.data(), current.size()));
        !copied) {
        return copied;
    }
    if (Status set = text_.set_text(element, std::string_view(scratch_.data(), scratch_.size()),
                                    style);
        !set) {
        return set;
    }
    part = part_record(element);
    part->text_style = style;
    if (part->span_count > 0 && part->widget != kNone) {
        const Widget& owner = widgets_[part->widget];
        if (Status coloured = text_.set_colours(
                element, owner.spans.span().subspan(part->first_span, part->span_count));
            !coloured) {
            return coloured;
        }
    }
    return ok();
}

Status Toolkit::apply_paint(ElementId element) noexcept {
    Part* part = part_record(element);
    PaintData* current = store_.paint(element);
    if (part == nullptr || current == nullptr) {
        return ok();
    }
    PaintData final = part->styled;
    overlay(part->transition, final);
    overlay(part->animated, final);
    if (!same_paint(final, *current)) {
        *current = final;
        store_.mark(element, Dirty::Paint);
    }
    if (part->has_text) {
        TextStyle style = part->text_style;
        style.colour = part->transition.has(Channel::Text)
                           ? part->transition.colours[static_cast<u32>(Channel::Text)]
                           : part->styled_text;
        style.size = part->inherits_size ? part->styled_font_size : part->base_text_size;
        if (!same_text(style, part->text_style)) {
            const bool resized = style.size != part->text_style.size;
            if (Status set = restyle_text(element, style); !set) {
                return set;
            }
            store_.mark(element, resized ? (Dirty::Measure | Dirty::Paint) : Dirty::Paint);
        }
    }
    return ok();
}

Status Toolkit::start_transition(Part& part, u32 property, f32 from, f32 to, u32 from_c,
                                 u32 to_c) noexcept {
    const auto channel = static_cast<Channel>(property);
    // A transition already running on the channel is replaced, from where it had got to: a button
    // left before its hover finished fades back from the colour it reached, not from the target.
    for (usize index = tracks_.size(); index > 0; --index) {
        const Track& track = tracks_[index - 1];
        if (track.element == part.element && track.channel == channel && track.transition) {
            tracks_.remove_unordered(index - 1);
        }
    }
    Track track;
    track.element = part.element;
    track.channel = channel;
    track.transition = true;
    track.decorative = false;
    track.delay = part.transition_delay;
    track.duration = part.transition_duration;
    track.easing = part.transition_easing;
    track.from = from;
    track.to = to;
    track.from_colour = from_c;
    track.to_colour = to_c;
    part.transition.set(channel, from, from_c);
    return tracks_.push_back(track);
}

Status Toolkit::restyle_element(ElementId element) noexcept {
    Part* part = part_record(element);
    if (part == nullptr) {
        return ok();
    }
    part->restyled_frame = style_frame_;

    // THE SUBJECT AND ITS ANCESTORS, nearest first. A part's states are its widget's: hovering a
    // slider hovers its thumb, so `slider-thumb:hover` is the selector an author writes.
    StyleSubject subjects[kMaxDepth];
    const StyleSubject* chain[kMaxDepth] = {};
    u32 depth = 0;
    for (ElementId walk = element; walk.is_valid() && depth < kMaxDepth; ++depth) {
        StyleSubject& subject = subjects[depth];
        subject.type = store_.type_of(walk);
        ElementFlags flags = store_.flags(walk);
        if (const Part* walked = part_record(walk); walked != nullptr && walked->widget != kNone) {
            const Widget& owner = widgets_[walked->widget];
            if (owner.alive) {
                flags = store_.flags(owner.element);
                if (owner.element == walk) {
                    subject.id = owner.id;
                    subject.class_count = owner.class_count;
                    for (u32 index = 0; index < owner.class_count; ++index) {
                        subject.classes[index] = owner.classes[index];
                    }
                }
            }
        }
        subject.states = states_of(flags);
        const Hierarchy* node = store_.hierarchy(walk);
        if (node != nullptr && node->parent.is_valid()) {
            const Hierarchy* parent = store_.hierarchy(node->parent);
            if (parent != nullptr && parent->first_child == walk) {
                subject.states = subject.states | PseudoState::FirstChild;
            }
            if (parent != nullptr && parent->last_child == walk) {
                subject.states = subject.states | PseudoState::LastChild;
            }
        }
        chain[depth] = &subject;
        walk = (node != nullptr) ? node->parent : kNoElement;
    }
    subjects[0].ancestors = Span<const StyleSubject* const>(chain + 1, depth > 0 ? depth - 1 : 0);

    // WHAT THE PARENT PASSES DOWN: the nearest ancestor part's resolved `color` and `font-size`.
    ResolvedStyle inherited(*allocator_);
    for (ElementId walk = store_.hierarchy(element)->parent; walk.is_valid();) {
        if (const Part* above = part_record(walk); above != nullptr) {
            if (above->inherits_colour) {
                ResolvedProperty colour;
                colour.property = StyleProperty::Colour;
                colour.value.kind = StyleValue::Kind::Colour;
                colour.value.colour = above->colour;
                (void)inherited.set(colour);
            }
            if (above->inherits_size) {
                ResolvedProperty size;
                size.property = StyleProperty::FontSize;
                size.value.number = above->font_size;
                (void)inherited.set(size);
            }
            break;
        }
        const Hierarchy* node = store_.hierarchy(walk);
        walk = (node != nullptr) ? node->parent : kNoElement;
    }

    Span<const StyleDeclaration> inline_overrides;
    if (part->widget != kNone && widgets_[part->widget].element == element) {
        inline_overrides = widgets_[part->widget].inline_style.span();
    }
    ResolvedStyle resolved(*allocator_);
    Array<StyleDiagnostic> diagnostics(*allocator_);
    if (Status cascaded = resolve_style(sheet_, &theme_, subjects[0], inline_overrides,
                                        &inherited, resolved, diagnostics);
        !cascaded) {
        return cascaded;
    }

    LayoutInput layout = part->base_layout;
    PaintData paint = part->base_paint;
    (void)apply_style(resolved, layout, paint);
    const ResolvedProperty* colour = resolved.find(StyleProperty::Colour);
    const ResolvedProperty* size = resolved.find(StyleProperty::FontSize);
    const ResolvedProperty* duration = resolved.find(StyleProperty::TransitionDuration);
    const ResolvedProperty* delay = resolved.find(StyleProperty::TransitionDelay);
    const ResolvedProperty* easing = resolved.find(StyleProperty::TransitionTimingFunction);
    const ResolvedProperty* display = resolved.find(StyleProperty::Display);
    part->transition_duration = duration != nullptr ? duration->value.number : 0.0F;
    part->transition_delay = delay != nullptr ? delay->value.number : 0.0F;
    part->transition_easing =
        easing != nullptr ? easing_from_name(easing->value.keyword.text()) : Easing::Ease;
    const bool colour_was = part->inherits_colour;
    const u32 colour_before = part->colour;
    const f32 size_before = part->font_size;
    part->inherits_colour = colour != nullptr;
    part->colour = colour != nullptr ? colour->value.colour : 0xFFFFFFFFU;
    part->inherits_size = size != nullptr;
    part->font_size = size != nullptr ? size->value.number : 0.0F;

    // LAYOUT CHANGES AT ONCE and relays out; the store's own propagation decides how far.
    LayoutInput* current = store_.layout_input(element);
    if (!same_layout(*current, layout)) {
        *current = layout;
        store_.mark(element, Dirty::Measure | Dirty::Arrange);
    }
    const bool hidden = display != nullptr && display->value.keyword.text() == "none";
    if (hidden != part->styled_hidden) {
        part->styled_hidden = hidden;
        const ElementFlags flags = store_.flags(element);
        (void)store_.set_flags(element, hidden ? (flags | ElementFlags::Collapsed)
                                               : without_flag(flags, ElementFlags::Collapsed));
    }
    if (part->hit_follows_background) {
        (void)store_.set_hit_test_mode(element, paint.background != 0U ? HitTestMode::Block
                                                                       : HitTestMode::Ignore);
    }

    // PAINT TRANSITIONS: a cascaded paint value that moved, under a duration, is interpolated from
    // what is SHOWN — the running transition's value when there is one — to the new value.
    const u32 text_colour = part->colour;
    if (part->styled_once && part->transition_duration > 0.0F) {
        for (u32 channel = 0; channel < static_cast<u32>(Channel::Count); ++channel) {
            const auto which = static_cast<Channel>(channel);
            const bool colour_channel = is_colour_channel(which);
            const u32 old_colour =
                which == Channel::Text ? part->styled_text : colour_of(part->styled, which);
            const u32 new_colour = which == Channel::Text ? text_colour : colour_of(paint, which);
            const f32 old_number = number_of(part->styled, which);
            const f32 new_number = number_of(paint, which);
            if (colour_channel ? old_colour == new_colour : old_number == new_number) {
                continue;
            }
            const bool running = part->transition.has(which);
            const u32 from_colour = running ? part->transition.colours[channel] : old_colour;
            const f32 from_number = running ? part->transition.numbers[channel] : old_number;
            if (Status started = start_transition(*part, channel, from_number, new_number,
                                                  from_colour, new_colour);
                !started) {
                return started;
            }
            part = part_record(element);
        }
    }
    part->styled = paint;
    part->styled_text = text_colour;
    part->styled_font_size = part->font_size;
    part->styled_once = true;
    if (Status applied = apply_paint(element); !applied) {
        return applied;
    }
    part = part_record(element);
    // A CHANGE IN WHAT IS INHERITED RESTYLES THE SUBTREE: a panel's `color` reaches its labels.
    if (part != nullptr && (colour_was != part->inherits_colour || colour_before != part->colour ||
                            size_before != part->font_size)) {
        Array<ElementId> children(*allocator_);
        if (Status listed = store_.children_of(element, children); !listed) {
            return listed;
        }
        for (const ElementId child : children.span()) {
            if (Status restyled = restyle_subtree(child); !restyled) {
                return restyled;
            }
        }
    }
    return ok();
}

Status Toolkit::restyle_subtree(ElementId element) noexcept {
    // Parents before children, so every child inherits from a parent restyled this frame.
    Array<ElementId> stack(*allocator_);
    if (Status pushed = stack.push_back(element); !pushed) {
        return pushed;
    }
    Array<ElementId> children(*allocator_);
    while (!stack.empty()) {
        const ElementId next = stack[stack.size() - 1];
        stack.pop_back();
        const Part* part = part_record(next);
        if (part == nullptr || part->restyled_frame != style_frame_) {
            if (Status restyled = restyle_element(next); !restyled) {
                return restyled;
            }
        }
        if (Status listed = store_.children_of(next, children); !listed) {
            return listed;
        }
        for (usize index = children.size(); index > 0; --index) {
            if (Status pushed = stack.push_back(children[index - 1]); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status Toolkit::restyle() noexcept {
    ++style_frame_;
    if (restyle_all_) {
        restyle_all_ = false;
        restyle_.clear();
        for (const ElementId root : {root_, overlay_}) {
            if (Status restyled = restyle_subtree(root); !restyled) {
                return restyled;
            }
        }
        return ok();
    }
    if (restyle_.empty()) {
        return ok();
    }
    // Shallowest first, each once: an element whose ancestor was restyled in this pass and passed
    // its inheritance down is already done.
    Array<ElementId> pending = std::move(restyle_);
    restyle_ = Array<ElementId>(*allocator_);
    std::sort(pending.begin(), pending.end(), [this](ElementId a, ElementId b) noexcept {
        const Hierarchy* left = store_.hierarchy(a);
        const Hierarchy* right = store_.hierarchy(b);
        const u16 left_depth = left != nullptr ? left->depth : 0U;
        const u16 right_depth = right != nullptr ? right->depth : 0U;
        return left_depth < right_depth;
    });
    for (const ElementId element : pending.span()) {
        const Part* part = part_record(element);
        if (part == nullptr || part->restyled_frame == style_frame_) {
            continue;
        }
        if (Status restyled = restyle_element(element); !restyled) {
            return restyled;
        }
    }
    return ok();
}

// --- Tracks --------------------------------------------------------------------------------------

Status Toolkit::advance_tracks(f32 seconds) noexcept {
    Array<ElementId> touched(*allocator_);
    for (usize index = 0; index < tracks_.size();) {
        Track& track = tracks_[index];
        Part* part = part_record(track.element);
        if (part == nullptr) {
            tracks_.remove_unordered(index);
            continue;
        }
        track.elapsed += seconds;
        if (track.decorative && settings_.reduced_motion) {
            // REDUCED MOTION: a decorative animation is over the moment it starts.
            track.elapsed = track.delay + track.duration;
        }
        const f32 running = track.elapsed - track.delay;
        const f32 t = track.duration > 0.0F ? running / track.duration : (running >= 0.0F ? 1.0F
                                                                                          : 0.0F);
        const f32 eased = ease(track.easing, t);
        const f32 number = track.from + ((track.to - track.from) * eased);
        const u32 colour = mix_colour(track.from_colour, track.to_colour, eased);
        ChannelValues& layer = track.transition ? part->transition : part->animated;
        layer.set(track.channel, number, colour);
        if (Status pushed = touched.push_back(track.element); !pushed) {
            return pushed;
        }
        if (t >= 1.0F) {
            // A finished transition hands the property back to the cascade; a finished animation
            // leaves its last value standing until another changes it.
            if (track.transition) {
                part->transition.clear(track.channel);
            }
            tracks_.remove_unordered(index);
            continue;
        }
        ++index;
    }
    for (const ElementId element : touched.span()) {
        if (Status applied = apply_paint(element); !applied) {
            return applied;
        }
    }
    return ok();
}

Status Toolkit::animate(WidgetId widget_id, const Animation& animation) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (animation.property >= AnimatedProperty::Count || !std::isfinite(animation.duration) ||
        animation.duration < 0.0F || !std::isfinite(animation.delay) || animation.delay < 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "toolkit: an animation names a property and a finite, non-negative time");
    }
    const Channel channel = animation.property == AnimatedProperty::Background
                                ? Channel::Background
                                : channel_of(animation.property);
    for (usize index = tracks_.size(); index > 0; --index) {
        const Track& track = tracks_[index - 1];
        if (track.element == widget_id && track.channel == channel && !track.transition) {
            tracks_.remove_unordered(index - 1);
        }
    }
    Track track;
    track.element = widget_id;
    track.channel = channel;
    track.decorative = animation.decorative;
    track.delay = animation.delay;
    track.duration = animation.duration;
    track.easing = animation.easing;
    track.from = animation.from;
    track.to = animation.to;
    track.from_colour = animation.from_colour;
    track.to_colour = animation.to_colour;
    if (Status pushed = tracks_.push_back(track); !pushed) {
        return pushed;
    }
    // From its first frame — through its delay too — the element shows where it starts: a
    // staggered entrance's later items wait unseen rather than flashing in and back out.
    part_record(widget_id)->animated.set(channel, animation.from, animation.from_colour);
    return apply_paint(widget_id);
}

Status Toolkit::animate_staggered(Span<const WidgetId> widgets, const Animation& animation,
                                  f32 step) noexcept {
    Animation each = animation;
    for (usize index = 0; index < widgets.size(); ++index) {
        each.delay = animation.delay + (step * static_cast<f32>(index));
        if (Status started = animate(widgets[index], each); !started) {
            return started;
        }
    }
    return ok();
}

Status Toolkit::fade(WidgetId widget, f32 from, f32 to, f32 seconds) noexcept {
    Animation animation;
    animation.property = AnimatedProperty::Opacity;
    animation.from = from;
    animation.to = to;
    animation.duration = seconds;
    animation.easing = Easing::EaseOut;
    return animate(widget, animation);
}

Status Toolkit::slide(WidgetId widget, Vec2 from, f32 seconds) noexcept {
    Animation animation;
    animation.duration = seconds;
    animation.easing = Easing::EaseOut;
    animation.to = 0.0F;
    animation.property = AnimatedProperty::TranslateX;
    animation.from = from.x;
    if (Status started = animate(widget, animation); !started) {
        return started;
    }
    animation.property = AnimatedProperty::TranslateY;
    animation.from = from.y;
    return animate(widget, animation);
}

Status Toolkit::pop(WidgetId widget, f32 seconds) noexcept {
    Animation animation;
    animation.duration = seconds;
    animation.property = AnimatedProperty::Scale;
    animation.from = 0.92F;
    animation.to = 1.0F;
    animation.easing = Easing::Back;
    if (Status started = animate(widget, animation); !started) {
        return started;
    }
    return fade(widget, 0.0F, 1.0F, seconds);
}

void Toolkit::stop_animations(WidgetId widget) noexcept {
    for (usize index = tracks_.size(); index > 0; --index) {
        const Track& track = tracks_[index - 1];
        if (track.element == widget && !track.transition) {
            tracks_.remove_unordered(index - 1);
        }
    }
}

u32 Toolkit::running_animations() const noexcept {
    u32 count = 0;
    for (const Track& track : tracks_.span()) {
        count += track.transition ? 0U : 1U;
    }
    return count;
}

}  // namespace cy::ui
