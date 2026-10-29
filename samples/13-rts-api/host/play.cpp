// SPDX-License-Identifier: MIT
#include "play.h"

#include <cy/core/platform/display_server.h>
#include <cy/platform/sdl3_event_injector.h>
#include <cy/platform/sdl3_input_source.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "battle_host.h"
#include "input_script.h"
#include "letterbox.h"
#include "rts_host.h"

#if defined(CY_RTS_API_MAP)
#    include <cy/backends/rhi/device.h>
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#    include <cy/core/memory/system_allocator.h>
#    include <cy/platform/sdl3_display_server.h>

#    include "present_pass.h"
#endif

namespace sample::rts {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec2;
using cy::Vec3;

/// A chest, not the feet: where a person aims at a soldier.
constexpr f32 kChest = 0.9F;
/// The host's step, and the most frames one display frame may catch up on.
constexpr std::chrono::nanoseconds kStep{16'666'667};
constexpr u64 kMostCatchUp = 4;

/// A view point as the window's: what a person's pointer would report there.
[[nodiscard]] Vec2 to_window(const cy::PointerMapping& mapping, Vec2 view) noexcept {
    return Vec2{(view.x / mapping.scale_x) + mapping.offset_x,
                (view.y / mapping.scale_y) + mapping.offset_y};
}

// --- The written hand
// ------------------------------------------------------------------------------

/// `--input`: a script's events pushed into SDL's queue at their frames, as a person's would be.
class Hand {
public:
    /// The script's events, replacing any. The injector stays: it owns SDL's virtual devices.
    void load(std::vector<ScriptEvent> events) noexcept {
        events_ = std::move(events);
        cursor_ = 0;
    }

    /// Push frame `frame`'s events. `quit` is set by a `quit` line.
    [[nodiscard]] cy::Status play(u64 frame, RtsHost& host, const cy::PointerMapping& mapping,
                                  bool& quit) noexcept {
        for (; cursor_ < events_.size() && events_[cursor_].frame <= frame; ++cursor_) {
            if (cy::Status pushed = push(events_[cursor_], host, mapping, quit); !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    }

    [[nodiscard]] const cy::Sdl3EventInjector& injector() const noexcept { return injector_; }
    /// A `resize` line's size, taken once: zero when there is none to apply.
    [[nodiscard]] cy::Extent take_resize() noexcept { return std::exchange(resize_, cy::Extent{}); }
    [[nodiscard]] u32 missed_aims() const noexcept { return missed_; }

private:
    [[nodiscard]] cy::Status push(const ScriptEvent& event, RtsHost& host,
                                  const cy::PointerMapping& mapping, bool& quit) noexcept {
        switch (event.kind) {
            case ScriptEvent::Kind::Key:
                return injector_.key(event.key, event.down);
            case ScriptEvent::Kind::Press:
            case ScriptEvent::Kind::Release:
                return injector_.mouse_button(event.button, event.down, at_.x, at_.y);
            case ScriptEvent::Kind::Wheel:
                return injector_.mouse_wheel(event.x, at_.x, at_.y);
            case ScriptEvent::Kind::Quit:
                quit = true;
                return cy::ok();
            case ScriptEvent::Kind::Move:
                return move(mapping, Vec2{event.x, event.y});
            case ScriptEvent::Kind::AimUnit:
            case ScriptEvent::Kind::AimField:
                return aim(event, host, mapping);
            case ScriptEvent::Kind::AimUi:
                return aim_ui(event, host, mapping);
            case ScriptEvent::Kind::Resize:
                resize_ = cy::Extent{static_cast<cy::i32>(event.x), static_cast<cy::i32>(event.y)};
                return cy::ok();
        }
        return cy::ok();
    }

    [[nodiscard]] cy::Status move(const cy::PointerMapping& mapping, Vec2 view) noexcept {
        // The motion's relative part is from where the hand last left the pointer, as a mouse's is.
        const Vec2 window = to_window(mapping, view);
        const Vec2 from = at_;
        at_ = window;
        return injector_.mouse_motion(window.x - from.x, window.y - from.y, window.x, window.y);
    }

    /// The pointer onto a unit's chest or a field point, through the camera the game reads: a
    /// person's eyes on the screen.
    [[nodiscard]] cy::Status aim(const ScriptEvent& event, RtsHost& host,
                                 const cy::PointerMapping& mapping) noexcept {
        Vec3 point{event.x + battle::kFieldX, 0.0F, event.y + battle::kFieldZ};
        Vec2 offset{};
        if (event.kind == ScriptEvent::Kind::AimUnit) {
            const cy::game_backend::LockstepSession* session = host.lockstep();
            if (session == nullptr || event.unit >= session->units()) {
                ++missed_;
                return cy::ok();
            }
            point = field_to_level(session->unit(event.unit)->position);
            point.y = kChest;
            offset = Vec2{event.x, event.y};
        }
        Vec2 view{};
        if (!host.project(point, view.x, view.y)) {
            ++missed_;
            return cy::ok();
        }
        return move(mapping, Vec2{view.x + offset.x, view.y + offset.y});
    }

    /// The pointer onto an interface element, where the last layout put it.
    [[nodiscard]] cy::Status aim_ui(const ScriptEvent& event, const RtsHost& host,
                                    const cy::PointerMapping& mapping) noexcept {
        Vec2 view{};
        if (!host.ui_centre(event.name.c_str(), view.x, view.y)) {
            ++missed_;
            return cy::ok();
        }
        return move(mapping, view);
    }

    std::vector<ScriptEvent> events_;
    std::size_t cursor_ = 0;
    cy::Sdl3EventInjector injector_;
    /// Where the hand left the pointer, in the window's coordinates.
    Vec2 at_{};
    u32 missed_ = 0;
    cy::Extent resize_{};
};

// --- The clock
// -------------------------------------------------------------------------------------

/// How many of the host's sixty-a-second frames are due: one a step of wall time since the start,
/// at most `kMostCatchUp` at once — a stall is not made up by running the game fast.
class Pacer {
public:
    [[nodiscard]] u64 due() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (!started_) {
            started_ = true;
            origin_ = now;
        }
        const auto elapsed = static_cast<u64>((now - origin_) / kStep);
        u64 owed = elapsed + 1U - done_;
        if (owed > kMostCatchUp) {
            done_ = elapsed + 1U - kMostCatchUp;
            owed = kMostCatchUp;
        }
        return owed;
    }
    void ran() noexcept { ++done_; }

private:
    std::chrono::steady_clock::time_point origin_;
    u64 done_ = 0;
    bool started_ = false;
};

#if defined(CY_RTS_API_MAP)

/// The view the game lays out, picks and hit-tests in (rts_host.cpp's viewport).
constexpr f32 kViewWidth = 1280.0F;
constexpr f32 kViewHeight = 720.0F;
/// The pointer mapping that inverts a letterbox: window points into the view's pixels.
[[nodiscard]] cy::PointerMapping view_mapping(f32 window_width, f32 window_height) noexcept {
    const Letterbox box = letterbox(kViewWidth, kViewHeight, window_width, window_height);
    cy::PointerMapping mapping;
    if (box.width <= 0.0F) {
        return mapping;
    }
    mapping.scale_x = kViewWidth / box.width;
    mapping.scale_y = kViewHeight / box.height;
    mapping.offset_x = box.x;
    mapping.offset_y = box.y;
    return mapping;
}

// --- The window
// ------------------------------------------------------------------------------------

/// The window, its surface and its swapchain over the renderer's device, and the frame presented.
class PlayWindow {
public:
    PlayWindow() noexcept = default;
    ~PlayWindow() { close(); }

    PlayWindow(const PlayWindow&) = delete;
    PlayWindow& operator=(const PlayWindow&) = delete;

    /// The display, before the device: a machine with none is refused before anything is made.
    [[nodiscard]] cy::Status open_display(const PlayOptions& options) noexcept {
        if (cy::Status up = display_.initialise(); !up) {
            return cy::fail(cy::ErrorCode::Unavailable,
                            "no requested graphics device on this machine (no display)");
        }
        cy::WindowDescription description;
        description.title = "CyberEngine RTS - The Frontier";
        description.size = cy::Extent{static_cast<cy::i32>(options.render.width),
                                      static_cast<cy::i32>(options.render.height)};
        description.flags = cy::WindowFlags::Resizable;
        auto window = display_.create_window(description);
        if (!window || !display_.has_feature(cy::Feature::VulkanSurface)) {
            return cy::fail(cy::ErrorCode::Unavailable,
                            "no requested graphics device on this machine (no Vulkan window)");
        }
        window_ = *window;
        vsync_ = options.vsync;
        return cy::ok();
    }

    /// The surface and the swapchain, on the renderer's device.
    [[nodiscard]] cy::Status open_swapchain(cy::rhi::Device& device) noexcept {
        device_ = &device;
        cy::SurfaceDescription description;
        description.api = cy::GraphicsApi::Vulkan;
        description.api_instance = cy::rhi::vulkan::vulkan_instance_handle(device);
        auto surface = display_.create_surface(window_, description);
        if (!surface) {
            return cy::make_unexpected(surface.error());
        }
        surface_ = *surface;
        has_surface_ = true;
        auto size = display_.window_size(window_);
        if (!size) {
            return cy::make_unexpected(size.error());
        }
        cy::rhi::SwapchainDescription swapchain;
        swapchain.name = "rts play";
        swapchain.native_surface = surface_.handle;
        swapchain.extent =
            cy::rhi::Extent2D{static_cast<u32>(size->width), static_cast<u32>(size->height)};
        // The frame's bytes are display-referred: a UNORM image takes them as they are.
        swapchain.preferred_format = cy::rhi::Format::Bgra8Unorm;
        swapchain.present_mode =
            vsync_ ? cy::rhi::PresentMode::Fifo : cy::rhi::PresentMode::Mailbox;
        auto made = device.create_swapchain(swapchain);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        swapchain_ = *made;
        auto acquired = device.create_semaphore();
        if (!acquired) {
            return cy::make_unexpected(acquired.error());
        }
        acquired_ = *acquired;
        window_size_ = *size;
        return ensure_present_semaphores();
    }

    /// The window's events: closing, and a size the swapchain follows.
    [[nodiscard]] bool pump(PlayReport& report) noexcept {
        display_.pump_events();
        cy::WindowEvent event;
        bool closing = false;
        while (display_.poll_event(event)) {
            closing = closing || event.type == cy::WindowEventType::CloseRequested;
            if (event.type == cy::WindowEventType::Resized) {
                stale_ = true;
                ++report.resizes;
            }
        }
        return closing;
    }

    /// Ask the window manager for a new size; the Resized event it answers with rebuilds the
    /// swapchain, as a person's drag would.
    void resize(cy::Extent size) noexcept {
        // A window the window manager maximised keeps its size until it is restored, as a
        // person's double click on its title bar would.
        (void)display_.set_window_mode(window_, cy::WindowMode::Windowed);
        (void)display_.set_window_size(window_, size);
    }

    /// Window points to the view's pixels, through the letterbox of the window's size now.
    [[nodiscard]] cy::PointerMapping mapping() const noexcept {
        return view_mapping(static_cast<f32>(window_size_.width),
                            static_cast<f32>(window_size_.height));
    }

    /// Draw the frame the host just ran onto the next swapchain image and present it. A swapchain
    /// the window outgrew is rebuilt and this frame drawn without presenting.
    [[nodiscard]] cy::Status draw(MapRenderer& renderer, RtsHost& host,
                                  PlayReport& report) noexcept {
        if (stale_) {
            if (cy::Status rebuilt = rebuild(); !rebuilt) {
                return rebuilt;
            }
        }
        auto index = device_->acquire_next_image(swapchain_, acquired_, 1'000'000'000ULL);
        if (!index) {
            stale_ = true;
            return renderer.frame(host);
        }
        const cy::rhi::SwapchainInfo info = device_->swapchain_info(swapchain_);
        PresentTarget target;
        target.image = device_->swapchain_texture(swapchain_, *index);
        target.format = info.format;
        target.width = info.extent.width;
        target.height = info.extent.height;
        target.acquired = acquired_;
        target.presented = presented_[*index];
        if (cy::Status drawn = renderer.frame(host, &target); !drawn) {
            return drawn;
        }
        if (device_->present(swapchain_, *index, presented_[*index])) {
            ++report.presented;
        } else {
            stale_ = true;
        }
        report.width = info.extent.width;
        report.height = info.extent.height;
        return cy::ok();
    }

    void close() noexcept {
        if (device_ != nullptr) {
            (void)device_->wait_idle();
            for (const cy::rhi::SemaphoreHandle semaphore : presented_) {
                device_->destroy_semaphore(semaphore);
            }
            presented_.clear();
            if (!acquired_.is_null()) {
                device_->destroy_semaphore(acquired_);
            }
            if (!swapchain_.is_null()) {
                device_->destroy_swapchain(swapchain_);
            }
            device_ = nullptr;
        }
        if (has_surface_) {
            display_.destroy_surface(surface_);
            has_surface_ = false;
        }
        if (window_ != cy::kInvalidWindow) {
            display_.destroy_window(window_);
            window_ = cy::kInvalidWindow;
        }
        display_.shutdown();
    }

private:
    /// One presentation semaphore an image: a binary semaphore waited by a present may be signalled
    /// again only once that image has come back, which is what acquiring it again says.
    [[nodiscard]] cy::Status ensure_present_semaphores() noexcept {
        const u32 images = device_->swapchain_info(swapchain_).image_count;
        while (presented_.size() < images) {
            auto made = device_->create_semaphore();
            if (!made) {
                return cy::make_unexpected(made.error());
            }
            presented_.push_back(*made);
        }
        return cy::ok();
    }

    [[nodiscard]] cy::Status rebuild() noexcept {
        auto size = display_.window_size(window_);
        if (!size || size->width <= 0 || size->height <= 0) {
            return cy::ok();  // minimised: nothing to present to until it comes back
        }
        (void)device_->wait_idle();
        if (cy::Status resized = device_->resize_swapchain(
                swapchain_,
                cy::rhi::Extent2D{static_cast<u32>(size->width), static_cast<u32>(size->height)});
            !resized) {
            return resized;
        }
        window_size_ = *size;
        stale_ = false;
        return ensure_present_semaphores();
    }

    cy::Sdl3DisplayServer display_;
    cy::WindowId window_ = cy::kInvalidWindow;
    cy::NativeSurface surface_{};
    bool has_surface_ = false;
    cy::rhi::Device* device_ = nullptr;
    cy::rhi::SwapchainHandle swapchain_;
    cy::rhi::SemaphoreHandle acquired_;
    std::vector<cy::rhi::SemaphoreHandle> presented_;
    cy::Extent window_size_{};
    bool vsync_ = true;
    bool stale_ = false;
};

#endif

// --- The session
// -----------------------------------------------------------------------------------

/// One person's session: the platform's input on the host, the hand, the window, and the loop.
class Session {
public:
    Session(RtsHost& host, const PlayOptions& options, PlayReport& report) noexcept
        : host_(host), options_(options), report_(report) {}

    ~Session() {
        source_.detach();
#if defined(CY_RTS_API_MAP)
        window_.close();
        renderer_.reset();
#endif
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] cy::Status start() noexcept {
        if (!options_.input.empty()) {
            std::vector<ScriptEvent> events;
            std::string detail;
            if (cy::Status read = read_input_script(options_.input, events, detail); !read) {
                std::fprintf(stderr, "rts-api: --input: %s\n", detail.c_str());
                return read;
            }
            hand_.load(std::move(events));
        }
        if (cy::Status opened = open_output(); !opened) {
            return opened;
        }
        // The platform's keyboard and mouse, the user's, feeding the host's input server.
        if (cy::Status attached = source_.attach(host_.input()); !attached) {
            return attached;
        }
        source_.set_pointer_mapping(mapping());
        return host_.adopt_devices(source_.keyboard(), source_.mouse());
    }

    /// Until the game, the window, the script, the frame count or the replay says to stop.
    [[nodiscard]] cy::Status run() noexcept {
        while (report_.ended[0] == '\0') {
            if (cy::Status stepped = step(); !stepped) {
                return stepped;
            }
        }
        return cy::ok();
    }

private:
    [[nodiscard]] cy::Status open_output() noexcept {
        if (!options_.window) {
            return events_.start();
        }
#if defined(CY_RTS_API_MAP)
        if (options_.map == nullptr) {
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "--play draws The Frontier: give it --map <frontier.cypak>");
        }
        if (cy::Status shown = window_.open_display(options_); !shown) {
            return shown;
        }
        renderer_ = std::make_unique<MapRenderer>(cy::system_allocator(cy::MemoryDomain::Renderer));
        if (cy::Status started = renderer_->start(host_, *options_.map, options_.render);
            !started) {
            return started;
        }
        report_.window = true;
        return window_.open_swapchain(*renderer_->device());
#else
        return cy::fail(cy::ErrorCode::Unavailable,
                        "--play needs the Vulkan renderer for a window; this build has none "
                        "(--headless runs without one)");
#endif
    }

    [[nodiscard]] cy::PointerMapping mapping() const noexcept {
#if defined(CY_RTS_API_MAP)
        if (options_.window) {
            return window_.mapping();
        }
#endif
        return cy::PointerMapping{};
    }

    /// The host frames due now — one, for a written hand — then the frame drawn and presented.
    [[nodiscard]] cy::Status step() noexcept {
#if defined(CY_RTS_API_MAP)
        if (options_.window) {
            if (window_.pump(report_)) {
                report_.ended = "window closed";
                return cy::ok();
            }
            source_.set_pointer_mapping(window_.mapping());
        }
#endif
        const bool paced = options_.window && options_.input.empty();
        const u64 due = paced ? pacer_.due() : 1U;
        if (due == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return cy::ok();
        }
        for (u64 index = 0; index < due && report_.ended[0] == '\0'; ++index) {
            if (cy::Status ran = host_frame(); !ran) {
                return ran;
            }
            pacer_.ran();
        }
        return draw();
    }

    [[nodiscard]] cy::Status host_frame() noexcept {
        bool quit = false;
        if (cy::Status played = hand_.play(report_.frames, host_, mapping(), quit); !played) {
            return played;
        }
        apply_resize(hand_.take_resize());
        if (cy::Status ran = host_.frame(); !ran) {
            return ran;
        }
        if (!options_.window) {
            cy::Sdl3EventInjector::drain();
        }
        ++report_.frames;
        report_.ticks = host_.ticks();
        report_.ended = why_end(quit);
        return cy::ok();
    }

    void apply_resize(cy::Extent size) noexcept {
#if defined(CY_RTS_API_MAP)
        if (size.width > 0 && renderer_ != nullptr) {
            window_.resize(size);
        }
#else
        (void)size;
#endif
    }

    [[nodiscard]] const char* why_end(bool quit) const noexcept {
        if (host_.session_request().quit) {
            return "game quit";
        }
        if (quit) {
            return "script quit";
        }
        if (options_.frames != 0U && report_.frames >= options_.frames) {
            return "frames";
        }
        if (options_.frames == 0U && options_.replay != nullptr &&
            host_.ticks() >= options_.replay->ticks) {
            return "replay ran its ticks";
        }
        return "";
    }

    [[nodiscard]] cy::Status draw() noexcept {
        report_.injected = hand_.injector().pushed();
        report_.missed_aims = hand_.missed_aims();
#if defined(CY_RTS_API_MAP)
        if (renderer_ != nullptr) {
            if (cy::Status drawn = window_.draw(*renderer_, host_, report_); !drawn) {
                return drawn;
            }
            const MapRenderReport& frame = renderer_->report();
            const auto timed = static_cast<cy::f64>(std::max<u64>(frame.timed, 1U));
            report_.frame_ms = frame.frame_ms / timed;
            report_.worst_ms = frame.worst_ms;
            report_.present_ms = frame.present_ms / timed;
            report_.outlined = frame.outlined;
            report_.validation_errors = frame.validation_errors;
        }
#endif
        return cy::ok();
    }

    RtsHost& host_;
    const PlayOptions& options_;
    PlayReport& report_;
    Hand hand_;
    Pacer pacer_;
    cy::Sdl3EventSystem events_;
    cy::Sdl3InputSource source_;
#if defined(CY_RTS_API_MAP)
    PlayWindow window_;
    std::unique_ptr<MapRenderer> renderer_;
#endif
};

}  // namespace

cy::Status run_play(RtsHost& host, const PlayOptions& options, PlayReport& report) noexcept {
    report = PlayReport{};
    Session session(host, options, report);
    if (cy::Status started = session.start(); !started) {
        return started;
    }
    if (cy::Status ran = session.run(); !ran) {
        return ran;
    }
    if (options.record.empty() || host.lockstep() == nullptr) {
        return cy::ok();
    }
    auto log = capture_replay(*host.lockstep(), host.follower_digest());
    if (!log) {
        return cy::make_unexpected(log.error());
    }
    return write_replay(options.record, *log);
}

void print_play_report(const PlayReport& report) noexcept {
    std::printf(
        "rts play     frames=%llu ticks=%llu window=%d width=%u height=%u resizes=%u "
        "presented=%llu injected=%llu missed_aims=%u ended='%s' frame_ms=%.3f "
        "worst_ms=%.3f present_ms=%.3f outlined=%u validation_errors=%u\n",
        static_cast<unsigned long long>(report.frames),
        static_cast<unsigned long long>(report.ticks), report.window ? 1 : 0, report.width,
        report.height, report.resizes, static_cast<unsigned long long>(report.presented),
        static_cast<unsigned long long>(report.injected), report.missed_aims, report.ended,
        report.frame_ms, report.worst_ms, report.present_ms, report.outlined,
        report.validation_errors);
}

}  // namespace sample::rts
