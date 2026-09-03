// The headless DisplayServer. Task 3.3.4.
//
// `core-platform-abstraction`: "A headless implementation SHALL exist that satisfies the interface
// without a window system." It is not a set of stubs that return errors — it is a display server
// whose desktop is described rather than discovered. Windows have positions and sizes, screens have
// resolutions and scale factors, and a window that moves between screens of different scale
// delivers a DPI-change event, because that is what the interface promises and CI has no other way
// to see it happen.
//
// It is what every test that is not about a real window runs against, and what `just run-sample
// empty --headless` uses.
//
// MODELLED DESKTOP. The default is two screens with different scale factors — 1920x1080 at 1.0 and
// 2560x1440 at 2.0, side by side. A single-screen headless server would make the specification's
// "DPI change while running" scenario untestable anywhere but on hardware with two mismatched
// monitors, which is not a machine CI has.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/display_support.h>

namespace cy {

// The desktop the headless server models. Screens are laid out by the caller; nothing checks that
// they do not overlap, because a real desktop's screens do not always agree either.
struct HeadlessDesktop {
    static constexpr usize kMaxScreens = 4;

    ScreenInfo screens[kMaxScreens];
    usize screen_count = 0;
};

// The default two-screen desktop described above.
HeadlessDesktop default_headless_desktop();

class HeadlessDisplayServer final : public DisplayServer {
public:
    static constexpr usize kMaxWindows = 8;

    HeadlessDisplayServer() = default;
    ~HeadlessDisplayServer() override = default;

    // Constructors cannot report failure with exceptions disabled, so initialisation is a call.
    // Re-initialising an already-initialised server is an error, not a silent reset.
    Status initialise(const HeadlessDesktop& desktop);
    Status initialise() { return initialise(default_headless_desktop()); }
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "headless"; }
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

    [[nodiscard]] usize screen_count() const override { return desktop_.screen_count; }
    Expected<ScreenInfo, Error> screen(usize index) const override;
    Expected<ScreenInfo, Error> screen_by_id(ScreenId id) const override;

    Expected<NativeSurface, Error> create_surface(WindowId window,
                                                  const SurfaceDescription& description) override;
    void destroy_surface(const NativeSurface& surface) override;

    void pump_events() override {}  // there is no operating system queue to drain
    bool poll_event(WindowEvent& event) override { return events_.pop(event); }
    u32 take_dropped_event_count() override { return events_.take_dropped_count(); }

    // --- Headless-only controls -------------------------------------------------------------
    //
    // A window system delivers these; without one, a test does. They are on the concrete class
    // rather than the interface so that nothing written against DisplayServer can reach them.

    // What a window manager's close button produces.
    Status post_close_request(WindowId window);

    // Changes a screen's scale factor — the platform-side half of a DPI change, as distinct from a
    // window moving to a different screen. Every window on that screen is notified.
    Status set_screen_scale(ScreenId screen, f32 scale);

    // The window's title, for a test that set one. Empty for an unknown window.
    [[nodiscard]] std::string_view window_title(WindowId window) const;

private:
    struct Window {
        bool alive = false;
        WindowId id = kInvalidWindow;
        Point position;
        Extent size;
        Extent minimum_size;
        Extent maximum_size;
        char title[128] = {};
        WindowMode mode = WindowMode::Windowed;
        WindowFlags flags = WindowFlags::None;
        VSyncMode vsync = VSyncMode::Enabled;
        ScreenId screen = kInvalidScreen;
        f32 dpi_scale = 1.0F;
        bool has_icon = false;
    };

    Window* find(WindowId window);
    const Window* find(WindowId window) const;

    // The screen whose rectangle contains the window's centre; the first screen when none does,
    // which is what a real window manager does with a window dragged off the desktop.
    [[nodiscard]] ScreenId screen_containing(Point position, Extent size) const;
    const ScreenInfo* screen_record(ScreenId id) const;

    // Re-reads the window's screen and scale after a move or a scale change, emitting
    // ScreenChanged and DpiChanged when they actually changed. The one place either event is
    // produced, so the two cannot drift apart.
    void refresh_window_screen(Window& window);

    void push(WindowEventType type, const Window& window);

    HeadlessDesktop desktop_;
    Window windows_[kMaxWindows];
    WindowEventQueue events_;
    WindowId next_id_ = 1;
    bool initialised_ = false;
};

}  // namespace cy
