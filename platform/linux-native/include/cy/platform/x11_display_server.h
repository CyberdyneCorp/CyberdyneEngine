// SPDX-License-Identifier: MIT
// The native X11 DisplayServer — Xlib and XRandR, with no SDL3 beneath it. M11.d task 4.1.
//
// WHY X11 AND NOT WAYLAND, MEASURED RATHER THAN PREFERRED. design.md §4 chose Linux and left the
// window system to the implementer. The measurement on the machine this was written on:
//
//   XDG_SESSION_TYPE=x11, WAYLAND_DISPLAY unset — a Wayland backend could be compiled here and
//   never once run, which is the exact failure mode this rung moved Metal and D3D12 out for;
//   libX11 1.8.7, libXrandr 1.5.2 present — so screen enumeration with a refresh rate is real
//   rather than stubbed;
//   xcb-icccm, xcb-randr and xkbcommon-x11 absent — so an xcb backend would hand-roll ICCCM and
//   lose screen enumeration, for no gain this rung can measure.
//
// The residual is recorded rather than argued away, in platform/linux-native/README.md: a Wayland
// backend is a third implementation of the same interface and it is the one that would test the
// assumptions X11 and SDL3 happen to share. `platform/stub/` is the cheaper and stricter half of
// that cover, because it shares no desktop assumption at all.
//
// NO X11 TYPE APPEARS IN THIS HEADER. `Display*`, `Window` and `Atom` are held behind void* and
// u64 in the implementation, exactly as SDL_Window* is in sdl3_display_server.h, and for the same
// reason: the rule is about the engine's layers, not about which library is beneath them.
// tools/layercheck's `windowlib` check refuses an X11 header above platform/ as it refuses an SDL
// one.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/display_support.h>

namespace cy {

/// Observes every X event this server pumps, before the server decides whether it is a window
/// event. It is how `X11InputSource` sees key, button and motion events without a second event
/// loop — two consumers of one queue is the defect `Sdl3InputSource`'s header describes at length,
/// and X11 has no `SDL_AddEventWatch`, so the observer is a member of this class instead.
///
/// The pointer is an `XEvent*`; the source casts it. It is `const void*` here because this header
/// may not name an Xlib type.
using X11EventObserver = void (*)(const void* x_event, void* user);

class X11DisplayServer final : public DisplayServer {
public:
    static constexpr usize kMaxWindows = 8;
    static constexpr usize kMaxScreens = 16;

    X11DisplayServer() = default;
    ~X11DisplayServer() override;

    /// Opens the X display named by $DISPLAY. Fails, rather than opening a window that cannot be
    /// drawn to, when there is no display to talk to — which is the case CI runs
    /// `platform/headless/` for.
    Status initialise();
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "linux-x11"; }
    [[nodiscard]] bool has_feature(Feature feature) const override;

    /// The X server's vendor string, for a diagnostic: several feature answers depend on what is
    /// on the other end of the socket, and "which X server" is the first question when one of them
    /// surprises somebody.
    [[nodiscard]] std::string_view vendor() const;

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

    [[nodiscard]] usize screen_count() const override { return screen_count_; }
    Expected<ScreenInfo, Error> screen(usize index) const override;
    Expected<ScreenInfo, Error> screen_by_id(ScreenId id) const override;

    Expected<NativeSurface, Error> create_surface(WindowId window,
                                                  const SurfaceDescription& description) override;
    void destroy_surface(const NativeSurface& surface) override;

    void pump_events() override;
    bool poll_event(WindowEvent& event) override { return events_.pop(event); }
    u32 take_dropped_event_count() override { return events_.take_dropped_count(); }

    /// Installs the observer described above. One at a time; a null observer removes it.
    void set_event_observer(X11EventObserver observer, void* user);

    /// The `Display*` this server opened, for the input source, which needs it to translate a
    /// keycode. Held as void* so that no Xlib type reaches this header.
    [[nodiscard]] void* native_display() const { return display_; }

private:
    struct Window {
        bool alive = false;
        WindowId id = kInvalidWindow;
        /// The X window id. An XID is an unsigned long, carried as u64 so that this header names no
        /// Xlib type.
        u64 handle = 0;
        WindowFlags flags = WindowFlags::None;
        WindowMode mode = WindowMode::Windowed;
        /// Recorded, not enacted: V-sync on X11 is a property of the graphics API's presentation
        /// path — `VK_PRESENT_MODE_*` — and not of the window system. The request is remembered so
        /// that the RHI can read it back; see set_window_vsync().
        VSyncMode vsync = VSyncMode::Enabled;
        Extent size{0, 0};
        Point position;
        /// The last values seen, so a redundant ConfigureNotify does not become a spurious engine
        /// event: `DisplayServer` promises an event when the value changed, not whenever X mentions
        /// the subject.
        f32 dpi_scale = 1.0F;
        ScreenId screen = kInvalidScreen;
    };

    Window* find(WindowId window);
    const Window* find(WindowId window) const;
    Window* find_by_handle(u64 handle);

    void refresh_screens();
    /// The screen whose rectangle contains the centre of `rectangle`, or the first screen.
    ScreenId screen_containing(Point position, Extent size) const;
    const ScreenInfo* screen_record(ScreenId id) const;

    void translate_event(const void* x_event);
    void apply_configure(Window& window, i32 x, i32 y, i32 width, i32 height, Nanoseconds now);
    void push(WindowEventType type, const Window& window, Nanoseconds timestamp);
    /// Sends a `_NET_WM_STATE` client message to the root window, which is how a compliant window
    /// manager is asked to change a state on a mapped window.
    void set_net_wm_state(u64 handle, u64 first, u64 second, bool enable);
    void apply_flags(u64 handle, WindowFlags flags);
    void apply_size_hints(const Window& window);

    void* display_ = nullptr;
    int default_screen_ = 0;
    u64 root_ = 0;
    bool initialised_ = false;

    /// The interned atoms, in the order of `AtomIndex` in the implementation.
    u64 atoms_[24] = {};

    Window windows_[kMaxWindows];
    WindowEventQueue events_;
    WindowId next_id_ = 1;

    ScreenInfo screens_[kMaxScreens];
    usize screen_count_ = 0;

    /// The scale every window reports. X11 has no per-window scale factor: the value comes from the
    /// X resource database's `Xft.dpi`, which is the one number every toolkit on this window system
    /// agrees to read. See window_dpi_scale().
    f32 dpi_scale_ = 1.0F;

    /// True when the X server has a 32-bit TrueColor visual AND a compositing manager owns
    /// `_NET_WM_CM_S<n>`. Both are required for a transparent window to be transparent rather than
    /// black, and answering `Feature::WindowTransparency` without checking the second is how a
    /// caller gets a window it cannot see through and no warning.
    bool compositor_ = false;

    /// True when the X server has the RandR extension. Without it there is one screen, at the
    /// display's dimensions, with no refresh rate — which is what `ScreenInfo`'s zero means.
    bool randr_ = false;

    X11EventObserver observer_ = nullptr;
    void* observer_user_ = nullptr;
};

}  // namespace cy
