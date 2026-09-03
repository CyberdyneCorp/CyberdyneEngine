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
    {StartupStage::ModulesCore, nullptr, nullptr},
    {StartupStage::Display, &Runtime::enter_display, &Runtime::leave_display},
    {StartupStage::Servers, nullptr, nullptr},
    {StartupStage::ModulesServers, nullptr, nullptr},
    {StartupStage::EcsScene, nullptr, nullptr},
    {StartupStage::ModulesScene, nullptr, nullptr},
    {StartupStage::Scripting, nullptr, nullptr},
    {StartupStage::Editor, nullptr, nullptr},
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

// --- Stage 11: Boot ------------------------------------------------------------------------------
//
// "Load the startup scene or project, enter the main loop." There is no scene until M2 and the loop
// is the host's, so what remains is the frame clock's origin: the first tick measures from here
// rather than from whenever the host happens to call it.

Status Runtime::enter_boot() {
    last_frame_ns_ = config_.platform->monotonic_nanoseconds();
    accumulator_ns_ = 0;
    frame_ = FrameStats{};
    return ok();
}

void Runtime::leave_boot() {
    accumulator_ns_ = 0;
}

// --- The frame -----------------------------------------------------------------------------------

Status Runtime::tick() {
    CY_ASSERT_MSG(started_, "Runtime::tick() before startup()");

    const Nanoseconds now = config_.platform->monotonic_nanoseconds();
    const Nanoseconds delta = now > last_frame_ns_ ? now - last_frame_ns_ : 0;
    last_frame_ns_ = now;

    diag::trace_frame_begin(frame_.frame_index);

    frame_.duration_ns = delta;
    frame_.events_handled = 0;
    frame_.events_dropped = 0;
    frame_.ticks = 0;

    pump_events();
    run_fixed_steps(delta);

    frame_.interpolation_alpha = static_cast<f32>(static_cast<f64>(accumulator_ns_) /
                                                  static_cast<f64>(config_.fixed_step_ns));

    diag::trace_frame_end(frame_.frame_index);
    ++frame_.frame_index;
    return ok();
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

void Runtime::run_fixed_steps(Nanoseconds delta_ns) {
    accumulator_ns_ += delta_ns;

    while (accumulator_ns_ >= config_.fixed_step_ns && frame_.ticks < config_.max_ticks_per_frame) {
        // The stages a tick runs — PreSimulation, Physics, Simulation, PostSimulation — arrive with
        // the ECS at M2. The boundaries are on the timeline from M0 because a capture is read
        // against them.
        diag::trace_tick_begin(frame_.total_ticks);
        diag::trace_tick_end(frame_.total_ticks);
        accumulator_ns_ -= config_.fixed_step_ns;
        ++frame_.ticks;
        ++frame_.total_ticks;
    }

    if (accumulator_ns_ >= config_.fixed_step_ns) {
        // `engine-architecture`: exceeding the cap discards the excess so the loop cannot enter a
        // death spiral, with a counter incremented rather than the time silently vanishing.
        const u64 discarded = static_cast<u64>(accumulator_ns_);
        frame_.discarded_ns += discarded;
        accumulator_ns_ = 0;
        const diag::FieldValue fields[] = {
            diag::field_u64(frame_index(), frame_.frame_index),
            diag::field_u64(discarded_ns(), discarded),
        };
        diag::trace_instant(tick_cap_name(), runtime_category(), diag::Channel::Critical, fields,
                            2);
    }
}

void Runtime::trace_instant_shutdown() const {
    if (trace_open_) {
        const diag::FieldValue fields[] = {diag::field_u64(frame_index(), frame_.frame_index)};
        diag::trace_instant(shutdown_name(), runtime_category(), diag::Channel::Critical, fields,
                            1);
    }
}

}  // namespace cy
