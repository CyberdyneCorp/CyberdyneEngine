// The DisplayServer interface. Tasks 3.3.1, 3.3.2, 3.3.5 and 3.3.6.
//
// `core-platform-abstraction` fixes the surface: "creation and destruction, position, size,
// minimum and maximum size, title, icon, mode (windowed, minimised, maximised, fullscreen,
// exclusive fullscreen), flags (resizable, borderless, always-on-top, transparent, no-focus, popup,
// mouse pass-through), DPI scale, screen enumeration with resolution and refresh rate, V-sync mode,
// and native surface creation for the active graphics backend."
//
// Two properties of that surface are the reason this is an interface rather than a wrapper:
//
//   * Capabilities are QUERIED, never assumed — has_feature(Feature). A window flag the platform
//     cannot honour is dropped with a warning and the window is still created. Requesting
//     transparency on a platform without it is not an error; assuming you got it would be.
//
//   * The graphics surface comes from here, so no backend contains a platform #ifdef. There is no
//     RHI until M3, so create_surface() is exercised at M0 with GraphicsApi::None, which returns
//     the same native handles a Vulkan or Metal surface would be built from.
//
// The implementations live under platform/<name>/: desktop-sdl3 and headless. No SDL type appears
// in this header or anywhere above platform/ — design.md §4, enforced by tools/layercheck/.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy {

// Identifies a window for the lifetime of the DisplayServer. Zero is never a window.
using WindowId = u32;
inline constexpr WindowId kInvalidWindow = 0;

// Identifies a screen. Stable while the screen is connected; enumeration order is not.
using ScreenId = u32;
inline constexpr ScreenId kInvalidScreen = 0;

enum class WindowMode {
    Windowed,
    Minimised,
    Maximised,
    Fullscreen,           // borderless, at the desktop resolution
    ExclusiveFullscreen,  // the display's mode is changed
};

// A bitmask. Every flag is a request: one the platform does not support is reported by
// has_feature() and dropped from the window rather than refused.
enum class WindowFlags : u32 {
    None = 0,
    Resizable = 1u << 0,
    Borderless = 1u << 1,
    AlwaysOnTop = 1u << 2,
    Transparent = 1u << 3,
    NoFocus = 1u << 4,
    Popup = 1u << 5,
    MousePassthrough = 1u << 6,
    HighDpi = 1u << 7,  // back buffer at the screen's pixel density rather than in logical units
};

constexpr WindowFlags operator|(WindowFlags a, WindowFlags b) {
    return static_cast<WindowFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr WindowFlags operator&(WindowFlags a, WindowFlags b) {
    return static_cast<WindowFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr WindowFlags operator~(WindowFlags a) {
    return static_cast<WindowFlags>(~static_cast<u32>(a));
}
constexpr WindowFlags& operator|=(WindowFlags& a, WindowFlags b) {
    a = a | b;
    return a;
}
constexpr WindowFlags& operator&=(WindowFlags& a, WindowFlags b) {
    a = a & b;
    return a;
}
constexpr bool has_flag(WindowFlags value, WindowFlags flag) {
    return static_cast<u32>(value & flag) != 0;
}

enum class VSyncMode {
    Disabled,
    Enabled,   // one presentation per refresh
    Adaptive,  // tear rather than stall when a frame is late
    Mailbox,   // present the newest finished frame, never stall, never tear
};

// What a platform may or may not be able to do. The set a caller may ask about; it is deliberately
// larger than what M0 implements, because a query that has no enumerator is a query that gets
// assumed instead.
enum class Feature {
    // Window flags, one per honourable request.
    WindowResizable,
    WindowBorderless,
    WindowAlwaysOnTop,
    WindowTransparency,
    WindowNoFocus,
    WindowPopup,
    MousePassthrough,

    // Display properties.
    HighDpi,
    PerScreenDpiScale,  // two screens may report different scales, and a window may move between
    ExclusiveFullscreen,
    VSyncAdaptive,
    VSyncMailbox,
    ScreenRefreshRate,

    // Surfaces. Queried before create_surface(), which is otherwise a guess.
    VulkanSurface,
    MetalSurface,
    D3D12Surface,

    // System integration. `core-platform-abstraction` requires these to be feature-gated in the
    // same way; the calls themselves arrive with the capabilities that need them, and every
    // implementation reports false for them at M0.
    Clipboard,
    NativeFileDialog,
    NativeMessageDialog,
    CustomCursor,
    ImePositioning,
    OnScreenKeyboard,
    ScreenOrientation,
    KeepAwake,
    SystemTray,
};

// The enumerator's own spelling, for the warning an unsupported request produces. Never null.
const char* feature_name(Feature feature);

struct Point {
    i32 x = 0;
    i32 y = 0;
};

struct Extent {
    i32 width = 0;
    i32 height = 0;
};

struct ScreenInfo {
    ScreenId id = kInvalidScreen;
    // The screen's rectangle in the desktop's coordinate space, in logical units.
    Point position;
    Extent resolution;
    // Zero when the platform does not report one, which is not the same as a screen that does not
    // refresh; a caller that needs a number treats zero as unknown.
    f32 refresh_rate_hz = 0.0F;
    // 1.0 is 96 dpi. 2.0 is a "retina" screen.
    f32 dpi_scale = 1.0F;
    // Truncated rather than allocated; empty when the platform gives no name.
    char name[64] = {};
};

// A request. `position` is honoured only when `use_position` is set, because (0, 0) is a position.
struct WindowDescription {
    const char* title = "CyberdyneEngine";
    Extent size{1280, 720};
    Point position;
    bool use_position = false;
    WindowMode mode = WindowMode::Windowed;
    WindowFlags flags = WindowFlags::Resizable;
    VSyncMode vsync = VSyncMode::Enabled;
    // The screen to open on; kInvalidScreen means the platform's choice.
    ScreenId screen = kInvalidScreen;
};

// A window icon as tightly packed 8-bit RGBA, `width * height * 4` bytes.
struct IconImage {
    const u8* pixels = nullptr;
    i32 width = 0;
    i32 height = 0;
};

enum class GraphicsApi {
    // The M0 seam: no API is initialised, and create_surface() returns the native handles a real
    // surface would be built from. It is what proves the shape is right before there is an RHI.
    None,
    Vulkan,
    Metal,
    D3D12,
};

// What the platform hands the graphics backend. Which members are meaningful depends on `api` and
// on the platform; a backend reads the two it knows and never asks how they were obtained.
//
//   Vulkan   handle = VkSurfaceKHR                    display = unused
//   Metal    handle = CAMetalLayer*                   display = unused
//   D3D12    handle = HWND                            display = unused
//   None     handle = the native window (X11 Window id, wl_surface*, HWND, NSWindow*)
//            display = the native display where the platform has one (X11 Display*, wl_display*)
struct NativeSurface {
    GraphicsApi api = GraphicsApi::None;
    void* handle = nullptr;
    void* display = nullptr;
};

struct SurfaceDescription {
    GraphicsApi api = GraphicsApi::None;
    // The API's own object the surface is created against: a VkInstance for Vulkan. Null for None.
    void* api_instance = nullptr;
};

enum class WindowEventType {
    // The window manager asked for the window to close. Closing it is the application's decision.
    CloseRequested,
    Resized,
    Moved,
    FocusGained,
    FocusLost,
    // The window moved to another screen. Carries the new screen in `screen`.
    ScreenChanged,
    // The scale factor the window is rendered at changed — because it moved to a screen with a
    // different scale, or because the screen's own scale changed. Carries the new one in
    // `dpi_scale`, and the UI re-lays out at it.
    DpiChanged,
};

struct WindowEvent {
    WindowEventType type = WindowEventType::CloseRequested;
    WindowId window = kInvalidWindow;
    // The event's timestamp on the monotonic clock, taken as close to the platform's observation as
    // the platform allows.
    Nanoseconds timestamp = 0;
    Point position;                    // Moved
    Extent size;                       // Resized
    ScreenId screen = kInvalidScreen;  // ScreenChanged, DpiChanged
    f32 dpi_scale = 1.0F;              // DpiChanged
};

class DisplayServer {
public:
    DisplayServer() = default;
    virtual ~DisplayServer() = default;

    DisplayServer(const DisplayServer&) = delete;
    DisplayServer& operator=(const DisplayServer&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

    // Queried, never assumed. An implementation answers for the machine it is running on, not for
    // the platform in general: the same binary on X11 and on Wayland may answer differently.
    [[nodiscard]] virtual bool has_feature(Feature feature) const = 0;

    // --- Windows ----------------------------------------------------------------------------
    //
    // A flag the platform does not support is dropped with a warning and the window is created
    // anyway. window_flags() then reports what the window actually has, which is how a caller finds
    // out — the return of create_window() is a window, not a report.

    virtual Expected<WindowId, Error> create_window(const WindowDescription& description) = 0;
    virtual void destroy_window(WindowId window) = 0;
    [[nodiscard]] virtual bool window_exists(WindowId window) const = 0;

    virtual Expected<Point, Error> window_position(WindowId window) const = 0;
    virtual Status set_window_position(WindowId window, Point position) = 0;

    virtual Expected<Extent, Error> window_size(WindowId window) const = 0;
    virtual Status set_window_size(WindowId window, Extent size) = 0;

    // A zero component means "no limit on that axis".
    virtual Status set_window_minimum_size(WindowId window, Extent size) = 0;
    virtual Status set_window_maximum_size(WindowId window, Extent size) = 0;

    virtual Status set_window_title(WindowId window, const char* title) = 0;
    virtual Status set_window_icon(WindowId window, const IconImage& icon) = 0;

    virtual Expected<WindowMode, Error> window_mode(WindowId window) const = 0;
    virtual Status set_window_mode(WindowId window, WindowMode mode) = 0;

    // The flags the window has, which are the requested flags minus any the platform refused.
    virtual Expected<WindowFlags, Error> window_flags(WindowId window) const = 0;

    // The scale the window's content is drawn at: 1.0 at 96 dpi, 2.0 on a doubled screen.
    virtual Expected<f32, Error> window_dpi_scale(WindowId window) const = 0;
    virtual Expected<ScreenId, Error> window_screen(WindowId window) const = 0;

    virtual Expected<VSyncMode, Error> window_vsync(WindowId window) const = 0;
    virtual Status set_window_vsync(WindowId window, VSyncMode mode) = 0;

    // --- Screens ----------------------------------------------------------------------------

    [[nodiscard]] virtual usize screen_count() const = 0;
    virtual Expected<ScreenInfo, Error> screen(usize index) const = 0;
    virtual Expected<ScreenInfo, Error> screen_by_id(ScreenId id) const = 0;

    // --- Surfaces ---------------------------------------------------------------------------

    virtual Expected<NativeSurface, Error> create_surface(
        WindowId window, const SurfaceDescription& description) = 0;
    virtual void destroy_surface(const NativeSurface& surface) = 0;

    // --- Events -----------------------------------------------------------------------------
    //
    // pump_events() drains the operating system's queue into this server's own; poll_event() takes
    // one event from it and returns false when there are none left. Splitting them is what lets the
    // host pump once per frame and consume the whole frame's events in one loop.

    virtual void pump_events() = 0;
    virtual bool poll_event(WindowEvent& event) = 0;
    // Events dropped because the queue was full since the last call, and zero if none were. Loss is
    // reported rather than hidden — `diagnostics-profiling-and-crash` requires the same of the
    // trace, and for the same reason.
    virtual u32 take_dropped_event_count() = 0;
};

}  // namespace cy
