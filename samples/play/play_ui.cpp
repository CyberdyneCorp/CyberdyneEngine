// SPDX-License-Identifier: MIT
// play_ui.cpp — see play_ui.h.

#include "play_ui.h"

#include <cy/core/memory/ownership.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/interface_font.h>
#include <cy_features.h>

#if defined(CY_TEXT)
#    include <cy/backends/text/complete_backend.h>
#endif

namespace cy::play {
namespace {

/// The first atlas page the interface's text is drawn from: the built-in font's one page, or the
/// interface font's coverage, distance-field and colour pages from here (`upload_text_atlases`).
constexpr u16 kTextPage = 1;

}  // namespace

#if defined(CY_TEXT)
struct PlayUi::Backend {
    text::CompleteTextBackend complete;
};
#else
struct PlayUi::Backend {};
#endif

PlayUi::PlayUi(Allocator& allocator) noexcept
    : allocator_(&allocator),
      text_(allocator),
      store_(allocator),
      resume_(Name::intern("play-resume")),
      quit_(Name::intern("play-quit")) {
    settings_.mode = ui::ScaleMode::FixedPixel;
}

// The members go in reverse: the toolkit and the adapter before the store and the painter, the
// painter before the server it rasterises through, the server before its backend.
PlayUi::~PlayUi() = default;

Status PlayUi::start_text() noexcept {
#if defined(CY_TEXT)
    // THE INTERFACE FONT, a distance field, so a menu reads at any window size; the built-in
    // bitmap font behind it in the chain, and alone where the build has no complete text backend.
    backend_ = std::make_unique<Backend>();
    text::TextServerConfig config;
    config.atlas.initial_extent = 512;
    if (backend_->complete.start() && server_.start_with(config, backend_->complete) &&
        text_.start(server_, text::FontSource{ui::interface_font_bytes(), 0},
                    ui::interface_font_desc(), kTextPage, 16.0F)) {
        return ok();
    }
    server_.stop();
    backend_->complete.stop();
#endif
    if (Status started = server_.start(text::TextServerConfig{}); !started) {
        return started;
    }
    return text_.start(server_, ui::builtin_font(), kTextPage);
}

Status PlayUi::start() noexcept {
    if (Status text = start_text(); !text) {
        return text;
    }
    auto root = store_.create(ui::kNoElement, Name::intern("screen"));
    if (!root) {
        return make_unexpected(root.error());
    }
    root_ = *root;
    store_.layout_input(root_)->model = ui::LayoutModel::Absolute;
    adapter_ = std::make_unique<game_backend::UiAdapter>(*allocator_, store_, text_, root_);
    if (Status started = adapter_->start(); !started) {
        return started;
    }
    // ONE SCREEN: the toolkit's widgets under the same root as the elements, so a document's
    // anchors are the window's, as a HUD element's are.
    toolkit_ = std::make_unique<ui::Toolkit>(*allocator_, store_, text_);
    if (Status started = toolkit_->start(root_); !started) {
        return started;
    }
    resolver_ = std::make_unique<ui::FileDocumentResolver>(*allocator_, ".");
    adapter_->attach_toolkit(toolkit_.get(), resolver_.get());
    return ok();
}

void PlayUi::bind(abi::Host& host) noexcept {
    game_backend::bind(host, adapter_.get());
}

void PlayUi::unbind(abi::Host& host) noexcept {
    game_backend::bind(host, nullptr);
}

Status PlayUi::frame(f32 seconds, Vec2 viewport, const UiPointer& pointer,
                     const std::function<void(const CyUiEvent&)>& deliver) noexcept {
    viewport_ = viewport;
    if (Status laid = adapter_->frame(seconds, settings_, viewport); !laid) {
        return laid;
    }
    if (Status routed =
            adapter_->route_pointer(pointer.position, pointer.pressed, pointer.released);
        !routed) {
        return routed;
    }
    adapter_->drain_events([&](const CyUiEvent& event) noexcept {
        note(event);
        deliver(event);
    });
    return ok();
}

void PlayUi::note(const CyUiEvent& event) noexcept {
    if (event.kind != CY_UI_EVENT_CLICK) {
        return;
    }
    const ui::WidgetId widget = toolkit_->widget_of(game_backend::element_of(event.element));
    const Name id = toolkit_->id_of(widget);
    requests_.resume = requests_.resume || id == resume_;
    requests_.quit = requests_.quit || id == quit_;
}

Status PlayUi::settle() noexcept {
    return adapter_->layout(settings_, viewport_);
}

UiRequests PlayUi::take_requests() noexcept {
    const UiRequests taken = requests_;
    requests_ = UiRequests{};
    return taken;
}

bool PlayUi::pointer_over() const noexcept {
    return adapter_ != nullptr && adapter_->pointer_over();
}

}  // namespace cy::play
