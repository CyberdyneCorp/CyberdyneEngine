// SPDX-License-Identifier: MIT
// The native Linux platform backend, driven for real. M11.d task 4.1.
//
// ================================================================================================
// WHAT THIS SUITE IS ACTUALLY FOR
// ================================================================================================
//
// Not "does Xlib work". It is the evidence that `Platform` and `DisplayServer` can be satisfied by
// an implementation that shares no code and no library with the one they were designed against —
// which is the claim `core-platform-abstraction`'s Complete cell rests on and which nothing could
// check while SDL3 was the only implementation in the tree.
//
// So the assertions are about the INTERFACE'S CONTRACT rather than about X11: a text call that
// refuses a short buffer instead of truncating, a monotonic clock that does not go backwards, a
// child that can be written to and read from, a window whose flags report what the window actually
// got. Every one of them is a sentence in cy/core/platform/platform.h or display_server.h.
//
// ================================================================================================
// THE DISPLAY HALF SKIPS LOUDLY, AND THE PROCESS HALF NEVER DOES
// ================================================================================================
//
// `Platform` needs no window system, so its cases run on every machine including a CI container.
// `X11DisplayServer` needs an X server, and a suite that FAILED there is a suite somebody disables
// — the same argument tests/render/device.h makes about a GPU. The skip prints why, so "the suite
// passed" and "the suite found no display" are never confusable.

#include <cy/core/memory/system_allocator.h>
#include <cy/platform/linux_platform.h>
#include <cy/platform/x11_display_server.h>
#include <cy/platform/x11_input_source.h>
#include <cy/test/test.h>

#include <X11/Xlib.h>

#undef None
#undef Always
#undef Success
// X11/Xlib.h:83 is `#define Status int`, which collides with cy::Status — see
// x11_display_server.cpp's note. A macro, so namespace scoping cannot save it.
#undef Status

#include <cstdio>
#include <cstdlib>
#include <cstring>

using cy::f32;
using cy::i32;
using cy::u32;
using cy::usize;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// Whether there is an X server to talk to. Checked by the same call the backend makes, rather than
/// by reading $DISPLAY: a variable that names a server nobody is running is the case that matters.
bool display_available() noexcept {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }
    XCloseDisplay(display);
    return true;
}

bool announce_skip(const char* what) noexcept {
    std::fprintf(stderr,
                 "linux-native: SKIPPING %s — no X display on this machine. The Platform cases in "
                 "this suite still ran; only the window-system ones were skipped.\n",
                 what);
    return true;
}

/// A display server with one mapped window, torn down with the case.
struct WindowFixture {
    ~WindowFixture() { server.shutdown(); }

    [[nodiscard]] bool open() noexcept {
        if (!server.initialise().has_value()) {
            return false;
        }
        cy::WindowDescription description;
        description.title = "cyberdyne linux-native test";
        description.size = cy::Extent{320, 240};
        auto created = server.create_window(description);
        if (!created) {
            return false;
        }
        window = created.value();
        // One pump so the window is mapped and the server has answered before anything is asked of
        // it. Without it the first XGetWindowAttributes can race the MapNotify.
        server.pump_events();
        return true;
    }

    cy::X11DisplayServer server;
    cy::WindowId window = cy::kInvalidWindow;
};

}  // namespace

// --- Platform: no window system needed --------------------------------------------------------

CY_TEST_CASE(
    "linux-native: the platform's name is its own, and it needs no initialisation ritual") {
    cy::LinuxPlatform platform;
    CY_CHECK(platform.name() == "linux-native");
    char* arguments[] = {const_cast<char*>("test"), nullptr};
    CY_REQUIRE(platform.initialise(1, arguments).has_value());
    // Twice is an error rather than a silent reset — the same contract Sdl3Platform states, because
    // a caller that can swap the two must not have to read two contracts.
    const cy::Status again = platform.initialise(1, arguments);
    CY_CHECK_FALSE(again.has_value());
    CY_CHECK_EQ(again.error().code, cy::ErrorCode::AlreadyExists);
    CY_CHECK_EQ(platform.argument_count(), usize{1});
    CY_CHECK(platform.argument(0) == "test");
    // Past the end is empty rather than out of bounds.
    CY_CHECK(platform.argument(7).empty());
    platform.shutdown();
}

CY_TEST_CASE("linux-native: a text call refuses a short buffer rather than truncating into it") {
    cy::LinuxPlatform platform;
    CY_REQUIRE(platform.initialise(0, nullptr).has_value());
    CY_REQUIRE(
        platform.set_environment_variable("CY_LINUX_NATIVE_TEST", "a-long-value").has_value());

    char small[4];
    const auto refused =
        platform.environment_variable("CY_LINUX_NATIVE_TEST", small, sizeof(small));
    CY_REQUIRE_FALSE(refused.has_value());
    // BufferTooSmall and not a truncated answer. A caller that got "a-l" would use it.
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::BufferTooSmall);

    char big[64];
    const auto written = platform.environment_variable("CY_LINUX_NATIVE_TEST", big, sizeof(big));
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(std::strcmp(big, "a-long-value"), 0);
    CY_CHECK_EQ(written.value(), std::strlen("a-long-value"));

    const auto absent =
        platform.environment_variable("CY_NOT_SET_ANYWHERE_12345", big, sizeof(big));
    CY_REQUIRE_FALSE(absent.has_value());
    CY_CHECK_EQ(absent.error().code, cy::ErrorCode::NotFound);
    platform.shutdown();
}

CY_TEST_CASE("linux-native: the three user directories exist and end in a separator") {
    cy::LinuxPlatform platform;
    CY_REQUIRE(platform.initialise(0, nullptr).has_value());

    char data[1024];
    char config[1024];
    char cache[1024];
    CY_REQUIRE(platform.user_data_directory(data, sizeof(data)).has_value());
    CY_REQUIRE(platform.user_config_directory(config, sizeof(config)).has_value());
    CY_REQUIRE(platform.user_cache_directory(cache, sizeof(cache)).has_value());

    for (const char* path : {data, config, cache}) {
        const usize length = std::strlen(path);
        CY_REQUIRE(length > 0);
        // "Each ends in the platform's separator" — platform.h. A caller concatenates a file name
        // onto it, and one missing slash is a file written beside the directory instead of in it.
        CY_CHECK_EQ(path[length - 1], '/');
    }
    // Config and cache are UNDER data, which is the engine's own convention and the one the SDL3
    // backend follows. Two backends that disagreed here would put a player's saves in two places.
    CY_CHECK_EQ(std::strncmp(config, data, std::strlen(data)), 0);
    CY_CHECK_EQ(std::strncmp(cache, data, std::strlen(data)), 0);
    platform.shutdown();
}

CY_TEST_CASE("linux-native: the executable path is this binary, and the clocks are two clocks") {
    cy::LinuxPlatform platform;
    CY_REQUIRE(platform.initialise(0, nullptr).has_value());

    char path[1024];
    const auto written = platform.executable_path(path, sizeof(path));
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(path[0], '/');

    const cy::Nanoseconds first = platform.monotonic_nanoseconds();
    const cy::Nanoseconds second = platform.monotonic_nanoseconds();
    CY_CHECK(second >= first);
    CY_CHECK(first > 0);
    // The wall clock is a DIFFERENT clock, not a copy: the monotonic one's zero is arbitrary and
    // the wall one's is 1970, so a backend that answered the same figure for both would have wired
    // frame timing to a clock the user can move.
    CY_CHECK(platform.wall_nanoseconds() > first);

    CY_CHECK(platform.cpu_count() >= 1);
    const auto memory = platform.memory_statistics();
    CY_REQUIRE(memory.has_value());
    CY_CHECK(memory.value().total_physical_bytes > 0);
    platform.shutdown();
}

CY_TEST_CASE("linux-native: a piped child is written to, read from, and its pid is the real one") {
    cy::LinuxPlatform platform;
    CY_REQUIRE(platform.initialise(0, nullptr).has_value());

    // `cat` is the smallest program that proves the whole channel: what goes in comes out, so a
    // reply that arrives could only have come from the child.
    const char* argv[] = {"cat", nullptr};
    cy::ProcessOptions options;
    options.arguments = argv;
    options.argument_count = 1;
    options.piped_standard_streams = true;

    const auto spawned = platform.spawn_process(options);
    CY_REQUIRE(spawned.has_value());
    const cy::ProcessHandle child = spawned.value();

    const auto pid = platform.process_id(child);
    CY_REQUIRE(pid.has_value());
    // The operating system's own identifier, not this Platform's bookkeeping: the handle above is
    // 1 for the first child of every process, and a pid never is.
    CY_CHECK(pid.value() > 1);

    CY_REQUIRE(platform.write_process_input(child, "hello\n").has_value());
    CY_REQUIRE(platform.close_process_input(child).has_value());

    // WAIT FIRST, THEN READ, and the order is the point rather than a convenience. `cat` copies its
    // input to its output and exits when its input closes, so once wait_process() has returned
    // everything the child will ever say is already in the pipe. Polling the non-blocking read
    // before that is a race against process start-up — two thousand attempts took under a
    // millisecond and read nothing, which is exactly what the interface promises a non-blocking
    // read does and was this case's first result.
    const auto code = platform.wait_process(child);
    CY_REQUIRE(code.has_value());
    CY_CHECK_EQ(code.value(), 0);

    // A zero is "nothing at this instant" OR "closed" and the interface deliberately does not
    // distinguish them; the loop stops on the second zero after something arrived, which is all a
    // caller can do and all it needs to.
    char reply[64] = {};
    usize total = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto read =
            platform.read_process_output(child, reply + total, sizeof(reply) - total - 1);
        CY_REQUIRE(read.has_value());
        if (read.value() == 0 && total > 0) {
            break;
        }
        total += read.value();
    }
    CY_CHECK_EQ(std::strncmp(reply, "hello", 5), 0);
    platform.release_process(child);

    // An unpiped child refuses the three stream calls rather than pretending to have a channel.
    cy::ProcessOptions quiet;
    const char* true_argv[] = {"true", nullptr};
    quiet.arguments = true_argv;
    quiet.argument_count = 1;
    quiet.inherit_standard_streams = false;
    const auto second = platform.spawn_process(quiet);
    CY_REQUIRE(second.has_value());
    const cy::Status refused = platform.close_process_input(second.value());
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unsupported);
    (void)platform.wait_process(second.value());
    platform.release_process(second.value());

    platform.shutdown();
}

// --- DisplayServer: needs an X server ------------------------------------------------------------

CY_TEST_CASE("linux-native: the display server refuses cleanly when there is no display") {
    if (display_available()) {
        // The inverse of the skip: where there IS a display, this case checks that bringing the
        // server up twice is refused, which is the same contract on the other side.
        cy::X11DisplayServer server;
        CY_REQUIRE(server.initialise().has_value());
        const cy::Status again = server.initialise();
        CY_CHECK_FALSE(again.has_value());
        CY_CHECK_EQ(again.error().code, cy::ErrorCode::AlreadyExists);
        server.shutdown();
        return;
    }
    cy::X11DisplayServer server;
    const cy::Status refused = server.initialise();
    CY_REQUIRE_FALSE(refused.has_value());
    // Unavailable and a message naming the headless server, rather than a window that cannot be
    // drawn to. This is the case CI runs platform/headless/ for.
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unavailable);
}

CY_TEST_CASE("linux-native: a window reports the flags it actually got, not the ones asked for") {
    if (!display_available()) {
        CY_CHECK(announce_skip("the window cases"));
        return;
    }
    WindowFixture fixture;
    CY_REQUIRE(fixture.open());

    const auto flags = fixture.server.window_flags(fixture.window);
    CY_REQUIRE(flags.has_value());
    // The window was asked for Resizable, which this backend supports, so it kept it. An
    // unsupported flag would have been dropped with a warning and the window created anyway — the
    // specification's degradation rule, and the reason the return of create_window() is a window
    // rather than a report.
    CY_CHECK(cy::has_flag(flags.value(), cy::WindowFlags::Resizable));

    const auto size = fixture.server.window_size(fixture.window);
    CY_REQUIRE(size.has_value());
    CY_CHECK_EQ(size.value().width, 320);
    CY_CHECK_EQ(size.value().height, 240);

    // Every window is on a screen, and every screen this server reports can be found by its id.
    const auto screen = fixture.server.window_screen(fixture.window);
    CY_REQUIRE(screen.has_value());
    CY_CHECK(fixture.server.screen_by_id(screen.value()).has_value());
    CY_CHECK(fixture.server.screen_count() >= 1);

    const auto scale = fixture.server.window_dpi_scale(fixture.window);
    CY_REQUIRE(scale.has_value());
    CY_CHECK(scale.value() > 0.0F);

    // A window that does not exist is NotFound on every call, rather than a crash or a default.
    CY_CHECK_FALSE(fixture.server.window_size(4242).has_value());
    CY_CHECK_FALSE(fixture.server.window_exists(4242));
}

CY_TEST_CASE("linux-native: a capability this backend lacks is refused, not silently accepted") {
    if (!display_available()) {
        CY_CHECK(announce_skip("the capability cases"));
        return;
    }
    WindowFixture fixture;
    CY_REQUIRE(fixture.open());

    // has_feature() and the call agree. That is the whole contract: a caller asks, and a backend
    // that answered false and then succeeded — or answered true and then refused — would make the
    // query worthless.
    CY_CHECK_FALSE(fixture.server.has_feature(cy::Feature::ExclusiveFullscreen));
    const cy::Status refused =
        fixture.server.set_window_mode(fixture.window, cy::WindowMode::ExclusiveFullscreen);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unsupported);

    CY_CHECK_FALSE(fixture.server.has_feature(cy::Feature::VSyncMailbox));
    CY_CHECK_FALSE(
        fixture.server.set_window_vsync(fixture.window, cy::VSyncMode::Mailbox).has_value());
    // The mode it CAN honour is recorded and reads back, because the RHI reads the request through
    // this interface rather than through a channel of its own.
    CY_REQUIRE(
        fixture.server.set_window_vsync(fixture.window, cy::VSyncMode::Disabled).has_value());
    const auto vsync = fixture.server.window_vsync(fixture.window);
    CY_REQUIRE(vsync.has_value());
    CY_CHECK_EQ(static_cast<int>(vsync.value()), static_cast<int>(cy::VSyncMode::Disabled));

    // The M0 surface seam, which is the only kind of surface anything in this tree asks for: the
    // native window a Vulkan or Metal surface would be built from.
    const cy::SurfaceDescription none{cy::GraphicsApi::None, nullptr};
    const auto surface = fixture.server.create_surface(fixture.window, none);
    CY_REQUIRE(surface.has_value());
    CY_CHECK(surface.value().handle != nullptr);
    CY_CHECK(surface.value().display != nullptr);
}

CY_TEST_CASE(
    "linux-native: the input source sees the display server's events without stealing them") {
    if (!display_available()) {
        CY_CHECK(announce_skip("the input cases"));
        return;
    }
    WindowFixture fixture;
    CY_REQUIRE(fixture.open());

    cy::input::InputServer server(allocator());
    cy::input::InputServerConfig config;
    config.event_capacity = 64;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    cy::X11InputSource source;
    CY_REQUIRE(source.attach(fixture.server, server).has_value());
    CY_CHECK(source.attached());
    CY_CHECK_FALSE(source.keyboard().is_null());
    CY_CHECK_FALSE(source.mouse().is_null());

    // Assigned to nobody. `input-and-actions` requires device assignment to be EXPLICIT, and a
    // platform layer that handed the keyboard to player one would be the assumption it forbids.
    const cy::input::DeviceRecord* keyboard = server.devices().find(source.keyboard());
    CY_REQUIRE(keyboard != nullptr);
    CY_CHECK_EQ(keyboard->user, cy::input::kNoUser);

    // A real X KeyPress, sent to our own window. keycode 25 is evdev KEY_W + 8 on every Linux X
    // server, which is the physical key under the left hand's middle finger on QWERTY and under a
    // French player's Z — the whole reason this backend maps physical keys and not keysyms.
    auto* display = static_cast<Display*>(fixture.server.native_display());
    XEvent event{};
    event.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(
        fixture.server.create_surface(fixture.window, {cy::GraphicsApi::None, nullptr})
            .value()
            .handle));
    event.xkey.root = DefaultRootWindow(display);
    event.xkey.keycode = 25;
    event.xkey.state = 0;
    event.xkey.time = 4242;
    event.xkey.same_screen = 1;
    XSendEvent(display, event.xkey.window, 0, KeyPressMask, &event);
    XFlush(display);

    for (int attempt = 0; attempt < 100 && server.pending().empty(); ++attempt) {
        fixture.server.pump_events();
    }
    CY_REQUIRE_FALSE(server.pending().empty());
    const cy::input::DeviceEvent& forwarded = server.pending()[0];
    CY_CHECK(forwarded.control == cy::input::key_control(cy::input::Key::W));
    CY_CHECK_EQ(forwarded.value, 1.0F);
    CY_CHECK(forwarded.device == source.keyboard());
    // The time the platform observed it. X reports milliseconds; the engine's stream is
    // nanoseconds, so it is converted rather than carried — two clocks in one stream is a latency
    // figure that means nothing.
    CY_CHECK_EQ(forwarded.timestamp, 4242LL * 1'000'000LL);
    CY_CHECK(source.forwarded() >= 1);

    // AND THE DISPLAY SERVER STILL GOT ITS OWN EVENTS. The observer consumes nothing; if it did,
    // the failure would look like "input works when the window is idle and stops when it is being
    // resized", which is the defect sdl3_input_source.h describes at length.
    fixture.server.pump_events();
    CY_CHECK_EQ(fixture.server.take_dropped_event_count(), u32{0});

    source.detach();
    CY_CHECK_FALSE(source.attached());
}
