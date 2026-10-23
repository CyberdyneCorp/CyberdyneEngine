// SPDX-License-Identifier: MIT
// cy/game_backend/ui_backend.h — the interface adapter behind ABI 1.6.
//
// Implements cy/abi/game/ui.h over CyberUI: elements in the embedder's `cy::ui::ElementStore`,
// text through its `cy::ui::TextPainter`, the pointer through a `cy::ui::Interaction` this adapter
// owns. Behind `CY_UI`, as all of CyberUI is: a build without it has no `cy::game-backend-ui`,
// binds no `UiBackend`, and every `ui_*` entry answers UNAVAILABLE.
//
// ================================================================================================
// WHAT A MODULE MAY TOUCH
// ================================================================================================
//
// The root the embedder hands over — an ABSOLUTE element covering the window — and the elements the
// module created under it, nothing else. Every element made through `create` has a record here
// (its kind, its owner and, for a progress bar, its fill); a handle without one answers NOT_FOUND,
// so the developer console the embedder built in the same store cannot be restyled or destroyed by
// a game. The root itself is a parent, never a target.
//
// The root is made transparent to the pointer (`HitTestMode::Ignore`) and pushed as a `Hud` layer
// that neither captures focus nor blocks input: a point over no element is over the world, which is
// what `ui_hit_test` answering null means to a game.
//
// ================================================================================================
// ONE FRAME
// ================================================================================================
//
//   layout(settings, viewport)             measure and arrange what is dirty
//   route_pointer(position, pressed, ...)  hover, press, release; a press and a release of the
//                                          left button on one button is a CY_UI_EVENT_CLICK, and a
//                                          change of focus is a BLUR and a FOCUS
//   drain_events(→ BehaviourRuntime::ui_event)  the embedder delivers them to their owners; what
//                                          a receiver's own writes raise waits for the next frame
//
// `layout` runs before routing so the pointer is tested against this frame's rects, and the
// behaviours' writes during `frame_update` are laid out by the next frame's `layout` — or by the
// embedder's own call before `flatten`.
//
// WHAT EACH WRITE DIRTIES is what `samples/13-rts-selection/hud.cpp` marks for the same change: a
// text is Measure and Paint, a layout Measure and Arrange, a style or an opacity Paint, a
// visibility whatever `ElementStore::set_flags` decides. That is what makes a HUD built through the
// ABI lay out and draw exactly as the same HUD built in C++, and `render.rts_api_hud` holds the two
// to the byte.
//
// ================================================================================================
// THE WIDGET TOOLKIT, ABI 1.12
// ================================================================================================
//
// `attach_toolkit` gives the adapter a started `cy::ui::Toolkit` — its own root, beside this
// adapter's — and every 1.12 entry then reaches it: widgets made one at a time or a `.cyui`
// document at once, their values, items and text, a sheet, animations and navigation. A widget a
// module made is recorded with its owner, and a document's widgets with the document's owner; a
// widget the embedder made itself answers NOT_FOUND as the console does. The toolkit's events —
// a click, a change, a submission, focus, a dialog opening — are queued as `CyUiEvent`s for the
// owner, beside 1.6's, and leave through the same `drain_events`.
//
// With a toolkit, `frame` replaces `layout`: the toolkit's update — bindings, animations, styles,
// the lists' rows — lays the whole store out, 1.6's elements with it — and it polls the documents
// loaded by path, and clears the toolkit's event queue once it is translated: an embedder that
// reads the toolkit's events itself reads them before `frame`. `route_pointer` routes the pointer
// through both, so a click lands on whichever is under it. `destroy`, `set_visibility`, `rect` and
// `set_focus` take a module's widget as they take a 1.6 element.

#pragma once

#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ui/interaction.h>
#include <cy/ui/layout.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>

#include <string_view>
#include <utility>

namespace cy::game_backend {

/// `CyUiElement` ↔ `cy::ui::ElementId`: the generation high, the index low. Zero is null.
[[nodiscard]] constexpr CyUiElement to_abi(ui::ElementId element) noexcept {
    return (static_cast<CyUiElement>(element.generation) << 32U) | element.index;
}
[[nodiscard]] constexpr ui::ElementId element_of(CyUiElement element) noexcept {
    return ui::ElementId{static_cast<u32>(element & 0xFFFFFFFFULL),
                         static_cast<u32>(element >> 32U)};
}

/// Implements `cy::abi::game::UiBackend`.
class UiAdapter final : public abi::game::UiBackend {
public:
    /// `store`, `text` and `root` are the embedder's and outlive the adapter. `text` must be
    /// started: a label is measured through it.
    UiAdapter(Allocator& allocator, ui::ElementStore& store, ui::TextPainter& text,
              ui::ElementId root) noexcept;

    UiAdapter(const UiAdapter&) = delete;
    UiAdapter& operator=(const UiAdapter&) = delete;
    UiAdapter(UiAdapter&&) = delete;
    UiAdapter& operator=(UiAdapter&&) = delete;
    ~UiAdapter() override = default;

    /// Make the root transparent to the pointer and push the interface layer. Once, before binding.
    [[nodiscard]] Status start() noexcept;

    // --- The embedder's frame -------------------------------------------------------------------

    /// Lay the store out — the whole store, the embedder's elements as well — measuring text.
    [[nodiscard]] Status layout(const ui::ScaleSettings& settings, Vec2 viewport) noexcept;

    /// ABI 1.12: the widget toolkit the widget entries reach, started on the same store and text.
    /// `resolver` reads a document's paths and its `use`d documents and sheets; `files`, when
    /// given, is where a document loaded by path is read from and watched, and reloaded on a save.
    /// Both outlive the adapter, as the toolkit does. Null detaches nothing already loaded.
    void attach_toolkit(ui::Toolkit* toolkit, ui::DocumentResolver* resolver,
                        assets::VirtualFileSystem* files = nullptr) noexcept;
    [[nodiscard]] ui::Toolkit* toolkit() const noexcept { return toolkit_; }
    /// One frame of the interface: with a toolkit, its update — which lays out the whole store —
    /// and the reload of any document whose file settled into a change at `now_ns`; without one,
    /// `layout`.
    [[nodiscard]] Status frame(f32 seconds, const ui::ScaleSettings& settings, Vec2 viewport,
                               i64 now_ns = 0) noexcept;
    /// Documents loaded through 1.12 and still alive.
    [[nodiscard]] u32 documents() const noexcept { return static_cast<u32>(documents_.size()); }
    /// Route the pointer. `pressed` and `released` are CY_INPUT_BUTTON_* edges since the last
    /// call; only the left button presses a button.
    [[nodiscard]] Status route_pointer(Vec2 position, u32 pressed, u32 released) noexcept;
    /// True when the last routed position was over any element of the store but the root — the
    /// embedder's own included — which is CY_INPUT_POINTER_OVER_UI.
    [[nodiscard]] bool pointer_over() const noexcept { return pointer_over_; }
    /// The events routed since `clear_events`, in the order they happened.
    [[nodiscard]] Span<const CyUiEvent> events() const noexcept { return events_.span(); }
    void clear_events() noexcept { events_.clear(); }
    /// Hand every queued event to `deliver` (a `const CyUiEvent&` callable), in order, and empty
    /// the queue. A receiver may write the interface — move focus, destroy what it clicked — and
    /// the events that raises go to a fresh queue for the next call, never into the batch being
    /// walked: no write moves the event a receiver holds, and none is lost to a clear after.
    template <typename Deliver>
    void drain_events(Deliver&& deliver) noexcept {
        const Array<CyUiEvent> batch = std::move(events_);
        for (const CyUiEvent& event : batch.span()) {
            deliver(event);
        }
    }

    /// The first module element whose type name is `name`, or CY_UI_ELEMENT_NULL. For a driver and
    /// a test; a game holds its handles.
    [[nodiscard]] CyUiElement find(std::string_view name) const noexcept;
    /// Elements the module has created and not destroyed.
    [[nodiscard]] u32 elements() const noexcept { return live_; }
    /// Clicks routed since `start`, delivered or not.
    [[nodiscard]] u32 clicks() const noexcept { return clicks_; }
    [[nodiscard]] const ui::ElementStore& store() const noexcept { return store_; }

    // --- UiBackend ------------------------------------------------------------------------------
    [[nodiscard]] CyUiElement root() const noexcept override { return to_abi(root_); }
    [[nodiscard]] CyResult create(CyUiElement parent, const CyUiElementDesc& desc,
                                  CyUiElement& out) noexcept override;
    [[nodiscard]] CyResult destroy(CyUiElement element) noexcept override;
    [[nodiscard]] CyResult set_layout(CyUiElement element,
                                      const CyUiLayout& layout) noexcept override;
    [[nodiscard]] CyResult set_style(CyUiElement element, const CyUiStyle& style) noexcept override;
    [[nodiscard]] CyResult set_text(CyUiElement element, const char* utf8, u32 colour,
                                    u32 pixel_scale) noexcept override;
    [[nodiscard]] CyResult set_image(CyUiElement element, u32 atlas_page,
                                     const f32 (&uv)[4]) noexcept override;
    [[nodiscard]] CyResult set_progress(CyUiElement element, f32 value) noexcept override;
    [[nodiscard]] CyResult set_visibility(CyUiElement element,
                                          CyUiVisibility visibility) noexcept override;
    [[nodiscard]] CyResult set_opacity(CyUiElement element, f32 opacity) noexcept override;
    [[nodiscard]] CyResult rect(CyUiElement element, f32 (&out)[4]) const noexcept override;
    [[nodiscard]] CyUiElement hit_test(f32 x, f32 y) const noexcept override;
    [[nodiscard]] CyUiElement focus() const noexcept override;
    [[nodiscard]] CyResult set_focus(CyUiElement element) noexcept override;

    // --- UiBackend, 1.12 -----------------------------------------------------------------------
    [[nodiscard]] CyResult load_document(CyUiElement parent, const CyUiDocumentDesc& desc,
                                         CyUiElement& out_root) noexcept override;
    [[nodiscard]] CyResult load_stylesheet(const char* utf8, u64 size) noexcept override;
    [[nodiscard]] CyResult find(CyUiElement scope, const char* id,
                                CyUiElement& out) const noexcept override;
    [[nodiscard]] CyResult create_widget(CyUiElement parent, const CyUiWidgetDesc& desc,
                                         CyUiElement& out) noexcept override;
    [[nodiscard]] CyResult widget_set_text(CyUiElement widget, const char* utf8) noexcept override;
    [[nodiscard]] CyResult widget_text(CyUiElement widget, const char*& out,
                                       u32& size) const noexcept override;
    [[nodiscard]] CyResult widget_set_value(CyUiElement widget, f32 value) noexcept override;
    [[nodiscard]] CyResult widget_value(CyUiElement widget, f32& out) const noexcept override;
    [[nodiscard]] CyResult widget_set_selected(CyUiElement widget, u32 index) noexcept override;
    [[nodiscard]] CyResult widget_selected(CyUiElement widget, u32& out) const noexcept override;
    [[nodiscard]] CyResult widget_set_items(CyUiElement widget, const char* const* items,
                                            u32 count) noexcept override;
    [[nodiscard]] CyResult widget_set_enabled(CyUiElement widget, bool enabled) noexcept override;
    [[nodiscard]] CyResult widget_set_classes(CyUiElement widget,
                                              const char* classes) noexcept override;
    [[nodiscard]] CyResult widget_open(CyUiElement widget, bool open) noexcept override;
    [[nodiscard]] CyResult widget_animate(CyUiElement widget,
                                          const CyUiAnimation& animation) noexcept override;
    [[nodiscard]] CyResult navigate(u32 action, bool& consumed) noexcept override;

private:
    /// A widget a module made, and whose behaviours receive its events.
    struct WidgetRecord {
        ui::WidgetId widget;
        CyEntity owner = CY_ENTITY_NULL;
    };

    /// One loaded document: from text, or — `host` — from a watched path.
    struct LoadedDocument {
        UniquePtr<ui::DocumentInstance> instance;
        UniquePtr<ui::DocumentHost> host;
        CyEntity owner = CY_ENTITY_NULL;

        /// The loaded instance; null only for a record whose load failed.
        [[nodiscard]] const ui::DocumentInstance* document() const noexcept {
            return host ? &host->instance() : instance.get();
        }
    };

    /// A live widget a module made — not a 1.6 element — that the 1.6 entries also take.
    [[nodiscard]] bool is_widget(CyUiElement element) const noexcept;
    /// The toolkit, or null having reported UNAVAILABLE.
    [[nodiscard]] ui::Toolkit* kit() const noexcept;
    /// The module's widget, or none having reported NOT_FOUND (or UNAVAILABLE, with no toolkit).
    [[nodiscard]] ui::WidgetId owned_widget(CyUiElement widget) const noexcept;
    /// A parent for a new widget or document: the module's widget, or the toolkit's root for null.
    [[nodiscard]] bool parent_widget(CyUiElement parent, ui::WidgetId& out) const noexcept;
    /// Who made `widget` or a widget above it, or null when no module did.
    [[nodiscard]] const CyEntity* owner_of(ui::WidgetId widget) const noexcept;
    /// The toolkit's events since the last call, as `CyUiEvent`s for their owners.
    void pump_widget_events() noexcept;
    void forget_dead_widgets() noexcept;
    [[nodiscard]] CyResult load_text(ui::WidgetId parent, std::string_view text,
                                     LoadedDocument& loaded) noexcept;
    [[nodiscard]] CyResult load_path(ui::WidgetId parent, const char* path,
                                     LoadedDocument& loaded) noexcept;
    /// A widget's call answered: OK, or the toolkit's refusal as INVALID_ARGUMENT.
    [[nodiscard]] static CyResult answer(const Status& status) noexcept;

    /// What the adapter knows about one module element, at its store index.
    struct Record {
        ui::ElementId element;
        /// A progress bar's fill: a child the module cannot address.
        ui::ElementId fill;
        CyUiKind kind = CY_UI_PANEL;
        CyEntity owner = CY_ENTITY_NULL;
    };

    /// The record for a live module element, or null having reported NOT_FOUND.
    [[nodiscard]] Record* record(CyUiElement element) noexcept;
    [[nodiscard]] const Record* lookup(ui::ElementId element) const noexcept;
    /// The nearest module element at or above `element`, or none.
    [[nodiscard]] const Record* owned_ancestor(ui::ElementId element) const noexcept;
    [[nodiscard]] Status remember(const Record& made) noexcept;
    /// Records whose element a destroy took with it are dropped, with their text.
    void forget_dead() noexcept;
    /// Queue a BLUR and a FOCUS when focus moved since the last call.
    void note_focus() noexcept;
    void push_event(CyUiEventKind kind, const Record& target, Vec2 position, u32 button) noexcept;

    Allocator* allocator_;
    ui::ElementStore& store_;
    ui::TextPainter& text_;
    ui::ElementId root_;
    ui::Interaction interaction_;
    Array<Record> records_;
    Array<CyUiEvent> events_;
    ui::ElementId pressed_;
    ui::ElementId focus_;
    u32 live_ = 0;
    u32 clicks_ = 0;
    bool pointer_over_ = false;

    ui::Toolkit* toolkit_ = nullptr;
    ui::DocumentResolver* resolver_ = nullptr;
    assets::VirtualFileSystem* files_ = nullptr;
    Array<WidgetRecord> widget_records_;
    Array<LoadedDocument> documents_;
    /// How many of the toolkit's queued events are already translated; `frame` clears the queue.
    usize events_seen_ = 0;
    /// The primary button as the toolkit was last told it, from `route_pointer`'s edges.
    bool pointer_down_ = false;
};

/// Bind `adapter` as the host's interface; null unbinds it, and every `ui_*` entry then answers
/// UNAVAILABLE.
inline void bind(cy::abi::Host& host, UiAdapter* adapter) noexcept {
    host.game.ui = adapter;
}

}  // namespace cy::game_backend
