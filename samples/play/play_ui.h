// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_UI_H
#define CY_SAMPLE_PLAY_PLAY_UI_H
// play_ui.h — the interface a played game draws with: CyberUI's store, text and widget toolkit,
// and the ABI's `UiAdapter` over them, which the game host binds so the Swift game builds its HUD
// and its pause menu through ABI 1.6 and 1.12.
//
// THE PLAY HOST READS TWO WIDGETS, BY NAME, AND NOTHING ELSE. A click on a widget whose author id
// is `play-resume` resumes a paused game, and one on `play-quit` ends the run (CyberdyneKit's
// `PauseMenu` names its buttons so). Every event still goes to its owner first — the game hears its
// own buttons — and the host only notes the two names as they pass. That is the whole contract
// between a game's menu and the host that runs it: no component, no callback, no ABI entry, and a
// game with no menu loses nothing.
//
// One frame, from the game host's frame update, before the behaviours' `onUpdate`:
//
//   frame(seconds, viewport, pointer, deliver)   the toolkit's update (layout, animations), the
//                                                pointer routed, every event handed to `deliver`
//                                                (the runtime's `ui_event`) and the two names noted
//
// The presenter flattens the store and draws it over the presented picture (presenter.h).

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/servers/text/server.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>

#include <functional>
#include <memory>

namespace cy::play {

/// The pointer as the frame routes it: window pixels, and the CY_INPUT_BUTTON_* edges since the
/// last frame.
struct UiPointer {
    Vec2 position{0.0F, 0.0F};
    u32 pressed = 0;
    u32 released = 0;
};

/// What the game's interface asked of the host this frame.
struct UiRequests {
    bool resume = false;
    bool quit = false;
};

class PlayUi {
public:
    explicit PlayUi(Allocator& allocator) noexcept;
    ~PlayUi();

    PlayUi(const PlayUi&) = delete;
    PlayUi& operator=(const PlayUi&) = delete;

    /// The text server, the painter over the built-in font, the store's screen root, the adapter
    /// and the toolkit. Before a game binds it.
    [[nodiscard]] Status start() noexcept;
    /// Bind the adapter as `host`'s interface (`ui_*`).
    void bind(abi::Host& host) noexcept;
    void unbind(abi::Host& host) noexcept;

    /// One frame of the interface; see the header. `deliver` receives every event in order.
    [[nodiscard]] Status frame(f32 seconds, Vec2 viewport, const UiPointer& pointer,
                               const std::function<void(const CyUiEvent&)>& deliver) noexcept;
    /// Lay out what the behaviours wrote since `frame` — a menu shown, a counter changed — so the
    /// picture presented this frame is this frame's interface, not the last one's.
    [[nodiscard]] Status settle() noexcept;
    /// What the frames since the last call asked for, and cleared.
    [[nodiscard]] UiRequests take_requests() noexcept;
    /// Whether the pointer is over an element: CY_INPUT_POINTER_OVER_UI.
    [[nodiscard]] bool pointer_over() const noexcept;

    [[nodiscard]] ui::ElementStore& store() noexcept { return store_; }
    [[nodiscard]] ui::TextPainter& text() noexcept { return text_; }
    [[nodiscard]] Vec2 viewport() const noexcept { return viewport_; }
    [[nodiscard]] const ui::ScaleSettings& scale() const noexcept { return settings_; }
    [[nodiscard]] u32 widgets() const noexcept { return toolkit_->widget_count(); }

private:
    /// The complete text backend, where the build has one: the interface font's rasteriser.
    struct Backend;

    [[nodiscard]] Status start_text() noexcept;
    void note(const CyUiEvent& event) noexcept;

    Allocator* allocator_;
    std::unique_ptr<Backend> backend_;
    text::TextServer server_;
    ui::TextPainter text_;
    ui::ElementStore store_;
    ui::ElementId root_ = ui::kNoElement;
    std::unique_ptr<game_backend::UiAdapter> adapter_;
    std::unique_ptr<ui::Toolkit> toolkit_;
    std::unique_ptr<ui::FileDocumentResolver> resolver_;
    ui::ScaleSettings settings_;
    Vec2 viewport_{1280.0F, 720.0F};
    UiRequests requests_;
    Name resume_;
    Name quit_;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_UI_H
