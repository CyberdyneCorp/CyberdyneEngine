// SPDX-License-Identifier: MIT
#pragma once
// The stub DisplayServer: one window, fixed, that nobody can resize and that has no pointer.
// M11.d task 4.4. See stub_platform.h for what the stub platform is for.
//
// It answers **false to every `Feature`** — which is the strongest thing a `DisplayServer` can say
// and the one no desktop backend ever says. `filter_unsupported_flags()` therefore drops EVERY flag
// a caller asks for, `window_flags()` reports `WindowFlags::None`, and the window is still created.
// That is the specification's degradation rule taken to its limit, and if any caller in this engine
// depended on getting a flag it asked for, this is where it stops working.
//
// ONE WINDOW, AT THE SCREEN'S SIZE. A second `create_window()` is refused rather than queued: a
// target with a single surface has one, and an editor that opens a floating panel as a second
// operating-system window does not run here. The requested size is ignored — the window is the
// display — which is the other assumption a desktop never tests.
//
// NO POINTER AND NO KEYBOARD. There is no input source in this module at all, so nothing connects a
// mouse or a keyboard to `input::InputServer`. A game that cannot be played without one is a game
// that cannot be ported, and the way to find that out is for the devices to be genuinely absent
// rather than present and idle.

#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/display_support.h>

namespace cy {

class StubDisplayServer final : public DisplayServer {
public:
    StubDisplayServer() = default;
    ~StubDisplayServer() override = default;

    /// The display's size. There is one screen and it is this; the single window fills it.
    Status initialise(Extent resolution = Extent{1280, 720});
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "stub"; }

    /// False. Always, for every enumerator, including ones added later — which is why this is not a
    /// switch with cases.
    [[nodiscard]] bool has_feature(Feature feature) const override;

    Expected<WindowId, Error> create_window(const WindowDescription& description) override;
    void destroy_window(WindowId window) override;
    [[nodiscard]] bool window_exists(WindowId window) const override;

    Expected<Point, Error> window_position(WindowId window) const override;
    Status set_window_position(WindowId window, Point position) override;

    Expected<Extent, Error> window_size(WindowId window) const override;
    Status set_window_size(WindowId window, Extent size) override;

    Status set_window_minimum_size(WindowId window, Extent size) override;
    Status set_window_maximum_size(WindowId window, Extent size) override;

    Status set_window_title(WindowId window, const char* title) override;
    Status set_window_icon(WindowId window, const IconImage& icon) override;

    Expected<WindowMode, Error> window_mode(WindowId window) const override;
    Status set_window_mode(WindowId window, WindowMode mode) override;

    Expected<WindowFlags, Error> window_flags(WindowId window) const override;

    Expected<f32, Error> window_dpi_scale(WindowId window) const override;
    Expected<ScreenId, Error> window_screen(WindowId window) const override;

    Expected<VSyncMode, Error> window_vsync(WindowId window) const override;
    Status set_window_vsync(WindowId window, VSyncMode mode) override;

    [[nodiscard]] usize screen_count() const override { return initialised_ ? 1 : 0; }
    Expected<ScreenInfo, Error> screen(usize index) const override;
    Expected<ScreenInfo, Error> screen_by_id(ScreenId id) const override;

    Expected<NativeSurface, Error> create_surface(WindowId window,
                                                  const SurfaceDescription& description) override;
    void destroy_surface(const NativeSurface& surface) override;

    void pump_events() override {}
    bool poll_event(WindowEvent& event) override { return events_.pop(event); }
    u32 take_dropped_event_count() override { return events_.take_dropped_count(); }

    /// Posts a `CloseRequested` for the window, which is how the host of a stub target says the
    /// system asked the application to stop. It is the only event this server ever produces, and it
    /// exists so that a caller's shutdown path can be exercised without a window manager.
    Status request_close();

private:
    static constexpr ScreenId kScreen = 1;

    bool initialised_ = false;
    Extent resolution_{1280, 720};
    WindowId window_ = kInvalidWindow;
    VSyncMode vsync_ = VSyncMode::Enabled;
    WindowEventQueue events_;
};

}  // namespace cy
