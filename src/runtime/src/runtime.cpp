#include <cy/runtime/runtime.h>

#include <cy/core/base/assert.h>
#include <cy/core/base/diagnostic_sink.h>
#include <cy/core/diagnostics/crash.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

namespace cy {
namespace {

// The runtime's diagnostic vocabulary, declared once. Every field carries its classification
// because CY_TRACE_FIELD has no overload that omits one — design.md §2.
CY_TRACE_CATEGORY(runtime_category, "runtime")

CY_TRACE_NAME(startup_name, "runtime.startup")
CY_TRACE_NAME(shutdown_name, "runtime.shutdown")
CY_TRACE_NAME(window_event_name, "runtime.window_event")
CY_TRACE_NAME(tick_cap_name, "runtime.tick_cap")
CY_TRACE_NAME(events_dropped_name, "runtime.events_dropped")

CY_TRACE_FIELD(stage_count, u64, cy::Privacy::Public)
CY_TRACE_FIELD(window_id, u64, cy::Privacy::Public)
CY_TRACE_FIELD(window_event, string, cy::Privacy::Public)
CY_TRACE_FIELD(frame_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(discarded_ns, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(server_count, u64, cy::Privacy::Public)

const char* window_event_type_name(WindowEventType type) {
    switch (type) {
        case WindowEventType::CloseRequested:
            return "close_requested";
        case WindowEventType::Resized:
            return "resized";
        case WindowEventType::Moved:
            return "moved";
        case WindowEventType::FocusGained:
            return "focus_gained";
        case WindowEventType::FocusLost:
            return "focus_lost";
        case WindowEventType::ScreenChanged:
            return "screen_changed";
        case WindowEventType::DpiChanged:
            return "dpi_changed";
    }
    return "unknown";
}

// An error from a call that returns a value, as the Status this stage returns. Expected<T, Error>
// and Expected<void, Error> share the error type but not the alternative, so the error is rebuilt
// rather than converted.
Status forward(const Error& error) {
    return fail(error.code, error.message, error.system_code);
}

diag::FieldValue text_field(diag::FieldId field, const char* text) {
    u32 length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return diag::field_text(field, text, length);
}

}  // namespace

const char* startup_stage_name(StartupStage stage) {
    switch (stage) {
        case StartupStage::Platform:
            return "platform";
        case StartupStage::Core:
            return "core";
        case StartupStage::ModulesCore:
            return "modules-core";
        case StartupStage::Display:
            return "display";
        case StartupStage::Servers:
            return "servers";
        case StartupStage::ModulesServers:
            return "modules-servers";
        case StartupStage::EcsScene:
            return "ecs-scene";
        case StartupStage::ModulesScene:
            return "modules-scene";
        case StartupStage::Scripting:
            return "scripting";
        case StartupStage::Editor:
            return "editor";
        case StartupStage::Boot:
            return "boot";
    }
    return "unknown";
}

// The order, as data. Read it top to bottom for startup and bottom to top for shutdown; there is no
// other order in this module, and no code path that produces a different one.
const Runtime::StageOps Runtime::kStages[kStartupStageCount] = {
    {StartupStage::Platform, &Runtime::enter_platform, &Runtime::leave_platform},
    {StartupStage::Core, nullptr, nullptr},
    {StartupStage::ModulesCore, &Runtime::enter_modules_core, &Runtime::leave_modules_core},
    {StartupStage::Display, &Runtime::enter_display, &Runtime::leave_display},
    {StartupStage::Servers, &Runtime::enter_servers, &Runtime::leave_servers},
    {StartupStage::ModulesServers, &Runtime::enter_modules_servers,
     &Runtime::leave_modules_servers},
    {StartupStage::EcsScene, &Runtime::enter_ecs_scene, &Runtime::leave_ecs_scene},
    {StartupStage::ModulesScene, &Runtime::enter_modules_scene, &Runtime::leave_modules_scene},
    {StartupStage::Scripting, nullptr, nullptr},
    {StartupStage::Editor, &Runtime::enter_modules_editor, &Runtime::leave_modules_editor},
    {StartupStage::Boot, &Runtime::enter_boot, &Runtime::leave_boot},
};

Runtime::~Runtime() {
    shutdown();
}

Status Runtime::startup(const RuntimeConfig& config) {
    CY_ASSERT_MSG(!started_, "Runtime::startup() on a runtime that is already started");
    if (config.platform == nullptr || config.display == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "RuntimeConfig requires an initialised Platform and DisplayServer");
    }

    config_ = config;

    // The pacing clock, configured before any stage runs so that an invalid rate is a startup
    // failure rather than a frame that never ticks. When a simulation is attached it carries its
    // own clock and this one stays at tick zero; both are configured from the same three fields, so
    // a runtime and its simulation cannot be paced differently.
    determinism::ClockConfig clock;
    clock.rate = config.tick_rate;
    clock.max_ticks_per_frame = config.max_ticks_per_frame;
    clock.mode = config.tick_mode;
    clock.fixed_ticks_per_frame = config.fixed_ticks_per_frame;
    if (Status paced = clock_.configure(clock); !paced) {
        return paced;
    }

    startup_count_ = 0;
    shutdown_count_ = 0;
    unwound_ = false;

    for (const StageOps& ops : kStages) {
        if (ops.enter != nullptr) {
            const Status entered = (this->*ops.enter)();
            if (!entered) {
                const Error error = entered.error();
                emit_diagnosticf(
                    DiagnosticSeverity::Error, "runtime", "startup failed in stage '%s': %s (%s)",
                    startup_stage_name(ops.stage), error.message, error_code_name(error.code));
                unwind();
                return entered;
            }
        }
        startup_journal_[startup_count_] = ops.stage;
        ++startup_count_;
    }

    started_ = true;
    last_frame_ns_ = config_.platform->monotonic_nanoseconds();

    CY_LOG(runtime_category(), diag::LogLevel::Info, "runtime.started",
           diag::field_u64(stage_count(), static_cast<u64>(startup_count_)));
    return ok();
}

void Runtime::shutdown() {
    if (unwound_ || startup_count_ == 0) {
        return;
    }
    trace_instant_shutdown();
    unwind();
    started_ = false;
}

void Runtime::unwind() {
    shutdown_count_ = 0;
    for (usize i = startup_count_; i > 0; --i) {
        const StartupStage stage = startup_journal_[i - 1];
        const StageOps& ops = kStages[static_cast<usize>(stage)];
        if (ops.leave != nullptr) {
            (this->*ops.leave)();
        }
        shutdown_journal_[shutdown_count_] = stage;
        ++shutdown_count_;
    }
    unwound_ = true;

    // Recorded and asserted, in every configuration: the check is eleven comparisons, and an
    // ordering that has drifted is exactly the defect this journal exists to catch.
    const bool reversed = journal_is_reversed();
    if (!reversed) {
        emit_diagnostic(DiagnosticSeverity::Error, "runtime",
                        "shutdown order is not the exact reverse of startup order");
    }
    CY_ASSERT_MSG(reversed, "shutdown order is not the exact reverse of startup order");
}

bool Runtime::journal_is_reversed() const {
    if (shutdown_count_ != startup_count_) {
        return false;
    }
    for (usize i = 0; i < shutdown_count_; ++i) {
        if (shutdown_journal_[i] != startup_journal_[startup_count_ - 1 - i]) {
            return false;
        }
    }
    return true;
}

std::span<const StartupStage> Runtime::startup_order() const {
    return {startup_journal_, startup_count_};
}

std::span<const StartupStage> Runtime::shutdown_order() const {
    return {shutdown_journal_, shutdown_count_};
}

bool Runtime::is_running() const {
    return started_ && !config_.platform->exit_requested();
}

// --- Stage 1: Platform ---------------------------------------------------------------------------
//
// Process setup, command line, logging and the crash handler. The platform itself was initialised
// by the host — it had to be, to parse the command line that produced this configuration — so what
// this stage owns is the diagnostics: the trace every subsystem publishes into, base's two seams
// (installed by trace_open()), and the crash artefact.

Status Runtime::enter_platform() {
    if (config_.install_crash_handler) {
        diag::CrashConfig crash;
        crash.build_identity = config_.build_identity;
        const auto installed = diag::install_crash_handler(crash);
        if (!installed) {
            return forward(installed.error());
        }
        crash_handler_ = true;
    }

    if (config_.trace_path != nullptr) {
        diag::TraceConfig trace;
        trace.path = config_.trace_path;
        trace.consumer_thread = config_.trace_consumer_thread;
        trace.build_identity = config_.build_identity;
        const auto opened = diag::trace_open(trace);
        if (!opened) {
            return forward(opened.error());
        }
        trace_open_ = true;
        diag::trace_instant(startup_name(), runtime_category(), diag::Channel::Critical);
    }
    return ok();
}

void Runtime::leave_platform() {
    if (trace_open_) {
        (void)diag::trace_close();
        trace_open_ = false;
    }
    if (crash_handler_) {
        diag::uninstall_crash_handler();
        crash_handler_ = false;
    }
}

// --- Stages 3, 6, 8 and 10: modules at a registration level --------------------------------------
//
// Task 4.5. `engine-architecture` puts modules at level Core after the core subsystems and before
// the display server, modules at Servers after the servers, modules at Scene after the world, and
// modules at Editor last. The four stages are the same operation on four levels, so they are one
// function called four times rather than four functions that must be kept in step.
//
// The order *within* a level is not decided here. cy::config::ModuleRegistry sorts by (level, name)
// and journals what it started and stopped, so the order is the project graph's and the evidence
// for it is a record rather than an argument. A registration that fails returns the error as this
// stage's, and Runtime::startup() unwinds the stages below.

Status Runtime::start_modules(config::ModuleLevel level) const {
    if (config_.modules == nullptr) {
        return ok();
    }
    const Status started = config_.modules->start(level);
    if (!started) {
        emit_diagnosticf(DiagnosticSeverity::Error, "runtime",
                         "a module at registration level %s failed to register: %s",
                         config::module_level_name(level), started.error().message);
    }
    return started;
}

void Runtime::stop_modules(config::ModuleLevel level) const {
    if (config_.modules != nullptr) {
        config_.modules->stop(level);
    }
}

Status Runtime::enter_modules_core() {
    return start_modules(config::ModuleLevel::Core);
}

void Runtime::leave_modules_core() {
    stop_modules(config::ModuleLevel::Core);
}

Status Runtime::enter_modules_servers() {
    return start_modules(config::ModuleLevel::Servers);
}

void Runtime::leave_modules_servers() {
    stop_modules(config::ModuleLevel::Servers);
}

Status Runtime::enter_modules_scene() {
    return start_modules(config::ModuleLevel::Scene);
}

void Runtime::leave_modules_scene() {
    stop_modules(config::ModuleLevel::Scene);
}

// The Editor stage is the specification's "Editor (tools builds) and modules at level Editor". The
// editor itself arrives at M5; the modules at that level are here now, because their position in
// the sequence is what M1 is fixing.
Status Runtime::enter_modules_editor() {
    return start_modules(config::ModuleLevel::Editor);
}

void Runtime::leave_modules_editor() {
    stop_modules(config::ModuleLevel::Editor);
}

// --- Stage 4: Display ----------------------------------------------------------------------------

Status Runtime::enter_display() {
    if (!config_.create_window) {
        return ok();
    }

    const auto created = config_.display->create_window(config_.window);
    if (!created) {
        return forward(created.error());
    }
    window_ = created.value();

    if (config_.create_surface) {
        // The M0 seam: no graphics API is initialised, so this returns the native window a Vulkan
        // or Metal surface would be built from at M3. A platform that cannot answer is not a
        // startup failure — there is nothing yet that consumes the surface.
        const SurfaceDescription description{GraphicsApi::None, nullptr};
        const auto surface = config_.display->create_surface(window_, description);
        if (surface) {
            surface_ = surface.value();
        } else {
            emit_diagnosticf(DiagnosticSeverity::Warning, "runtime",
                             "the display server did not provide a native surface: %s",
                             surface.error().message);
        }
    }

    const diag::FieldValue fields[] = {diag::field_u64(window_id(), window_)};
    diag::trace_instant(startup_name(), runtime_category(), diag::Channel::Important, fields, 1);
    return ok();
}

void Runtime::leave_display() {
    if (surface_.handle != nullptr) {
        config_.display->destroy_surface(surface_);
        surface_ = NativeSurface{};
    }
    if (window_ != kInvalidWindow) {
        config_.display->destroy_window(window_);
        window_ = kInvalidWindow;
    }
}

// --- Stage 5: Servers ----------------------------------------------------------------------------
//
// Task 4.1.1. Every slot is filled from the host's registry: the requested backend, then the
// documented default, then the null implementation that keeps handle bookkeeping valid. None of
// those three is a startup failure — falling back is the documented behaviour, and a game that must
// have a particular backend reads `ServerRegistry::outcome()` and says so itself.
//
// A configuration with no registry leaves the stage empty, exactly as a configuration with no
// module registry leaves the four module stages empty. The sequence is unchanged either way, which
// is the property the ordering journal exists to protect.

Status Runtime::enter_servers() {
    if (config_.servers == nullptr) {
        return ok();
    }
    if (Status selected =
            config_.servers->select_all(config_.server_backends, config_.server_backend_count);
        !selected) {
        return selected;
    }
    servers_selected_ = true;
    CY_LOG(runtime_category(), diag::LogLevel::Info, "runtime.servers",
           diag::field_u64(server_count(), config_.servers->live_backends()));
    return ok();
}

void Runtime::leave_servers() {
    if (servers_selected_ && config_.servers != nullptr) {
        config_.servers->shutdown_all();
    }
    servers_selected_ = false;
}

// --- Stage 7: ECS and Scene ----------------------------------------------------------------------
//
// Task 4.1.1. "World creation, component and node type registration."
//
// The simulation is the host's, not the runtime's, and the reason is registration: a game registers
// its components and systems between constructing the simulation and starting the runtime, and a
// simulation the runtime constructed would give it nowhere to do that. So this stage brings up a
// simulation the host has not brought up itself, and is a no-op for one the host initialised
// earlier in order to register against it. Both are correct; what the stage fixes is the *position*
// in the sequence at which a world exists.

// Every entry in kStages is a `Status (Runtime::*)()`, so a stage that happens to touch nothing of
// this object's own still has to have the signature the table declares.
//
// NOLINTNEXTLINE(readability-make-member-function-const)
Status Runtime::enter_ecs_scene() {
    if (config_.simulation == nullptr) {
        return ok();
    }
    if (config_.simulation->initialized()) {
        return ok();
    }
    return config_.simulation->initialize();
}

void Runtime::leave_ecs_scene() {
    // Nothing: the simulation is the host's and outlives the runtime. Present so that the stage has
    // a leave function for the journal to call and so that whoever gives the runtime ownership of a
    // world has a place to release it.
}

// --- Stage 11: Boot ------------------------------------------------------------------------------
//
// "Load the startup scene or project, enter the main loop." There is no scene until M2 and the loop
// is the host's, so what remains is the frame clock's origin: the first tick measures from here
// rather than from whenever the host happens to call it.

Status Runtime::enter_boot() {
    // Registration closes here, in the stage `engine-architecture` calls "load the startup scene or
    // project", and before the first tick. `simulation-and-determinism` requires registries whose
    // contents affect simulation to be "finalised in a deterministic order derived from stable
    // identifiers **before simulation begins**"; doing it lazily on the first tick would close them
    // at a point that depends on when the first tick happened to arrive.
    if (config_.simulation != nullptr) {
        if (Status closed = config_.simulation->finalize_registration(); !closed) {
            return closed;
        }
    }

    last_frame_ns_ = config_.platform->monotonic_nanoseconds();
    frame_ = FrameStats{};
    return ok();
}

void Runtime::leave_boot() {}

// --- The frame -----------------------------------------------------------------------------------

Status Runtime::tick() {
    CY_ASSERT_MSG(started_, "Runtime::tick() before startup()");

    // The one place in the engine where wall-clock time enters the simulation's sense of time: it
    // is measured here and handed to a clock that cannot measure it itself. Everything downstream
    // reads ticks.
    const Nanoseconds now = config_.platform->monotonic_nanoseconds();
    const Nanoseconds delta = now > last_frame_ns_ ? now - last_frame_ns_ : 0;
    last_frame_ns_ = now;

    diag::trace_frame_begin(frame_.frame_index);

    frame_.duration_ns = delta;
    frame_.events_handled = 0;
    frame_.events_dropped = 0;
    frame_.ticks = 0;

    pump_events();
    Status stepped = run_fixed_steps(delta);

    // The alpha is computed before the variable half runs and handed to it, which is the shape
    // `engine-architecture`'s "Smooth rendering between ticks" scenario needs: the render half is
    // told where between two ticks it is, it does not go and ask.
    frame_.interpolation_alpha = interpolation_alpha();
    if (stepped && config_.simulation != nullptr) {
        const Status framed = config_.simulation->frame(frame_.interpolation_alpha, config_.jobs);
        if (!framed) {
            diag::trace_frame_end(frame_.frame_index);
            ++frame_.frame_index;
            return framed;
        }
    }

    diag::trace_frame_end(frame_.frame_index);
    ++frame_.frame_index;
    return stepped;
}

const determinism::SimulationClock& Runtime::clock() const noexcept {
    return config_.simulation != nullptr ? config_.simulation->clock() : clock_;
}

determinism::FrameTicks Runtime::accumulate(Nanoseconds delta_ns) noexcept {
    return config_.simulation != nullptr ? config_.simulation->begin_frame(delta_ns)
                                         : clock_.accumulate(delta_ns);
}

f32 Runtime::interpolation_alpha() const noexcept {
    return clock().interpolation_alpha();
}

void Runtime::pump_events() {
    config_.display->pump_events();

    WindowEvent event;
    while (config_.display->poll_event(event)) {
        ++frame_.events_handled;
        const diag::FieldValue fields[] = {
            diag::field_u64(window_id(), event.window),
            text_field(window_event(), window_event_type_name(event.type)),
        };
        diag::trace_instant(window_event_name(), runtime_category(), diag::Channel::Important,
                            fields, 2);

        if (event.type == WindowEventType::CloseRequested) {
            // Closing is the application's decision, and this application's decision is to stop.
            // The runtime records the intent; the host's loop observes it and returns from main().
            CY_LOG(runtime_category(), diag::LogLevel::Info, "runtime.close_requested",
                   diag::field_u64(window_id(), event.window));
            config_.platform->request_exit(0);
        }
    }

    frame_.events_dropped = config_.display->take_dropped_event_count();
    if (frame_.events_dropped != 0) {
        diag::trace_counter(events_dropped_name(), runtime_category(), diag::Channel::Critical,
                            frame_.events_dropped);
    }
}

Status Runtime::run_fixed_steps(Nanoseconds delta_ns) {
    const u64 discarded_before = frame_.discarded_ns;
    const determinism::FrameTicks ticks = accumulate(delta_ns);

    for (u32 index = 0; index < ticks.ticks; ++index) {
        diag::trace_tick_begin(frame_.total_ticks);
        if (config_.simulation != nullptr) {
            // The four fixed-step stages and the commit boundary. Everything a tick does is inside
            // this call, deliberately: `simulation-and-determinism` requires one point per tick at
            // which state becomes authoritative, and a second call site here would be a second one.
            const auto committed = config_.simulation->step(config_.jobs);
            if (!committed) {
                diag::trace_tick_end(frame_.total_ticks);
                return forward(committed.error());
            }
            frame_.committed = committed.value().point;
            frame_.state_version = committed.value().state_version;
        } else {
            // No simulation: the loop still runs, and the clock still advances, so a host that only
            // wants the frame pacing gets it. This is the M0 and M1 configuration.
            clock_.advance();
        }
        diag::trace_tick_end(frame_.total_ticks);
        ++frame_.ticks;
        ++frame_.total_ticks;
    }

    frame_.discarded_ns = ticks.discarded_ns;
    if (frame_.discarded_ns != discarded_before) {
        // `engine-architecture`: exceeding the cap discards the excess so the loop cannot enter a
        // death spiral, with a counter incremented rather than the time silently vanishing. The
        // clock did the discarding — it owns the accumulator — and this reports it.
        const diag::FieldValue fields[] = {
            diag::field_u64(frame_index(), frame_.frame_index),
            diag::field_u64(discarded_ns(), frame_.discarded_ns - discarded_before),
        };
        diag::trace_instant(tick_cap_name(), runtime_category(), diag::Channel::Critical, fields,
                            2);
    }
    return ok();
}

void Runtime::trace_instant_shutdown() const {
    if (trace_open_) {
        const diag::FieldValue fields[] = {diag::field_u64(frame_index(), frame_.frame_index)};
        diag::trace_instant(shutdown_name(), runtime_category(), diag::Channel::Critical, fields,
                            1);
    }
}

}  // namespace cy
