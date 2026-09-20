// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/platform/display_server.h>
#include <cy/core/platform/display_support.h>

namespace cy {

/// The single fullscreen UIKit scene presented by an iOS application.
class IosDisplayServer final : public DisplayServer {
public:
    Status initialise(void* ui_view, void* metal_layer, Extent pixels, f32 scale,
                      f32 refresh_rate_hz);
    void shutdown();
    void update_metrics(Extent pixels, f32 scale, f32 refresh_rate_hz);

    [[nodiscard]] std::string_view name() const override { return "ios-uikit"; }
    [[nodiscard]] bool has_feature(Feature feature) const override;
    Expected<WindowId, Error> create_window(const WindowDescription&) override;
    void destroy_window(WindowId) override;
    [[nodiscard]] bool window_exists(WindowId) const override;
    Expected<Point, Error> window_position(WindowId) const override;
    Status set_window_position(WindowId, Point) override;
    Expected<Extent, Error> window_size(WindowId) const override;
    Status set_window_size(WindowId, Extent) override;
    Status set_window_minimum_size(WindowId, Extent) override;
    Status set_window_maximum_size(WindowId, Extent) override;
    Status set_window_title(WindowId, const char*) override;
    Status set_window_icon(WindowId, const IconImage&) override;
    Expected<WindowMode, Error> window_mode(WindowId) const override;
    Status set_window_mode(WindowId, WindowMode) override;
    Expected<WindowFlags, Error> window_flags(WindowId) const override;
    Expected<f32, Error> window_dpi_scale(WindowId) const override;
    Expected<ScreenId, Error> window_screen(WindowId) const override;
    Expected<VSyncMode, Error> window_vsync(WindowId) const override;
    Status set_window_vsync(WindowId, VSyncMode) override;
    [[nodiscard]] usize screen_count() const override { return initialised_ ? 1 : 0; }
    Expected<ScreenInfo, Error> screen(usize) const override;
    Expected<ScreenInfo, Error> screen_by_id(ScreenId) const override;
    Expected<NativeSurface, Error> create_surface(WindowId, const SurfaceDescription&) override;
    void destroy_surface(const NativeSurface&) override {}
    void pump_events() override {}
    bool poll_event(WindowEvent& event) override { return events_.pop(event); }
    u32 take_dropped_event_count() override { return events_.take_dropped_count(); }

private:
    static constexpr WindowId kWindow = 1;
    static constexpr ScreenId kScreen = 1;
    bool valid(WindowId window) const { return window_created_ && window == kWindow; }

    void* ui_view_ = nullptr;
    void* metal_layer_ = nullptr;
    Extent pixels_{};
    f32 scale_ = 1.0F;
    f32 refresh_rate_hz_ = 0.0F;
    bool initialised_ = false;
    bool window_created_ = false;
    WindowEventQueue events_;
};

}  // namespace cy
