// SPDX-License-Identifier: MIT
// The widget toolkit behind ABI 1.12. See cy/game_backend/ui_backend.h.

#include <cy/abi/errors.h>
#include <cy/game_backend/ui_backend.h>

#include <cstdio>
#include <cstring>

namespace cy::game_backend {
namespace {

constexpr f32 kDefaultRowHeight = 22.0F;
/// The most items one `ui_widget_set_items` or description passes on the stack's behalf.
constexpr u32 kMaxItems = 4096;

[[nodiscard]] std::string_view view(const char* text) noexcept {
    return text != nullptr ? std::string_view(text) : std::string_view();
}

[[nodiscard]] bool has_selection(ui::WidgetKind kind) noexcept {
    return kind == ui::WidgetKind::RadioGroup || kind == ui::WidgetKind::Dropdown ||
           kind == ui::WidgetKind::ListView || kind == ui::WidgetKind::Tabs;
}

[[nodiscard]] bool is_switch(ui::WidgetKind kind) noexcept {
    return kind == ui::WidgetKind::Toggle || kind == ui::WidgetKind::Checkbox;
}

[[nodiscard]] CyUiEventKind event_kind(ui::WidgetEventKind kind) noexcept {
    switch (kind) {
        case ui::WidgetEventKind::Clicked:
            return CY_UI_EVENT_CLICK;
        case ui::WidgetEventKind::Changed:
            return CY_UI_EVENT_CHANGED;
        case ui::WidgetEventKind::Submitted:
            return CY_UI_EVENT_SUBMITTED;
        case ui::WidgetEventKind::FocusGained:
            return CY_UI_EVENT_FOCUS;
        case ui::WidgetEventKind::FocusLost:
            return CY_UI_EVENT_BLUR;
        case ui::WidgetEventKind::Opened:
            return CY_UI_EVENT_OPENED;
        case ui::WidgetEventKind::Closed:
        case ui::WidgetEventKind::Count:
            break;
    }
    return CY_UI_EVENT_CLOSED;
}

/// The borrowed C strings as views, into `out`. False for more than `kMaxItems`.
[[nodiscard]] bool items_of(const char* const* items, u32 count,
                            Array<std::string_view>& out) noexcept {
    if (count > kMaxItems || !out.reserve(count)) {
        return false;
    }
    for (u32 index = 0; items != nullptr && index < count; ++index) {
        (void)out.push_back(view(items[index]));
    }
    return true;
}

[[nodiscard]] ui::WidgetDesc widget_desc(const CyUiWidgetDesc& desc,
                                         Span<const std::string_view> items) noexcept {
    ui::WidgetDesc made;
    made.kind = static_cast<ui::WidgetKind>(desc.kind);
    made.id = view(desc.id);
    made.classes = view(desc.classes);
    made.text = view(desc.text);
    made.placeholder = view(desc.placeholder);
    made.title = view(desc.title);
    made.items = items;
    made.value = desc.value;
    made.minimum = desc.minimum;
    made.maximum = desc.maximum == 0.0F ? 1.0F : desc.maximum;
    made.step = desc.step;
    made.columns = static_cast<u16>(desc.columns > 0xFFFFU ? 0xFFFFU : desc.columns);
    made.row_height = desc.row_height == 0.0F ? kDefaultRowHeight : desc.row_height;
    made.checked = (desc.flags & CY_UI_WIDGET_CHECKED) != 0U;
    made.enabled = (desc.flags & CY_UI_WIDGET_DISABLED) == 0U;
    return made;
}

/// A document that did not parse, reported with its first mistake's line.
[[nodiscard]] CyResult refuse_document(const ui::DocumentInstance& document,
                                       const Error& error) noexcept {
    const Span<const ui::DocumentDiagnostic> diagnostics = document.diagnostics();
    if (diagnostics.empty()) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT, error.message);
    }
    const ui::DocumentDiagnostic& first = diagnostics[0];
    char message[192];
    const std::string_view detail = first.detail.text();
    (void)std::snprintf(message, sizeof(message), "ui_load_document: line %u: %s %.*s", first.line,
                        first.message, static_cast<int>(detail.size()), detail.data());
    return abi::report(CY_RESULT_INVALID_ARGUMENT, message);
}

}  // namespace

// --- The embedder's side -------------------------------------------------------------------------

void UiAdapter::attach_toolkit(ui::Toolkit* toolkit, ui::DocumentResolver* resolver,
                               assets::VirtualFileSystem* files) noexcept {
    toolkit_ = toolkit;
    resolver_ = resolver;
    files_ = files;
    events_seen_ = 0;
}

Status UiAdapter::frame(f32 seconds, const ui::ScaleSettings& settings, Vec2 viewport,
                        i64 now_ns) noexcept {
    if (toolkit_ == nullptr) {
        return layout(settings, viewport);
    }
    for (LoadedDocument& loaded : documents_.span()) {
        // A save that does not parse keeps the interface; the document's diagnostics say why.
        if (loaded.host) {
            (void)loaded.host->poll(now_ns);
        }
    }
    if (Status updated = toolkit_->update(seconds, settings, viewport); !updated) {
        return updated;
    }
    pump_widget_events();
    toolkit_->clear_events();
    events_seen_ = 0;
    forget_dead_widgets();
    return ok();
}

// --- Records -------------------------------------------------------------------------------------

ui::Toolkit* UiAdapter::kit() const noexcept {
    if (toolkit_ == nullptr) {
        (void)abi::report(CY_RESULT_UNAVAILABLE, "no widget toolkit is attached to this interface");
    }
    return toolkit_;
}

const CyEntity* UiAdapter::owner_of(ui::WidgetId widget) const noexcept {
    for (ui::WidgetId at = widget; toolkit_->alive(at); at = toolkit_->parent(at)) {
        for (const WidgetRecord& made : widget_records_.span()) {
            if (made.widget == at) {
                return &made.owner;
            }
        }
        for (const LoadedDocument& loaded : documents_.span()) {
            const ui::DocumentInstance* document = loaded.document();
            if (document == nullptr) {
                continue;
            }
            for (const ui::WidgetId root : document->roots()) {
                if (root == at) {
                    return &loaded.owner;
                }
            }
        }
    }
    return nullptr;
}

ui::WidgetId UiAdapter::owned_widget(CyUiElement widget) const noexcept {
    if (kit() == nullptr) {
        return ui::kNoElement;
    }
    const ui::WidgetId id = element_of(widget);
    if (widget == CY_UI_ELEMENT_NULL || owner_of(id) == nullptr) {
        (void)abi::report(CY_RESULT_NOT_FOUND,
                          "no such widget: stale, or not made by a module through this interface");
        return ui::kNoElement;
    }
    return id;
}

bool UiAdapter::parent_widget(CyUiElement parent, ui::WidgetId& out) const noexcept {
    if (parent == CY_UI_ELEMENT_NULL) {
        out = ui::kNoElement;
        return true;
    }
    out = owned_widget(parent);
    return out.is_valid();
}

CyResult UiAdapter::answer(const Status& status) noexcept {
    if (status.has_value()) {
        return CY_RESULT_OK;
    }
    return abi::report(CY_RESULT_INVALID_ARGUMENT, status.error().message);
}

void UiAdapter::pump_widget_events() noexcept {
    const Span<const ui::WidgetEvent> queued = toolkit_->events();
    for (usize index = events_seen_; index < queued.size(); ++index) {
        const ui::WidgetEvent& happened = queued[index];
        const CyEntity* owner = owner_of(happened.widget);
        if (owner == nullptr) {
            continue;  // the embedder's own widget: its events are the embedder's to read
        }
        CyUiEvent event{};
        event.struct_size = sizeof(CyUiEvent);
        event.kind = event_kind(happened.kind);
        event.element = to_abi(happened.widget);
        event.owner = *owner;
        event.position[0] = happened.value;
        event.button = happened.index;
        // An event that cannot be queued is dropped rather than failing the frame that raised it.
        (void)events_.push_back(event);
    }
    events_seen_ = queued.size();
}

void UiAdapter::forget_dead_widgets() noexcept {
    for (usize index = widget_records_.size(); index > 0; --index) {
        if (!toolkit_->alive(widget_records_[index - 1].widget)) {
            widget_records_.remove_unordered(index - 1);
        }
    }
}

// --- UiBackend, 1.12 -----------------------------------------------------------------------------

CyResult UiAdapter::load_text(ui::WidgetId parent, std::string_view text,
                              LoadedDocument& loaded) noexcept {
    Expected<UniquePtr<ui::DocumentInstance>, Error> made =
        make_unique<ui::DocumentInstance>(*allocator_, *allocator_, *toolkit_);
    if (!made.has_value()) {
        return abi::report(made.error());
    }
    loaded.instance = std::move(*made);
    if (Status opened = loaded.instance->load(text, parent, resolver_, nullptr); !opened) {
        return refuse_document(*loaded.instance, opened.error());
    }
    return CY_RESULT_OK;
}

CyResult UiAdapter::load_path(ui::WidgetId parent, const char* path,
                              LoadedDocument& loaded) noexcept {
    if (files_ != nullptr) {
        Expected<UniquePtr<ui::DocumentHost>, Error> made =
            make_unique<ui::DocumentHost>(*allocator_, *allocator_, *toolkit_, *files_);
        if (!made.has_value()) {
            return abi::report(made.error());
        }
        loaded.host = std::move(*made);
        if (Status opened = loaded.host->open(path, parent, nullptr); !opened) {
            return refuse_document(loaded.host->instance(), opened.error());
        }
        return CY_RESULT_OK;
    }
    if (resolver_ == nullptr) {
        return abi::report(CY_RESULT_UNAVAILABLE,
                           "ui_load_document: this interface reads no files; pass the text");
    }
    Array<char> text(*allocator_);
    if (Status read = resolver_->read(path, text); !read) {
        return abi::report(CY_RESULT_NOT_FOUND, read.error().message);
    }
    return load_text(parent, std::string_view(text.data(), text.size()), loaded);
}

CyResult UiAdapter::load_document(CyUiElement parent, const CyUiDocumentDesc& desc,
                                  CyUiElement& out_root) noexcept {
    ui::WidgetId under;
    if (kit() == nullptr || !parent_widget(parent, under)) {
        return abi::last_error_code();
    }
    LoadedDocument loaded;
    loaded.owner = desc.owner;
    const CyResult result =
        desc.text != nullptr
            ? load_text(under, std::string_view(desc.text, static_cast<usize>(desc.text_size)),
                        loaded)
            : load_path(under, desc.path, loaded);
    if (result != CY_RESULT_OK) {
        return result;
    }
    const ui::DocumentInstance* document = loaded.document();
    const CyUiElement first = (document == nullptr || document->roots().empty())
                                  ? CY_UI_ELEMENT_NULL
                                  : to_abi(document->roots()[0]);
    if (Status kept = documents_.push_back(std::move(loaded)); !kept) {
        return abi::report(kept.error());
    }
    out_root = first;
    return CY_RESULT_OK;
}

CyResult UiAdapter::load_stylesheet(const char* utf8, u64 size) noexcept {
    if (kit() == nullptr) {
        return CY_RESULT_UNAVAILABLE;
    }
    return answer(toolkit_->set_stylesheet(std::string_view(utf8, static_cast<usize>(size))));
}

CyResult UiAdapter::find(CyUiElement scope, const char* id, CyUiElement& out) const noexcept {
    out = CY_UI_ELEMENT_NULL;
    if (scope != CY_UI_ELEMENT_NULL) {
        const ui::WidgetId within = owned_widget(scope);
        if (!within.is_valid()) {
            return abi::last_error_code();
        }
        out = to_abi(toolkit_->find(within, id));
        return CY_RESULT_OK;
    }
    if (kit() == nullptr) {
        return CY_RESULT_UNAVAILABLE;
    }
    // Everything a module made: its widgets, then its documents', in the order they were made.
    for (const WidgetRecord& made : widget_records_.span()) {
        if (const ui::WidgetId found = toolkit_->find(made.widget, id); found.is_valid()) {
            out = to_abi(found);
            return CY_RESULT_OK;
        }
    }
    for (const LoadedDocument& loaded : documents_.span()) {
        const ui::DocumentInstance* document = loaded.document();
        if (document == nullptr) {
            continue;
        }
        for (const ui::WidgetId root : document->roots()) {
            if (const ui::WidgetId found = toolkit_->find(root, id); found.is_valid()) {
                out = to_abi(found);
                return CY_RESULT_OK;
            }
        }
    }
    return CY_RESULT_OK;
}

CyResult UiAdapter::create_widget(CyUiElement parent, const CyUiWidgetDesc& desc,
                                  CyUiElement& out) noexcept {
    ui::WidgetId under;
    if (kit() == nullptr || !parent_widget(parent, under)) {
        return abi::last_error_code();
    }
    if (desc.kind == CY_UI_WIDGET_IMAGE) {
        return abi::report(
            CY_RESULT_INVALID_ARGUMENT,
            "ui_create_widget: an image is a document's `image`, or 1.6's ui_create");
    }
    Array<std::string_view> items(*allocator_);
    if (!items_of(desc.items, desc.item_count, items)) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT, "ui_create_widget: too many items");
    }
    Expected<ui::WidgetId, Error> made = toolkit_->create(under, widget_desc(desc, items.span()));
    if (!made.has_value()) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT, made.error().message);
    }
    if (Status kept = widget_records_.push_back(WidgetRecord{*made, desc.owner}); !kept) {
        (void)toolkit_->destroy(*made);
        return abi::report(kept.error());
    }
    out = to_abi(*made);
    return CY_RESULT_OK;
}

CyResult UiAdapter::widget_set_text(CyUiElement widget, const char* utf8) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    return answer(toolkit_->set_text(target, view(utf8)));
}

CyResult UiAdapter::widget_text(CyUiElement widget, const char*& out, u32& size) const noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    const std::string_view text = toolkit_->text(target);
    out = text.data();
    size = static_cast<u32>(text.size());
    return CY_RESULT_OK;
}

CyResult UiAdapter::widget_set_value(CyUiElement widget, f32 value) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    if (is_switch(toolkit_->kind(target))) {
        return answer(toolkit_->set_checked(target, value >= 0.5F));
    }
    return answer(toolkit_->set_value(target, value));
}

CyResult UiAdapter::widget_value(CyUiElement widget, f32& out) const noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    out = toolkit_->value(target);
    if (is_switch(toolkit_->kind(target))) {
        out = toolkit_->checked(target) ? 1.0F : 0.0F;
    }
    return CY_RESULT_OK;
}

CyResult UiAdapter::widget_set_selected(CyUiElement widget, u32 index) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    if (!has_selection(toolkit_->kind(target))) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT,
                           "ui_widget_set_selected: this widget has no selection");
    }
    return answer(toolkit_->set_selected(target, index));
}

CyResult UiAdapter::widget_selected(CyUiElement widget, u32& out) const noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    if (!has_selection(toolkit_->kind(target))) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT,
                           "ui_widget_selected: this widget has no selection");
    }
    out = toolkit_->selected(target);
    return CY_RESULT_OK;
}

CyResult UiAdapter::widget_set_items(CyUiElement widget, const char* const* items,
                                     u32 count) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    Array<std::string_view> views(*allocator_);
    if (!items_of(items, count, views)) {
        return abi::report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_set_items: too many items");
    }
    return answer(toolkit_->set_items(target, views.span()));
}

CyResult UiAdapter::widget_set_enabled(CyUiElement widget, bool enabled) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    return answer(toolkit_->set_enabled(target, enabled));
}

CyResult UiAdapter::widget_set_classes(CyUiElement widget, const char* classes) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    return answer(toolkit_->set_classes(target, view(classes)));
}

CyResult UiAdapter::widget_open(CyUiElement widget, bool open) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    const CyResult result = answer(open ? toolkit_->open(target) : toolkit_->close(target));
    pump_widget_events();
    return result;
}

CyResult UiAdapter::widget_animate(CyUiElement widget, const CyUiAnimation& animation) noexcept {
    const ui::WidgetId target = owned_widget(widget);
    if (!target.is_valid()) {
        return abi::last_error_code();
    }
    ui::Animation made;
    made.property = static_cast<ui::AnimatedProperty>(animation.property);
    made.from = animation.from;
    made.to = animation.to;
    made.from_colour = animation.from_colour;
    made.to_colour = animation.to_colour;
    made.duration = animation.duration;
    made.delay = animation.delay;
    made.easing = static_cast<ui::Easing>(animation.easing);
    made.decorative = (animation.flags & CY_UI_ANIMATION_STATE_FEEDBACK) == 0U;
    return answer(toolkit_->animate(target, made));
}

CyResult UiAdapter::navigate(u32 action, bool& consumed) noexcept {
    if (kit() == nullptr) {
        return CY_RESULT_UNAVAILABLE;
    }
    consumed = toolkit_->action(static_cast<ui::UiAction>(action));
    pump_widget_events();
    return CY_RESULT_OK;
}

}  // namespace cy::game_backend
