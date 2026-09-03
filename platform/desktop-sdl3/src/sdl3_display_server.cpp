#include <cy/platform/sdl3_display_server.h>

#include "host/host.h"

#include <cy/core/base/diagnostic_sink.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <utility>

namespace cy {
namespace {

constexpr usize kSdlErrorCapacity = 256;
thread_local char g_sdl_error[kSdlErrorCapacity];

const char* capture_sdl_error() {
    std::snprintf(g_sdl_error, sizeof(g_sdl_error), "%s", SDL_GetError());
    return g_sdl_error;
}

Unexpected<Error> sdl_failure(ErrorCode code) {
    return Unexpected<Error>(Error{code, capture_sdl_error(), 0});
}

Unexpected<Error> no_such_window() {
    return fail(ErrorCode::NotFound, "no such window");
}

SDL_Window* as_window(void* handle) {
    return static_cast<SDL_Window*>(handle);
}

// A video driver that draws nothing. SDL offers these so a program can run without a display; the
// engine has its own headless DisplayServer for that, and reporting a window feature as supported
// under one of them would be a lie the caller could not check.
bool is_offscreen_driver(std::string_view driver) {
    return driver == "dummy" || driver == "offscreen";
}

ScreenInfo describe_display(SDL_DisplayID display) {
    ScreenInfo info;
    info.id = static_cast<ScreenId>(display);

    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(display, &bounds)) {
        info.position = Point{bounds.x, bounds.y};
        info.resolution = Extent{bounds.w, bounds.h};
    }
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display); mode != nullptr) {
        info.refresh_rate_hz = mode->refresh_rate;
    }
    info.dpi_scale = SDL_GetDisplayContentScale(display);
    if (info.dpi_scale <= 0.0F) {
        info.dpi_scale = 1.0F;
    }
    if (const char* name = SDL_GetDisplayName(display); name != nullptr) {
        std::snprintf(info.name, sizeof(info.name), "%s", name);
    }
    return info;
}

SDL_WindowFlags to_sdl_flags(WindowFlags flags) {
    SDL_WindowFlags sdl = 0;
    if (has_flag(flags, WindowFlags::Resizable)) {
        sdl |= SDL_WINDOW_RESIZABLE;
    }
    if (has_flag(flags, WindowFlags::Borderless)) {
        sdl |= SDL_WINDOW_BORDERLESS;
    }
    if (has_flag(flags, WindowFlags::AlwaysOnTop)) {
        sdl |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (has_flag(flags, WindowFlags::Transparent)) {
        sdl |= SDL_WINDOW_TRANSPARENT;
    }
    if (has_flag(flags, WindowFlags::NoFocus)) {
        sdl |= SDL_WINDOW_NOT_FOCUSABLE;
    }
    if (has_flag(flags, WindowFlags::HighDpi)) {
        sdl |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    // WindowFlags::Popup and WindowFlags::MousePassthrough have no SDL_CreateWindow flag; both are
    // reported unsupported by has_feature() and dropped before this is reached.
    return sdl;
}

}  // namespace

Sdl3DisplayServer::~Sdl3DisplayServer() {
    if (initialised_) {
        shutdown();
    }
}

Status Sdl3DisplayServer::initialise() {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the SDL3 display server is already initialised");
    }
    // Reference-counted against whatever else initialised SDL, so the Platform and the
    // DisplayServer may be brought up and torn down in either order.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return sdl_failure(ErrorCode::Unavailable);
    }
    initialised_ = true;
    return ok();
}

void Sdl3DisplayServer::shutdown() {
    for (Window& window : windows_) {
        if (window.alive && window.handle != nullptr) {
            SDL_DestroyWindow(as_window(window.handle));
        }
        window = Window{};
    }
    events_.clear();
    (void)events_.take_dropped_count();
    if (initialised_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        initialised_ = false;
    }
}

std::string_view Sdl3DisplayServer::video_driver() {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? std::string_view{driver} : std::string_view{};
}

// NOLINTBEGIN(bugprone-branch-clone) — several groups below answer `false` for unrelated reasons,
// and the comment above each is the reason. Merging them into one label would delete the argument.
bool Sdl3DisplayServer::has_feature(Feature feature) const {
    const std::string_view driver = video_driver();
    const bool visual = !driver.empty() && !is_offscreen_driver(driver);

    switch (feature) {
        case Feature::WindowResizable:
        case Feature::WindowBorderless:
        case Feature::WindowAlwaysOnTop:
        case Feature::WindowNoFocus:
        case Feature::HighDpi:
        case Feature::PerScreenDpiScale:
        case Feature::ExclusiveFullscreen:
        case Feature::ScreenRefreshRate:
            return visual;

        // SDL_WINDOW_TRANSPARENT is honoured by the Cocoa, Wayland and Windows backends. Under X11
        // it needs a compositing manager, whose presence SDL does not expose, so the honest answer
        // is false: a caller that is told "no" and gets an opaque window is correct, and one that
        // is told "yes" and gets an opaque window has no way to find out. This is design.md §4's
        // "queries degrade to what SDL3 exposes", spelled out.
        case Feature::WindowTransparency:
            return visual && (driver == "cocoa" || driver == "wayland" || driver == "windows");

        // SDL3 has no window-level mouse pass-through and no popup flag on SDL_CreateWindow — a
        // popup is a different call, SDL_CreatePopupWindow, taking a parent this interface does not
        // model. Both requests are dropped with a warning and the window is still created.
        case Feature::MousePassthrough:
        case Feature::WindowPopup:
            return false;

        // V-sync belongs to whatever presents, and nothing does until the RHI lands at M3.
        // Reporting a mode as available before anything can honour it would be a promise made by
        // the wrong layer.
        case Feature::VSyncAdaptive:
        case Feature::VSyncMailbox:
            return false;

        // The surface seam exists (create_surface with GraphicsApi::None) but no graphics API is
        // initialised at M0, so no API-specific surface can be produced. M3.
        case Feature::VulkanSurface:
        case Feature::MetalSurface:
        case Feature::D3D12Surface:
            return false;

        // SDL3 provides all of these, and this DisplayServer does not expose them yet: the
        // capabilities that need them — `ui-and-editor-framework`, `input-and-actions` — arrive
        // later. Answering false for a call that does not exist is the only answer that cannot
        // mislead; each becomes true in the change that adds its call.
        case Feature::Clipboard:
        case Feature::NativeFileDialog:
        case Feature::NativeMessageDialog:
        case Feature::CustomCursor:
        case Feature::ImePositioning:
        case Feature::OnScreenKeyboard:
        case Feature::ScreenOrientation:
        case Feature::KeepAwake:
        case Feature::SystemTray:
            return false;
    }
    return false;
}
// NOLINTEND(bugprone-branch-clone)

// --- Windows ------------------------------------------------------------------------------------

Expected<WindowId, Error> Sdl3DisplayServer::create_window(const WindowDescription& description) {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "the SDL3 display server is not initialised");
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
                    "the SDL3 display server holds Sdl3DisplayServer::kMaxWindows windows");
    }

    // The specification's degradation rule: an unsupported flag is dropped with a warning here, and
    // the window is created without it rather than not created at all.
    const WindowFlags flags = filter_unsupported_flags(*this, description.flags);

    SDL_Window* handle =
        SDL_CreateWindow(description.title != nullptr ? description.title : "",
                         description.size.width, description.size.height, to_sdl_flags(flags));
    if (handle == nullptr) {
        return sdl_failure(ErrorCode::Unavailable);
    }

    *slot = Window{};
    slot->alive = true;
    slot->id = next_id_++;
    slot->sdl_id = SDL_GetWindowID(handle);
    slot->handle = handle;
    slot->flags = flags;
    slot->vsync = description.vsync;

    if (description.use_position) {
        SDL_SetWindowPosition(handle, description.position.x, description.position.y);
    } else if (description.screen != kInvalidScreen) {
        const auto centred = static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(description.screen));
        SDL_SetWindowPosition(handle, centred, centred);
    }

    if (description.mode != WindowMode::Windowed) {
        (void)set_window_mode(slot->id, description.mode);
    }
    (void)set_window_vsync(slot->id, description.vsync);

    slot->dpi_scale = SDL_GetWindowDisplayScale(handle);
    if (slot->dpi_scale <= 0.0F) {
        slot->dpi_scale = 1.0F;
    }
    slot->screen = static_cast<ScreenId>(SDL_GetDisplayForWindow(handle));

    SDL_ShowWindow(handle);
    return slot->id;
}

void Sdl3DisplayServer::destroy_window(WindowId window) {
    Window* record = find(window);
    if (record == nullptr) {
        return;
    }
    SDL_DestroyWindow(as_window(record->handle));
    *record = Window{};
}

bool Sdl3DisplayServer::window_exists(WindowId window) const {
    return find(window) != nullptr;
}

Expected<Point, Error> Sdl3DisplayServer::window_position(WindowId window) const {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    Point position;
    if (!SDL_GetWindowPosition(as_window(handle), &position.x, &position.y)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return position;
}

Status Sdl3DisplayServer::set_window_position(WindowId window, Point position) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (!SDL_SetWindowPosition(as_window(handle), position.x, position.y)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Expected<Extent, Error> Sdl3DisplayServer::window_size(WindowId window) const {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    Extent size;
    if (!SDL_GetWindowSize(as_window(handle), &size.width, &size.height)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return size;
}

Status Sdl3DisplayServer::set_window_size(WindowId window, Extent size) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (size.width <= 0 || size.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a window needs a positive width and height");
    }
    if (!SDL_SetWindowSize(as_window(handle), size.width, size.height)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Status Sdl3DisplayServer::set_window_minimum_size(WindowId window, Extent size) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (!SDL_SetWindowMinimumSize(as_window(handle), size.width, size.height)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Status Sdl3DisplayServer::set_window_maximum_size(WindowId window, Extent size) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (!SDL_SetWindowMaximumSize(as_window(handle), size.width, size.height)) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Status Sdl3DisplayServer::set_window_title(WindowId window, const char* title) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (!SDL_SetWindowTitle(as_window(handle), title != nullptr ? title : "")) {
        return sdl_failure(ErrorCode::Internal);
    }
    return ok();
}

Status Sdl3DisplayServer::set_window_icon(WindowId window, const IconImage& icon) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    if (icon.pixels == nullptr || icon.width <= 0 || icon.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "an icon needs RGBA pixels and a positive extent");
    }

    // SDL copies the pixels into the surface's own storage before the window takes it, so the
    // caller's buffer does not have to outlive this call.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        icon.width, icon.height, SDL_PIXELFORMAT_RGBA32,
        const_cast<void*>(static_cast<const void*>(icon.pixels)), icon.width * 4);
    if (surface == nullptr) {
        return sdl_failure(ErrorCode::Internal);
    }
    const bool applied = SDL_SetWindowIcon(as_window(handle), surface);
    SDL_DestroySurface(surface);
    if (!applied) {
        return sdl_failure(ErrorCode::Unsupported);
    }
    return ok();
}

Expected<WindowMode, Error> Sdl3DisplayServer::window_mode(WindowId window) const {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(as_window(handle));
    if ((flags & SDL_WINDOW_FULLSCREEN) != 0) {
        // Exclusive fullscreen is the one with a display mode set; borderless fullscreen has none.
        const bool exclusive = SDL_GetWindowFullscreenMode(as_window(handle)) != nullptr;
        return exclusive ? WindowMode::ExclusiveFullscreen : WindowMode::Fullscreen;
    }
    if ((flags & SDL_WINDOW_MINIMIZED) != 0) {
        return WindowMode::Minimised;
    }
    if ((flags & SDL_WINDOW_MAXIMIZED) != 0) {
        return WindowMode::Maximised;
    }
    return WindowMode::Windowed;
}

Status Sdl3DisplayServer::set_window_mode(WindowId window, WindowMode mode) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    SDL_Window* sdl = as_window(handle);

    if (mode == WindowMode::ExclusiveFullscreen && !has_feature(Feature::ExclusiveFullscreen)) {
        emit_diagnosticf(DiagnosticSeverity::Warning, "display",
                         "the %.*s video driver does not support ExclusiveFullscreen; the window "
                         "is left in borderless fullscreen instead",
                         static_cast<int>(video_driver().size()), video_driver().data());
        mode = WindowMode::Fullscreen;
    }

    switch (mode) {
        case WindowMode::Windowed:
            SDL_SetWindowFullscreen(sdl, false);
            SDL_RestoreWindow(sdl);
            return ok();
        case WindowMode::Minimised:
            SDL_SetWindowFullscreen(sdl, false);
            return SDL_MinimizeWindow(sdl) ? ok() : Status{sdl_failure(ErrorCode::Internal)};
        case WindowMode::Maximised:
            SDL_SetWindowFullscreen(sdl, false);
            return SDL_MaximizeWindow(sdl) ? ok() : Status{sdl_failure(ErrorCode::Internal)};
        case WindowMode::Fullscreen:
            // A null mode is SDL's borderless fullscreen at the desktop resolution.
            SDL_SetWindowFullscreenMode(sdl, nullptr);
            return SDL_SetWindowFullscreen(sdl, true) ? ok()
                                                      : Status{sdl_failure(ErrorCode::Internal)};
        case WindowMode::ExclusiveFullscreen: {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl);
            const SDL_DisplayMode* display_mode = SDL_GetCurrentDisplayMode(display);
            if (display_mode == nullptr) {
                return sdl_failure(ErrorCode::Unavailable);
            }
            SDL_SetWindowFullscreenMode(sdl, display_mode);
            return SDL_SetWindowFullscreen(sdl, true) ? ok()
                                                      : Status{sdl_failure(ErrorCode::Internal)};
        }
    }
    return fail(ErrorCode::InvalidArgument, "no such window mode");
}

Expected<WindowFlags, Error> Sdl3DisplayServer::window_flags(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<WindowFlags, Error>{record->flags}
                             : Expected<WindowFlags, Error>{no_such_window()};
}

Expected<f32, Error> Sdl3DisplayServer::window_dpi_scale(WindowId window) const {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    const float scale = SDL_GetWindowDisplayScale(as_window(handle));
    return scale > 0.0F ? scale : 1.0F;
}

Expected<ScreenId, Error> Sdl3DisplayServer::window_screen(WindowId window) const {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(as_window(handle));
    if (display == 0) {
        return sdl_failure(ErrorCode::Unavailable);
    }
    return static_cast<ScreenId>(display);
}

Expected<VSyncMode, Error> Sdl3DisplayServer::window_vsync(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? Expected<VSyncMode, Error>{record->vsync}
                             : Expected<VSyncMode, Error>{no_such_window()};
}

Status Sdl3DisplayServer::set_window_vsync(WindowId window, VSyncMode mode) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }

    const bool exotic = mode == VSyncMode::Adaptive || mode == VSyncMode::Mailbox;
    if (exotic && !has_feature(mode == VSyncMode::Adaptive ? Feature::VSyncAdaptive
                                                           : Feature::VSyncMailbox)) {
        emit_diagnostic(DiagnosticSeverity::Warning, "display",
                        "this display server cannot honour that V-sync mode; it is recorded and "
                        "the presenting backend will fall back to VSyncMode::Enabled (M3)");
        mode = VSyncMode::Enabled;
    }

    // Recorded, not applied: nothing presents until the RHI lands at M3, and the swapchain is what
    // will read this. Storing it now means the window carries its own intent rather than the
    // renderer having to be told again.
    record->vsync = mode;
    return ok();
}

// --- Screens ------------------------------------------------------------------------------------

usize Sdl3DisplayServer::screen_count() const {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_free(displays);
    return count > 0 ? static_cast<usize>(count) : 0;
}

Expected<ScreenInfo, Error> Sdl3DisplayServer::screen(usize index) const {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (displays == nullptr || count <= 0) {
        SDL_free(displays);
        return fail(ErrorCode::Unavailable, "the host reports no screens");
    }
    if (std::cmp_greater_equal(index, count)) {
        SDL_free(displays);
        return fail(ErrorCode::OutOfRange, "no screen at that index");
    }
    const ScreenInfo info = describe_display(displays[index]);
    SDL_free(displays);
    return info;
}

Expected<ScreenInfo, Error> Sdl3DisplayServer::screen_by_id(ScreenId id) const {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (displays == nullptr) {
        return fail(ErrorCode::Unavailable, "the host reports no screens");
    }
    for (int i = 0; i < count; ++i) {
        if (static_cast<ScreenId>(displays[i]) == id) {
            const ScreenInfo info = describe_display(displays[i]);
            SDL_free(displays);
            return info;
        }
    }
    SDL_free(displays);
    return fail(ErrorCode::NotFound, "no such screen");
}

// --- Surfaces -----------------------------------------------------------------------------------

Expected<NativeSurface, Error> Sdl3DisplayServer::create_surface(
    WindowId window, const SurfaceDescription& description) {
    void* handle = handle_of(window);
    if (handle == nullptr) {
        return no_such_window();
    }

    if (description.api != GraphicsApi::None) {
        // The seam is here and it has the right shape; what is missing is an RHI to hand a
        // VkSurfaceKHR or a CAMetalLayer to. M3 fills this in, and has_feature() already answers
        // false for all three so that a caller finds out by asking rather than by failing.
        return fail(ErrorCode::NotImplemented,
                    "no graphics API surface until the RHI lands at M3; GraphicsApi::None yields "
                    "the native window this surface would be built from");
    }

    NativeSurface surface;
    surface.api = GraphicsApi::None;
    if (!host::query_native_window(as_window(handle), surface)) {
        return fail(ErrorCode::Unavailable,
                    "this video driver has no native window — an offscreen or dummy driver; use "
                    "the headless display server instead");
    }
    return surface;
}

void Sdl3DisplayServer::destroy_surface(const NativeSurface& /*surface*/) {
    // Nothing is owned: the handles belong to the window, and GraphicsApi::None allocates nothing.
    // The API-specific surfaces M3 adds are destroyed here.
}

// --- Events -------------------------------------------------------------------------------------

void Sdl3DisplayServer::pump_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        translate_event(&event);
    }
}

void Sdl3DisplayServer::translate_event(const void* sdl_event) {
    const auto& event = *static_cast<const SDL_Event*>(sdl_event);
    const auto timestamp = static_cast<Nanoseconds>(event.common.timestamp);

    // SDL_EVENT_QUIT is not addressed to a window; it is the desktop asking the application to go
    // away. Every live window is told, because a host's loop watches windows and there is nothing
    // else for it to watch.
    if (event.type == SDL_EVENT_QUIT) {
        for (Window& window : windows_) {
            if (window.alive) {
                push(WindowEventType::CloseRequested, window, timestamp);
            }
        }
        return;
    }

    if (event.type < SDL_EVENT_WINDOW_FIRST || event.type > SDL_EVENT_WINDOW_LAST) {
        // Keyboard, mouse, gamepad: the input backend's, at M2.
        return;
    }

    Window* window = find_by_sdl_id(event.window.windowID);
    if (window == nullptr) {
        return;
    }

    switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            push(WindowEventType::CloseRequested, *window, timestamp);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            push(WindowEventType::Resized, *window, timestamp);
            break;
        case SDL_EVENT_WINDOW_MOVED:
            push(WindowEventType::Moved, *window, timestamp);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            push(WindowEventType::FocusGained, *window, timestamp);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            push(WindowEventType::FocusLost, *window, timestamp);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            // Both arrive when a window is dragged between screens of different scale, and SDL does
            // not promise which or how many. refresh_window_screen() emits an engine event only for
            // a value that actually changed, so the pair cannot become two DPI-change events.
            refresh_window_screen(*window, timestamp);
            break;
        default:
            break;
    }
}

void Sdl3DisplayServer::refresh_window_screen(Window& window, Nanoseconds timestamp) {
    SDL_Window* sdl = as_window(window.handle);
    const auto screen = static_cast<ScreenId>(SDL_GetDisplayForWindow(sdl));
    float scale = SDL_GetWindowDisplayScale(sdl);
    if (scale <= 0.0F) {
        scale = window.dpi_scale;
    }

    if (screen != kInvalidScreen && screen != window.screen) {
        window.screen = screen;
        push(WindowEventType::ScreenChanged, window, timestamp);
    }
    // Compared exactly: the scale is a value the platform reports rather than one that is computed,
    // so two readings of the same screen are bit-identical and a difference is a real change.
    if (scale != window.dpi_scale) {
        window.dpi_scale = scale;
        push(WindowEventType::DpiChanged, window, timestamp);
    }
}

void Sdl3DisplayServer::push(WindowEventType type, const Window& window, Nanoseconds timestamp) {
    SDL_Window* sdl = as_window(window.handle);

    WindowEvent event;
    event.type = type;
    event.window = window.id;
    event.timestamp = timestamp;
    SDL_GetWindowPosition(sdl, &event.position.x, &event.position.y);
    SDL_GetWindowSize(sdl, &event.size.width, &event.size.height);
    event.screen = window.screen;
    event.dpi_scale = window.dpi_scale;
    events_.push(event);
}

// --- Lookup -------------------------------------------------------------------------------------

Sdl3DisplayServer::Window* Sdl3DisplayServer::find(WindowId window) {
    const auto* self = this;
    return const_cast<Window*>(self->find(window));
}

const Sdl3DisplayServer::Window* Sdl3DisplayServer::find(WindowId window) const {
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

Sdl3DisplayServer::Window* Sdl3DisplayServer::find_by_sdl_id(u32 sdl_id) {
    for (Window& candidate : windows_) {
        if (candidate.alive && candidate.sdl_id == sdl_id) {
            return &candidate;
        }
    }
    return nullptr;
}

void* Sdl3DisplayServer::handle_of(WindowId window) const {
    const Window* record = find(window);
    return record != nullptr ? record->handle : nullptr;
}

}  // namespace cy
