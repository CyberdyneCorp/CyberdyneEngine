// SPDX-License-Identifier: MIT
// The toolkit's input: the pointer, semantic actions, text, focus, selection, and the layers a
// drop-down and a dialog open. See toolkit.h.

#include "toolkit_internal.h"

#include <algorithm>
#include <cmath>

namespace cy::ui {
namespace {

/// The slider's thumb radius: its track is inset by it, so the thumb never leaves the slider.
constexpr f32 kThumbInset = 7.0F;

[[nodiscard]] bool is_navigation(UiAction action) noexcept {
    return action == UiAction::NavigateUp || action == UiAction::NavigateDown ||
           action == UiAction::NavigateLeft || action == UiAction::NavigateRight;
}

[[nodiscard]] bool contains_element(const ElementStore& store, ElementId ancestor,
                                    ElementId element) noexcept {
    for (ElementId walk = element; walk.is_valid();) {
        if (walk == ancestor) {
            return true;
        }
        const Hierarchy* node = store.hierarchy(walk);
        if (node == nullptr) {
            return false;
        }
        walk = node->parent;
    }
    return false;
}

/// Whether a kind does something when it is clicked or accepted.
[[nodiscard]] bool activates(WidgetKind kind) noexcept {
    switch (kind) {
        case WidgetKind::Button:
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
        case WidgetKind::Radio:
        case WidgetKind::Tab:
        case WidgetKind::Option:
        case WidgetKind::ListRow:
        case WidgetKind::Dropdown:
        case WidgetKind::TextField:
        case WidgetKind::ListView:
        case WidgetKind::Slider:
            return true;
        default:
            return false;
    }
}

}  // namespace

Toolkit::Widget* Toolkit::interactive_widget(ElementId element) noexcept {
    for (ElementId walk = element; walk.is_valid();) {
        Widget* widget = record(widget_of(walk));
        if (widget != nullptr && activates(widget->kind)) {
            return widget;
        }
        const Hierarchy* node = store_.hierarchy(walk);
        if (node == nullptr) {
            return nullptr;
        }
        walk = node->parent;
    }
    return nullptr;
}

// --- Focus ---------------------------------------------------------------------------------------

WidgetId Toolkit::focused() const noexcept {
    const ElementId focus = interaction_.focus();
    return record(focus) != nullptr ? focus : kNoElement;
}

Status Toolkit::focus(WidgetId widget_id) noexcept {
    if (record(widget_id) == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    // A widget that is not drawn — on a tab page not shown, under a collapsed panel — is not a
    // place focus may go, whatever its own flags say.
    for (ElementId walk = widget_id; walk.is_valid();) {
        const ElementFlags flags = store_.flags(walk);
        if (!has_flag(flags, ElementFlags::Visible) || has_flag(flags, ElementFlags::Collapsed)) {
            return fail(ErrorCode::InvalidArgument, "toolkit: that widget is not shown");
        }
        walk = store_.hierarchy(walk)->parent;
    }
    if (Status set = interaction_.set_focus(store_, widget_id); !set) {
        return set;
    }
    focus_visible_ = true;
    note_focus();
    scroll_into_view(widget_id);
    return ok();
}

void Toolkit::note_focus() noexcept {
    const ElementId current = interaction_.focus();
    if (current == last_focus_) {
        return;
    }
    // THE STORE'S `Focused` FLAG IS WHAT `:focus` READS, and `Interaction` keeps focus without
    // touching flags, so the toolkit moves it — and raises the events — in one place.
    if (Widget* lost = record(last_focus_); lost != nullptr) {
        (void)store_.set_flags(lost->element,
                               without_flag(store_.flags(lost->element), ElementFlags::Focused));
        push_event(WidgetEventKind::FocusLost, *lost);
        mark_widget_style(*lost);
        if (lost->kind == WidgetKind::TextField) {
            // Leaving a field ends its composition: the platform's IME is switched off with it.
            (void)lost->editor.compose({}, 0);
            (void)refresh_text_field(*lost);
        }
    }
    last_focus_ = current;
    caret_seconds_ = 0.0F;
    if (Widget* gained = record(current); gained != nullptr) {
        (void)store_.set_flags(gained->element,
                               store_.flags(gained->element) | ElementFlags::Focused);
        push_event(WidgetEventKind::FocusGained, *gained);
        mark_widget_style(*gained);
        if (gained->kind == WidgetKind::TextField) {
            (void)refresh_text_field(*gained);
        }
    }
}

void Toolkit::scroll_into_view(ElementId element) noexcept {
    const LayoutOutput* target = store_.layout_output(element);
    if (target == nullptr) {
        return;
    }
    for (ElementId walk = element; walk.is_valid();) {
        const Hierarchy* node = store_.hierarchy(walk);
        if (node == nullptr) {
            return;
        }
        Widget* owner = record(widget_of(walk));
        if (owner != nullptr && owner->kind == WidgetKind::ScrollView && walk != owner->element &&
            owner->element != element) {
            const LayoutOutput* port = store_.layout_output(owner->parts[0]);
            if (port == nullptr) {
                return;
            }
            // In the content's own coordinates: the viewport's top is `scroll` down it.
            const f32 top = target->rect.y - port->rect.y;
            const f32 bottom = top + target->rect.height;
            f32 wanted = owner->scroll;
            if (top < 0.0F) {
                wanted += top;
            } else if (bottom > port->rect.height) {
                wanted += bottom - port->rect.height;
            }
            (void)set_scroll(owner->element, wanted);
            return;
        }
        walk = node->parent;
    }
}

// --- Selection and values ------------------------------------------------------------------------

Status Toolkit::user_changed(Widget& widget) noexcept {
    push_event(WidgetEventKind::Changed, widget);
    for (const BoundWidget& binding : bindings_.span()) {
        if (binding.widget == widget.element && binding.mode == BindingMode::TwoWay) {
            if (Status written = write_back(widget, binding.property); !written) {
                return written;
            }
        }
    }
    return ok();
}

Status Toolkit::select(Widget& widget, u32 index, bool by_player) noexcept {
    const WidgetId id = widget.element;
    switch (widget.kind) {
        case WidgetKind::RadioGroup:
        case WidgetKind::Tabs: {
            const auto count = static_cast<u32>(widget.rows.size());
            widget.selected = (count == 0) ? 0U : std::min(index, count - 1);
            for (usize row = 0; row < widget.rows.size(); ++row) {
                Widget* item = record(record(id)->rows[row]);
                if (item != nullptr) {
                    item->checked = (row == record(id)->selected);
                    if (Status refreshed = update_parts(*item); !refreshed) {
                        return refreshed;
                    }
                }
            }
            if (widget.kind == WidgetKind::Tabs) {
                // ONLY THE SELECTED PAGE TAKES PART IN LAYOUT: the others are collapsed, so a
                // hidden page costs nothing and nothing on it is a place focus can go.
                Array<WidgetId> pages(*allocator_);
                if (Status listed = children(id, pages); !listed) {
                    return listed;
                }
                for (const WidgetId page : pages.span()) {
                    const Widget* page_widget = record(page);
                    if (Status shown =
                            set_visible(page, page_widget->index == record(id)->selected);
                        !shown) {
                        return shown;
                    }
                }
            }
            break;
        }
        case WidgetKind::Dropdown: {
            const u32 count = item_count(id);
            widget.selected = (count == 0) ? 0U : std::min(index, count - 1);
            if (Status refreshed = update_parts(widget); !refreshed) {
                return refreshed;
            }
            break;
        }
        case WidgetKind::ListView: {
            const u32 count = item_count(id);
            widget.selected = (count == 0) ? 0U : std::min(index, count - 1);
            widget.rows_dirty = true;
            break;
        }
        default:
            return ok();
    }
    Widget* selected = record(id);
    return by_player ? user_changed(*selected) : ok();
}

Status Toolkit::set_slider_from(Widget& widget, f32 x) noexcept {
    const LayoutOutput* output = store_.layout_output(widget.element);
    if (output == nullptr || output->rect.width <= 2.0F * kThumbInset) {
        return ok();
    }
    const f32 fraction =
        std::clamp((x - output->rect.x - kThumbInset) / (output->rect.width - (2.0F * kThumbInset)),
                   0.0F, 1.0F);
    f32 value = widget.minimum + (fraction * (widget.maximum - widget.minimum));
    if (widget.step > 0.0F) {
        value = widget.minimum + (std::round((value - widget.minimum) / widget.step) * widget.step);
        value = std::clamp(value, widget.minimum, widget.maximum);
    }
    if (value == widget.value) {
        return ok();
    }
    widget.value = value;
    if (Status refreshed = update_parts(widget); !refreshed) {
        return refreshed;
    }
    return user_changed(widget);
}

// --- Activation ----------------------------------------------------------------------------------

bool Toolkit::activate(Widget& widget) noexcept {
    if (has_flag(store_.flags(widget.element), ElementFlags::Disabled)) {
        return true;
    }
    const WidgetId owner_id = widget.owner;
    switch (widget.kind) {
        case WidgetKind::Button:
            push_event(WidgetEventKind::Clicked, widget);
            return true;
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
            widget.checked = !widget.checked;
            (void)update_parts(widget);
            (void)user_changed(widget);
            return true;
        case WidgetKind::Radio:
        case WidgetKind::Tab:
        case WidgetKind::ListRow: {
            Widget* owner = record(owner_id);
            if (owner != nullptr && widget.index != owner->selected) {
                (void)select(*owner, widget.index, true);
            }
            return true;
        }
        case WidgetKind::Option: {
            const u32 index = widget.index;
            Widget* owner = record(owner_id);
            if (owner != nullptr && index != owner->selected) {
                (void)select(*owner, index, true);
            }
            (void)close_dropdown();
            return true;
        }
        case WidgetKind::Dropdown:
            (void)(widget.open ? close_dropdown() : open_dropdown(widget));
            return true;
        case WidgetKind::TextField:
        case WidgetKind::ListView:
            push_event(WidgetEventKind::Submitted, widget);
            return true;
        default:
            return false;
    }
}

// --- The pointer ---------------------------------------------------------------------------------

bool Toolkit::handle_press(Widget& widget, Vec2 position) noexcept {
    switch (widget.kind) {
        case WidgetKind::Slider:
            dragging_ = true;
            (void)set_slider_from(widget, position.x);
            return true;
        case WidgetKind::TextField:
            dragging_ = true;
            (void)place_caret(widget, position.x, false);
            return true;
        case WidgetKind::ListRow: {
            // A row is not focusable; its list is, and takes the focus a press gives.
            if (const Widget* list = record(widget.owner); list != nullptr) {
                (void)interaction_.set_focus(store_, list->element);
            }
            return true;
        }
        default:
            return true;
    }
}

bool Toolkit::handle_drag(Widget& widget, Vec2 position) noexcept {
    if (!dragging_) {
        return false;
    }
    switch (widget.kind) {
        case WidgetKind::Slider:
            (void)set_slider_from(widget, position.x);
            return true;
        case WidgetKind::TextField:
            (void)place_caret(widget, position.x, true);
            return true;
        default:
            return false;
    }
}

bool Toolkit::handle_scroll(ElementId target, f32 notches) noexcept {
    for (ElementId walk = target; walk.is_valid();) {
        Widget* widget = record(widget_of(walk));
        if (widget != nullptr &&
            (widget->kind == WidgetKind::ScrollView || widget->kind == WidgetKind::ListView)) {
            (void)set_scroll(widget->element, widget->scroll + (notches * settings_.scroll_step));
            return true;
        }
        const Hierarchy* node = store_.hierarchy(walk);
        if (node == nullptr) {
            return false;
        }
        walk = node->parent;
    }
    return false;
}

bool Toolkit::pointer(const PointerInput& input) noexcept {
    PointerEvent event;
    event.position = input.position;
    event.pointer = input.pointer;
    event.pressed = input.down && !pointer_down_;
    event.released = !input.down && pointer_down_;
    event.scroll = input.scroll;
    pointer_down_ = input.down;
    pointer_position_ = input.position;

    // A PRESS OUTSIDE AN OPEN DROP-DOWN CLOSES IT, and is consumed: the click that dismissed the
    // list does not also land on whatever was beneath it.
    if (event.pressed && popup_.is_valid()) {
        const RouteResult probe = interaction_.probe(store_, input.position);
        if (!probe.target.is_valid() || !contains_element(store_, popup_, probe.target)) {
            (void)close_dropdown();
            note_focus();
            return true;
        }
    }
    Expected<RouteResult, Error> routed = interaction_.route_pointer(store_, event);
    if (!routed) {
        return false;
    }
    const RouteResult& route = *routed;
    Widget* target = interactive_widget(route.target);
    const WidgetId target_id = target != nullptr ? target->element : kNoElement;

    // The widget the pointer rests on, for its tooltip.
    const WidgetId resting = widget_of(route.target);
    if (resting != hovered_) {
        hovered_ = resting;
        hover_seconds_ = 0.0F;
    }

    if (event.pressed) {
        focus_visible_ = false;
        pressed_ = target_id;
        dragging_ = false;
        tooltip_target_ = kNoElement;
        if (target != nullptr && !has_flag(store_.flags(target_id), ElementFlags::Disabled)) {
            (void)handle_press(*target, input.position);
        }
    } else if (input.down && pressed_.is_valid()) {
        if (Widget* held = record(pressed_); held != nullptr) {
            (void)handle_drag(*held, input.position);
        }
    }
    if (event.released) {
        // A CLICK is a press and a release on the same widget: a press dragged off a button and let
        // go elsewhere does nothing, as on every platform. The pressed widget holds the capture, so
        // the routed target is it wherever the pointer went; what decides is what is UNDER it.
        const RouteResult under = interaction_.probe(store_, input.position);
        const Widget* released_over = interactive_widget(under.target);
        if (pressed_.is_valid() && pressed_ == target_id && !dragging_ &&
            released_over != nullptr && released_over->element == pressed_) {
            (void)activate(*target);
        }
        pressed_ = kNoElement;
        dragging_ = false;
    }
    bool consumed = route.target.is_valid() || route.blocked_by_layer || route.captured;
    if (input.scroll.y != 0.0F) {
        consumed = handle_scroll(route.target, input.scroll.y) || consumed;
    }
    note_focus();
    return consumed;
}

// --- Actions -------------------------------------------------------------------------------------

bool Toolkit::handle_layer_cancel() noexcept {
    if (popup_.is_valid()) {
        (void)close_dropdown();
        return true;
    }
    if (!dialogs_.empty()) {
        if (Widget* dialog = record(dialogs_[dialogs_.size() - 1]); dialog != nullptr) {
            (void)close_dialog(*dialog);
            return true;
        }
    }
    return false;
}

bool Toolkit::handle_widget_action(Widget& widget, UiAction action) noexcept {
    if (action == UiAction::Accept) {
        return activate(widget);
    }
    if (widget.kind == WidgetKind::Slider &&
        (action == UiAction::NavigateLeft || action == UiAction::NavigateRight)) {
        // A FOCUSED SLIDER TAKES LEFT AND RIGHT AS ITS OWN: a step, or a hundredth of the range.
        const f32 step =
            widget.step > 0.0F ? widget.step : (widget.maximum - widget.minimum) / 100.0F;
        const f32 next =
            std::clamp(widget.value + (action == UiAction::NavigateRight ? step : -step),
                       widget.minimum, widget.maximum);
        if (next != widget.value) {
            widget.value = next;
            (void)update_parts(widget);
            (void)user_changed(widget);
        }
        return true;
    }
    if (widget.kind == WidgetKind::ListView &&
        (action == UiAction::NavigateUp || action == UiAction::NavigateDown)) {
        // A FOCUSED LIST TAKES UP AND DOWN AS SELECTION until its ends, where navigation leaves it.
        const u32 count = item_count(widget.element);
        if (count == 0) {
            return false;
        }
        const bool down = action == UiAction::NavigateDown;
        if ((down && widget.selected + 1 >= count) || (!down && widget.selected == 0)) {
            return false;
        }
        const u32 next = down ? widget.selected + 1 : widget.selected - 1;
        (void)select(widget, next, true);
        Widget* list = record(widget.element);
        const f32 top = static_cast<f32>(next) * list->row_height;
        const LayoutOutput* port = store_.layout_output(list->parts[0]);
        const f32 height = port != nullptr ? port->rect.height : 0.0F;
        if (top < list->scroll) {
            (void)set_scroll(list->element, top);
        } else if (top + list->row_height > list->scroll + height) {
            (void)set_scroll(list->element, top + list->row_height - height);
        }
        return true;
    }
    if (action == UiAction::ScrollUp || action == UiAction::ScrollDown) {
        return handle_scroll(widget.element, action == UiAction::ScrollDown ? 1.0F : -1.0F);
    }
    return false;
}

bool Toolkit::handle_tabs_action(UiAction action) noexcept {
    // The tabs the focus is in — through the widget tree, a page's parent being its tabs — or the
    // first tabs the active layer shows.
    Widget* tabs = nullptr;
    for (WidgetId walk = focused(); walk.is_valid() && tabs == nullptr;) {
        Widget* widget = record(walk);
        if (widget == nullptr) {
            break;
        }
        if (widget->kind == WidgetKind::Tabs) {
            tabs = widget;
            break;
        }
        walk = widget->owner.is_valid() ? widget->owner : widget->parent;
    }
    if (tabs == nullptr) {
        for (Widget& candidate : widgets_.span()) {
            if (candidate.alive && candidate.kind == WidgetKind::Tabs &&
                !has_flag(store_.flags(candidate.element), ElementFlags::Collapsed)) {
                tabs = &candidate;
                break;
            }
        }
    }
    if (tabs == nullptr || tabs->rows.empty()) {
        return false;
    }
    const auto count = static_cast<u32>(tabs->rows.size());
    const u32 next = (action == UiAction::NextTab) ? (tabs->selected + 1) % count
                                                   : (tabs->selected + count - 1) % count;
    (void)select(*tabs, next, true);
    return true;
}

bool Toolkit::action(UiAction action) noexcept {
    if (action == UiAction::Cancel) {
        const bool handled = handle_layer_cancel();
        note_focus();
        return handled;
    }
    if (Widget* widget = record(interaction_.focus()); widget != nullptr) {
        if (handle_widget_action(*widget, action)) {
            note_focus();
            return true;
        }
    }
    if (action == UiAction::NextTab || action == UiAction::PreviousTab) {
        return handle_tabs_action(action);
    }
    const bool had_focus = record(interaction_.focus()) != nullptr;
    if (action == UiAction::FocusNext || action == UiAction::FocusPrevious ||
        (is_navigation(action) && !had_focus)) {
        // Tab order — and the first press of a stick with nothing focused, which focuses the first
        // control of the active layer rather than doing nothing.
        const bool reverse = action == UiAction::FocusPrevious || action == UiAction::NavigateUp ||
                             action == UiAction::NavigateLeft;
        (void)interaction_.focus_next(store_, reverse && had_focus);
        focus_visible_ = true;
        note_focus();
        scroll_into_view(interaction_.focus());
        return record(interaction_.focus()) != nullptr;
    }
    if (is_navigation(action)) {
        (void)interaction_.navigate(store_, action);
        focus_visible_ = true;
        note_focus();
        scroll_into_view(interaction_.focus());
        return true;
    }
    // Anything else is the interface's only while a layer holds the input.
    return captures_input();
}

// --- Text ----------------------------------------------------------------------------------------

bool Toolkit::wants_text_input() const noexcept {
    const Widget* widget = record(interaction_.focus());
    return widget != nullptr && widget->kind == WidgetKind::TextField;
}

bool Toolkit::captures_input() const noexcept {
    return popup_.is_valid() || !dialogs_.empty() || wants_text_input();
}

bool Toolkit::text_input_rect(Rect& out) const noexcept {
    const Widget* widget = record(interaction_.focus());
    if (widget == nullptr || widget->kind != WidgetKind::TextField) {
        return false;
    }
    const LayoutOutput* caret = store_.layout_output(widget->parts[4]);
    if (caret == nullptr) {
        return false;
    }
    out = caret->rect;
    return true;
}

bool Toolkit::text_input(const TextInput& input) noexcept {
    Widget* widget = record(interaction_.focus());
    if (widget == nullptr || widget->kind != WidgetKind::TextField ||
        has_flag(store_.flags(widget->element), ElementFlags::Disabled)) {
        return false;
    }
    if (input.composing) {
        (void)widget->editor.compose(input.text, input.composition_caret);
    } else {
        (void)widget->editor.compose({}, 0);
        Expected<bool, Error> changed = widget->editor.insert(input.text);
        if (changed && *changed) {
            (void)user_changed(*widget);
        }
    }
    caret_seconds_ = 0.0F;
    (void)refresh_text_field(*widget);
    return true;
}

bool Toolkit::edit(TextCommand command, bool extend) noexcept {
    Widget* widget = record(interaction_.focus());
    if (widget == nullptr || widget->kind != WidgetKind::TextField ||
        has_flag(store_.flags(widget->element), ElementFlags::Disabled)) {
        return false;
    }
    Expected<bool, Error> changed = widget->editor.apply(command, extend, clipboard_);
    if (changed && *changed) {
        (void)user_changed(*widget);
    }
    caret_seconds_ = 0.0F;
    (void)refresh_text_field(*widget);
    return true;
}

Status Toolkit::place_caret(Widget& widget, f32 x, bool extend) noexcept {
    const LayoutOutput* output = store_.layout_output(widget.element);
    const LayoutInput* input = store_.layout_input(widget.element);
    if (output == nullptr || input == nullptr) {
        return ok();
    }
    const Part* value = part_record(widget.parts[1]);
    const std::string_view text = widget.editor.text();
    const f32 local = x - (output->rect.x + input->padding.left) + widget.text_scroll;
    // The code point boundary nearest the pointer: the first whose prefix is past the midpoint
    // between it and the one before.
    u32 best = 0;
    f32 previous_width = 0.0F;
    for (u32 offset = 0; offset < text.size();) {
        const u32 next = utf8_next(text, offset);
        const f32 width = text_.measure(text.substr(0, next), value->text_style).x;
        if (local < (previous_width + width) * 0.5F) {
            break;
        }
        best = next;
        previous_width = width;
        offset = next;
    }
    widget.editor.set_caret(best, extend);
    caret_seconds_ = 0.0F;
    return refresh_text_field(widget);
}

Status Toolkit::refresh_text_field(Widget& widget) noexcept {
    // parts: 0 selection, 1 value, 2 placeholder, 3 composition, 4 caret.
    Array<char> shown(*allocator_);
    if (Status displayed = widget.editor.displayed(shown); !displayed) {
        return displayed;
    }
    const std::string_view text(shown.data(), shown.size());
    if (Status set = set_element_text(widget.parts[1], text); !set) {
        return set;
    }
    const TextStyle style = part_record(widget.parts[1])->text_style;
    const f32 line = text_.measure("Ag", style).y;
    const auto width_to = [this, &text, &style](u32 bytes) noexcept {
        return text_.measure(text.substr(0, bytes), style).x;
    };
    const TextEditor& editor = widget.editor;
    const u32 caret_bytes = editor.caret() + (editor.composing() ? editor.composition_caret() : 0U);
    const f32 caret_x = width_to(caret_bytes);
    // KEEP THE CARET INSIDE THE FIELD: scroll the text under it rather than letting it run off.
    const LayoutOutput* output = store_.layout_output(widget.element);
    const LayoutInput* input = store_.layout_input(widget.element);
    const f32 inner = (output != nullptr && input != nullptr)
                          ? output->rect.width - input->padding.horizontal()
                          : 0.0F;
    if (inner > 0.0F) {
        if (caret_x - widget.text_scroll > inner - 2.0F) {
            widget.text_scroll = caret_x - inner + 2.0F;
        } else if (caret_x < widget.text_scroll) {
            widget.text_scroll = caret_x;
        }
    }
    const f32 origin = -widget.text_scroll;
    const f32 half = line * 0.5F;
    const auto place = [this](ElementId element, f32 left, f32 top, f32 right,
                              f32 bottom) noexcept {
        Part* part = part_record(element);
        LayoutInput next = part->base_layout;
        next.anchor_min = Vec2{0.0F, 0.5F};
        next.anchor_max = Vec2{0.0F, 0.5F};
        next.offset_min = Vec2{left, top};
        next.offset_max = Vec2{right, bottom};
        part->base_layout = next;
        mark_style(element);
    };
    const f32 text_width = width_to(static_cast<u32>(text.size()));
    place(widget.parts[1], origin, -half, origin + std::max(text_width, 1.0F), half);
    place(widget.parts[2], 0.0F, -half,
          std::max(text_.measure(text_.text_of(widget.parts[2]), style).x, 1.0F), half);
    const bool selecting = editor.has_selection() && !editor.composing();
    const f32 begin_x = width_to(editor.selection_begin());
    const f32 end_x = width_to(editor.selection_end());
    place(widget.parts[0], origin + begin_x, -half, origin + std::max(end_x, begin_x + 1.0F), half);
    const f32 composition_begin = width_to(editor.caret());
    const f32 composition_end =
        width_to(editor.caret() + static_cast<u32>(editor.composition().size()));
    place(widget.parts[3], origin + composition_begin, half - 1.0F,
          origin + std::max(composition_end, composition_begin + 1.0F), half);
    place(widget.parts[4], origin + caret_x, -half, origin + caret_x + 1.5F, half);

    const bool focused_now = interaction_.focus() == widget.element;
    const auto show = [this](ElementId element, bool on) noexcept {
        const ElementFlags flags = store_.flags(element);
        if (has_flag(flags, ElementFlags::Visible) != on) {
            (void)store_.set_flags(element, on ? (flags | ElementFlags::Visible)
                                               : without_flag(flags, ElementFlags::Visible));
        }
    };
    show(widget.parts[0], selecting);
    show(widget.parts[2], text.empty());
    show(widget.parts[3], editor.composing());
    show(widget.parts[4], focused_now);
    return ok();
}

// --- Layers: the drop-down's list and the dialog -------------------------------------------------

Status Toolkit::open(WidgetId widget_id) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (widget->open) {
        return ok();
    }
    if (widget->kind == WidgetKind::Dropdown) {
        return open_dropdown(*widget);
    }
    if (widget->kind == WidgetKind::Dialog) {
        return open_dialog(*widget);
    }
    return fail(ErrorCode::InvalidArgument, "toolkit: only a drop-down or a dialog opens");
}

Status Toolkit::close(WidgetId widget_id) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (!widget->open) {
        return ok();
    }
    if (widget->kind == WidgetKind::Dropdown) {
        return close_dropdown();
    }
    return close_dialog(*widget);
}

bool Toolkit::is_open(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr && widget->open;
}

Status Toolkit::open_dropdown(Widget& widget) noexcept {
    if (popup_.is_valid()) {
        if (Status closed = close_dropdown(); !closed) {
            return closed;
        }
    }
    const WidgetId owner_id = widget.element;
    const LayoutOutput* output = store_.layout_output(owner_id);
    const Rect rect = output != nullptr ? output->rect : Rect{};
    Expected<ElementId, Error> popup = make_element(overlay_, "dropdown-popup", kNone);
    if (!popup) {
        return make_unexpected(popup.error());
    }
    popup_ = *popup;
    popup_owner_ = owner_id;
    // Under the drop-down, as wide as it, as tall as its items: a degenerate vertical anchor pair
    // is "the desired height at this point".
    Part* popup_part = part_record(popup_);
    popup_part->base_layout = container(LayoutModel::Flex, FlexDirection::Column);
    popup_part->base_layout.anchor_min = Vec2{0.0F, 0.0F};
    popup_part->base_layout.anchor_max = Vec2{0.0F, 0.0F};
    popup_part->base_layout.offset_min = Vec2{rect.x, rect.bottom() + 2.0F};
    popup_part->base_layout.offset_max = Vec2{rect.right(), rect.bottom() + 2.0F};
    (void)store_.set_hit_test_mode(popup_, HitTestMode::Block);

    const u32 count = item_count(owner_id);
    WidgetId selected_option;
    for (u32 index = 0; index < count; ++index) {
        Array<char> caption(*allocator_);
        if (Status read = item_text(owner_id, index, caption); !read) {
            return read;
        }
        WidgetDesc option;
        option.kind = WidgetKind::Option;
        option.text = std::string_view(caption.data(), caption.size());
        Expected<WidgetId, Error> made = create(kNoElement, option);
        if (!made) {
            return make_unexpected(made.error());
        }
        if (Status moved = store_.reparent(*made, popup_); !moved) {
            return moved;
        }
        Widget* option_widget = record(*made);
        option_widget->owner = owner_id;
        option_widget->index = index;
        option_widget->checked = (index == record(owner_id)->selected);
        if (Status refreshed = update_parts(*option_widget); !refreshed) {
            return refreshed;
        }
        if (option_widget->checked) {
            selected_option = *made;
        }
    }
    Layer layer;
    layer.name = Name::intern("dropdown");
    layer.kind = LayerKind::Overlay;
    layer.behaviour = default_behaviour(LayerKind::Overlay);
    layer.behaviour.dim_beneath = 0.0F;
    layer.root = popup_;
    if (Status pushed = interaction_.push_layer(layer); !pushed) {
        return pushed;
    }
    if (selected_option.is_valid()) {
        (void)interaction_.set_focus(store_, selected_option);
    } else {
        (void)interaction_.focus_next(store_, false);
    }
    Widget* owner = record(owner_id);
    owner->open = true;
    push_event(WidgetEventKind::Opened, *owner);
    note_focus();
    return ok();
}

Status Toolkit::close_dropdown() noexcept {
    if (!popup_.is_valid()) {
        return ok();
    }
    // Destroy the options as widgets, then their list; then the layer, which puts focus back on
    // the drop-down.
    Array<ElementId> options(*allocator_);
    if (Status listed = store_.children_of(popup_, options); !listed) {
        return listed;
    }
    for (const ElementId option : options.span()) {
        if (Status destroyed = destroy(option); !destroyed) {
            return destroyed;
        }
    }
    forget(popup_);
    if (Status destroyed = store_.destroy(popup_); !destroyed) {
        return destroyed;
    }
    popup_ = kNoElement;
    if (Status popped = interaction_.pop_layer(); !popped) {
        return popped;
    }
    if (Widget* owner = record(popup_owner_); owner != nullptr) {
        owner->open = false;
        push_event(WidgetEventKind::Closed, *owner);
    }
    popup_owner_ = kNoElement;
    note_focus();
    return ok();
}

Status Toolkit::open_dialog(Widget& widget) noexcept {
    const WidgetId dialog_id = widget.element;
    const ElementId backdrop = widget.parts[0];
    (void)store_.set_flags(backdrop, without_flag(store_.flags(backdrop), ElementFlags::Collapsed));
    // A DIALOG FADES IN AS ONE: an opacity group, so its overlapping panels do not show through
    // each other while it is half there.
    (void)store_.set_flags(dialog_id, store_.flags(dialog_id) | ElementFlags::OpacityGroup);
    Layer layer;
    layer.name = Name::intern("dialog");
    layer.kind = LayerKind::Modal;
    layer.behaviour = default_behaviour(LayerKind::Modal);
    layer.root = backdrop;
    if (Status pushed = interaction_.push_layer(layer); !pushed) {
        return pushed;
    }
    if (Status pushed = dialogs_.push_back(dialog_id); !pushed) {
        return pushed;
    }
    (void)interaction_.focus_next(store_, false);
    focus_visible_ = true;
    Widget* dialog = record(dialog_id);
    dialog->open = true;
    push_event(WidgetEventKind::Opened, *dialog);
    note_focus();
    return pop(dialog_id, layer.behaviour.enter_seconds);
}

Status Toolkit::close_dialog(Widget& widget) noexcept {
    const WidgetId dialog_id = widget.element;
    if (dialogs_.empty() || dialogs_[dialogs_.size() - 1] != dialog_id) {
        return fail(ErrorCode::InvalidArgument, "toolkit: only the dialog on top closes");
    }
    dialogs_.pop_back();
    (void)store_.set_flags(widget.parts[0],
                           store_.flags(widget.parts[0]) | ElementFlags::Collapsed);
    stop_animations(dialog_id);
    if (Status popped = interaction_.pop_layer(); !popped) {
        return popped;
    }
    Widget* dialog = record(dialog_id);
    dialog->open = false;
    push_event(WidgetEventKind::Closed, *dialog);
    note_focus();
    return ok();
}

}  // namespace cy::ui
