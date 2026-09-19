// The native X11 DisplayServer. See x11_display_server.h for why X11 and not Wayland.
//
// ================================================================================================
// THE INCLUDE ORDER IS LOAD-BEARING, AND SO IS THE #undef BELOW
// ================================================================================================
//
// X11/X.h defines `None`, `Always` and `Success` as OBJECT-LIKE MACROS. `GraphicsApi::None` and
// `WindowFlags::None` are enumerators of an engine-owned interface that predates this backend by
// eleven milestones, and a macro named `None` in scope turns both into `GraphicsApi::0L`. So the
// engine headers are included first, Xlib second, and the three macros are undefined immediately
// afterwards with the values they stood for spelled out as constants.
//
// This is a REAL FINDING about the porting surface and it is recorded rather than worked around
// quietly: a window system whose headers collide with an interface's vocabulary is exactly the kind
// of thing a second implementation discovers, and the cost of it here is nine lines in one file
// because `DisplayServer`'s vocabulary is scoped enumerations rather than bare constants. An
// interface that had spelled its enumerators `None`, `Resizable`, `Success` at namespace scope
// would have made this backend impossible to write without renaming the interface.

#include <cy/platform/x11_display_server.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

// THE THREE #undefs, AND THE THIRD IS THE SERIOUS ONE.
//
//   None      X11/X.h:  #define None 0L      — collides with GraphicsApi::None, WindowFlags::None
//   Always    X11/X.h:  #define Always 2     — a plausible name for an enumerator
//   Status    X11/Xlib.h:83: #define Status int — COLLIDES WITH cy::Status, the engine's universal
//             return type. Every `Status X11DisplayServer::set_window_*()` in this file became
//             `int ...`, and the compiler's diagnostic was "no declaration matches", nine times
//             over. It is a macro, not a typedef, so namespace scoping cannot save it.
//
// Success is undefined with them because X.h defines it as 0 and the word appears in this project's
// vocabulary as well.
#undef None
#undef Always
#undef Success
#undef Status

#include <dlfcn.h>
#include <unistd.h>

namespace cy {
namespace {

// What the three undefined macros stood for. Named rather than inlined so that a reader who greps
// for `XNone` finds this comment and not a bare 0.
constexpr unsigned long kXNone = 0UL;
constexpr int kXSuccess = 0;

Display* as_display(void* handle) {
    return static_cast<Display*>(handle);
}

::Window as_window(u64 handle) {
    return static_cast<::Window>(handle);
}

Nanoseconds monotonic_now() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<Nanoseconds>(now.tv_sec) * 1'000'000'000LL +
           static_cast<Nanoseconds>(now.tv_nsec);
}

Unexpected<Error> no_such_window() {
    return fail(ErrorCode::NotFound, "no such window");
}

void copy_name(char (&destination)[64], const char* text) {
    std::snprintf(destination, sizeof(destination), "%s", text != nullptr ? text : "");
}

// The atoms this backend interns, in one place so the indices and the names cannot drift apart.
enum AtomIndex : usize {
    kWmProtocols,
    kWmDeleteWindow,
    kWmState,
    kNetWmName,
    kUtf8String,
    kNetWmState,
    kNetWmStateFullscreen,
    kNetWmStateMaximizedVert,
    kNetWmStateMaximizedHorz,
    kNetWmStateAbove,
    kNetWmStateHidden,
    kNetWmIcon,
    kMotifWmHints,
    kNetWmWindowType,
    kNetWmWindowTypePopupMenu,
    kNetWmPid,
    kAtomCount,
};

constexpr const char* kAtomNames[kAtomCount] = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_STATE",
    "_NET_WM_NAME",
    "UTF8_STRING",
    "_NET_WM_STATE",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_ICON",
    "_MOTIF_WM_HINTS",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_PID",
};

static_assert(sizeof(kAtomNames) / sizeof(kAtomNames[0]) == kAtomCount,
              "every atom index needs a name");

// Motif's window hints, which is how every X11 toolkit since 1989 asks for a borderless window.
// There is no EWMH equivalent; `_NET_WM_WINDOW_TYPE_SPLASH` is a different request with different
// side effects. Five CARDINALs, of which only the second and third are read here.
struct MotifWmHints {
    unsigned long flags = 0;
    unsigned long functions = 0;
    unsigned long decorations = 0;
    long input_mode = 0;
    unsigned long status = 0;
};

constexpr unsigned long kMotifHintsDecorations = 1UL << 1;

// A window's `Xft.dpi` as a scale factor. X11 has no per-window and no per-screen scale: the
// resource database is the one number every toolkit on this window system agrees to read, and the
// physical size in millimetres that XRandR reports is famously wrong on projectors and on any
// display that lies in its EDID. So this reads Xft.dpi and falls back to 1.0 rather than
// manufacturing a scale from millimetres.
f32 read_xft_dpi_scale(Display* display) {
    char* resources = XResourceManagerString(display);
    if (resources == nullptr) {
        return 1.0F;
    }
    XrmDatabase database = XrmGetStringDatabase(resources);
    if (database == nullptr) {
        return 1.0F;
    }
    char* type = nullptr;
    XrmValue value{};
    f32 scale = 1.0F;
    if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) != 0 &&
        value.addr != nullptr) {
        const double dpi = std::strtod(value.addr, nullptr);
        if (dpi > 0.0) {
            scale = static_cast<f32>(dpi / 96.0);
        }
    }
    XrmDestroyDatabase(database);
    return scale;
}

// Whether a compositing manager owns the `_NET_WM_CM_S<screen>` selection. Transparency without one
// gives a window that is black where it should be see-through, and no warning.
bool compositor_present(Display* display, int screen) {
    char selection[32];
    std::snprintf(selection, sizeof(selection), "_NET_WM_CM_S%d", screen);
    const Atom atom = XInternAtom(display, selection, 0);
    return atom != kXNone && XGetSelectionOwner(display, atom) != kXNone;
}

bool has_argb_visual(Display* display, int screen) {
    XVisualInfo request{};
    request.screen = screen;
    request.depth = 32;
    request.c_class = TrueColor;
    int count = 0;
    XVisualInfo* found = XGetVisualInfo(
        display, VisualScreenMask | VisualDepthMask | VisualClassMask, &request, &count);
    if (found != nullptr) {
        XFree(found);
    }
    return count > 0;
}

// --- The Vulkan surface seam
// ----------------------------------------------------------------------
//
// `VK_KHR_xlib_surface`, reached through the loader by name. This file declares the three types it
// needs itself and includes no Vulkan header — which is exactly what SDL3 does in SDL_vulkan.h, and
// what keeps tools/layercheck's `gpuapi` rule true for platform/ as well as for everything above
// it. A non-dispatchable Vulkan handle is 64 bits on every platform this engine targets, and the
// `DisplayServer` seam carries it as a void*.

using VkInstanceHandle = void*;
using VkSurfaceHandle = u64;
using PFN_vkVoidFunction = void (*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction (*)(VkInstanceHandle, const char*);

// VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR.
constexpr u32 kXlibSurfaceCreateInfo = 1000004000U;

struct VkXlibSurfaceCreateInfo {
    u32 type = kXlibSurfaceCreateInfo;
    const void* next = nullptr;
    u32 flags = 0;
    Display* display = nullptr;
    ::Window window = 0;
};

using PFN_vkCreateXlibSurfaceKHR = i32 (*)(VkInstanceHandle, const VkXlibSurfaceCreateInfo*,
                                           const void*, VkSurfaceHandle*);
using PFN_vkDestroySurfaceKHR = void (*)(VkInstanceHandle, VkSurfaceHandle, const void*);

// The loader, opened once and never closed: unloading it while an instance created through it is
// alive is undefined, and nothing here can know that it is not.
PFN_vkGetInstanceProcAddr vulkan_loader() {
    static PFN_vkGetInstanceProcAddr address = [] {
        void* library = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            library = ::dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (library == nullptr) {
            return static_cast<PFN_vkGetInstanceProcAddr>(nullptr);
        }
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            ::dlsym(library, "vkGetInstanceProcAddr"));
    }();
    return address;
}

}  // namespace

X11DisplayServer::~X11DisplayServer() {
    if (initialised_) {
        shutdown();
    }
}

Status X11DisplayServer::initialise() {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the X11 display server is already initialised");
    }
    // Threaded Xlib, because the engine's jobs system exists and a future caller reaching Xlib from
    // two threads must not corrupt the connection silently. It is a no-op when already called.
    XInitThreads();

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "no X display: $DISPLAY names nothing this process can connect to. Use the "
                    "headless display server, which is what continuous integration runs");
    }

    display_ = display;
    default_screen_ = DefaultScreen(display);
    root_ = RootWindow(display, default_screen_);

    for (usize i = 0; i < kAtomCount; ++i) {
        atoms_[i] = XInternAtom(display, kAtomNames[i], 0);
    }

    int randr_event_base = 0;
    int randr_error_base = 0;
    randr_ = XRRQueryExtension(display, &randr_event_base, &randr_error_base) != 0;
    compositor_ =
        has_argb_visual(display, default_screen_) && compositor_present(display, default_screen_);
    dpi_scale_ = read_xft_dpi_scale(display);

    initialised_ = true;
    refresh_screens();
    return ok();
}

void X11DisplayServer::shutdown() {
    if (!initialised_) {
        return;
    }
    for (Window& window : windows_) {
        if (window.alive) {
            XDestroyWindow(as_display(display_), as_window(window.handle));
        }
        window = Window{};
    }
    events_.clear();
    (void)events_.take_dropped_count();
    XCloseDisplay(as_display(display_));
    display_ = nullptr;
    next_id_ = 1;
    screen_count_ = 0;
    initialised_ = false;
}

std::string_view X11DisplayServer::vendor() const {
    if (!initialised_) {
        return {};
    }
    const char* text = ServerVendor(as_display(display_));
    return text != nullptr ? std::string_view{text} : std::string_view{};
}

void X11DisplayServer::set_event_observer(X11EventObserver observer, void* user) {
    observer_ = observer;
    observer_user_ = user;
}

// --- Capabilities -----------------------------------------------------------------------------
//
// Every `false` below is a DEGRADATION WITH A REASON, in the arrangement sdl3_display_server.h
// describes: one case, one sentence, and a caller that asked would have been told. The interface's
// rule is that an unsupported flag is dropped with a warning and the window is still created.

bool X11DisplayServer::has_feature(Feature feature) const {
    switch (feature) {
        case Feature::WindowResizable:
        case Feature::WindowBorderless:   // _MOTIF_WM_HINTS, which every X11 window manager reads
        case Feature::WindowAlwaysOnTop:  // _NET_WM_STATE_ABOVE
        case Feature::WindowNoFocus:      // WM_HINTS input = False
        case Feature::WindowPopup:        // override-redirect
            return true;

        // X11 hands out pixels and never logical units, so a window's back buffer is always at the
        // screen's pixel density. The flag is honoured by being unavoidable.
        case Feature::HighDpi:
            return true;

        // Both need a compositing manager AND a 32-bit visual, and both are checked rather than
        // assumed at initialise().
        case Feature::WindowTransparency:
            return compositor_;

        case Feature::ScreenRefreshRate:
            return randr_;

        // NOT IMPLEMENTED, AND EACH FOR A STATED REASON RATHER THAN FOR WANT OF TIME:
        //
        //   PerScreenDpiScale   X11 has one resource database for the whole display. Xft.dpi is a
        //                       display-wide number; a per-screen scale would have to be invented.
        //   ExclusiveFullscreen a mode change through XRandR, which changes the state of the user's
        //                       desktop and must be undone on every exit path including a crash.
        //                       `WindowMode::Fullscreen` — borderless at the desktop resolution —
        //                       is supported and is what a game should want.
        //   MousePassthrough    XFixes' input shape region. One more library for one flag nothing
        //                       in this engine sets today.
        //   VSyncAdaptive,      V-sync on X11 is the graphics API's presentation mode, not the
        //   VSyncMailbox        window system's. set_window_vsync() records the request; the RHI's
        //                       swapchain is what honours it.
        //   VulkanSurface       answered by whether the Vulkan loader is on this machine, below.
        //   Clipboard and the   system-integration group. Every implementation in this tree answers
        //   rest                false for these, and this one has no reason to be the first not to.
        case Feature::VulkanSurface:
            return vulkan_loader() != nullptr;

        default:
            return false;
    }
}

// --- Windows ------------------------------------------------------------------------------------

X11DisplayServer::Window* X11DisplayServer::find(WindowId window) {
    if (window == kInvalidWindow) {
        return nullptr;
    }
    for (Window& candidate : windows_) {
        if (candidate.alive && candidate.id == window) {
            return &candidate;
        }
    }
    return nullptr;
}

const X11DisplayServer::Window* X11DisplayServer::find(WindowId window) const {
    return const_cast<X11DisplayServer*>(this)->find(window);
}

X11DisplayServer::Window* X11DisplayServer::find_by_handle(u64 handle) {
    for (Window& candidate : windows_) {
        if (candidate.alive && candidate.handle == handle) {
            return &candidate;
        }
    }
    return nullptr;
}

void X11DisplayServer::set_net_wm_state(u64 handle, u64 first, u64 second, bool enable) {
    XEvent event{};
    event.type = ClientMessage;
    event.xclient.window = as_window(handle);
    event.xclient.message_type = static_cast<Atom>(atoms_[kNetWmState]);
    event.xclient.format = 32;
    event.xclient.data.l[0] = enable ? 1 : 0;  // _NET_WM_STATE_ADD / _REMOVE
    event.xclient.data.l[1] = static_cast<long>(first);
    event.xclient.data.l[2] = static_cast<long>(second);
    event.xclient.data.l[3] = 1;  // the request came from the application
    XSendEvent(as_display(display_), as_window(root_), 0,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

void X11DisplayServer::apply_flags(u64 handle, WindowFlags flags) {
    Display* display = as_display(display_);

    if (has_flag(flags, WindowFlags::Borderless)) {
        MotifWmHints hints;
        hints.flags = kMotifHintsDecorations;
        hints.decorations = 0;
        XChangeProperty(display, as_window(handle), static_cast<Atom>(atoms_[kMotifWmHints]),
                        static_cast<Atom>(atoms_[kMotifWmHints]), 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&hints), 5);
    }

    if (has_flag(flags, WindowFlags::NoFocus)) {
        XWMHints hints{};
        hints.flags = InputHint;
        hints.input = 0;
        XSetWMHints(display, as_window(handle), &hints);
    }

    if (has_flag(flags, WindowFlags::AlwaysOnTop)) {
        set_net_wm_state(handle, atoms_[kNetWmStateAbove], kXNone, true);
    }

    if (has_flag(flags, WindowFlags::Popup)) {
        const Atom type = static_cast<Atom>(atoms_[kNetWmWindowTypePopupMenu]);
        XChangeProperty(display, as_window(handle), static_cast<Atom>(atoms_[kNetWmWindowType]),
                        XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&type),
                        1);
    }
}

void X11DisplayServer::apply_size_hints(const Window& window) {
    XSizeHints hints{};
    hints.flags = PPosition | PSize;
    hints.x = window.position.x;
    hints.y = window.position.y;
    hints.width = window.size.width;
    hints.height = window.size.height;
    // A window that is not resizable is one whose minimum and maximum are its size. That is how
    // ICCCM says it, and there is no other way to say it.
    if (!has_flag(window.flags, WindowFlags::Resizable)) {
        hints.flags |= PMinSize | PMaxSize;
        hints.min_width = hints.max_width = window.size.width;
        hints.min_height = hints.max_height = window.size.height;
    }
    XSetWMNormalHints(as_display(display_), as_window(window.handle), &hints);
}

Expected<WindowId, Error> X11DisplayServer::create_window(const WindowDescription& description) {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "the X11 display server is not initialised");
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
                    "the X11 display server holds X11DisplayServer::kMaxWindows windows");
    }

    Display* display = as_display(display_);
    const WindowFlags flags = filter_unsupported_flags(*this, description.flags);

    XSetWindowAttributes attributes{};
    attributes.background_pixel = BlackPixel(display, default_screen_);
    attributes.border_pixel = 0;
    attributes.event_mask = StructureNotifyMask | FocusChangeMask | ExposureMask | KeyPressMask |
                            KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask;
    // Popup windows bypass the window manager entirely, which is what override-redirect means and
    // what a menu needs.
    attributes.override_redirect = has_flag(flags, WindowFlags::Popup) ? 1 : 0;

    const Point position = description.use_position ? description.position : Point{0, 0};
    const ::Window handle =
        XCreateWindow(display, as_window(root_), position.x, position.y,
                      static_cast<unsigned>(description.size.width),
                      static_cast<unsigned>(description.size.height), 0, CopyFromParent,
                      InputOutput, CopyFromParent,
                      CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect, &attributes);
    if (handle == kXNone) {
        return fail(ErrorCode::Unavailable, "XCreateWindow refused");
    }

    // WM_DELETE_WINDOW, which is what turns the close button into an event rather than into a
    // connection the X server drops under the application's feet.
    Atom protocols[] = {static_cast<Atom>(atoms_[kWmDeleteWindow])};
    XSetWMProtocols(display, handle, protocols, 1);

    const long pid = static_cast<long>(::getpid());
    XChangeProperty(display, handle, static_cast<Atom>(atoms_[kNetWmPid]), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);

    *slot = Window{};
    slot->alive = true;
    slot->id = next_id_++;
    slot->handle = static_cast<u64>(handle);
    slot->flags = flags;
    slot->vsync = description.vsync;
    slot->size = description.size;
    slot->position = position;
    slot->dpi_scale = dpi_scale_;
    slot->screen = screen_containing(position, description.size);

    apply_flags(slot->handle, flags);
    apply_size_hints(*slot);
    (void)set_window_title(slot->id, description.title);

    XMapWindow(display, handle);
    XFlush(display);

    if (description.mode != WindowMode::Windowed) {
        (void)set_window_mode(slot->id, description.mode);
    } else {
        slot->mode = WindowMode::Windowed;
    }
    return slot->id;
}

void X11DisplayServer::destroy_window(WindowId window) {
    Window* record = find(window);
    if (record == nullptr) {
        return;
    }
    XDestroyWindow(as_display(display_), as_window(record->handle));
    XFlush(as_display(display_));
    *record = Window{};
}

bool X11DisplayServer::window_exists(WindowId window) const {
    return find(window) != nullptr;
}

Expected<Point, Error> X11DisplayServer::window_position(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    // The window manager reparents the window into a frame, so the window's own x and y are its
    // position INSIDE that frame — usually (0, 0) — and not on the desktop. Translating to the root
    // is the only answer that means what the interface says it means.
    int x = 0;
    int y = 0;
    ::Window child = kXNone;
    if (XTranslateCoordinates(as_display(display_), as_window(record->handle), as_window(root_), 0,
                              0, &x, &y, &child) == 0) {
        return record->position;
    }
    return Point{x, y};
}

Status X11DisplayServer::set_window_position(WindowId window, Point position) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    XMoveWindow(as_display(display_), as_window(record->handle), position.x, position.y);
    XFlush(as_display(display_));
    return ok();
}

Expected<Extent, Error> X11DisplayServer::window_size(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(as_display(display_), as_window(record->handle), &attributes) == 0) {
        return record->size;
    }
    return Extent{attributes.width, attributes.height};
}

Status X11DisplayServer::set_window_size(WindowId window, Extent size) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (size.width <= 0 || size.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a window needs a positive width and height");
    }
    record->size = size;
    // The hints first: a window whose minimum and maximum are its old size cannot be resized, and
    // the request would be refused by the window manager rather than by X.
    apply_size_hints(*record);
    XResizeWindow(as_display(display_), as_window(record->handle),
                  static_cast<unsigned>(size.width), static_cast<unsigned>(size.height));
    XFlush(as_display(display_));
    return ok();
}

Status X11DisplayServer::set_window_minimum_size(WindowId window, Extent size) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    XSizeHints hints{};
    long supplied = 0;
    XGetWMNormalHints(as_display(display_), as_window(record->handle), &hints, &supplied);
    // A zero component means "no limit on that axis", which ICCCM spells as the hint being absent.
    if (size.width > 0 || size.height > 0) {
        hints.flags |= PMinSize;
        hints.min_width = size.width;
        hints.min_height = size.height;
    } else {
        hints.flags &= ~PMinSize;
    }
    XSetWMNormalHints(as_display(display_), as_window(record->handle), &hints);
    return ok();
}

Status X11DisplayServer::set_window_maximum_size(WindowId window, Extent size) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    XSizeHints hints{};
    long supplied = 0;
    XGetWMNormalHints(as_display(display_), as_window(record->handle), &hints, &supplied);
    if (size.width > 0 || size.height > 0) {
        hints.flags |= PMaxSize;
        hints.max_width = size.width;
        hints.max_height = size.height;
    } else {
        hints.flags &= ~PMaxSize;
    }
    XSetWMNormalHints(as_display(display_), as_window(record->handle), &hints);
    return ok();
}

Status X11DisplayServer::set_window_title(WindowId window, const char* title) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    const char* text = title != nullptr ? title : "";
    Display* display = as_display(display_);
    // _NET_WM_NAME in UTF-8 is what a modern window manager reads; WM_NAME in Latin-1 is what the
    // ICCCM requires and what xterm-era tools still read. Both are set, which is what every toolkit
    // does and what stops the title from being mojibake in one place and right in another.
    XChangeProperty(display, as_window(record->handle), static_cast<Atom>(atoms_[kNetWmName]),
                    static_cast<Atom>(atoms_[kUtf8String]), 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(text),
                    static_cast<int>(std::strlen(text)));
    XStoreName(display, as_window(record->handle), text);
    XFlush(display);
    return ok();
}

Status X11DisplayServer::set_window_icon(WindowId window, const IconImage& icon) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (icon.pixels == nullptr || icon.width <= 0 || icon.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "a window icon needs pixels and a positive size");
    }
    // _NET_WM_ICON is width, height, then one 32-bit ARGB pixel per element — but the property
    // format is 32, which on a 64-bit host means Xlib reads an array of LONGS and takes the low 32
    // bits of each. Getting that wrong gives a half-drawn icon, and it is the single most common
    // X11 mistake in this call.
    const usize count = static_cast<usize>(icon.width) * static_cast<usize>(icon.height);
    constexpr usize kMaxIconPixels = 256 * 256;
    if (count > kMaxIconPixels) {
        return fail(ErrorCode::InvalidArgument, "a window icon is at most 256 by 256 pixels");
    }
    // Static because half a megabyte of longs is not a stack frame. A DisplayServer is used from
    // the thread that pumps it — the same single-producer contract the event queue documents — so
    // one buffer is enough and two callers racing here would already be a contract violation.
    static long buffer[kMaxIconPixels + 2];
    buffer[0] = icon.width;
    buffer[1] = icon.height;
    for (usize i = 0; i < count; ++i) {
        const u8* pixel = icon.pixels + i * 4;
        buffer[i + 2] = (static_cast<long>(pixel[3]) << 24) | (static_cast<long>(pixel[0]) << 16) |
                        (static_cast<long>(pixel[1]) << 8) | static_cast<long>(pixel[2]);
    }
    XChangeProperty(as_display(display_), as_window(record->handle),
                    static_cast<Atom>(atoms_[kNetWmIcon]), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(buffer), static_cast<int>(count + 2));
    XFlush(as_display(display_));
    return ok();
}

Expected<WindowMode, Error> X11DisplayServer::window_mode(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    return record->mode;
}

Status X11DisplayServer::set_window_mode(WindowId window, WindowMode mode) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    if (mode == WindowMode::ExclusiveFullscreen) {
        return fail(ErrorCode::Unsupported,
                    "exclusive fullscreen changes the desktop's video mode through XRandR and this "
                    "backend does not; has_feature(ExclusiveFullscreen) answers false, and "
                    "WindowMode::Fullscreen is borderless at the desktop resolution");
    }

    Display* display = as_display(display_);
    const ::Window handle = as_window(record->handle);

    // Leaving whatever the window is in now, before entering the next one. A window that is both
    // fullscreen and maximised is a window whose size nobody can predict.
    set_net_wm_state(record->handle, atoms_[kNetWmStateFullscreen], kXNone, false);
    set_net_wm_state(record->handle, atoms_[kNetWmStateMaximizedVert],
                     atoms_[kNetWmStateMaximizedHorz], false);

    switch (mode) {
        case WindowMode::Windowed:
            XMapWindow(display, handle);
            break;
        case WindowMode::Minimised:
            XIconifyWindow(display, handle, default_screen_);
            break;
        case WindowMode::Maximised:
            set_net_wm_state(record->handle, atoms_[kNetWmStateMaximizedVert],
                             atoms_[kNetWmStateMaximizedHorz], true);
            break;
        case WindowMode::Fullscreen:
            set_net_wm_state(record->handle, atoms_[kNetWmStateFullscreen], kXNone, true);
            break;
        case WindowMode::ExclusiveFullscreen:
            break;  // refused above
    }
    XFlush(display);
    record->mode = mode;
    return ok();
}

Expected<WindowFlags, Error> X11DisplayServer::window_flags(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    return record->flags;
}

Expected<f32, Error> X11DisplayServer::window_dpi_scale(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    return record->dpi_scale;
}

Expected<ScreenId, Error> X11DisplayServer::window_screen(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    return record->screen;
}

Expected<VSyncMode, Error> X11DisplayServer::window_vsync(WindowId window) const {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    return record->vsync;
}

Status X11DisplayServer::set_window_vsync(WindowId window, VSyncMode mode) {
    Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }
    // RECORDED, NOT ENACTED, and said out loud rather than returning ok() over nothing. On X11 the
    // presentation interval belongs to the graphics API — a Vulkan present mode, a GLX swap
    // interval — and the window system has no say in it. The value is stored so that whoever
    // creates the swapchain can read the request back through this interface rather than inventing
    // its own channel. Adaptive and mailbox are refused here because has_feature() answers false
    // for them, and a caller that asked would have been told.
    if (mode == VSyncMode::Adaptive || mode == VSyncMode::Mailbox) {
        return fail(ErrorCode::Unsupported,
                    "this display server records V-sync for the RHI to honour and cannot promise "
                    "adaptive or mailbox presentation; has_feature() answers false for both");
    }
    record->vsync = mode;
    return ok();
}

// --- Screens
// --------------------------------------------------------------------------------------

void X11DisplayServer::refresh_screens() {
    screen_count_ = 0;
    Display* display = as_display(display_);

    if (randr_) {
        XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, as_window(root_));
        if (resources != nullptr) {
            for (int i = 0; i < resources->ncrtc && screen_count_ < kMaxScreens; ++i) {
                XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, resources->crtcs[i]);
                if (crtc == nullptr) {
                    continue;
                }
                // A CRTC with no mode is a disconnected output, not a screen of zero size.
                if (crtc->mode != kXNone && crtc->width > 0 && crtc->height > 0) {
                    ScreenInfo& info = screens_[screen_count_];
                    info = ScreenInfo{};
                    info.id = static_cast<ScreenId>(screen_count_ + 1);
                    info.position = Point{crtc->x, crtc->y};
                    info.resolution =
                        Extent{static_cast<i32>(crtc->width), static_cast<i32>(crtc->height)};
                    info.dpi_scale = dpi_scale_;
                    for (int m = 0; m < resources->nmode; ++m) {
                        const XRRModeInfo& mode = resources->modes[m];
                        if (mode.id != crtc->mode || mode.hTotal == 0 || mode.vTotal == 0) {
                            continue;
                        }
                        info.refresh_rate_hz =
                            static_cast<f32>(mode.dotClock) /
                            (static_cast<f32>(mode.hTotal) * static_cast<f32>(mode.vTotal));
                        break;
                    }
                    char name[64];
                    std::snprintf(name, sizeof(name), "crtc-%d", i);
                    copy_name(info.name, name);
                    ++screen_count_;
                }
                XRRFreeCrtcInfo(crtc);
            }
            XRRFreeScreenResources(resources);
        }
    }

    if (screen_count_ == 0) {
        // No RandR, or a server that reports no active CRTC. One screen, at the display's
        // dimensions, with no refresh rate — and zero is what `ScreenInfo` says "unknown" is.
        ScreenInfo& info = screens_[0];
        info = ScreenInfo{};
        info.id = 1;
        info.resolution =
            Extent{DisplayWidth(display, default_screen_), DisplayHeight(display, default_screen_)};
        info.dpi_scale = dpi_scale_;
        copy_name(info.name, "x11-default");
        screen_count_ = 1;
    }
}

const ScreenInfo* X11DisplayServer::screen_record(ScreenId id) const {
    for (usize i = 0; i < screen_count_; ++i) {
        if (screens_[i].id == id) {
            return &screens_[i];
        }
    }
    return nullptr;
}

ScreenId X11DisplayServer::screen_containing(Point position, Extent size) const {
    const Point centre{position.x + size.width / 2, position.y + size.height / 2};
    for (usize i = 0; i < screen_count_; ++i) {
        const ScreenInfo& info = screens_[i];
        if (centre.x >= info.position.x && centre.x < info.position.x + info.resolution.width &&
            centre.y >= info.position.y && centre.y < info.position.y + info.resolution.height) {
            return info.id;
        }
    }
    return screen_count_ > 0 ? screens_[0].id : kInvalidScreen;
}

Expected<ScreenInfo, Error> X11DisplayServer::screen(usize index) const {
    if (index >= screen_count_) {
        return fail(ErrorCode::OutOfRange, "no screen at that index");
    }
    return screens_[index];
}

Expected<ScreenInfo, Error> X11DisplayServer::screen_by_id(ScreenId id) const {
    const ScreenInfo* info = screen_record(id);
    if (info == nullptr) {
        return fail(ErrorCode::NotFound, "no such screen");
    }
    return *info;
}

// --- Surfaces
// -------------------------------------------------------------------------------------

Expected<NativeSurface, Error> X11DisplayServer::create_surface(
    WindowId window, const SurfaceDescription& description) {
    const Window* record = find(window);
    if (record == nullptr) {
        return no_such_window();
    }

    if (description.api == GraphicsApi::Vulkan) {
        PFN_vkGetInstanceProcAddr get_proc = vulkan_loader();
        if (get_proc == nullptr) {
            return fail(ErrorCode::Unsupported,
                        "no Vulkan loader on this machine; has_feature(VulkanSurface) answers "
                        "false and the RHI falls back to its null backend");
        }
        if (description.api_instance == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "a Vulkan surface is created against a VkInstance; pass it in "
                        "SurfaceDescription::api_instance");
        }
        auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
            get_proc(description.api_instance, "vkCreateXlibSurfaceKHR"));
        if (create == nullptr) {
            return fail(ErrorCode::Unsupported,
                        "this Vulkan instance was created without VK_KHR_xlib_surface, so it "
                        "cannot present to an X11 window");
        }
        VkXlibSurfaceCreateInfo info;
        info.display = as_display(display_);
        info.window = as_window(record->handle);
        VkSurfaceHandle created = 0;
        if (create(description.api_instance, &info, nullptr, &created) != 0) {
            return fail(ErrorCode::Unavailable, "vkCreateXlibSurfaceKHR refused this window");
        }
        NativeSurface surface;
        surface.api = GraphicsApi::Vulkan;
        // A non-dispatchable Vulkan handle is 64 bits and the seam carries it as a void*; the RHI
        // does the matching cast and never asks how the surface was made.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        surface.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(created));
        surface.display = description.api_instance;
        return surface;
    }

    if (description.api != GraphicsApi::None) {
        return fail(ErrorCode::NotImplemented,
                    "Metal and D3D12 have no surface on X11; has_feature() answers false for both");
    }

    NativeSurface surface;
    surface.api = GraphicsApi::None;
    // X11 names a window by number, not by pointer. NativeSurface::handle is the one opaque field
    // every backend's handle passes through; it is carried, never dereferenced.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    surface.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(record->handle));
    surface.display = display_;
    return surface;
}

void X11DisplayServer::destroy_surface(const NativeSurface& surface) {
    // GraphicsApi::None owns nothing: those handles belong to the window and to this server. A
    // Vulkan surface is an object created against an instance, and whoever created it owns it.
    if (surface.api != GraphicsApi::Vulkan || surface.handle == nullptr ||
        surface.display == nullptr) {
        return;
    }
    PFN_vkGetInstanceProcAddr get_proc = vulkan_loader();
    if (get_proc == nullptr) {
        return;
    }
    auto destroy =
        reinterpret_cast<PFN_vkDestroySurfaceKHR>(get_proc(surface.display, "vkDestroySurfaceKHR"));
    if (destroy != nullptr) {
        destroy(surface.display,
                static_cast<VkSurfaceHandle>(reinterpret_cast<std::uintptr_t>(surface.handle)),
                nullptr);
    }
}

// --- Events
// ---------------------------------------------------------------------------------------

void X11DisplayServer::pump_events() {
    if (!initialised_) {
        return;
    }
    Display* display = as_display(display_);
    // XPending flushes the output buffer first, so a request made this frame is on the wire before
    // its reply is waited for. A loop on QLength() instead would hang on a freshly created window.
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        // The observer sees EVERY event, before this server decides whether it is a window event.
        // That is what lets the input source exist without a second XNextEvent loop — two consumers
        // of one queue is the defect sdl3_input_source.h describes, and X11 has no event watch.
        if (observer_ != nullptr) {
            observer_(&event, observer_user_);
        }
        translate_event(&event);
    }
}

void X11DisplayServer::push(WindowEventType type, const Window& window, Nanoseconds timestamp) {
    WindowEvent event;
    event.type = type;
    event.window = window.id;
    event.timestamp = timestamp;
    event.position = window.position;
    event.size = window.size;
    event.screen = window.screen;
    event.dpi_scale = window.dpi_scale;
    events_.push(event);
}

void X11DisplayServer::apply_configure(Window& window, i32 x, i32 y, i32 width, i32 height,
                                       Nanoseconds now) {
    if (width != window.size.width || height != window.size.height) {
        window.size = Extent{width, height};
        push(WindowEventType::Resized, window, now);
    }
    if (x != window.position.x || y != window.position.y) {
        window.position = Point{x, y};
        push(WindowEventType::Moved, window, now);
    }
    const ScreenId screen = screen_containing(window.position, window.size);
    if (screen != window.screen) {
        window.screen = screen;
        push(WindowEventType::ScreenChanged, window, now);
    }
}

void X11DisplayServer::translate_event(const void* x_event) {
    const auto& event = *static_cast<const XEvent*>(x_event);
    const Nanoseconds now = monotonic_now();

    switch (event.type) {
        case ClientMessage: {
            if (event.xclient.message_type != static_cast<Atom>(atoms_[kWmProtocols]) ||
                static_cast<Atom>(event.xclient.data.l[0]) !=
                    static_cast<Atom>(atoms_[kWmDeleteWindow])) {
                return;
            }
            if (Window* record = find_by_handle(static_cast<u64>(event.xclient.window));
                record != nullptr) {
                // The window manager asked. Closing the window is the application's decision, which
                // is why this is an event and not a destruction.
                push(WindowEventType::CloseRequested, *record, now);
            }
            return;
        }

        case ConfigureNotify: {
            Window* record = find_by_handle(static_cast<u64>(event.xconfigure.window));
            if (record == nullptr) {
                return;
            }
            // send_event is set when the window manager synthesised the event, and only then are x
            // and y in root coordinates. A real ConfigureNotify carries the position inside the
            // frame, which is (0, 0) under most window managers — pushing it as a Moved event would
            // report every window as being at the origin.
            const bool root_relative = event.xconfigure.send_event != 0;
            const i32 x = root_relative ? event.xconfigure.x : record->position.x;
            const i32 y = root_relative ? event.xconfigure.y : record->position.y;
            apply_configure(*record, x, y, event.xconfigure.width, event.xconfigure.height, now);
            return;
        }

        case FocusIn:
        case FocusOut: {
            if (Window* record = find_by_handle(static_cast<u64>(event.xfocus.window));
                record != nullptr) {
                push(event.type == FocusIn ? WindowEventType::FocusGained
                                           : WindowEventType::FocusLost,
                     *record, now);
            }
            return;
        }

        default:
            // Key, button, motion: the input source's, through the observer. Everything else —
            // Expose, MapNotify, PropertyNotify — has no shape in this interface.
            return;
    }
}

}  // namespace cy
