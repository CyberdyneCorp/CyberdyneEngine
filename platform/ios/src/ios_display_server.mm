// SPDX-License-Identifier: MIT
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <cy/platform/ios_display_server.h>

#include <chrono>
#include <cstring>

namespace cy {
namespace {

Unexpected<Error> no_window() { return fail(ErrorCode::NotFound, "no such iOS window"); }
Status fixed_window() {
    return fail(ErrorCode::Unsupported, "the iOS window is the fullscreen UIKit scene");
}
Nanoseconds now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Status IosDisplayServer::initialise(void* view, void* layer, Extent pixels, f32 scale,
                                    f32 refresh_rate_hz) {
    if (initialised_) return fail(ErrorCode::AlreadyExists, "iOS display is already initialised");
    if (view == nullptr || layer == nullptr || pixels.width <= 0 || pixels.height <= 0 ||
        scale <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "iOS display needs a UIView, CAMetalLayer, and positive pixel metrics");
    }
    if (![(__bridge id)layer isKindOfClass:CAMetalLayer.class]) {
        return fail(ErrorCode::InvalidArgument, "iOS Metal surface is not a CAMetalLayer");
    }
    ui_view_ = view;
    metal_layer_ = layer;
    pixels_ = pixels;
    scale_ = scale;
    refresh_rate_hz_ = refresh_rate_hz;
    initialised_ = true;
    return ok();
}

void IosDisplayServer::shutdown() {
    window_created_ = false;
    initialised_ = false;
    ui_view_ = nullptr;
    metal_layer_ = nullptr;
    events_.clear();
    (void)events_.take_dropped_count();
}

void IosDisplayServer::update_metrics(Extent pixels, f32 scale, f32 refresh) {
    if (!initialised_ || pixels.width <= 0 || pixels.height <= 0 || scale <= 0.0F) return;
    const bool resized = pixels.width != pixels_.width || pixels.height != pixels_.height;
    const bool rescaled = scale != scale_;
    pixels_ = pixels;
    scale_ = scale;
    refresh_rate_hz_ = refresh;
    if (!window_created_) return;
    if (resized) {
        WindowEvent event;
        event.type = WindowEventType::Resized;
        event.window = kWindow;
        event.timestamp = now();
        event.size = pixels_;
        events_.push(event);
    }
    if (rescaled) {
        WindowEvent event;
        event.type = WindowEventType::DpiChanged;
        event.window = kWindow;
        event.timestamp = now();
        event.screen = kScreen;
        event.dpi_scale = scale_;
        events_.push(event);
    }
}

bool IosDisplayServer::has_feature(Feature feature) const {
    return feature == Feature::HighDpi || feature == Feature::PerScreenDpiScale ||
           feature == Feature::ScreenRefreshRate || feature == Feature::MetalSurface;
}

Expected<WindowId, Error> IosDisplayServer::create_window(const WindowDescription& description) {
    if (!initialised_) return fail(ErrorCode::Unavailable, "iOS display is not initialised");
    if (window_created_) return fail(ErrorCode::Unsupported, "iOS exposes one fullscreen window");
    (void)filter_unsupported_flags(*this, description.flags);
    window_created_ = true;
    return kWindow;
}

void IosDisplayServer::destroy_window(WindowId window) { if (valid(window)) window_created_ = false; }
bool IosDisplayServer::window_exists(WindowId window) const { return valid(window); }
Expected<Point, Error> IosDisplayServer::window_position(WindowId window) const {
    if (!valid(window)) return no_window();
    return Point{};
}
Status IosDisplayServer::set_window_position(WindowId window, Point) {
    return valid(window) ? fixed_window() : no_window();
}
Expected<Extent, Error> IosDisplayServer::window_size(WindowId window) const {
    if (!valid(window)) return no_window();
    return pixels_;
}
Status IosDisplayServer::set_window_size(WindowId window, Extent) {
    return valid(window) ? fixed_window() : no_window();
}
Status IosDisplayServer::set_window_minimum_size(WindowId window, Extent) {
    return valid(window) ? fixed_window() : no_window();
}
Status IosDisplayServer::set_window_maximum_size(WindowId window, Extent) {
    return valid(window) ? fixed_window() : no_window();
}
Status IosDisplayServer::set_window_title(WindowId window, const char*) {
    return valid(window) ? fixed_window() : no_window();
}
Status IosDisplayServer::set_window_icon(WindowId window, const IconImage&) {
    return valid(window) ? fixed_window() : no_window();
}
Expected<WindowMode, Error> IosDisplayServer::window_mode(WindowId window) const {
    if (!valid(window)) return no_window();
    return WindowMode::Fullscreen;
}
Status IosDisplayServer::set_window_mode(WindowId window, WindowMode mode) {
    if (!valid(window)) return no_window();
    return mode == WindowMode::Fullscreen ? ok() : fixed_window();
}
Expected<WindowFlags, Error> IosDisplayServer::window_flags(WindowId window) const {
    if (!valid(window)) return no_window();
    return WindowFlags::HighDpi;
}
Expected<f32, Error> IosDisplayServer::window_dpi_scale(WindowId window) const {
    if (!valid(window)) return no_window();
    return scale_;
}
Expected<ScreenId, Error> IosDisplayServer::window_screen(WindowId window) const {
    if (!valid(window)) return no_window();
    return kScreen;
}
Expected<VSyncMode, Error> IosDisplayServer::window_vsync(WindowId window) const {
    if (!valid(window)) return no_window();
    return VSyncMode::Enabled;
}
Status IosDisplayServer::set_window_vsync(WindowId window, VSyncMode mode) {
    if (!valid(window)) return no_window();
    if (mode == VSyncMode::Enabled) return ok();
    return fail(ErrorCode::Unsupported, "iOS presents on the display refresh callback");
}
Expected<ScreenInfo, Error> IosDisplayServer::screen(usize index) const {
    if (!initialised_ || index != 0) return fail(ErrorCode::OutOfRange, "iOS has one active screen");
    ScreenInfo info;
    info.id = kScreen;
    info.resolution = pixels_;
    info.refresh_rate_hz = refresh_rate_hz_;
    info.dpi_scale = scale_;
    std::strncpy(info.name, "iOS main screen", sizeof(info.name) - 1);
    return info;
}
Expected<ScreenInfo, Error> IosDisplayServer::screen_by_id(ScreenId id) const {
    if (id != kScreen) return fail(ErrorCode::NotFound, "no such iOS screen");
    return screen(0);
}
Expected<NativeSurface, Error> IosDisplayServer::create_surface(
    WindowId window, const SurfaceDescription& description) {
    if (!valid(window)) return no_window();
    if (description.api == GraphicsApi::Metal)
        return NativeSurface{GraphicsApi::Metal, metal_layer_, nullptr};
    if (description.api == GraphicsApi::None)
        return NativeSurface{GraphicsApi::None, ui_view_, nullptr};
    return fail(ErrorCode::Unsupported, "iOS display exposes only a Metal graphics surface");
}

}  // namespace cy
