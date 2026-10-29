// SPDX-License-Identifier: MIT
// play_host.cpp — see play_host.h.

#include "play_host.h"

#include <cy/platform/host_loop.h>
#include <cy/platform/sdl3_display_server.h>
#include <cy/platform/sdl3_event_injector.h>
#include <cy/platform/sdl3_input_source.h>
#include <cy/platform/sdl3_platform.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "bindings_file.h"
#include "input_script.h"

namespace cy::play {
namespace {

[[nodiscard]] f64 since_ms(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

[[nodiscard]] FrameTimes summarise(std::vector<f64> samples) noexcept {
    FrameTimes times;
    if (samples.empty()) {
        return times;
    }
    times.frames = samples.size();
    f64 sum = 0.0;
    for (const f64 sample : samples) {
        sum += sample;
    }
    times.mean = sum / static_cast<f64>(samples.size());
    std::sort(samples.begin(), samples.end());
    const auto at = [&](f64 quantile) {
        const auto index = static_cast<usize>(quantile * static_cast<f64>(samples.size() - 1U));
        return samples[index];
    };
    times.p50 = at(0.50);
    times.p99 = at(0.99);
    times.worst = samples.back();
    return times;
}

/// A key or a pad button going down, as the pause toggle reads it.
[[nodiscard]] bool toggles_pause(const input::DeviceEvent& event) noexcept {
    if (event.value < 0.5F) {
        return false;
    }
    return event.control == input::key_control(input::Key::Escape) ||
           event.control == input::gamepad_control(input::GamepadControl::Start);
}

}  // namespace

/// The platform's half: SDL's process services, its display, its input and the injector a script
/// pushes through, and the presenter.
struct PlayHost::Platform {
    Sdl3Platform process;
    Sdl3DisplayServer display;
    Sdl3InputSource source;
    Sdl3EventInjector inject;
    InputScript script;
    WindowId window = kInvalidWindow;
    std::unique_ptr<Presenter> presenter;
    bool process_up = false;
    bool display_up = false;
};

PlayHost::PlayHost(Allocator& allocator) noexcept : allocator_(&allocator) {}

PlayHost::~PlayHost() {
    close();
}

rhi::Device* PlayHost::device() noexcept {
    return platform_ != nullptr && platform_->presenter != nullptr
               ? platform_->presenter->device()
               : nullptr;
}

Status PlayHost::open(const PlayOptions& options) noexcept {
    options_ = options;
    ClockSettings settings;
    settings.fixed = options.fixed_clock;
    clock_ = FixedStepClock(settings);
    platform_ = std::make_unique<Platform>();
    Platform& platform = *platform_;
    if (Status started = platform.process.initialise(0, nullptr); !started) {
        return started;
    }
    platform.process_up = true;
    if (options.headless) {
        // Read by SDL when its video starts, below: the dummy driver's windows are real to SDL and
        // to everything above it, and draw nowhere.
        if (Status chosen = platform.process.set_environment_variable("SDL_VIDEODRIVER", "dummy");
            !chosen) {
            return chosen;
        }
        options_.draw = false;
    }
    if (Status started = platform.display.initialise(); !started) {
        return started;
    }
    platform.display_up = true;
    WindowDescription window;
    window.title = options.title;
    window.size = Extent{options.window_width, options.window_height};
    window.flags = WindowFlags::Resizable;
    window.mode = options.fullscreen ? WindowMode::Fullscreen : WindowMode::Windowed;
    window.vsync = options.vsync ? VSyncMode::Enabled : VSyncMode::Disabled;
    const Expected<WindowId, Error> made = platform.display.create_window(window);
    if (!made) {
        return make_unexpected(made.error());
    }
    platform.window = *made;
    ui_ = std::make_unique<PlayUi>(*allocator_);
    if (Status started = ui_->start(); !started) {
        return started;
    }
    if (options.input_script != nullptr) {
        if (Status loaded = platform.script.load(options.input_script); !loaded) {
            return loaded;
        }
    }
    open_ = true;
    if (!options_.draw) {
        return ok();
    }
#if defined(CY_PLAY_PRESENTER)
    platform.presenter = std::make_unique<Presenter>(*allocator_);
    PresenterOptions presenting;
    presenting.vsync = options.vsync;
    presenting.validation = options.validation;
    presenting.application = options.title;
    return platform.presenter->start(platform.display, platform.window, ui_.get(), presenting);
#else
    return fail(ErrorCode::Unsupported,
                "this build has no Vulkan renderer, so a played game cannot be drawn; "
                "--no-draw runs it without a picture");
#endif
}

Status PlayHost::attach(PlayGame& game) noexcept {
    game_ = &game;
    Platform& platform = *platform_;
    input::InputServer& input = game.input();
    if (Status attached = platform.source.attach(input); !attached) {
        return attached;
    }
    // EXPLICIT ASSIGNMENT, which `input-and-actions` requires: the platform registered the
    // keyboard and the mouse unassigned, and this host gives them to its one player.
    const Nanoseconds now = platform.process.monotonic_nanoseconds();
    for (const input::DeviceId device : {platform.source.keyboard(), platform.source.mouse()}) {
        if (Status assigned = input.assign(device, 0, now); !assigned) {
            return assigned;
        }
    }
    if (options_.bindings != nullptr) {
        const BindingsResult rebound = apply_bindings_file(input, 0, options_.bindings);
        if (!rebound.error.empty()) {
            std::fprintf(stderr, "play: %s: %s\n", options_.bindings, rebound.error.c_str());
            return fail(ErrorCode::InvalidArgument, "the rebinding file was refused");
        }
        std::fprintf(stderr, "play: %u binding(s) from %s\n", rebound.applied, options_.bindings);
    }
    return ok();
}

void PlayHost::assign_gamepads() noexcept {
    input::InputServer& input = game_->input();
    const input::DeviceRegistry& devices = input.devices();
    for (u32 index = 0; index < devices.count(); ++index) {
        const input::DeviceRecord& record = devices.at(index);
        if (record.connected && record.description.kind == input::DeviceKind::Gamepad &&
            record.user == input::kNoUser) {
            (void)input.assign(record.id, 0, platform_->process.monotonic_nanoseconds());
        }
    }
}

void PlayHost::scan_pause_keys() noexcept {
    // EACH EVENT ONCE: the window is scanned every frame, and a paused frame drains it at the same
    // tick, so the scan is keyed on the server's resolution count rather than on the tick.
    const input::InputServer& input = game_->input();
    if (input.resolutions() != scanned_window_) {
        scanned_window_ = input.resolutions();
        scanned_ = 0;
    }
    const auto& pending = input.pending();
    for (usize index = scanned_; index < pending.size(); ++index) {
        if (toggles_pause(pending[index])) {
            pause_.toggle();
            report_.resumes += pause_.paused() ? 0U : 1U;
        }
    }
    scanned_ = pending.size();
}

Status PlayHost::window_events() noexcept {
    Platform& platform = *platform_;
    WindowEvent event;
    while (platform.display.poll_event(event)) {
        switch (event.type) {
            case WindowEventType::CloseRequested:
                report_.closed = true;
                platform.process.request_exit(0);
                break;
            case WindowEventType::FocusLost:
                if (options_.pause_on_focus_loss) {
                    pause_.focus_lost();
                }
                break;
            case WindowEventType::FocusGained:
                pause_.focus_gained();
                break;
            case WindowEventType::Resized:
                ++report_.resizes;
#if defined(CY_PLAY_PRESENTER)
                if (platform.presenter != nullptr) {
                    if (Status resized = platform.presenter->resize(event.size); !resized) {
                        return resized;
                    }
                }
#endif
                break;
            default:
                break;
        }
    }
    return ok();
}

void PlayHost::hold_mouse() noexcept {
    Platform& platform = *platform_;
    const bool hold = pause_.hold_mouse();
    if (hold == held_ || !platform.display.has_feature(Feature::RelativeMouse)) {
        return;
    }
    if (platform.display.set_window_relative_mouse(platform.window, hold)) {
        held_ = hold;
        report_.relative_mouse = report_.relative_mouse || hold;
    }
}

Vec2 PlayHost::viewport() const noexcept {
    const Expected<Extent, Error> size = platform_->display.window_size(platform_->window);
    if (!size || size->width <= 0 || size->height <= 0) {
        return Vec2{static_cast<f32>(options_.window_width),
                    static_cast<f32>(options_.window_height)};
    }
    return Vec2{static_cast<f32>(size->width), static_cast<f32>(size->height)};
}

Status PlayHost::simulate(const StepPlan& plan, Nanoseconds now) noexcept {
    if (pause_.paused()) {
        ++report_.paused_frames;
        return game_->drain(now);
    }
    for (u32 tick = 0; tick < plan.ticks; ++tick) {
        if (Status ticked = game_->tick(now); !ticked) {
            return ticked;
        }
    }
    report_.ticks += plan.ticks;
    report_.max_ticks_in_frame = std::max(report_.max_ticks_in_frame, plan.ticks);
    report_.dropped_ticks += plan.dropped;
    return ok();
}

Status PlayHost::present_frame(f64& waited_ms) noexcept {
    waited_ms = 0.0;
#if defined(CY_PLAY_PRESENTER)
    Platform& platform = *platform_;
    if (platform.presenter == nullptr) {
        return ok();
    }
    const Expected<PresentSource, Error> picture = game_->draw();
    if (!picture) {
        return make_unexpected(picture.error());
    }
    const bool capture = options_.capture_every != 0U && frame_ % options_.capture_every == 0U;
    const Expected<PresentResult, Error> shown = platform.presenter->present(*picture, capture);
    if (!shown) {
        return make_unexpected(shown.error());
    }
    report_.presented += shown->presented ? 1U : 0U;
    waited_ms = shown->present_ms;
    if (capture && shown->presented && on_capture) {
        const Presenter& presenter = *platform.presenter;
        const std::vector<u32>& pixels = presenter.captured();
        if (Status written = on_capture(frame_, Span<const u32>(pixels.data(), pixels.size()),
                                        presenter.captured_width(), presenter.captured_height());
            !written) {
            return written;
        }
        ++report_.captured;
    }
#endif
    return ok();
}

void PlayHost::record_time(f64 frame_ms, f64 work_ms) noexcept {
    if (frame_ < options_.warm_frames) {
        return;
    }
    frame_ms_.push_back(frame_ms);
    work_ms_.push_back(work_ms);
}

Status PlayHost::frame() noexcept {
    Platform& platform = *platform_;
    const auto started = std::chrono::steady_clock::now();
    ScriptTarget target;
    target.inject = &platform.inject;
    target.native_window = platform.display.native_window_id(platform.window);
    target.display = &platform.display;
    target.window = static_cast<u32>(platform.window);
    const Expected<bool, Error> scripted = platform.script.run(frame_, target);
    if (!scripted) {
        return make_unexpected(scripted.error());
    }
    platform.display.pump_events();
    if (Status handled = window_events(); !handled) {
        return handled;
    }
    scan_pause_keys();
    assign_gamepads();
    hold_mouse();
    const Nanoseconds now = platform.process.monotonic_nanoseconds();
    const f64 wall = last_ == 0 ? clock_.settings().step_seconds
                                : static_cast<f64>(now - last_) / 1.0e9;
    last_ = now;
    const StepPlan plan = clock_.advance(wall, pause_.paused());
    if (Status simulated = simulate(plan, now); !simulated) {
        return simulated;
    }
    FrameClock clock;
    clock.seconds = plan.frame_seconds;
    clock.interpolation = plan.interpolation;
    clock.ticks = pause_.paused() ? 0U : plan.ticks;
    clock.paused = pause_.paused();
    clock.viewport = viewport();
    if (Status framed = game_->frame(clock); !framed) {
        return framed;
    }
    const UiRequests requests = ui_->take_requests();
    if (requests.resume && pause_.paused()) {
        pause_.resume();
        ++report_.resumes;
    }
    if (requests.quit) {
        report_.quit_by_menu = true;
        platform.process.request_exit(0);
    }
    f64 presenting = 0.0;
    if (Status shown = present_frame(presenting); !shown) {
        return shown;
    }
    const f64 whole = since_ms(started);
    record_time(whole, whole - presenting);
    ++frame_;
    if (*scripted) {
        platform.process.request_exit(0);
    }
    return ok();
}

Expected<PlayReport, Error> PlayHost::run() noexcept {
    if (!open_ || game_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "open the play host and attach a game first");
    }
    HostLoopOptions loop;
    loop.frame_limit = options_.frame_limit;
    const HostLoopResult ran = run_host_loop(platform_->process, loop, [this] {
        if (failure_) {
            failure_ = frame();
        }
        if (!failure_) {
            platform_->process.request_exit(1);
        }
    });
    if (!failure_) {
        return make_unexpected(failure_.error());
    }
    report_.frames = ran.frames;
    report_.pauses = pause_.pauses();
    report_.focus_pauses = pause_.focus_pauses();
    report_.events_forwarded = platform_->source.forwarded();
    report_.events_injected = platform_->inject.pushed();
    report_.frame = summarise(frame_ms_);
    report_.work = summarise(work_ms_);
    const Vec2 size = viewport();
    report_.window_width = static_cast<u32>(size.x);
    report_.window_height = static_cast<u32>(size.y);
#if defined(CY_PLAY_PRESENTER)
    if (platform_->presenter != nullptr) {
        report_.validation_errors = platform_->presenter->validation_errors();
    }
#endif
    return report_;
}

void PlayHost::close() noexcept {
    if (platform_ == nullptr) {
        return;
    }
    Platform& platform = *platform_;
    // The input source closes the pads it opened BEFORE the injector detaches its virtual one:
    // SDL frees a virtual joystick's device when it is detached, and a gamepad still open on it
    // would then be closed through freed memory.
    platform.source.detach();
    platform.inject.detach_gamepad();
#if defined(CY_PLAY_PRESENTER)
    platform.presenter.reset();
#endif
    ui_.reset();
    if (platform.window != kInvalidWindow) {
        (void)platform.display.set_window_relative_mouse(platform.window, false);
        platform.display.destroy_window(platform.window);
        platform.window = kInvalidWindow;
    }
    if (platform.display_up) {
        platform.display.shutdown();
    }
    if (platform.process_up) {
        platform.process.shutdown();
    }
    platform_.reset();
    open_ = false;
}

}  // namespace cy::play
