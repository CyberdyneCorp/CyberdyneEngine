// The SDL3 DisplayServer for Linux, Windows and macOS. Tasks 3.3.2, 3.3.3, 3.3.5 and 3.3.6.
//
// design.md §4 records what this costs: "some DisplayServer feature queries degrade to what SDL3
// exposes — transparency, tray items, IME positioning. has_feature() already exists for exactly
// this, so the cost is answered by the interface rather than absorbed silently." Every such
// degradation is one `case` in has_feature() with the reason written next to it.
//
// No SDL type appears in this header. The window handles are held as void*, and every SDL call is
// inside sdl3_display_server.cpp — design.md §4, enforced by tools/layercheck/.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/display_support.h>

namespace cy {

class Sdl3DisplayServer final : public DisplayServer {
public:
    static constexpr usize kMaxWindows = 8;

    Sdl3DisplayServer() = default;
    ~Sdl3DisplayServer() override;

    // Initialises SDL's video subsystem. Fails, rather than opening a window that cannot be drawn
    // to, when there is no display to talk to — which is the case CI runs the headless server for.
    Status initialise();
    void shutdown();

    [[nodiscard]] std::string_view name() const override { return "desktop-sdl3"; }
    [[nodiscard]] bool has_feature(Feature feature) const override;

    // SDL's own name for the driver it selected: "x11", "wayland", "cocoa", "windows", "dummy".
    // Several feature answers depend on it, so it is worth reporting in a diagnostic.
    /// The video driver SDL selected. Static because SDL's video subsystem is process state, not
    /// this object's: two servers cannot disagree about it.
    [[nodiscard]] static std::string_view video_driver();

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

    [[nodiscard]] usize screen_count() const override;
    Expected<ScreenInfo, Error> screen(usize index) const override;
    Expected<ScreenInfo, Error> screen_by_id(ScreenId id) const override;

    Expected<NativeSurface, Error> create_surface(WindowId window,
                                                  const SurfaceDescription& description) override;
    void destroy_surface(const NativeSurface& surface) override;

    void pump_events() override;
    bool poll_event(WindowEvent& event) override { return events_.pop(event); }
    u32 take_dropped_event_count() override { return events_.take_dropped_count(); }

private:
    struct Window {
        bool alive = false;
        WindowId id = kInvalidWindow;
        // SDL's own window id, which is what an SDL_Event carries.
        u32 sdl_id = 0;
        // The SDL_Window*, held as void* so that no SDL type reaches this header.
        void* handle = nullptr;
        WindowFlags flags = WindowFlags::None;
        VSyncMode vsync = VSyncMode::Enabled;
        // The last values seen, so that a redundant SDL event does not become a spurious engine
        // one: DisplayServer promises a DPI-change event when the scale changed, not whenever SDL
        // mentions the subject.
        f32 dpi_scale = 1.0F;
        ScreenId screen = kInvalidScreen;
    };

    Window* find(WindowId window);
    const Window* find(WindowId window) const;
    Window* find_by_sdl_id(u32 sdl_id);

    // The SDL_Window* behind a WindowId, or null.
    void* handle_of(WindowId window) const;

    // Converts one SDL event, if it is a window event this interface has a shape for. Non-window
    // events — keyboard, mouse, gamepad — are discarded: they belong to the input backend, which
    // arrives at M2 and takes over the pump.
    void translate_event(const void* sdl_event);

    // Re-reads the window's screen and scale and emits ScreenChanged and DpiChanged only where they
    // actually changed. The single place either is produced.
    void refresh_window_screen(Window& window, Nanoseconds timestamp);

    void push(WindowEventType type, const Window& window, Nanoseconds timestamp);

    Window windows_[kMaxWindows];
    WindowEventQueue events_;
    WindowId next_id_ = 1;
    bool initialised_ = false;
};

}  // namespace cy
