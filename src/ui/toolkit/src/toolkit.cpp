// SPDX-License-Identifier: MIT
// The toolkit's tree: widgets made of elements, their values, and their parts. See toolkit.h.

#include "toolkit_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::ui {
namespace {

struct KindRow {
    WidgetKind kind;
    const char* name;
};

constexpr KindRow kKinds[] = {
    {WidgetKind::Panel, "panel"},
    {WidgetKind::Row, "row"},
    {WidgetKind::Column, "column"},
    {WidgetKind::Grid, "grid"},
    {WidgetKind::Stack, "stack"},
    {WidgetKind::ScrollView, "scroll-view"},
    {WidgetKind::Label, "label"},
    {WidgetKind::RichText, "rich-text"},
    {WidgetKind::Image, "image"},
    {WidgetKind::Button, "button"},
    {WidgetKind::Toggle, "toggle"},
    {WidgetKind::Checkbox, "checkbox"},
    {WidgetKind::RadioGroup, "radio-group"},
    {WidgetKind::Slider, "slider"},
    {WidgetKind::ProgressBar, "progress-bar"},
    {WidgetKind::TextField, "text-field"},
    {WidgetKind::Dropdown, "dropdown"},
    {WidgetKind::ListView, "list-view"},
    {WidgetKind::Tabs, "tabs"},
    {WidgetKind::Dialog, "dialog"},
    {WidgetKind::Radio, "radio"},
    {WidgetKind::Tab, "tab"},
    {WidgetKind::ListRow, "list-row"},
    {WidgetKind::Option, "option"},
};
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == static_cast<usize>(WidgetKind::Count),
              "every widget kind has a name");

[[nodiscard]] bool is_interactive(WidgetKind kind) noexcept {
    switch (kind) {
        case WidgetKind::Button:
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
        case WidgetKind::Slider:
        case WidgetKind::TextField:
        case WidgetKind::Dropdown:
        case WidgetKind::ListView:
        case WidgetKind::Radio:
        case WidgetKind::Tab:
        case WidgetKind::Option:
            return true;
        default:
            return false;
    }
}

/// Whether a kind keeps its items in the widget.
[[nodiscard]] bool has_items(WidgetKind kind) noexcept {
    return kind == WidgetKind::RadioGroup || kind == WidgetKind::Dropdown ||
           kind == WidgetKind::ListView;
}

[[nodiscard]] bool same_layout(const LayoutInput& a, const LayoutInput& b) noexcept {
    return a == b;
}

/// The caption part of a button, a tab or a drop-down's option.
[[nodiscard]] const char* caption_part(WidgetKind kind) noexcept {
    switch (kind) {
        case WidgetKind::Button:
            return "button-label";
        case WidgetKind::Tab:
            return "tab-label";
        default:
            return "option-label";
    }
}

/// The parts of a toggle, a checkbox or a radio: what holds the mark, the mark, the caption.
struct SwitchParts {
    const char* box;
    const char* mark;
    const char* label;
};

[[nodiscard]] SwitchParts switch_parts(WidgetKind kind) noexcept {
    switch (kind) {
        case WidgetKind::Toggle:
            return {"toggle-track", "toggle-knob", "toggle-label"};
        case WidgetKind::Checkbox:
            return {"checkbox-box", "checkbox-mark", "checkbox-label"};
        default:
            return {"radio-ring", "radio-dot", "radio-label"};
    }
}

}  // namespace

PseudoState states_of(ElementFlags flags) noexcept {
    PseudoState states = PseudoState::None;
    if (has_flag(flags, ElementFlags::Hovered)) {
        states = states | PseudoState::Hover;
    }
    if (has_flag(flags, ElementFlags::Focused)) {
        states = states | PseudoState::Focus;
    }
    if (has_flag(flags, ElementFlags::Pressed)) {
        states = states | PseudoState::Active;
    }
    if (has_flag(flags, ElementFlags::Disabled)) {
        states = states | PseudoState::Disabled;
    }
    if (has_flag(flags, ElementFlags::Checked)) {
        states = states | PseudoState::Checked;
    }
    return states;
}

const char* widget_kind_name(WidgetKind kind) noexcept {
    for (const KindRow& row : kKinds) {
        if (row.kind == kind) {
            return row.name;
        }
    }
    return "unknown";
}

WidgetKind widget_kind_from_name(std::string_view name) noexcept {
    for (const KindRow& row : kKinds) {
        if (name == row.name) {
            return row.kind;
        }
    }
    return WidgetKind::Count;
}

const char* widget_event_kind_name(WidgetEventKind kind) noexcept {
    switch (kind) {
        case WidgetEventKind::Clicked:
            return "clicked";
        case WidgetEventKind::Changed:
            return "changed";
        case WidgetEventKind::Submitted:
            return "submitted";
        case WidgetEventKind::FocusGained:
            return "focus-gained";
        case WidgetEventKind::FocusLost:
            return "focus-lost";
        case WidgetEventKind::Opened:
            return "opened";
        case WidgetEventKind::Closed:
            return "closed";
        case WidgetEventKind::Count:
            break;
    }
    return "unknown";
}

Toolkit::Toolkit(Allocator& allocator, ElementStore& store, TextPainter& text) noexcept
    : allocator_(&allocator),
      store_(store),
      text_(text),
      interaction_(allocator),
      theme_sheet_(allocator),
      sheet_(allocator),
      memory_clipboard_(allocator),
      clipboard_(&memory_clipboard_),
      widgets_(allocator),
      free_widgets_(allocator),
      part_of_(allocator),
      parts_(allocator),
      free_parts_(allocator),
      tracks_(allocator),
      bindings_(allocator),
      events_(allocator),
      restyle_(allocator),
      scratch_(allocator),
      dialogs_(allocator) {}

Toolkit::~Toolkit() = default;

// --- Records -------------------------------------------------------------------------------------

Toolkit::Part* Toolkit::part_record(ElementId element) noexcept {
    if (!store_.alive(element) || element.index >= part_of_.size()) {
        return nullptr;
    }
    const u32 slot = part_of_[element.index];
    if (slot == kNone || parts_[slot].element != element) {
        return nullptr;
    }
    return &parts_[slot];
}

const Toolkit::Part* Toolkit::part_record(ElementId element) const noexcept {
    if (!store_.alive(element) || element.index >= part_of_.size()) {
        return nullptr;
    }
    const u32 slot = part_of_[element.index];
    if (slot == kNone || parts_[slot].element != element) {
        return nullptr;
    }
    return &parts_[slot];
}

Toolkit::Widget* Toolkit::record(WidgetId widget) noexcept {
    const Part* part = part_record(widget);
    if (part == nullptr || part->widget == kNone) {
        return nullptr;
    }
    Widget& found = widgets_[part->widget];
    return (found.alive && found.element == widget) ? &found : nullptr;
}

const Toolkit::Widget* Toolkit::record(WidgetId widget) const noexcept {
    const Part* part = part_record(widget);
    if (part == nullptr || part->widget == kNone) {
        return nullptr;
    }
    const Widget& found = widgets_[part->widget];
    return (found.alive && found.element == widget) ? &found : nullptr;
}

Expected<ElementId, Error> Toolkit::make_element(ElementId parent, const char* type,
                                                 u32 widget) noexcept {
    Expected<ElementId, Error> created = store_.create(parent, Name::intern(type));
    if (!created) {
        return created;
    }
    const ElementId element = *created;
    if (element.index >= part_of_.size()) {
        const usize old = part_of_.size();
        if (Status grown = part_of_.resize(element.index + 1U); !grown) {
            return make_unexpected(grown.error());
        }
        for (usize index = old; index < part_of_.size(); ++index) {
            part_of_[index] = kNone;
        }
    }
    u32 slot = kNone;
    if (!free_parts_.empty()) {
        slot = free_parts_[free_parts_.size() - 1];
        free_parts_.pop_back();
        parts_[slot] = Part{};
    } else {
        slot = static_cast<u32>(parts_.size());
        if (Status pushed = parts_.push_back(Part{}); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    Part& part = parts_[slot];
    part.element = element;
    part.widget = widget;
    part.base_layout = *store_.layout_input(element);
    part.base_paint = *store_.paint(element);
    part.styled = part.base_paint;
    part_of_[element.index] = slot;
    (void)store_.set_hit_test_mode(element, HitTestMode::Ignore);
    mark_style(element);
    return element;
}

void Toolkit::forget(ElementId element) noexcept {
    Part* part = part_record(element);
    if (part == nullptr) {
        return;
    }
    if (part->has_text) {
        text_.clear_text(element);
    }
    for (usize index = tracks_.size(); index > 0; --index) {
        if (tracks_[index - 1].element == element) {
            tracks_.remove_unordered(index - 1);
        }
    }
    const u32 slot = part_of_[element.index];
    part_of_[element.index] = kNone;
    parts_[slot].element = kNoElement;
    (void)free_parts_.push_back(slot);
}

void Toolkit::mark_style(ElementId element) noexcept {
    (void)restyle_.push_back(element);
}

void Toolkit::mark_widget_style(const Widget& widget) noexcept {
    mark_style(widget.element);
    for (u32 index = 0; index < widget.part_count; ++index) {
        mark_style(widget.parts[index]);
    }
}

void Toolkit::push_event(WidgetEventKind kind, const Widget& widget) noexcept {
    WidgetEvent event;
    event.kind = kind;
    event.widget = widget.element;
    event.id = widget.id;
    event.value = widget.value;
    if (widget.kind == WidgetKind::Toggle || widget.kind == WidgetKind::Checkbox) {
        event.value = widget.checked ? 1.0F : 0.0F;
    }
    event.index = widget.selected;
    (void)events_.push_back(event);
}

// --- Start ---------------------------------------------------------------------------------------

Status Toolkit::start(ElementId root) noexcept {
    if (root_.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "toolkit: already started");
    }
    if (Status parsed = set_theme({}); !parsed) {
        return parsed;
    }
    if (root.is_valid()) {
        if (!store_.alive(root)) {
            return fail(ErrorCode::NotFound, "toolkit: the root is not an element");
        }
        root_ = root;
    } else {
        Expected<ElementId, Error> made = store_.create(kNoElement, Name::intern("root"));
        if (!made) {
            return make_unexpected(made.error());
        }
        root_ = *made;
        store_.layout_input(root_)->model = LayoutModel::Absolute;
        store_.mark(root_, Dirty::Measure | Dirty::Arrange);
    }
    (void)store_.set_hit_test_mode(root_, HitTestMode::Ignore);

    // THE OVERLAY: a second root, drawn after the first, holding what floats over the interface —
    // the focus ring, the tooltip, an open drop-down's list and every dialog's backdrop.
    Expected<ElementId, Error> overlay = make_element(kNoElement, "overlay", kNone);
    if (!overlay) {
        return make_unexpected(overlay.error());
    }
    overlay_ = *overlay;
    part_record(overlay_)->base_layout = container(LayoutModel::Absolute);
    Expected<ElementId, Error> ring = make_element(overlay_, "focus-ring", kNone);
    if (!ring) {
        return make_unexpected(ring.error());
    }
    focus_ring_ = *ring;
    Expected<ElementId, Error> tooltip = make_element(overlay_, "tooltip", kNone);
    if (!tooltip) {
        return make_unexpected(tooltip.error());
    }
    tooltip_ = *tooltip;
    part_record(tooltip_)->base_layout = container(LayoutModel::Flex);
    Expected<ElementId, Error> label = make_element(tooltip_, "tooltip-label", kNone);
    if (!label) {
        return make_unexpected(label.error());
    }
    tooltip_label_ = *label;
    part_record(tooltip_label_)->has_text = true;
    if (Status set = set_element_text(tooltip_label_, ""); !set) {
        return set;
    }
    for (const ElementId hidden : {focus_ring_, tooltip_}) {
        (void)store_.set_flags(hidden, without_flag(store_.flags(hidden), ElementFlags::Visible));
    }
    for (const ElementId placed : {overlay_, focus_ring_, tooltip_, tooltip_label_}) {
        *store_.layout_input(placed) = part_record(placed)->base_layout;
        store_.mark(placed, Dirty::Measure | Dirty::Arrange);
    }

    // The interface's own layer: drawn, interactive where it has a widget, and transparent to the
    // pointer everywhere else — a click on nothing is a click on the world.
    Layer layer;
    layer.name = Name::intern("toolkit");
    layer.kind = LayerKind::Hud;
    layer.behaviour = default_behaviour(LayerKind::Hud);
    layer.root = root_;
    if (Status pushed = interaction_.push_layer(layer); !pushed) {
        return pushed;
    }
    restyle_all_ = true;
    return ok();
}

// --- Creating ------------------------------------------------------------------------------------

Expected<WidgetId, Error> Toolkit::create(WidgetId parent, const WidgetDesc& desc) noexcept {
    if (!root_.is_valid()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "toolkit: not started", 0});
    }
    if (desc.kind >= WidgetKind::Count) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "toolkit: no such widget kind", 0});
    }
    const Widget* parent_widget = parent.is_valid() ? record(parent) : nullptr;
    if (parent.is_valid() && parent_widget == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "toolkit: the parent is not a widget", 0});
    }
    // Copied now: making the new widget's record may move every record, the parent's with them.
    const ElementId parent_content = parent_widget != nullptr ? parent_widget->content : root_;
    const WidgetId parent_element = parent_widget != nullptr ? parent_widget->element : kNoElement;
    u32 slot = kNone;
    if (!free_widgets_.empty()) {
        slot = free_widgets_[free_widgets_.size() - 1];
        free_widgets_.pop_back();
        widgets_[slot] = Widget(*allocator_);
    } else {
        slot = static_cast<u32>(widgets_.size());
        if (Status pushed = widgets_.push_back(Widget(*allocator_)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    // A DIALOG LIVES IN THE OVERLAY, whatever it was declared under: it is drawn over everything
    // and is the root of the modal layer it opens. Its declared parent is still its parent to
    // `find`.
    ElementId attach_to = parent_content;
    if (desc.kind == WidgetKind::Dialog) {
        Expected<ElementId, Error> backdrop = make_element(overlay_, "dialog-backdrop", slot);
        if (!backdrop) {
            return make_unexpected(backdrop.error());
        }
        attach_to = *backdrop;
    }
    Expected<ElementId, Error> element = make_element(attach_to, widget_kind_name(desc.kind), slot);
    if (!element) {
        return make_unexpected(element.error());
    }
    Widget& widget = widgets_[slot];
    widget.alive = true;
    widget.element = *element;
    widget.content = *element;
    widget.kind = desc.kind;
    widget.parent = parent_element;
    widget.value = desc.value;
    widget.minimum = desc.minimum;
    widget.maximum = desc.maximum;
    widget.step = desc.step;
    widget.checked = desc.checked;
    widget.row_height = desc.row_height > 0.0F ? desc.row_height : 22.0F;
    if (desc.kind == WidgetKind::Dialog) {
        widget.parts[widget.part_count++] = store_.hierarchy(widget.element)->parent;
    }
    if (!desc.id.empty()) {
        widget.id = Name::intern(desc.id);
    }
    ++live_;
    if (Status classes = set_classes(widget.element, desc.classes); !classes) {
        return make_unexpected(classes.error());
    }
    if (Status built = build(widgets_[slot], desc); !built) {
        return make_unexpected(built.error());
    }
    // `widget` is not read past `build`: a radio group makes its radios there, which can move every
    // record. `parent_element` is the same value, copied before anything could move it.
    if (Status attached = attach_to_parent(widgets_[slot], parent_element); !attached) {
        return make_unexpected(attached.error());
    }
    if (!desc.enabled) {
        if (Status disabled = set_enabled(widgets_[slot].element, false); !disabled) {
            return make_unexpected(disabled.error());
        }
    }
    Widget& made = widgets_[slot];
    mark_widget_style(made);
    if (Status refreshed = update_parts(made); !refreshed) {
        return make_unexpected(refreshed.error());
    }
    return widgets_[slot].element;
}

Status Toolkit::attach_to_parent(Widget& widget, WidgetId parent) noexcept {
    Widget* owner = parent.is_valid() ? record(parent) : nullptr;
    if (owner == nullptr) {
        return ok();
    }
    Part* part = part_record(widget.element);
    switch (owner->kind) {
        case WidgetKind::Stack:
            // A stack's children are layers, each filling it.
            anchor(part->base_layout, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F},
                   Vec2{0.0F, 0.0F});
            return ok();
        case WidgetKind::Tabs: {
            // EVERY CHILD OF A TABS IS A PAGE, with a tab in the bar titled by its title. Pages
            // fill the page area; only the selected one takes part in layout.
            anchor(part->base_layout, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F},
                   Vec2{0.0F, 0.0F});
            const u32 index = static_cast<u32>(owner->rows.size());
            WidgetDesc tab;
            tab.kind = WidgetKind::Tab;
            tab.text = widget.caption.empty()
                           ? std::string_view(widget.id.text())
                           : std::string_view(widget.caption.data(), widget.caption.size());
            const WidgetId owner_id = owner->element;
            const WidgetId page = widget.element;
            const ElementId bar = owner->parts[0];
            widget.index = index;
            Expected<WidgetId, Error> made = create(kNoElement, tab);
            if (!made) {
                return make_unexpected(made.error());
            }
            // `create` may have moved the widget array; look both up again.
            Widget* tab_widget = record(*made);
            if (Status moved = store_.reparent(*made, bar); !moved) {
                return moved;
            }
            tab_widget->owner = owner_id;
            tab_widget->index = index;
            Widget* tabs = record(owner_id);
            if (Status pushed = tabs->rows.push_back(*made); !pushed) {
                return pushed;
            }
            (void)page;
            return select(*tabs, tabs->selected, false);
        }
        default:
            return ok();
    }
}

Status Toolkit::build(Widget& widget, const WidgetDesc& desc) noexcept {
    const u32 slot = part_record(widget.element)->widget;
    Part* root = part_record(widget.element);
    // Every helper below may grow `parts_`; `root` and `widget` are refreshed after each one.
    const auto add = [this, slot](
                         ElementId parent, const char* type,
                         const LayoutInput& layout) noexcept -> Expected<ElementId, Error> {
        Expected<ElementId, Error> made = make_element(parent, type, slot);
        if (made) {
            part_record(*made)->base_layout = layout;
            Widget& owner = widgets_[slot];
            if (owner.part_count < kMaxParts) {
                owner.parts[owner.part_count++] = *made;
            }
        }
        return made;
    };
    const auto text_part = [this, &add](ElementId parent, const char* type,
                                        const LayoutInput& layout,
                                        std::string_view text) noexcept -> Status {
        Expected<ElementId, Error> made = add(parent, type, layout);
        if (!made) {
            return make_unexpected(made.error());
        }
        part_record(*made)->has_text = true;
        return set_element_text(*made, text);
    };
    const ElementId element = widget.element;
    const auto focusable = [this, element]() noexcept {
        (void)store_.set_flags(element, store_.flags(element) | ElementFlags::Focusable);
        (void)store_.set_hit_test_mode(element, HitTestMode::Block);
    };

    LayoutInput centred_row = container(LayoutModel::Flex, FlexDirection::Row);
    centred_row.align = Align::Centre;
    LayoutInput fill = container(LayoutModel::Absolute);
    anchor(fill, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F});

    switch (desc.kind) {
        case WidgetKind::Panel:
        case WidgetKind::Stack:
        case WidgetKind::Dialog:
            root->base_layout = container(
                desc.kind == WidgetKind::Dialog ? LayoutModel::Flex : LayoutModel::Absolute,
                FlexDirection::Column);
            root->hit_follows_background = true;
            if (desc.kind == WidgetKind::Dialog) {
                Part* backdrop = part_record(widget.parts[0]);
                backdrop->base_layout = container(LayoutModel::Flex, FlexDirection::Column);
                anchor(backdrop->base_layout, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F},
                       Vec2{0.0F, 0.0F});
                backdrop->base_layout.justify = Justify::Centre;
                backdrop->base_layout.align = Align::Centre;
                (void)store_.set_hit_test_mode(widget.parts[0], HitTestMode::Block);
                (void)store_.set_flags(widget.parts[0],
                                       store_.flags(widget.parts[0]) | ElementFlags::Collapsed);
                (void)store_.set_hit_test_mode(element, HitTestMode::Block);
                if (Status title = text_part(element, "dialog-title", LayoutInput{}, desc.title);
                    !title) {
                    return title;
                }
                Expected<ElementId, Error> body = add(
                    element, "dialog-body", container(LayoutModel::Flex, FlexDirection::Column));
                if (!body) {
                    return make_unexpected(body.error());
                }
                widgets_[slot].content = *body;
            }
            break;
        case WidgetKind::Row:
        case WidgetKind::Column:
            root->base_layout =
                container(LayoutModel::Flex, desc.kind == WidgetKind::Row ? FlexDirection::Row
                                                                          : FlexDirection::Column);
            root->hit_follows_background = true;
            break;
        case WidgetKind::Grid:
            root->base_layout = container(LayoutModel::Grid);
            root->base_layout.grid_columns = desc.columns;
            root->hit_follows_background = true;
            break;
        case WidgetKind::ScrollView: {
            root->base_layout = container(LayoutModel::Absolute);
            root->hit_follows_background = true;
            LayoutInput viewport = fill;
            Expected<ElementId, Error> port = add(element, "scroll-viewport", viewport);
            if (!port) {
                return make_unexpected(port.error());
            }
            (void)store_.set_flags(*port, store_.flags(*port) | ElementFlags::ClipsChildren);
            (void)store_.set_hit_test_mode(*port, HitTestMode::Block);
            LayoutInput content = container(LayoutModel::Flex, FlexDirection::Column);
            anchor(content, Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F});
            Expected<ElementId, Error> body = add(*port, "scroll-content", content);
            if (!body) {
                return make_unexpected(body.error());
            }
            widgets_[slot].content = *body;
            if (Expected<ElementId, Error> thumb =
                    add(element, "scroll-thumb", container(LayoutModel::Absolute));
                !thumb) {
                return make_unexpected(thumb.error());
            }
            break;
        }
        case WidgetKind::Label:
            root->has_text = true;
            if (Status set = set_element_text(element, desc.text); !set) {
                return set;
            }
            break;
        case WidgetKind::RichText:
            root->base_layout = container(LayoutModel::Flex, FlexDirection::Row);
            root->base_layout.align = Align::End;
            break;
        case WidgetKind::Image:
            root->base_paint.material = material_index(BuiltinMaterial::Image);
            root->base_paint.atlas = desc.image_page;
            root->base_paint.uv = desc.image_uv;
            root->base_paint.background = 0xFFFFFFFFU;
            if (desc.image_size.x > 0.0F && desc.image_size.y > 0.0F) {
                root->base_layout.preferred = desc.image_size;
            }
            break;
        case WidgetKind::Button:
        case WidgetKind::Tab:
        case WidgetKind::Option: {
            root->base_layout = centred_row;
            if (desc.kind != WidgetKind::Option) {
                root->base_layout.justify = Justify::Centre;
            }
            focusable();
            if (Status made = text_part(element, caption_part(desc.kind), LayoutInput{}, desc.text);
                !made) {
                return made;
            }
            break;
        }
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
        case WidgetKind::Radio: {
            root->base_layout = centred_row;
            focusable();
            const SwitchParts parts = switch_parts(desc.kind);
            const char* box = parts.box;
            const char* mark = parts.mark;
            const char* label = parts.label;
            Expected<ElementId, Error> outer = add(element, box, container(LayoutModel::Absolute));
            if (!outer) {
                return make_unexpected(outer.error());
            }
            if (Expected<ElementId, Error> inner = add(*outer, mark, fill); !inner) {
                return make_unexpected(inner.error());
            }
            if (Status made = text_part(element, label, LayoutInput{}, desc.text); !made) {
                return made;
            }
            break;
        }
        case WidgetKind::RadioGroup:
            root->base_layout = container(LayoutModel::Flex, FlexDirection::Column);
            break;
        case WidgetKind::Slider: {
            root->base_layout = container(LayoutModel::Absolute);
            focusable();
            for (const char* type : {"slider-track", "slider-fill", "slider-thumb"}) {
                if (Expected<ElementId, Error> made = add(element, type, LayoutInput{}); !made) {
                    return make_unexpected(made.error());
                }
            }
            break;
        }
        case WidgetKind::ProgressBar:
            root->base_layout = container(LayoutModel::Absolute);
            if (Expected<ElementId, Error> made = add(element, "progress-fill", LayoutInput{});
                !made) {
                return make_unexpected(made.error());
            }
            break;
        case WidgetKind::TextField: {
            root->base_layout = container(LayoutModel::Absolute);
            focusable();
            (void)store_.set_flags(element, store_.flags(element) | ElementFlags::ClipsChildren);
            Widget& field = widgets_[slot];
            field.editor.set_mask(desc.mask);
            field.editor.set_max_length(desc.max_length);
            if (Status set = field.editor.set_text(desc.text); !set) {
                return set;
            }
            if (Expected<ElementId, Error> made = add(element, "text-selection", LayoutInput{});
                !made) {
                return make_unexpected(made.error());
            }
            if (Status made = text_part(element, "text-value", LayoutInput{}, desc.text); !made) {
                return made;
            }
            if (Status made =
                    text_part(element, "text-placeholder", LayoutInput{}, desc.placeholder);
                !made) {
                return made;
            }
            for (const char* type : {"text-composition", "text-caret"}) {
                if (Expected<ElementId, Error> made = add(element, type, LayoutInput{}); !made) {
                    return make_unexpected(made.error());
                }
            }
            break;
        }
        case WidgetKind::Dropdown: {
            root->base_layout = centred_row;
            focusable();
            LayoutInput grow;
            grow.flex_grow = 1.0F;
            if (Status made = text_part(element, "dropdown-label", grow, ""); !made) {
                return made;
            }
            if (Status made = text_part(element, "dropdown-arrow", LayoutInput{}, "v"); !made) {
                return made;
            }
            break;
        }
        case WidgetKind::ListView: {
            root->base_layout = container(LayoutModel::Absolute);
            focusable();
            Expected<ElementId, Error> port = add(element, "list-viewport", fill);
            if (!port) {
                return make_unexpected(port.error());
            }
            (void)store_.set_flags(*port, store_.flags(*port) | ElementFlags::ClipsChildren);
            (void)store_.set_hit_test_mode(*port, HitTestMode::Block);
            LayoutInput content = container(LayoutModel::Absolute);
            anchor(content, Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F});
            Expected<ElementId, Error> body = add(*port, "list-content", content);
            if (!body) {
                return make_unexpected(body.error());
            }
            widgets_[slot].content = *body;
            if (Expected<ElementId, Error> thumb =
                    add(element, "list-thumb", container(LayoutModel::Absolute));
                !thumb) {
                return make_unexpected(thumb.error());
            }
            break;
        }
        case WidgetKind::ListRow: {
            root->base_layout = container(LayoutModel::Absolute);
            (void)store_.set_hit_test_mode(element, HitTestMode::Block);
            if (Status made = text_part(element, "list-row-label", LayoutInput{}, ""); !made) {
                return made;
            }
            break;
        }
        case WidgetKind::Tabs: {
            root->base_layout = container(LayoutModel::Flex, FlexDirection::Column);
            Expected<ElementId, Error> bar =
                add(element, "tab-bar", container(LayoutModel::Flex, FlexDirection::Row));
            if (!bar) {
                return make_unexpected(bar.error());
            }
            LayoutInput pages = container(LayoutModel::Absolute);
            pages.flex_grow = 1.0F;
            Expected<ElementId, Error> body = add(element, "tab-pages", pages);
            if (!body) {
                return make_unexpected(body.error());
            }
            widgets_[slot].content = *body;
            break;
        }
        case WidgetKind::Count:
            break;
    }
    // The caption: a page's or a dialog's title, or the text a captioned kind shows. Neither
    // `set_text` nor `set_items` keeps a reference into `widgets_`, which making a radio grows.
    if (!desc.title.empty()) {
        Widget& titled = widgets_[slot];
        if (Status kept =
                titled.caption.append(Span<const char>(desc.title.data(), desc.title.size()));
            !kept) {
            return kept;
        }
        if (desc.kind == WidgetKind::Dialog) {
            if (Status set = set_caption(titled, desc.title); !set) {
                return set;
            }
        }
    }
    const WidgetKind kind = desc.kind;
    if (kind == WidgetKind::Label || kind == WidgetKind::Button || kind == WidgetKind::Toggle ||
        kind == WidgetKind::Checkbox || kind == WidgetKind::RichText || kind == WidgetKind::Radio ||
        kind == WidgetKind::Tab || kind == WidgetKind::Option) {
        if (Status set = set_text(element, desc.text); !set) {
            return set;
        }
    }
    if (has_items(kind) && !desc.items.empty()) {
        if (Status set = set_items(element, desc.items); !set) {
            return set;
        }
    }
    if (is_interactive(desc.kind)) {
        (void)store_.set_hit_test_mode(element, HitTestMode::Block);
    }
    return ok();
}

// --- Destroying ----------------------------------------------------------------------------------

Status Toolkit::destroy(WidgetId widget_id) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (popup_owner_ == widget_id ||
        (popup_.is_valid() && widget->kind == WidgetKind::Dropdown && popup_owner_ == widget_id)) {
        (void)close_dropdown();
    }
    if (widget->kind == WidgetKind::Dialog && widget->open) {
        (void)close_dialog(*widget);
    }
    unbind(widget_id);
    // A TAB PAGE TAKES ITS TAB WITH IT, and the tabs after it move up one.
    if (Widget* owner = record(widget->parent);
        owner != nullptr && owner->kind == WidgetKind::Tabs) {
        for (usize index = 0; index < owner->rows.size(); ++index) {
            Widget* tab = record(owner->rows[index]);
            if (tab == nullptr || store_.hierarchy(widget_id) == nullptr) {
                continue;
            }
            // The page's position among the pages is the tab's index.
            if (tab->index == widget->index) {
                (void)destroy(owner->rows[index]);
                owner = record(widget->parent);
                owner->rows.erase(index);
                break;
            }
        }
    }
    widget = record(widget_id);
    // Everything under the widget's outermost element — a dialog's backdrop, or the root.
    const ElementId outermost =
        (widget->kind == WidgetKind::Dialog) ? widget->parts[0] : widget->element;
    Array<ElementId> doomed(*allocator_);
    Array<ElementId> stack(*allocator_);
    if (Status pushed = stack.push_back(outermost); !pushed) {
        return pushed;
    }
    while (!stack.empty()) {
        const ElementId element = stack[stack.size() - 1];
        stack.pop_back();
        if (Status pushed = doomed.push_back(element); !pushed) {
            return pushed;
        }
        Array<ElementId> children(*allocator_);
        if (Status listed = store_.children_of(element, children); !listed) {
            return listed;
        }
        if (Status appended = stack.append(children.span()); !appended) {
            return appended;
        }
    }
    for (const ElementId element : doomed.span()) {
        const Part* part = part_record(element);
        if (part != nullptr && part->widget != kNone) {
            Widget& owned = widgets_[part->widget];
            if (owned.alive && owned.element == element) {
                owned.alive = false;
                --live_;
                (void)free_widgets_.push_back(part->widget);
                for (usize index = bindings_.size(); index > 0; --index) {
                    if (bindings_[index - 1].widget == element) {
                        bindings_.remove_unordered(index - 1);
                    }
                }
            }
        }
        forget(element);
    }
    if (Status destroyed = store_.destroy(outermost); !destroyed) {
        return destroyed;
    }
    // Nothing the toolkit holds may name what is gone.
    for (WidgetId* held : {&pressed_, &hovered_, &last_focus_, &tooltip_target_, &popup_owner_}) {
        if (!store_.alive(*held)) {
            *held = kNoElement;
        }
    }
    if (interaction_.focus().is_valid() && !store_.alive(interaction_.focus())) {
        (void)interaction_.set_focus(store_, kNoElement);
    }
    return ok();
}

bool Toolkit::alive(WidgetId widget) const noexcept {
    return record(widget) != nullptr;
}

WidgetKind Toolkit::kind(WidgetId widget) const noexcept {
    const Widget* found = record(widget);
    return found != nullptr ? found->kind : WidgetKind::Count;
}

WidgetId Toolkit::parent(WidgetId widget) const noexcept {
    const Widget* found = record(widget);
    return found != nullptr ? found->parent : kNoElement;
}

Name Toolkit::id_of(WidgetId widget) const noexcept {
    const Widget* found = record(widget);
    return found != nullptr ? found->id : Name{};
}

WidgetId Toolkit::widget_of(ElementId element) const noexcept {
    const Part* part = part_record(element);
    if (part == nullptr || part->widget == kNone) {
        return kNoElement;
    }
    const Widget& widget = widgets_[part->widget];
    return widget.alive ? widget.element : kNoElement;
}

Status Toolkit::children(WidgetId widget, Array<WidgetId>& out) const noexcept {
    out.clear();
    const Widget* owner = widget.is_valid() ? record(widget) : nullptr;
    if (widget.is_valid() && owner == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    // In tree order: the widgets whose roots are the content element's children, then the dialogs
    // declared here, which live in the overlay.
    const ElementId content = (owner != nullptr) ? owner->content : root_;
    Array<ElementId> elements(*allocator_);
    if (Status listed = store_.children_of(content, elements); !listed) {
        return listed;
    }
    const WidgetId scope = (owner != nullptr) ? owner->element : kNoElement;
    for (const ElementId element : elements.span()) {
        const Widget* child = record(element);
        if (child != nullptr && child->parent == scope && !child->owner.is_valid()) {
            if (Status pushed = out.push_back(element); !pushed) {
                return pushed;
            }
        }
    }
    for (const Widget& candidate : widgets_.span()) {
        if (candidate.alive && candidate.kind == WidgetKind::Dialog && candidate.parent == scope) {
            if (Status pushed = out.push_back(candidate.element); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status Toolkit::reorder(WidgetId parent_id, Span<const WidgetId> order) noexcept {
    const Widget* owner = parent_id.is_valid() ? record(parent_id) : nullptr;
    if (parent_id.is_valid() && owner == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    const ElementId content = owner != nullptr ? owner->content : root_;
    // Re-appended in order: each one moved to the end leaves the ones before it in front.
    for (const WidgetId child : order) {
        const Hierarchy* node = store_.hierarchy(child);
        if (node == nullptr || node->parent != content) {
            continue;
        }
        if (Status moved = store_.reparent(child, content); !moved) {
            return moved;
        }
    }
    return ok();
}

WidgetId Toolkit::find(WidgetId scope, std::string_view id) const noexcept {
    if (id.empty()) {
        return kNoElement;
    }
    const Name wanted = Name::intern(id);
    const ElementId start = scope.is_valid() ? scope : root_;
    Array<ElementId> stack(*allocator_);
    if (!stack.push_back(start)) {
        return kNoElement;
    }
    // A dialog's elements live in the overlay; a search from the root reaches them through it.
    if (!scope.is_valid() && !stack.push_back(overlay_)) {
        return kNoElement;
    }
    Array<ElementId> children(*allocator_);
    while (!stack.empty()) {
        const ElementId element = stack[stack.size() - 1];
        stack.pop_back();
        const Widget* widget = record(element);
        if (widget != nullptr && widget->id == wanted) {
            return element;
        }
        if (!store_.children_of(element, children)) {
            return kNoElement;
        }
        for (usize index = children.size(); index > 0; --index) {
            if (!stack.push_back(children[index - 1])) {
                return kNoElement;
            }
        }
        // A dialog declared under this scope is searched as if it were here.
        if (widget != nullptr) {
            for (const Widget& candidate : widgets_.span()) {
                if (candidate.alive && candidate.kind == WidgetKind::Dialog &&
                    candidate.parent == element && scope.is_valid() &&
                    !stack.push_back(candidate.element)) {
                    return kNoElement;
                }
            }
        }
    }
    return kNoElement;
}

ElementId Toolkit::part(WidgetId widget, std::string_view type) const noexcept {
    const Widget* found = record(widget);
    if (found == nullptr) {
        return kNoElement;
    }
    const Name wanted = Name::intern(type);
    for (u32 index = 0; index < found->part_count; ++index) {
        if (store_.type_of(found->parts[index]) == wanted) {
            return found->parts[index];
        }
    }
    return kNoElement;
}

// --- Values --------------------------------------------------------------------------------------

Status Toolkit::set_element_text(ElementId element, std::string_view text) noexcept {
    Part* part = part_record(element);
    if (part == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such element");
    }
    part->has_text = true;
    const std::string_view previous = text_.text_of(element);
    if (previous == text && previous.data() != nullptr && !text.empty()) {
        return ok();
    }
    // Copied first: `text` may be a view into the painter's own storage, which `set_text` moves.
    scratch_.clear();
    if (Status copied = scratch_.append(Span<const char>(text.data(), text.size())); !copied) {
        return copied;
    }
    if (Status set = text_.set_text(element, std::string_view(scratch_.data(), scratch_.size()),
                                    part->text_style);
        !set) {
        return set;
    }
    store_.mark(element, Dirty::Measure | Dirty::Paint);
    return ok();
}

Status Toolkit::set_caption(Widget& widget, std::string_view text) noexcept {
    widget.caption.clear();
    if (Status kept = widget.caption.append(Span<const char>(text.data(), text.size())); !kept) {
        return kept;
    }
    switch (widget.kind) {
        case WidgetKind::Label:
            return set_element_text(widget.element, text);
        case WidgetKind::Button:
        case WidgetKind::Tab:
        case WidgetKind::Option:
            return set_element_text(widget.parts[0], text);
        case WidgetKind::Toggle:
        case WidgetKind::Checkbox:
        case WidgetKind::Radio:
            return set_element_text(widget.parts[2], text);
        case WidgetKind::Dialog:
            return set_element_text(widget.parts[1], text);
        default:
            return ok();
    }
}

Status Toolkit::set_text(WidgetId widget_id, std::string_view text) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (widget->kind == WidgetKind::TextField) {
        if (Status set = widget->editor.set_text(text); !set) {
            return set;
        }
        return refresh_text_field(*widget);
    }
    if (widget->kind == WidgetKind::RichText) {
        widget->caption.clear();
        if (Status kept = widget->caption.append(Span<const char>(text.data(), text.size()));
            !kept) {
            return kept;
        }
        return update_parts(*widget);
    }
    return set_caption(*widget, text);
}

std::string_view Toolkit::text(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return {};
    }
    if (widget->kind == WidgetKind::TextField) {
        return widget->editor.text();
    }
    return {widget->caption.data(), widget->caption.size()};
}

Status Toolkit::set_value(WidgetId widget_id, f32 value) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (!std::isfinite(value)) {
        return fail(ErrorCode::InvalidArgument, "toolkit: a value is finite");
    }
    if (widget->kind == WidgetKind::Toggle || widget->kind == WidgetKind::Checkbox) {
        return set_checked(widget_id, value >= 0.5F);
    }
    const f32 low = std::min(widget->minimum, widget->maximum);
    const f32 high = std::max(widget->minimum, widget->maximum);
    widget->value = std::clamp(value, low, high);
    return update_parts(*widget);
}

f32 Toolkit::value(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return 0.0F;
    }
    if (widget->kind == WidgetKind::Toggle || widget->kind == WidgetKind::Checkbox) {
        return widget->checked ? 1.0F : 0.0F;
    }
    return widget->value;
}

Status Toolkit::set_range(WidgetId widget_id, f32 minimum, f32 maximum, f32 step) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step) || step < 0.0F ||
        maximum < minimum) {
        return fail(ErrorCode::InvalidArgument,
                    "toolkit: a range is finite, its maximum not below its minimum, its step not "
                    "negative");
    }
    widget->minimum = minimum;
    widget->maximum = maximum;
    widget->step = step;
    return set_value(widget_id, widget->value);
}

Status Toolkit::set_checked(WidgetId widget_id, bool checked) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    widget->checked = checked;
    return update_parts(*widget);
}

bool Toolkit::checked(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr && widget->checked;
}

Status Toolkit::set_selected(WidgetId widget_id, u32 index) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    return select(*widget, index, false);
}

u32 Toolkit::selected(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr ? widget->selected : 0U;
}

Status Toolkit::set_items(WidgetId widget_id, Span<const std::string_view> items) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (!has_items(widget->kind)) {
        return fail(ErrorCode::InvalidArgument, "toolkit: this widget takes no items");
    }
    widget->strings.clear();
    widget->item_ends.clear();
    for (const std::string_view item : items) {
        if (Status kept = widget->strings.append(Span<const char>(item.data(), item.size()));
            !kept) {
            return kept;
        }
        if (Status ended = widget->item_ends.push_back(static_cast<u32>(widget->strings.size()));
            !ended) {
            return ended;
        }
    }
    widget->source = nullptr;
    return refresh_items(*widget);
}

Status Toolkit::set_list_source(WidgetId widget_id, const ListSource* source) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr || widget->kind != WidgetKind::ListView) {
        return fail(ErrorCode::InvalidArgument, "toolkit: a list source is a list view's");
    }
    widget->source = source;
    return refresh_items(*widget);
}

u32 Toolkit::item_count(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return 0;
    }
    if (widget->source != nullptr) {
        return widget->source->count();
    }
    return static_cast<u32>(widget->item_ends.size());
}

Status Toolkit::item_text(WidgetId widget_id, u32 index, Array<char>& out) const noexcept {
    const Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (widget->source != nullptr) {
        return index < widget->source->count()
                   ? widget->source->text(index, out)
                   : fail(ErrorCode::InvalidArgument, "toolkit: no such item");
    }
    if (index >= widget->item_ends.size()) {
        return fail(ErrorCode::InvalidArgument, "toolkit: no such item");
    }
    const u32 begin = (index == 0) ? 0U : widget->item_ends[index - 1];
    const u32 end = widget->item_ends[index];
    return out.append(Span<const char>(widget->strings.data() + begin, end - begin));
}

void Toolkit::refresh_items(WidgetId widget_id) noexcept {
    if (Widget* widget = record(widget_id); widget != nullptr) {
        widget->rows_dirty = true;
    }
}

Status Toolkit::refresh_items(Widget& widget) noexcept {
    const u32 count = item_count(widget.element);
    if (widget.kind == WidgetKind::RadioGroup) {
        // One radio per item, made or destroyed to match, each captioned again.
        const WidgetId group = widget.element;
        while (record(group)->rows.size() > count) {
            Widget* owner = record(group);
            const WidgetId last = owner->rows[owner->rows.size() - 1];
            owner->rows.pop_back();
            if (Status destroyed = destroy(last); !destroyed) {
                return destroyed;
            }
        }
        while (record(group)->rows.size() < count) {
            WidgetDesc radio;
            radio.kind = WidgetKind::Radio;
            Expected<WidgetId, Error> made = create(group, radio);
            if (!made) {
                return make_unexpected(made.error());
            }
            Widget* owner = record(group);
            Widget* made_widget = record(*made);
            made_widget->owner = group;
            made_widget->index = static_cast<u32>(owner->rows.size());
            if (Status pushed = owner->rows.push_back(*made); !pushed) {
                return pushed;
            }
        }
        Widget* owner = record(group);
        for (u32 index = 0; index < count; ++index) {
            scratch_.clear();
            Array<char> caption(*allocator_);
            if (Status read = item_text(group, index, caption); !read) {
                return read;
            }
            owner = record(group);
            if (Status set = set_caption(*record(owner->rows[index]),
                                         std::string_view(caption.data(), caption.size()));
                !set) {
                return set;
            }
            owner = record(group);
        }
        return select(*owner, owner->selected, false);
    }
    widget.rows_dirty = true;
    if (widget.kind == WidgetKind::ListView) {
        Part* content = part_record(widget.content);
        content->base_layout.preferred.y = static_cast<f32>(count) * widget.row_height;
        *store_.layout_input(widget.content) = content->base_layout;
        store_.mark(widget.content, Dirty::Measure | Dirty::Arrange);
    }
    return select(widget, widget.selected, false);
}

Status Toolkit::set_enabled(WidgetId widget_id, bool enabled) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    const ElementFlags flags = store_.flags(widget->element);
    if (Status set =
            store_.set_flags(widget->element, enabled ? without_flag(flags, ElementFlags::Disabled)
                                                      : (flags | ElementFlags::Disabled));
        !set) {
        return set;
    }
    if (!enabled && interaction_.focus() == widget->element) {
        (void)interaction_.set_focus(store_, kNoElement);
        note_focus();
    }
    mark_widget_style(*widget);
    return ok();
}

bool Toolkit::enabled(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr && !has_flag(store_.flags(widget->element), ElementFlags::Disabled);
}

Status Toolkit::set_visible(WidgetId widget_id, bool visible) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    const ElementFlags flags = store_.flags(widget->element);
    return store_.set_flags(widget->element, visible ? without_flag(flags, ElementFlags::Collapsed)
                                                     : (flags | ElementFlags::Collapsed));
}

bool Toolkit::visible(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr && !has_flag(store_.flags(widget->element), ElementFlags::Collapsed);
}

Status Toolkit::set_classes(WidgetId widget_id, std::string_view classes) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    widget->class_count = 0;
    usize cursor = 0;
    while (cursor < classes.size()) {
        while (cursor < classes.size() && (classes[cursor] == ' ' || classes[cursor] == '.')) {
            ++cursor;
        }
        const usize begin = cursor;
        while (cursor < classes.size() && classes[cursor] != ' ' && classes[cursor] != '.') {
            ++cursor;
        }
        if (cursor == begin) {
            continue;
        }
        if (widget->class_count >= kMaxClasses) {
            return fail(ErrorCode::InvalidArgument, "toolkit: at most four classes a widget");
        }
        widget->classes[widget->class_count++] =
            Name::intern(classes.substr(begin, cursor - begin));
    }
    mark_widget_style(*widget);
    return ok();
}

Status Toolkit::set_style(WidgetId widget_id, StyleProperty property,
                          const StyleValue& value) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    if (property >= StyleProperty::Count) {
        return fail(ErrorCode::InvalidArgument, "toolkit: no such style property");
    }
    for (StyleDeclaration& declaration : widget->inline_style.span()) {
        if (declaration.property == property) {
            declaration.value = value;
            mark_style(widget->element);
            return ok();
        }
    }
    StyleDeclaration declaration;
    declaration.property = property;
    declaration.value = value;
    if (Status pushed = widget->inline_style.push_back(declaration); !pushed) {
        return pushed;
    }
    mark_style(widget->element);
    return ok();
}

Status Toolkit::set_inline_style(WidgetId widget_id, std::string_view declarations) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    // Parsed as the body of a rule, so it is `.cyss` exactly, closed property set and all.
    Array<char> source(*allocator_);
    const std::string_view head = "_ {";
    if (Status built = source.append(Span<const char>(head.data(), head.size())); !built) {
        return built;
    }
    if (Status built = source.append(Span<const char>(declarations.data(), declarations.size()));
        !built) {
        return built;
    }
    if (Status built = source.push_back('}'); !built) {
        return built;
    }
    StyleSheet parsed(*allocator_);
    if (Status ok_parse = parse_stylesheet(std::string_view(source.data(), source.size()), parsed);
        !ok_parse) {
        return ok_parse;
    }
    if (!parsed.diagnostics().empty()) {
        return fail(ErrorCode::InvalidArgument, parsed.diagnostics()[0].message);
    }
    widget->inline_style.clear();
    for (const StyleDeclaration& declaration : parsed.declarations()) {
        if (Status pushed = widget->inline_style.push_back(declaration); !pushed) {
            return pushed;
        }
    }
    mark_style(widget->element);
    return ok();
}

Status Toolkit::set_anchors(WidgetId widget_id, Vec2 anchor_min, Vec2 anchor_max, Vec2 offset_min,
                            Vec2 offset_max) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    Part* part = part_record(widget->element);
    anchor(part->base_layout, anchor_min, anchor_max, offset_min, offset_max);
    mark_style(widget->element);
    return ok();
}

Status Toolkit::set_tooltip(WidgetId widget_id, std::string_view text) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    widget->tooltip.clear();
    return widget->tooltip.append(Span<const char>(text.data(), text.size()));
}

Status Toolkit::set_scroll(WidgetId widget_id, f32 offset) noexcept {
    Widget* widget = record(widget_id);
    if (widget == nullptr ||
        (widget->kind != WidgetKind::ScrollView && widget->kind != WidgetKind::ListView)) {
        return fail(ErrorCode::InvalidArgument, "toolkit: only a scroll view or a list scrolls");
    }
    const f32 clamped = std::clamp(offset, 0.0F, max_scroll(*widget));
    if (clamped == widget->scroll) {
        return ok();
    }
    widget->scroll = clamped;
    widget->rows_dirty = true;
    return update_parts(*widget);
}

f32 Toolkit::scroll(WidgetId widget_id) const noexcept {
    const Widget* widget = record(widget_id);
    return widget != nullptr ? widget->scroll : 0.0F;
}

Status Toolkit::set_neighbour(WidgetId from, UiAction direction, WidgetId to) noexcept {
    if (record(from) == nullptr || record(to) == nullptr) {
        return fail(ErrorCode::NotFound, "toolkit: no such widget");
    }
    return interaction_.set_neighbour(from, direction, to);
}

void Toolkit::set_clipboard(Clipboard* clipboard) noexcept {
    clipboard_ = (clipboard != nullptr) ? clipboard : &memory_clipboard_;
}

// --- Parts ---------------------------------------------------------------------------------------

namespace {

/// Set a flag on or off, through the store so it dirties what it should.
void set_flag(ElementStore& store, ElementId element, ElementFlags flag, bool on) noexcept {
    const ElementFlags flags = store.flags(element);
    if (has_flag(flags, flag) == on) {
        return;
    }
    (void)store.set_flags(element, on ? (flags | flag) : without_flag(flags, flag));
}

}  // namespace

Status Toolkit::update_parts(Widget& widget) noexcept {
    const auto place = [this](ElementId element, Vec2 anchor_min, Vec2 anchor_max, Vec2 offset_min,
                              Vec2 offset_max) noexcept {
        Part* part = part_record(element);
        if (part == nullptr) {
            return;
        }
        LayoutInput next = part->base_layout;
        anchor(next, anchor_min, anchor_max, offset_min, offset_max);
        if (same_layout(next, part->base_layout)) {
            return;
        }
        part->base_layout = next;
        mark_style(element);
    };
    const f32 range = widget.maximum - widget.minimum;
    const f32 fraction =
        (range > 0.0F) ? std::clamp((widget.value - widget.minimum) / range, 0.0F, 1.0F) : 0.0F;
    switch (widget.kind) {
        case WidgetKind::Toggle:
            set_flag(store_, widget.element, ElementFlags::Checked, widget.checked);
            // The knob sits at the end it is switched to: anchored to that edge of the track.
            place(widget.parts[1], Vec2{widget.checked ? 1.0F : 0.0F, 0.0F},
                  Vec2{widget.checked ? 1.0F : 0.0F, 1.0F},
                  Vec2{widget.checked ? -16.0F : 2.0F, 2.0F},
                  Vec2{widget.checked ? -2.0F : 16.0F, -2.0F});
            mark_widget_style(widget);
            break;
        case WidgetKind::Checkbox:
        case WidgetKind::Radio:
            set_flag(store_, widget.element, ElementFlags::Checked, widget.checked);
            set_flag(store_, widget.parts[1], ElementFlags::Visible, widget.checked);
            place(widget.parts[1], Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{4.0F, 4.0F},
                  Vec2{-4.0F, -4.0F});
            mark_widget_style(widget);
            break;
        case WidgetKind::Tab:
        case WidgetKind::Option:
        case WidgetKind::ListRow:
            set_flag(store_, widget.element, ElementFlags::Checked, widget.checked);
            mark_widget_style(widget);
            break;
        case WidgetKind::Slider:
            // A track inset by the thumb's radius, so the thumb's centre covers the whole range
            // and never leaves the slider: centre = 7 + f * (width - 14).
            place(widget.parts[0], Vec2{0.0F, 0.5F}, Vec2{1.0F, 0.5F}, Vec2{7.0F, -2.0F},
                  Vec2{-7.0F, 2.0F});
            place(widget.parts[1], Vec2{0.0F, 0.5F}, Vec2{fraction, 0.5F}, Vec2{7.0F, -2.0F},
                  Vec2{7.0F - (14.0F * fraction), 2.0F});
            place(widget.parts[2], Vec2{fraction, 0.5F}, Vec2{fraction, 0.5F},
                  Vec2{-14.0F * fraction, -7.0F}, Vec2{14.0F - (14.0F * fraction), 7.0F});
            break;
        case WidgetKind::ProgressBar:
            place(widget.parts[0], Vec2{0.0F, 0.0F}, Vec2{fraction, 1.0F}, Vec2{0.0F, 0.0F},
                  Vec2{0.0F, 0.0F});
            set_flag(store_, widget.parts[0], ElementFlags::Visible, fraction > 0.0F);
            break;
        case WidgetKind::ScrollView: {
            LayoutOutput* port = store_.layout_output(widget.parts[0]);
            if (port != nullptr && port->scroll.y != widget.scroll) {
                port->scroll.y = widget.scroll;
                store_.mark(widget.parts[0], Dirty::Arrange);
            }
            break;
        }
        case WidgetKind::ListView: {
            LayoutOutput* port = store_.layout_output(widget.parts[0]);
            if (port != nullptr && port->scroll.y != widget.scroll) {
                port->scroll.y = widget.scroll;
                store_.mark(widget.parts[0], Dirty::Arrange);
            }
            widget.rows_dirty = true;
            break;
        }
        case WidgetKind::RichText:
            return build_rich_text(widget);
        case WidgetKind::TextField:
            return refresh_text_field(widget);
        case WidgetKind::Dropdown: {
            Array<char> caption(*allocator_);
            if (widget.selected < item_count(widget.element)) {
                if (Status read = item_text(widget.element, widget.selected, caption); !read) {
                    return read;
                }
            }
            return set_element_text(widget.parts[0],
                                    std::string_view(caption.data(), caption.size()));
        }
        default:
            break;
    }
    return ok();
}

}  // namespace cy::ui
