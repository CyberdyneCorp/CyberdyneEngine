#include <cy/core/platform/display_server.h>

namespace cy {

const char* feature_name(Feature feature) {
    switch (feature) {
        case Feature::WindowResizable:
            return "WindowResizable";
        case Feature::WindowBorderless:
            return "WindowBorderless";
        case Feature::WindowAlwaysOnTop:
            return "WindowAlwaysOnTop";
        case Feature::WindowTransparency:
            return "WindowTransparency";
        case Feature::WindowNoFocus:
            return "WindowNoFocus";
        case Feature::WindowPopup:
            return "WindowPopup";
        case Feature::MousePassthrough:
            return "MousePassthrough";
        case Feature::HighDpi:
            return "HighDpi";
        case Feature::PerScreenDpiScale:
            return "PerScreenDpiScale";
        case Feature::ExclusiveFullscreen:
            return "ExclusiveFullscreen";
        case Feature::VSyncAdaptive:
            return "VSyncAdaptive";
        case Feature::VSyncMailbox:
            return "VSyncMailbox";
        case Feature::ScreenRefreshRate:
            return "ScreenRefreshRate";
        case Feature::VulkanSurface:
            return "VulkanSurface";
        case Feature::MetalSurface:
            return "MetalSurface";
        case Feature::D3D12Surface:
            return "D3D12Surface";
        case Feature::Clipboard:
            return "Clipboard";
        case Feature::NativeFileDialog:
            return "NativeFileDialog";
        case Feature::NativeMessageDialog:
            return "NativeMessageDialog";
        case Feature::CustomCursor:
            return "CustomCursor";
        case Feature::ImePositioning:
            return "ImePositioning";
        case Feature::OnScreenKeyboard:
            return "OnScreenKeyboard";
        case Feature::ScreenOrientation:
            return "ScreenOrientation";
        case Feature::KeepAwake:
            return "KeepAwake";
        case Feature::SystemTray:
            return "SystemTray";
    }
    return "<invalid>";
}

}  // namespace cy
