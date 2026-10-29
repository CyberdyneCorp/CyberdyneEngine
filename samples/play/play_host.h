// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_HOST_H
#define CY_SAMPLE_PLAY_PLAY_HOST_H
// play_host.h — the desktop play host: what turns a game that runs scripted into one a person
// plays, in a window, with a mouse, a keyboard and a pad. samples/15-first-person and
// samples/14-third-person run under it with `--play`; a future game implements `PlayGame` and
// gets the same window, input, loop, pause and presentation.
//
// ================================================================================================
// WHAT IT OWNS, AND WHAT IT DOES NOT
// ================================================================================================
//
// It owns the window (`Sdl3DisplayServer`), the platform's input (`Sdl3InputSource`, feeding the
// GAME's input server, whose action maps resolve it), the real-time loop, the pause, the
// interface the game builds through the ABI (play_ui.h) and, when it draws, the device and the
// swapchain (presenter.h). It decides NOTHING about the game: the game's ticks, its frame and its
// picture are the game's (`PlayGame`), and everything they do is the Swift game's.
//
// ================================================================================================
// ONE FRAME
// ================================================================================================
//
//   the input script's events for this frame, if any         (onto SDL's queue: input_script.h)
//   pump the platform                                        (SDL's queue drained: the input
//                                                             source's watch feeds the input
//                                                             server; window events queued)
//   window events: close → exit, resize → the swapchain, focus lost → pause
//   Esc or Start pressed since the last frame → pause toggled
//   what the interface asked last frame: resume, quit
//   the mouse held (relative mode) exactly while running with the focus
//   the clock: the frame's ticks and interpolation           (play_clock.h)
//   paused:  game.drain(now)                                 (the input resolved, no tick)
//   running: game.tick(now) × ticks
//   game.frame(clock)                                        (the interface, onUpdate, presentation)
//   game.draw() → presenter.present()                        (the picture, the interface over it)
//
// The loop is `run_host_loop` (platform/host/): the one `while` that drives frames in the engine.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/servers/input/server.h>

#include <functional>
#include <memory>
#include <vector>

#include "pause.h"
#include "play_clock.h"
#include "play_ui.h"
#include "presenter.h"

namespace cy::play {

/// The frame as the loop hands it to the game.
struct FrameClock {
    /// What the frame stands for: the wall's seconds, clamped — or a step, with a fixed clock.
    f64 seconds = 0.0;
    /// How far past the last committed tick the frame is, in [0, 1): `CyTime::interpolation`.
    f64 interpolation = 0.0;
    /// Ticks run before this frame.
    u32 ticks = 0;
    /// The simulation is stopped: `CY_TIME_PAUSED`, and presentation does not advance.
    bool paused = false;
    /// The window's size, in the interface's units: what the HUD is laid out in.
    Vec2 viewport{1280.0F, 720.0F};
};

/// What the play host needs of a game.
class PlayGame {
public:
    PlayGame() = default;
    virtual ~PlayGame() = default;
    PlayGame(const PlayGame&) = delete;
    PlayGame& operator=(const PlayGame&) = delete;

    /// The input server the platform's devices feed.
    [[nodiscard]] virtual input::InputServer& input() noexcept = 0;
    /// One fixed step. `now` is the platform's clock, which SDL stamps the events with.
    [[nodiscard]] virtual Status tick(Nanoseconds now) noexcept = 0;
    /// A paused frame's input, resolved again at the tick the game stopped on: the cursor the
    /// menu follows moves, and nothing pressed during the pause reaches the next tick as an edge.
    [[nodiscard]] virtual Status drain(Nanoseconds now) noexcept = 0;
    /// Everything after the ticks: the interface, the behaviours' `onUpdate`, presentation.
    [[nodiscard]] virtual Status frame(const FrameClock& clock) noexcept = 0;
    /// The frame drawn into the picture the presenter samples. Called only when drawing.
    [[nodiscard]] virtual Expected<PresentSource, Error> draw() noexcept = 0;
};

struct PlayOptions {
    const char* title = "CyberEngine";
    i32 window_width = 1280;
    i32 window_height = 720;
    bool fullscreen = false;
    bool vsync = true;
    /// False opens a window and drives the game with no device.
    bool draw = true;
    /// SDL's dummy video driver: a window with no display behind it, so the play loop — the
    /// platform's input, the focus events, the pause, the interface — runs on a machine with no
    /// display and no GPU, as CI is. Implies no drawing.
    bool headless = false;
    bool validation = false;
    /// One step a frame whatever the wall says: what a capture and a test want.
    bool fixed_clock = false;
    /// The window losing the focus pauses the game. A capture or a timed test on a shared desktop
    /// turns it off, so another window taking the focus does not stop the film.
    bool pause_on_focus_loss = true;
    /// Stop after this many frames; zero runs until the window closes or the game quits.
    u64 frame_limit = 0;
    /// Platform events at given frames (input_script.h).
    const char* input_script = nullptr;
    /// A player's rebinding (bindings_file.h), applied to input user 0.
    const char* bindings = nullptr;
    /// Every `capture_every`-th presented frame handed to `on_capture`.
    u32 capture_every = 0;
    /// Frames at the start not counted in the frame-time statistics.
    u32 warm_frames = 30;
};

/// Frame times over the counted frames, milliseconds.
struct FrameTimes {
    u64 frames = 0;
    f64 mean = 0.0;
    f64 p50 = 0.0;
    f64 p99 = 0.0;
    f64 worst = 0.0;
};

/// What a run did.
struct PlayReport {
    u64 frames = 0;
    u64 ticks = 0;
    u64 paused_frames = 0;
    u32 max_ticks_in_frame = 0;
    u64 dropped_ticks = 0;
    u32 pauses = 0;
    u32 focus_pauses = 0;
    u32 resumes = 0;
    u32 resizes = 0;
    u64 presented = 0;
    u64 captured = 0;
    u64 events_forwarded = 0;
    u64 events_injected = 0;
    bool relative_mouse = false;
    bool quit_by_menu = false;
    bool closed = false;
    /// The whole frame's wall time, the present (and any v-sync wait) included, and the frame's
    /// work alone: the ticks, the game's frame and its draw — the device's time for it included —
    /// without the present.
    FrameTimes frame;
    FrameTimes work;
    u64 validation_errors = 0;
    u32 window_width = 0;
    u32 window_height = 0;
};

class PlayHost {
public:
    explicit PlayHost(Allocator& allocator) noexcept;
    ~PlayHost();

    PlayHost(const PlayHost&) = delete;
    PlayHost& operator=(const PlayHost&) = delete;

    /// The platform, the window, the interface and — with `draw` — the device and the
    /// swapchain. Before the game starts: the game binds `ui()` and draws on `device()`.
    [[nodiscard]] Status open(const PlayOptions& options) noexcept;
    [[nodiscard]] PlayUi& ui() noexcept { return *ui_; }
    /// The device a game's renderer draws with; null when not drawing.
    [[nodiscard]] rhi::Device* device() noexcept;
    /// Feed `game`'s input server from the platform, and read its rebinding.
    [[nodiscard]] Status attach(PlayGame& game) noexcept;
    /// Run until the window closes, the game's menu quits, the script ends the run, the frame
    /// limit, or an interrupt.
    [[nodiscard]] Expected<PlayReport, Error> run() noexcept;
    /// Release the window and the platform, the device last. Idempotent.
    void close() noexcept;

    /// Called with each captured frame: its number, RGBA8 rows, its extent.
    std::function<Status(u64, Span<const u32>, u32, u32)> on_capture;

    [[nodiscard]] const PauseState& pause() const noexcept { return pause_; }

    struct Platform;

private:
    [[nodiscard]] Status frame() noexcept;
    [[nodiscard]] Status window_events() noexcept;
    void scan_pause_keys() noexcept;
    void assign_gamepads() noexcept;
    void hold_mouse() noexcept;
    [[nodiscard]] Status simulate(const StepPlan& plan, Nanoseconds now) noexcept;
    /// The game's picture presented; `waited_ms` is the presenter's share, v-sync included.
    [[nodiscard]] Status present_frame(f64& waited_ms) noexcept;
    void record_time(f64 frame_ms, f64 work_ms) noexcept;
    [[nodiscard]] Vec2 viewport() const noexcept;

    Allocator* allocator_;
    PlayOptions options_;
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<PlayUi> ui_;
    PlayGame* game_ = nullptr;
    PauseState pause_;
    FixedStepClock clock_;
    PlayReport report_;
    std::vector<f64> frame_ms_;
    std::vector<f64> work_ms_;
    Nanoseconds last_ = 0;
    u64 frame_ = 0;
    u64 scanned_window_ = 0;
    usize scanned_ = 0;
    bool held_ = false;
    bool open_ = false;
    Status failure_ = ok();
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_HOST_H
