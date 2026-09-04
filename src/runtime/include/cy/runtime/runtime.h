// The runtime: bootstrap, subsystem wiring, and tick(). Tasks 3.4.1 and 3.4.2.
//
// THE INVARIANT (design.md §3, `core-platform-abstraction` "Platform does not own the main loop").
// There is no `while (running)` in this module and there will not be one. The runtime exposes
// tick(); the loop lives in the host, under platform/, so a platform that drives frames itself —
// mobile, web — is not precluded. Nothing here calls a host loop, and nothing here can: the runtime
// does not know one exists.
//
// STARTUP ORDER IS DATA (`engine-architecture`, "Deterministic startup and shutdown"). The eleven
// stages below are the specification's list, in its order, and every one of them is entered on
// every run — including the stages whose subsystem does not exist until M1 or later. A stage with
// no work is still a stage: its position is fixed now, so the milestone that fills it fills a slot
// rather than choosing a new order. The order actually taken is recorded in a journal, shutdown
// walks that journal backwards, and the two are asserted to be exact reverses of one another.
// tests/smoke/test_startup_order.cpp compares the journal across a hundred separate processes.
//
// FOUR OF THE ELEVEN ARE MODULE STAGES — Core, Servers, Scene, Editor — and at M1 they stopped
// being empty (task 4.5). Each brings up the project graph's modules at its registration level
// through cy::config::ModuleRegistry, which orders them by (level, name) and journals what it
// actually did. The order within a level is therefore the project graph's, not the host's, and it
// is recorded rather than emergent; src/runtime/tests/test_module_order.cpp compares that journal
// across a hundred processes too. A configuration with no registry leaves the four stages empty,
// which is what a test that only wants the eleven asks for.

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/config/module_registry.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/platform/display_server.h>
#include <cy/core/platform/platform.h>
#include <cy/runtime/servers.h>
#include <cy/runtime/simulation.h>

#include <span>

// NOTE ON THE NAMESPACE. `Runtime`, `RuntimeConfig` and `StartupStage` are `cy::`, which is where
// M0 put them; the types M2 added are `cy::runtime::`, which is the convention every other module
// follows (`cy::ecs`, `cy::scene`, `cy::determinism`). Moving the first three would rename them in
// every sample, host and test that names one, for no benefit this milestone can point at — so the
// split is recorded here rather than fixed under a milestone whose subject is something else.

namespace cy {

/// The startup sequence `engine-architecture` fixes, in its order. Shutdown is its exact reverse.
///
/// The stages M0 implements are Platform (process setup, logging, crash handler), Display (the
/// DisplayServer's window and its surface) and Boot. The others are entered and leave immediately;
/// the comment on each names what fills it.
enum class StartupStage : u8 {
    Platform = 0,    // process setup, command line, logging, crash handler
    Core,            // allocators, job system, type registry, virtual filesystem, config — M1
    ModulesCore,     // modules at level Core — M1
    Display,         // DisplayServer, window and surface creation
    Servers,         // render device, RenderServer, Audio, Physics, Navigation, Text — M2, M3
    ModulesServers,  // modules at level Servers — M2
    EcsScene,        // world creation, component and node type registration — M2
    ModulesScene,    // modules at level Scene — M2
    Scripting,       // ABI export table, Swift runtime, game module load — M4, M5
    Editor,          // the editor and modules at level Editor, in tools builds — M5
    Boot,            // load the startup scene or project, and become ready to tick
};

inline constexpr usize kStartupStageCount = 11;

/// The enumerator's own spelling, for a diagnostic and for the ordering journal. Never null.
const char* startup_stage_name(StartupStage stage);

struct RuntimeConfig {
    /// Required, and initialised by the host before startup(): the runtime consumes the interface
    /// and never names a backend. The host owns both objects for the runtime's whole lifetime.
    Platform* platform = nullptr;
    DisplayServer* display = nullptr;

    /// Optional, and owned by the host for the runtime's whole lifetime. The four module stages
    /// bring up the registry's levels — Core, Servers, Scene, Editor — in that order and take them
    /// down in the exact reverse. Null leaves those stages empty: the sequence is unchanged, and a
    /// host that has no modules does not have to invent an empty registry to say so.
    ///
    /// The registry is populated *before* startup(). Registration is an explicit call rather than a
    /// static initialiser, because static initialisation order is link order — the very
    /// non-determinism this requirement exists to remove.
    config::ModuleRegistry* modules = nullptr;

    /// The window the Display stage opens. Ignored when `create_window` is false, which is what a
    /// test that only wants the startup sequence asks for.
    WindowDescription window;
    bool create_window = true;
    /// Exercise the M0 surface seam: create_surface(GraphicsApi::None) on the main window, which
    /// returns the native handles a Vulkan or Metal surface would be built from at M3.
    bool create_surface = true;

    /// Where the trace is written. Null opens no trace, and every emission below becomes a load and
    /// a return — which is what the ordering test wants and what a host without a path gets.
    const char* trace_path = nullptr;
    /// Recorded in the capture's metadata so an artefact identifies the build that wrote it.
    const char* build_identity = "";
    /// False keeps draining on the caller's flush. A test wants that; a running game does not.
    bool trace_consumer_thread = true;
    /// Install the crash handler in the Platform stage, and remove it when that stage is left.
    bool install_crash_handler = true;

    /// The simulation rate, as an exact rational, and the cap on ticks per frame — the two numbers
    /// `engine-architecture` fixes at 1/60 s and 8. Exceeding the cap discards the excess time so
    /// the loop cannot enter a death spiral; lengthening the step to catch up is on
    /// `simulation-and-determinism`'s forbidden list and there is no field here that would let it.
    determinism::TickRate tick_rate{60, 1};
    u32 max_ticks_per_frame = 8;
    /// `engine-architecture`'s `--fixed-step <n>`: exactly this many ticks per frame regardless of
    /// wall-clock time, "producing reproducible simulation for recording and automated tests".
    determinism::TickMode tick_mode = determinism::TickMode::Realtime;
    u32 fixed_ticks_per_frame = 1;

    /// Optional, and owned by the host for the runtime's whole lifetime.
    ///
    /// The `Servers` stage fills every slot from this registry — requested backend, then documented
    /// default, then the null implementation. Backends are registered *before* startup(), which is
    /// what lets a module at level `Core` add one that the `Servers` stage then chooses. Null
    /// leaves the stage empty and the sequence unchanged.
    runtime::ServerRegistry* servers = nullptr;
    /// One requested backend name per `ServerKind`, or null for "no preference". Borrowed.
    const char* const* server_backends = nullptr;
    u32 server_backend_count = 0;

    /// Optional, and owned by the host. The `EcsScene` stage brings it up if the host has not, and
    /// the `Boot` stage closes its registration; `tick()` then runs its fixed steps and its frame.
    ///
    /// Host-owned rather than runtime-owned on purpose: a game registers its components and systems
    /// between constructing the simulation and starting the runtime, and a simulation the runtime
    /// constructed would give it nowhere to do that.
    runtime::Simulation* simulation = nullptr;
    /// Optional. Null runs every stage serially on the calling thread, which is what a test does.
    jobs::JobSystem* jobs = nullptr;
};

/// What the last frame did. Read by the host for a summary, and by a test for an assertion.
struct FrameStats {
    u64 frame_index = 0;
    Nanoseconds duration_ns = 0;
    /// Simulation ticks run in this frame, and since startup.
    u32 ticks = 0;
    u64 total_ticks = 0;
    /// The accumulator's remainder as a fraction of the fixed step, which the renderer will
    /// interpolate transforms with at M3. Computed here because the frame owns it.
    f32 interpolation_alpha = 0.0F;
    u32 events_handled = 0;
    u32 events_dropped = 0;
    /// Time discarded because the tick cap was reached, in total. Non-zero means frames were slower
    /// than `max_ticks_per_frame` steps and the excess was dropped rather than chased.
    u64 discarded_ns = 0;
    /// The simulation point the last tick of this frame committed at, and the state version it
    /// published. Both zero when no simulation is attached.
    determinism::SimulationPoint committed;
    u64 state_version = 0;
};

class Runtime {
public:
    Runtime() = default;
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Walk the stage table in order. A stage that fails unwinds every stage already entered, in
    /// reverse, and returns an error naming the stage that failed — `engine-architecture`,
    /// "Failure during startup unwinds cleanly".
    Status startup(const RuntimeConfig& config);

    /// Leave every entered stage in the exact reverse of the order it was entered. Idempotent.
    void shutdown();

    /// One frame. The host calls this; the runtime never calls it itself.
    ///
    /// Pumps the display's events, runs zero or more fixed steps, and computes the interpolation
    /// alpha the variable-rate half of the frame will use. The frame's stages — simulation,
    /// rendering, presentation — are empty at M0 and arrive with the subsystems that fill them.
    Status tick();

    /// Started, and nothing has asked the process to exit. The host's loop condition.
    [[nodiscard]] bool is_running() const;

    [[nodiscard]] const FrameStats& frame() const { return frame_; }
    /// The clock pacing the frame. The simulation's when one is attached; see `accumulate()`.
    [[nodiscard]] const determinism::SimulationClock& clock() const noexcept;
    [[nodiscard]] WindowId main_window() const { return window_; }
    [[nodiscard]] const NativeSurface& main_surface() const { return surface_; }

    /// The order stages were entered, and the order they were left. The second is the reverse of
    /// the first, asserted at shutdown and compared across processes by the ordering test.
    [[nodiscard]] std::span<const StartupStage> startup_order() const;
    [[nodiscard]] std::span<const StartupStage> shutdown_order() const;

private:
    // One entry per stage. A null `enter` is a stage with no work at M0; it is still journaled, so
    // the order is fixed before there is anything in it to reorder.
    struct StageOps {
        StartupStage stage;
        Status (Runtime::*enter)();
        void (Runtime::*leave)();
    };

    static const StageOps kStages[kStartupStageCount];

    Status enter_platform();
    void leave_platform();
    Status enter_modules_core();
    void leave_modules_core();
    Status enter_display();
    void leave_display();
    Status enter_servers();
    void leave_servers();
    Status enter_ecs_scene();
    void leave_ecs_scene();
    Status enter_modules_servers();
    void leave_modules_servers();
    Status enter_modules_scene();
    void leave_modules_scene();
    Status enter_modules_editor();
    void leave_modules_editor();
    Status enter_boot();
    void leave_boot();

    // The four module stages differ only in which registration level they name, so they share one
    // implementation and each is a one-line call into it. A level whose modules fail to register
    // reports the failure as the stage's, and Runtime::startup() unwinds the stages below it.
    // const because neither touches the runtime: the registry is the host's, reached through a
    // pointer the configuration carries, and what they change is its state and not this one's.
    Status start_modules(config::ModuleLevel level) const;
    void stop_modules(config::ModuleLevel level) const;

    // tick()'s halves, so that neither the frame nor this class needs a reader to hold two things
    // at once.
    void pump_events();
    [[nodiscard]] Status run_fixed_steps(Nanoseconds delta_ns);
    /// The clock the frame is paced by: the simulation's when one is attached, the runtime's own
    /// otherwise. There is exactly one — two would drift apart within a minute, and the tick number
    /// a diagnostic printed would stop being the tick a system saw.
    [[nodiscard]] determinism::FrameTicks accumulate(Nanoseconds delta_ns) noexcept;
    [[nodiscard]] f32 interpolation_alpha() const noexcept;

    /// Leave the stages in `startup_journal_`, last first, recording each in `shutdown_journal_`.
    void unwind();

    /// Whether the shutdown journal is the exact reverse of the startup journal. Checked in every
    /// configuration, and asserted on top of that.
    [[nodiscard]] bool journal_is_reversed() const;

    /// The last record the trace receives, written before the Platform stage closes it.
    void trace_instant_shutdown() const;

    RuntimeConfig config_;
    bool started_ = false;
    bool unwound_ = false;

    StartupStage startup_journal_[kStartupStageCount] = {};
    StartupStage shutdown_journal_[kStartupStageCount] = {};
    usize startup_count_ = 0;
    usize shutdown_count_ = 0;

    WindowId window_ = kInvalidWindow;
    NativeSurface surface_;
    bool trace_open_ = false;
    bool crash_handler_ = false;

    Nanoseconds last_frame_ns_ = 0;
    /// Paces the frame when no simulation is attached — the M0 and M1 configuration, and every
    /// test that only wants the loop. When one is attached it is that simulation's clock that
    /// advances and this one stays at tick zero.
    determinism::SimulationClock clock_;
    bool servers_selected_ = false;
    FrameStats frame_;
};

}  // namespace cy
