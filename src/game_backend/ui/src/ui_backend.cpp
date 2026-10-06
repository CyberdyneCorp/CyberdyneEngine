// SPDX-License-Identifier: MIT
// The interface adapter behind ABI 1.6. See cy/game_backend/ui_backend.h.

#include <cy/abi/errors.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/ui/paint.h>

namespace cy::game_backend {
namespace {

using ui::Dirty;
using ui::ElementFlags;
using ui::ElementId;

constexpr const char* kKindNames[] = {"panel", "label", "image", "progress", "button"};

[[nodiscard]] ui::Align align_of(u32 align) noexcept {
    switch (align) {
        case CY_UI_ALIGN_START:
            return ui::Align::Start;
        case CY_UI_ALIGN_CENTRE:
            return ui::Align::Centre;
        case CY_UI_ALIGN_END:
            return ui::Align::End;
        default:
            return ui::Align::Stretch;
    }
}

/// Zero is "the default" for an axis, as `CyUiLayout` states.
[[nodiscard]] Vec2 or_default(const f32 (&value)[2], f32 fallback) noexcept {
    return Vec2{value[0] == 0.0F ? fallback : value[0], value[1] == 0.0F ? fallback : value[1]};
}

[[nodiscard]] ui::Insets insets(const f32 (&value)[4]) noexcept {
    return ui::Insets{value[0], value[1], value[2], value[3]};
}

[[nodiscard]] Vec2 vec(const f32 (&value)[2]) noexcept {
    return Vec2{value[0], value[1]};
}

[[nodiscard]] f32 shrink_of(const CyUiLayout& layout) noexcept {
    if ((layout.flags & CY_UI_LAYOUT_NO_SHRINK) != 0U) {
        return 0.0F;
    }
    return layout.flex_shrink == 0.0F ? 1.0F : layout.flex_shrink;
}

/// `CyUiLayout` as the store's `LayoutInput`, with every zero read as its default.
[[nodiscard]] ui::LayoutInput input_of(const CyUiLayout& layout) noexcept {
    ui::LayoutInput input;
    input.model = static_cast<ui::LayoutModel>(layout.model);
    input.direction = static_cast<ui::FlexDirection>(layout.direction);
    input.justify = static_cast<ui::Justify>(layout.justify);
    input.align = align_of(layout.align);
    input.self_align = align_of(layout.self_align);
    input.wrap = (layout.flags & CY_UI_LAYOUT_WRAP) != 0U;
    input.gap = layout.gap;
    input.preferred = or_default(layout.preferred, -1.0F);
    input.minimum = vec(layout.minimum);
    input.maximum = or_default(layout.maximum, 1e9F);
    input.margin = insets(layout.margin);
    input.padding = insets(layout.padding);
    input.flex_grow = layout.flex_grow;
    input.flex_shrink = shrink_of(layout);
    input.aspect_ratio = layout.aspect_ratio;
    input.anchor_min = vec(layout.anchor_min);
    input.anchor_max = vec(layout.anchor_max);
    input.offset_min = vec(layout.offset_min);
    input.offset_max = vec(layout.offset_max);
    input.grid_column = layout.grid_column;
    input.grid_row = layout.grid_row;
    input.grid_column_span = layout.grid_column_span == 0U ? 1U : layout.grid_column_span;
    input.grid_row_span = layout.grid_row_span == 0U ? 1U : layout.grid_row_span;
    input.grid_columns = layout.grid_columns;
    return input;
}

[[nodiscard]] ElementFlags with_visibility(ElementFlags flags, CyUiVisibility visibility) noexcept {
    const ElementFlags rest = without_flag(flags, ElementFlags::Visible | ElementFlags::Collapsed);
    switch (visibility) {
        case CY_UI_VISIBLE:
            return rest | ElementFlags::Visible;
        case CY_UI_COLLAPSED:
            return rest | ElementFlags::Collapsed;
        case CY_UI_HIDDEN:
            break;
    }
    return rest;
}

[[nodiscard]] CyResult refuse(const char* message) noexcept {
    return abi::report(CY_RESULT_INVALID_ARGUMENT, message);
}

}  // namespace

UiAdapter::UiAdapter(Allocator& allocator, ui::ElementStore& store, ui::TextPainter& text,
                     ElementId root) noexcept
    : store_(store),
      text_(text),
      root_(root),
      interaction_(allocator),
      records_(allocator),
      events_(allocator) {}

Status UiAdapter::start() noexcept {
    if (!store_.alive(root_)) {
        return fail(ErrorCode::InvalidArgument, "ui adapter: the root is not an element");
    }
    if (Status ignored = store_.set_hit_test_mode(root_, ui::HitTestMode::Ignore); !ignored) {
        return ignored;
    }
    ui::Layer layer;
    layer.name = Name::intern("abi-interface");
    layer.kind = ui::LayerKind::Hud;
    layer.behaviour.captures_focus = false;
    layer.behaviour.blocks_input = false;
    layer.behaviour.handles_back = false;
    layer.root = root_;
    return interaction_.push_layer(layer);
}

// --- The embedder's frame ----------------------------------------------------------------------

Status UiAdapter::layout(const ui::ScaleSettings& settings, Vec2 viewport) noexcept {
    ui::LayoutReport report{};
    return ui::layout(store_, settings, viewport, &text_, report);
}

Status UiAdapter::route_pointer(Vec2 position, u32 pressed, u32 released) noexcept {
    // Where the pointer IS, before routing: a captured press receives the release wherever it
    // lands, and a click is a release over the button that was pressed, not merely the capture
    // ending.
    const ui::RouteResult under = interaction_.probe(store_, position);
    pointer_over_ = under.target.is_valid();
    ui::PointerEvent event;
    event.position = position;
    event.pressed = (pressed & CY_INPUT_BUTTON_LEFT) != 0U;
    event.released = (released & CY_INPUT_BUTTON_LEFT) != 0U;
    if (Expected<ui::RouteResult, Error> routed = interaction_.route_pointer(store_, event);
        !routed.has_value()) {
        return make_unexpected(routed.error());
    }
    interaction_.clear_events();

    const Record* over = owned_ancestor(under.target);
    const Record* button = (over != nullptr && over->kind == CY_UI_BUTTON) ? over : nullptr;
    if (event.pressed) {
        pressed_ = button != nullptr ? button->element : ui::kNoElement;
    }
    note_focus();
    if (event.released) {
        if (button != nullptr && button->element == pressed_) {
            ++clicks_;
            push_event(CY_UI_EVENT_CLICK, *button, position, CY_INPUT_BUTTON_LEFT);
        }
        pressed_ = ui::kNoElement;
    }
    return ok();
}

CyUiElement UiAdapter::find(std::string_view name) const noexcept {
    for (const Record& entry : records_.span()) {
        if (store_.alive(entry.element) && store_.type_of(entry.element).text() == name) {
            return to_abi(entry.element);
        }
    }
    return CY_UI_ELEMENT_NULL;
}

// --- Records -------------------------------------------------------------------------------------

UiAdapter::Record* UiAdapter::record(CyUiElement element) noexcept {
    const ElementId id = element_of(element);
    if (id.index < records_.size() && records_[id.index].element == id && store_.alive(id)) {
        return &records_[id.index];
    }
    (void)abi::report(CY_RESULT_NOT_FOUND,
                      "no such interface element: stale, or not created by this module");
    return nullptr;
}

const UiAdapter::Record* UiAdapter::lookup(ElementId element) const noexcept {
    if (!element.is_valid() || element.index >= records_.size()) {
        return nullptr;
    }
    const Record& entry = records_[element.index];
    return entry.element == element ? &entry : nullptr;
}

const UiAdapter::Record* UiAdapter::owned_ancestor(ElementId element) const noexcept {
    for (ElementId at = element; at.is_valid() && at != root_;) {
        if (const Record* found = lookup(at); found != nullptr) {
            return found;
        }
        const ui::Hierarchy* node = store_.hierarchy(at);
        at = node != nullptr ? node->parent : ui::kNoElement;
    }
    return nullptr;
}

Status UiAdapter::remember(const Record& made) noexcept {
    if (made.element.index >= records_.size()) {
        if (Status grown = records_.resize(made.element.index + 1U); !grown) {
            return grown;
        }
    }
    records_[made.element.index] = made;
    ++live_;
    return ok();
}

void UiAdapter::forget_dead() noexcept {
    for (Record& entry : records_.span()) {
        if (entry.element.is_valid() && !store_.alive(entry.element)) {
            text_.clear_text(entry.element);
            entry = Record{};
            --live_;
        }
    }
    if (pressed_.is_valid() && !store_.alive(pressed_)) {
        pressed_ = ui::kNoElement;
    }
}

void UiAdapter::note_focus() noexcept {
    const ElementId now = interaction_.focus();
    if (now == focus_) {
        return;
    }
    if (const Record* lost = lookup(focus_); lost != nullptr) {
        push_event(CY_UI_EVENT_BLUR, *lost, Vec2{}, 0);
    }
    if (const Record* gained = lookup(now); gained != nullptr) {
        push_event(CY_UI_EVENT_FOCUS, *gained, Vec2{}, 0);
    }
    focus_ = now;
}

void UiAdapter::push_event(CyUiEventKind kind, const Record& target, Vec2 position,
                           u32 button) noexcept {
    CyUiEvent event{};
    event.struct_size = sizeof(CyUiEvent);
    event.kind = kind;
    event.element = to_abi(target.element);
    event.owner = target.owner;
    event.position[0] = position.x;
    event.position[1] = position.y;
    event.button = button;
    // An event that cannot be queued is dropped rather than failing the frame that routed it.
    (void)events_.push_back(event);
}

// --- UiBackend -----------------------------------------------------------------------------------

CyResult UiAdapter::create(CyUiElement parent, const CyUiElementDesc& desc,
                           CyUiElement& out) noexcept {
    const ElementId under = element_of(parent);
    if (under != root_ && record(parent) == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    const char* name =
        (desc.name != nullptr && desc.name[0] != '\0') ? desc.name : kKindNames[desc.kind];
    Expected<ElementId, Error> made = store_.create(under, Name::intern(name));
    if (!made.has_value()) {
        return abi::report(made.error());
    }
    Record entry;
    entry.element = *made;
    entry.kind = static_cast<CyUiKind>(desc.kind);
    entry.owner = desc.owner;
    if (entry.kind == CY_UI_PROGRESS) {
        // A track whose fill is a child anchored over the fraction — the shape the C++ HUD's health
        // bar has, so the two lay out alike. The fill starts empty.
        store_.layout_input(entry.element)->model = ui::LayoutModel::Absolute;
        Expected<ElementId, Error> fill =
            store_.create(entry.element, Name::intern("progress-fill"));
        if (!fill.has_value()) {
            (void)store_.destroy(entry.element);
            return abi::report(fill.error());
        }
        entry.fill = *fill;
    } else if (entry.kind == CY_UI_IMAGE) {
        ui::PaintData& paint = *store_.paint(entry.element);
        paint.material = ui::material_index(ui::BuiltinMaterial::Image);
        paint.uv = ui::Rect{0.0F, 0.0F, 1.0F, 1.0F};
    } else if (entry.kind == CY_UI_BUTTON) {
        (void)store_.set_flags(entry.element, ElementFlags::Visible | ElementFlags::Focusable);
    }
    if (Status kept = remember(entry); !kept) {
        (void)store_.destroy(entry.element);
        return abi::report(kept.error());
    }
    out = to_abi(entry.element);
    return CY_RESULT_OK;
}

CyResult UiAdapter::destroy(CyUiElement element) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    if (Status gone = store_.destroy(entry->element); !gone) {
        return abi::report(gone.error());
    }
    forget_dead();
    note_focus();
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_layout(CyUiElement element, const CyUiLayout& layout) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    ui::LayoutInput input = input_of(layout);
    if (entry->kind == CY_UI_PROGRESS) {
        input.model = ui::LayoutModel::Absolute;  // its fill is placed by anchors, always
    }
    *store_.layout_input(entry->element) = input;
    store_.mark(entry->element, Dirty::Measure | Dirty::Arrange);
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_style(CyUiElement element, const CyUiStyle& style) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    ui::PaintData& paint = *store_.paint(entry->element);
    paint.background = style.background;
    paint.border_colour = style.border_colour;
    paint.border_width = style.border_width;
    paint.corner_radius = style.corner_radius;
    if (entry->fill.is_valid()) {
        ui::PaintData& fill = *store_.paint(entry->fill);
        fill.background = style.accent;
        fill.corner_radius = style.corner_radius;
        store_.mark(entry->fill, Dirty::Paint);
    }
    const ElementFlags flags = store_.flags(entry->element);
    const ElementFlags clipped = (style.flags & CY_UI_STYLE_CLIP_CHILDREN) != 0U
                                     ? flags | ElementFlags::ClipsChildren
                                     : without_flag(flags, ElementFlags::ClipsChildren);
    if (clipped != flags) {
        (void)store_.set_flags(entry->element, clipped);
    }
    store_.mark(entry->element, Dirty::Paint);
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_text(CyUiElement element, const char* utf8, u32 colour,
                             u32 pixel_scale) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    if (entry->kind != CY_UI_LABEL && entry->kind != CY_UI_BUTTON) {
        return refuse("ui_set_text: only a label or a button has text");
    }
    ui::TextStyle style;
    style.colour = colour;
    style.pixel_scale = pixel_scale;
    if (Status set = text_.set_text(entry->element, utf8, style); !set) {
        return abi::report(set.error());
    }
    // A different string may be a different width: measure again, and repaint.
    store_.mark(entry->element, Dirty::Measure | Dirty::Paint);
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_image(CyUiElement element, u32 atlas_page, const f32 (&uv)[4]) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    if (entry->kind != CY_UI_IMAGE) {
        return refuse("ui_set_image: only an image has an atlas page");
    }
    if (atlas_page > 0xFFFFU) {
        return refuse("ui_set_image: atlas pages are 16-bit");
    }
    ui::PaintData& paint = *store_.paint(entry->element);
    paint.atlas = static_cast<u16>(atlas_page);
    paint.uv = ui::Rect{uv[0], uv[1], uv[2], uv[3]};
    store_.mark(entry->element, Dirty::Paint);
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_progress(CyUiElement element, f32 value) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    if (entry->kind != CY_UI_PROGRESS) {
        return refuse("ui_set_progress: only a progress bar has a fraction");
    }
    ui::LayoutInput& fill = *store_.layout_input(entry->fill);
    fill.anchor_min = Vec2{0.0F, 0.0F};
    fill.anchor_max = Vec2{value, 1.0F};
    fill.offset_min = Vec2{0.0F, 0.0F};
    fill.offset_max = Vec2{0.0F, 0.0F};
    // The TRACK, not the fill: an element is placed by its parent's arrange, and a dirty element
    // under a clean parent keeps the rect it had (layout.cpp), so marking only the fill would leave
    // it drawn at the old fraction.
    store_.mark(entry->element, Dirty::Arrange);
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_visibility(CyUiElement element, CyUiVisibility visibility) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    const ElementFlags flags = store_.flags(entry->element);
    const ElementFlags wanted = with_visibility(flags, visibility);
    if (wanted != flags) {
        (void)store_.set_flags(entry->element, wanted);
    }
    return CY_RESULT_OK;
}

CyResult UiAdapter::set_opacity(CyUiElement element, f32 opacity) noexcept {
    Record* entry = record(element);
    if (entry == nullptr) {
        return CY_RESULT_NOT_FOUND;
    }
    store_.paint(entry->element)->opacity = opacity;
    store_.mark(entry->element, Dirty::Paint);
    return CY_RESULT_OK;
}

CyResult UiAdapter::rect(CyUiElement element, f32 (&out)[4]) const noexcept {
    const Record* entry = lookup(element_of(element));
    if (entry == nullptr || !store_.alive(entry->element)) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "no such interface element: stale, or not created by this module");
    }
    const ui::Rect& placed = store_.layout_output(entry->element)->rect;
    out[0] = placed.x;
    out[1] = placed.y;
    out[2] = placed.width;
    out[3] = placed.height;
    return CY_RESULT_OK;
}

CyUiElement UiAdapter::hit_test(f32 x, f32 y) const noexcept {
    const ui::RouteResult under = interaction_.probe(store_, Vec2{x, y});
    const Record* owned = owned_ancestor(under.target);
    return owned != nullptr ? to_abi(owned->element) : CY_UI_ELEMENT_NULL;
}

CyUiElement UiAdapter::focus() const noexcept {
    const Record* owned = lookup(interaction_.focus());
    return owned != nullptr ? to_abi(owned->element) : CY_UI_ELEMENT_NULL;
}

CyResult UiAdapter::set_focus(CyUiElement element) noexcept {
    ElementId target = ui::kNoElement;
    if (element != CY_UI_ELEMENT_NULL) {
        Record* entry = record(element);
        if (entry == nullptr) {
            return CY_RESULT_NOT_FOUND;
        }
        target = entry->element;
    }
    if (Status focused = interaction_.set_focus(store_, target); !focused) {
        return refuse("ui_set_focus: this element does not take focus");
    }
    note_focus();
    return CY_RESULT_OK;
}

}  // namespace cy::game_backend
