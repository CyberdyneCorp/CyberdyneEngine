#include <cy/platform/headless_display_server.h>

#include <cy/core/base/diagnostic_sink.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace cy {
namespace {

// The monotonic clock an event timestamp is taken on. The headless server holds no Platform — it is
// a display server, and the two are separate interfaces — so it reads the same steady clock the
// SDL3 Platform's monotonic clock is built on: never the wall clock.
Nanoseconds monotonic_now() {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

bool contains(const ScreenInfo& screen, Point point) {
    return point.x >= screen.position.x && point.x < screen.position.x + screen.resolution.width &&
           point.y >= screen.position.y && point.y < screen.position.y + screen.resolution.height;
}

void copy_name(char (&destination)[64], const char* text) {
    std::snprintf(destination, sizeof(destination), "%s", text);
}

Unexpected<Error> no_such_window() {
    return fail(ErrorCode::NotFound, "no such window");
}

}  // namespace

HeadlessDesktop default_headless_desktop() {
    HeadlessDesktop desktop;
    desktop.screen_count = 2;

    ScreenInfo& primary = desktop.screens[0];
    primary.id = 1;
    primary.position = Point{0, 0};
    primary.resolution = Extent{1920, 1080};
    primary.refresh_rate_hz = 60.0F;
    primary.dpi_scale = 1.0F;
    copy_name(primary.name, "headless-1");

    // Immediately to the right of the first, and at twice its scale: the pair that makes the
    // DPI-change requirement testable.
    ScreenInfo& secondary = desktop.screens[1];
    secondary.id = 2;
    secondary.position = Point{1920, 0};
    secondary.resolution = Extent{2560, 1440};
    secondary.refresh_rate_hz = 120.0F;
    secondary.dpi_scale = 2.0F;
    copy_name(secondary.name, "headless-2");

    return desktop;
}

Status HeadlessDisplayServer::initialise(const HeadlessDesktop& desktop) {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the headless display server is already initialised");
    }
    if (desktop.screen_count == 0 || desktop.screen_count > HeadlessDesktop::kMaxScreens) {
        return fail(ErrorCode::InvalidArgument,
                    "a headless desktop has between one and HeadlessDesktop::kMaxScreens screens");
    }
    for (usize i = 0; i < desktop.screen_count; ++i) {
        if (desktop.screens[i].id == kInvalidScreen) {
            return fail(ErrorCode::InvalidArgument, "a headless screen needs a non-zero id");
        }
    }

    desktop_ = desktop;
    initialised_ = true;
    return ok();
}

void HeadlessDisplayServer::shutdown() {
    for (Window& window : windows_) {
        window = Window{};
    }
    events_.clear();
    (void)events_.take_dropped_count();
    next_id_ = 1;
    initialised_ = false;
}

bool HeadlessDisplayServer::has_feature(Feature feature) const {
    switch (feature) {
        // A described desktop can honour any purely bookkeeping flag exactly.
        case Feature::WindowResizable:
        case Feature::WindowBorderless:
        case Feature::WindowAlwaysOnTop:
        case Feature::WindowNoFocus:
        case Feature::HighDpi:
        case Feature::PerScreenDpiScale:
        case Feature::ScreenRefreshRate:
            return true;

        // Everything below needs something the headless server does not have: a compositor, a
        // pointer, a graphics API, or a user.
        default:
            return false;
    }
}

Expected<WindowId, Error> HeadlessDisplayServer::create_window(
    const WindowDescription& description) {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "the headless display server is not initialised");
    }
    if (description.size.width <= 0 || description.size.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a window needs a positive width and height");
    }

    Window* slot = nullptr;
    for (Window& candidate : windows_) {
        if (!candidate.alive) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "the headless display server holds HeadlessDisplayServer::kMaxWindows windows");
    }

    *slot = Window{};
    slot->alive = true;
    slot->id = next_id_++;
    slot->size = description.size;
    slot->mode = description.mode;
    // The specification's degradation rule, applied before the window exists rather than after:
    // an unsupported flag is dropped with a warning and the window is created without it.
    slot->flags = filter_unsupported_flags(*this, description.flags);
    slot->vsync = description.vsync;
    std::snprintf(slot->title, sizeof(slot->title), "%s",
                  description.title != nullptr ? description.title : "");

    if (description.use_position) {
        slot->position = description.position;
    } else if (const ScreenInfo* screen = screen_record(description.screen); screen != nullptr) {
        slot->position = screen->position;
    } else {
        slot->position = desktop_.screens[0].position;
    }

    slot->screen = screen_containing(slot->position, slot->size);
    if (const ScreenInfo* screen = screen_record(slot->screen); screen != nullptr) {
        slot->dpi_scale = screen->dpi_scale;
    }
    return slot->id;
}

void HeadlessDisplayServer::destroy_window(WindowId window) {
    if (Window* record = find(window); record != nullptr) {
        *record = Window{};
    }
}

bool HeadlessDisplayServer::window_exists(WindowId window) const {
    return find(window) != nullptr;
}

Expected<Point, Error> HeadlessDisplayServer::window_position(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<Point, Error>{record->position}
                             : Expected<Point, Error>{no_such_window()};
}

Status HeadlessDisplayServer::set_window_position(WindowId window, Point position) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    record->position = position;
    push(WindowEventType::Moved, *record);
    refresh_window_screen(*record);
    return ok();
}

Expected<Extent, Error> HeadlessDisplayServer::window_size(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<Extent, Error>{record->size}
                             : Expected<Extent, Error>{no_such_window()};
}

Status HeadlessDisplayServer::set_window_size(WindowId window, Extent size) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (size.width <= 0 || size.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a window needs a positive width and height");
    }

    // A zero limit means "no limit on that axis", so each bound is applied only where it is set.
    if (record->minimum_size.width > 0) {
        size.width =
            size.width < record->minimum_size.width ? record->minimum_size.width : size.width;
    }
    if (record->minimum_size.height > 0) {
        size.height =
            size.height < record->minimum_size.height ? record->minimum_size.height : size.height;
    }
    if (record->maximum_size.width > 0 && size.width > record->maximum_size.width) {
        size.width = record->maximum_size.width;
    }
    if (record->maximum_size.height > 0 && size.height > record->maximum_size.height) {
        size.height = record->maximum_size.height;
    }

    record->size = size;
    push(WindowEventType::Resized, *record);
    refresh_window_screen(*record);
    return ok();
}

Status HeadlessDisplayServer::set_window_minimum_size(WindowId window, Extent size) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    record->minimum_size = size;
    return ok();
}

Status HeadlessDisplayServer::set_window_maximum_size(WindowId window, Extent size) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    record->maximum_size = size;
    return ok();
}

Status HeadlessDisplayServer::set_window_title(WindowId window, const char* title) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    std::snprintf(record->title, sizeof(record->title), "%s", title != nullptr ? title : "");
    return ok();
}

Status HeadlessDisplayServer::set_window_icon(WindowId window, const IconImage& icon) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (icon.pixels == nullptr || icon.width <= 0 || icon.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "an icon needs RGBA pixels and a positive extent");
    }
    // There is no taskbar to show it in; recording that one was set is the whole of the contract a
    // headless server can keep.
    record->has_icon = true;
    return ok();
}

Expected<WindowMode, Error> HeadlessDisplayServer::window_mode(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<WindowMode, Error>{record->mode}
                             : Expected<WindowMode, Error>{no_such_window()};
}

Status HeadlessDisplayServer::set_window_mode(WindowId window, WindowMode mode) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (mode == WindowMode::ExclusiveFullscreen && !has_feature(Feature::ExclusiveFullscreen)) {
        emit_diagnostic(DiagnosticSeverity::Warning, "display",
                        "headless does not support ExclusiveFullscreen; the window is left "
                        "fullscreen instead");
        mode = WindowMode::Fullscreen;
    }
    record->mode = mode;
    return ok();
}

Expected<WindowFlags, Error> HeadlessDisplayServer::window_flags(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<WindowFlags, Error>{record->flags}
                             : Expected<WindowFlags, Error>{no_such_window()};
}

Expected<f32, Error> HeadlessDisplayServer::window_dpi_scale(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<f32, Error>{record->dpi_scale}
                             : Expected<f32, Error>{no_such_window()};
}

Expected<ScreenId, Error> HeadlessDisplayServer::window_screen(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<ScreenId, Error>{record->screen}
                             : Expected<ScreenId, Error>{no_such_window()};
}

Expected<VSyncMode, Error> HeadlessDisplayServer::window_vsync(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<VSyncMode, Error>{record->vsync}
                             : Expected<VSyncMode, Error>{no_such_window()};
}

Status HeadlessDisplayServer::set_window_vsync(WindowId window, VSyncMode mode) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    // Nothing presents, so the mode is recorded and honoured by whatever eventually does.
    record->vsync = mode;
    return ok();
}

Expected<ScreenInfo, Error> HeadlessDisplayServer::screen(usize index) const {
    if (index >= desktop_.screen_count) {
        return fail(ErrorCode::OutOfRange, "no screen at that index");
    }
    return desktop_.screens[index];
}

Expected<ScreenInfo, Error> HeadlessDisplayServer::screen_by_id(ScreenId id) const {
    const ScreenInfo* record = screen_record(id);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such screen");
    }
    return *record;
}

Expected<NativeSurface, Error> HeadlessDisplayServer::create_surface(
    WindowId window, const SurfaceDescription& description) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (description.api != GraphicsApi::None) {
        return fail(ErrorCode::Unsupported,
                    "the headless display server has no graphics API; only GraphicsApi::None, "
                    "which yields an opaque window token, is available");
    }

    // There is no native window, so the handle is the window's own id as an opaque token: unique,
    // stable, and never dereferenced. It is what an offscreen backend would key its swapchain on,
    // and it keeps the seam the same shape as the SDL3 one (task 3.3.5).
    NativeSurface surface;
    surface.api = GraphicsApi::None;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) — the token is never dereferenced; see above.
    surface.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(record->id));
    surface.display = nullptr;
    return surface;
}

void HeadlessDisplayServer::destroy_surface(const NativeSurface& /*surface*/) {
    // The token owns nothing.
}

Status HeadlessDisplayServer::post_close_request(WindowId window) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    push(WindowEventType::CloseRequested, *record);
    return ok();
}

Status HeadlessDisplayServer::set_screen_scale(ScreenId screen, f32 scale) {
    if (scale <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "a screen's scale factor is positive");
    }
    ScreenInfo* record = nullptr;
    for (usize i = 0; i < desktop_.screen_count; ++i) {
        if (desktop_.screens[i].id == screen) {
            record = &desktop_.screens[i];
            break;
        }
    }
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such screen");
    }

    record->dpi_scale = scale;
    for (Window& window : windows_) {
        if (window.alive && window.screen == screen) {
            refresh_window_screen(window);
        }
    }
    return ok();
}

std::string_view HeadlessDisplayServer::window_title(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? std::string_view{record->title} : std::string_view{};
}

HeadlessDisplayServer::Window* HeadlessDisplayServer::find(WindowId window) {
    const auto* self = this;
    return const_cast<Window*>(self->find(window));
}

const HeadlessDisplayServer::Window* HeadlessDisplayServer::find(WindowId window) const {
    if (window == kInvalidWindow) {
        return nullptr;
    }
    for (const Window& candidate : windows_) {
        if (candidate.alive && candidate.id == window) {
            return &candidate;
        }
    }
    return nullptr;
}

ScreenId HeadlessDisplayServer::screen_containing(Point position, Extent size) const {
    const Point centre{position.x + (size.width / 2), position.y + (size.height / 2)};
    for (usize i = 0; i < desktop_.screen_count; ++i) {
        if (contains(desktop_.screens[i], centre)) {
            return desktop_.screens[i].id;
        }
    }
    return desktop_.screen_count > 0 ? desktop_.screens[0].id : kInvalidScreen;
}

const ScreenInfo* HeadlessDisplayServer::screen_record(ScreenId id) const {
    for (usize i = 0; i < desktop_.screen_count; ++i) {
        if (desktop_.screens[i].id == id) {
            return &desktop_.screens[i];
        }
    }
    return nullptr;
}

void HeadlessDisplayServer::refresh_window_screen(Window& window) {
    const ScreenId screen = screen_containing(window.position, window.size);
    const ScreenInfo* record = screen_record(screen);
    const f32 scale = record != nullptr ? record->dpi_scale : window.dpi_scale;

    if (screen != window.screen) {
        window.screen = screen;
        push(WindowEventType::ScreenChanged, window);
    }
    // Compared exactly on purpose: a scale is a value the platform reports, not one that is
    // computed, so two readings of the same screen are bit-identical and a difference is real.
    if (scale != window.dpi_scale) {
        window.dpi_scale = scale;
        push(WindowEventType::DpiChanged, window);
    }
}

void HeadlessDisplayServer::push(WindowEventType type, const Window& window) {
    WindowEvent event;
    event.type = type;
    event.window = window.id;
    event.timestamp = monotonic_now();
    event.position = window.position;
    event.size = window.size;
    event.screen = window.screen;
    event.dpi_scale = window.dpi_scale;
    events_.push(event);
}

}  // namespace cy
