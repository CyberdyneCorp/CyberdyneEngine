// SPDX-License-Identifier: MIT
// The stub DisplayServer. See stub_display_server.h for what it refuses and why.

#include <cy/platform/stub_display_server.h>

#include <ctime>

namespace cy {
namespace {

Unexpected<Error> no_such_window() {
    return fail(ErrorCode::NotFound, "no such window");
}

Nanoseconds monotonic_now() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<Nanoseconds>(now.tv_sec) * 1'000'000'000LL +
           static_cast<Nanoseconds>(now.tv_nsec);
}

}  // namespace

Status StubDisplayServer::initialise(Extent resolution) {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the stub display server is already initialised");
    }
    if (resolution.width <= 0 || resolution.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a display needs a positive width and height");
    }
    resolution_ = resolution;
    initialised_ = true;
    return ok();
}

void StubDisplayServer::shutdown() {
    window_ = kInvalidWindow;
    events_.clear();
    (void)events_.take_dropped_count();
    initialised_ = false;
}

bool StubDisplayServer::has_feature(Feature /*feature*/) const {
    // Not a switch. A switch would have to grow a case for every enumerator added later, and the
    // day somebody forgot would be the day this backend started claiming a capability by default —
    // which is the exact failure mode `has_feature()` exists to prevent.
    return false;
}

Expected<WindowId, Error> StubDisplayServer::create_window(const WindowDescription& description) {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "the stub display server is not initialised");
    }
    if (window_ != kInvalidWindow) {
        return fail(ErrorCode::Unsupported,
                    "the stub platform has ONE window and it already exists; a target with a "
                    "single surface cannot open a second, and an editor that floats a panel as an "
                    "operating-system window does not run here");
    }
    // The requested size and position are dropped, not honoured and not refused: the window is the
    // display. `filter_unsupported_flags()` drops every flag as well, because has_feature() answers
    // false for all of them — the warning it emits is how the caller finds out.
    (void)filter_unsupported_flags(*this, description.flags);
    window_ = 1;
    return window_;
}

void StubDisplayServer::destroy_window(WindowId window) {
    if (window == window_) {
        window_ = kInvalidWindow;
    }
}

bool StubDisplayServer::window_exists(WindowId window) const {
    return window != kInvalidWindow && window == window_;
}

Expected<Point, Error> StubDisplayServer::window_position(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return Point{0, 0};
}

Status StubDisplayServer::set_window_position(WindowId window, Point /*position*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return fail(ErrorCode::Unsupported,
                "the stub platform's window is the display; it has no position to set");
}

Expected<Extent, Error> StubDisplayServer::window_size(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return resolution_;
}

Status StubDisplayServer::set_window_size(WindowId window, Extent /*size*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return fail(ErrorCode::Unsupported,
                "the stub platform's window is not resizable; has_feature(WindowResizable) answers "
                "false and window_size() is the display's resolution");
}

Status StubDisplayServer::set_window_minimum_size(WindowId window, Extent /*size*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return fail(ErrorCode::Unsupported, "the stub platform's window is not resizable");
}

Status StubDisplayServer::set_window_maximum_size(WindowId window, Extent /*size*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return fail(ErrorCode::Unsupported, "the stub platform's window is not resizable");
}

Status StubDisplayServer::set_window_title(WindowId window, const char* /*title*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    // Accepted and discarded rather than refused: there is no title bar, and a caller that names
    // its window has done nothing wrong. This is the difference between "cannot" and "does not
    // show", and only the first is an error.
    return ok();
}

Status StubDisplayServer::set_window_icon(WindowId window, const IconImage& /*icon*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return ok();
}

Expected<WindowMode, Error> StubDisplayServer::window_mode(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    // Fullscreen, permanently: the window is the display, which is what fullscreen means.
    return WindowMode::Fullscreen;
}

Status StubDisplayServer::set_window_mode(WindowId window, WindowMode mode) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    if (mode == WindowMode::Fullscreen) {
        return ok();
    }
    return fail(ErrorCode::Unsupported,
                "the stub platform's window is permanently fullscreen: there is no desktop to be "
                "windowed on, nothing to minimise into and no mode to change");
}

Expected<WindowFlags, Error> StubDisplayServer::window_flags(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return WindowFlags::None;
}

Expected<f32, Error> StubDisplayServer::window_dpi_scale(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return 1.0F;
}

Expected<ScreenId, Error> StubDisplayServer::window_screen(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return kScreen;
}

Expected<VSyncMode, Error> StubDisplayServer::window_vsync(WindowId window) const {
    if (!window_exists(window)) {
        return no_such_window();
    }
    return vsync_;
}

Status StubDisplayServer::set_window_vsync(WindowId window, VSyncMode mode) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    if (mode == VSyncMode::Adaptive || mode == VSyncMode::Mailbox) {
        return fail(ErrorCode::Unsupported,
                    "the stub platform presents one frame per refresh and nothing else; "
                    "has_feature() answers false for adaptive and mailbox");
    }
    vsync_ = mode;
    return ok();
}

Expected<ScreenInfo, Error> StubDisplayServer::screen(usize index) const {
    if (!initialised_ || index != 0) {
        return fail(ErrorCode::OutOfRange, "the stub platform has exactly one screen");
    }
    ScreenInfo info;
    info.id = kScreen;
    info.resolution = resolution_;
    // Zero refresh rate — "unknown", per ScreenInfo — because a target that presents through a
    // callback does not necessarily know what rate it is being called at.
    info.refresh_rate_hz = 0.0F;
    info.dpi_scale = 1.0F;
    info.name[0] = 's';
    info.name[1] = 't';
    info.name[2] = 'u';
    info.name[3] = 'b';
    info.name[4] = '\0';
    return info;
}

Expected<ScreenInfo, Error> StubDisplayServer::screen_by_id(ScreenId id) const {
    if (id != kScreen) {
        return fail(ErrorCode::NotFound, "no such screen");
    }
    return screen(0);
}

Expected<NativeSurface, Error> StubDisplayServer::create_surface(
    WindowId window, const SurfaceDescription& /*description*/) {
    if (!window_exists(window)) {
        return no_such_window();
    }
    // NO NATIVE HANDLE AT ALL, for any API. `Runtime::enter_display()` treats this as a warning
    // rather than a startup failure — "a platform that cannot answer is not a startup failure" —
    // and this is the only implementation in the tree that exercises that path.
    return fail(ErrorCode::Unavailable,
                "the stub platform has no native window handle to build a surface from; nothing "
                "here can present");
}

void StubDisplayServer::destroy_surface(const NativeSurface& /*surface*/) {}

Status StubDisplayServer::request_close() {
    if (window_ == kInvalidWindow) {
        return fail(ErrorCode::NotFound, "there is no window to close");
    }
    WindowEvent event;
    event.type = WindowEventType::CloseRequested;
    event.window = window_;
    event.timestamp = monotonic_now();
    event.size = resolution_;
    event.screen = kScreen;
    events_.push(event);
    return ok();
}

}  // namespace cy
