#pragma once
// The rolling buffer, and the capture a condition triggers.
//
// `diagnostics-profiling-and-crash` — "Rolling buffer and automatic capture":
//
//   The engine SHALL maintain an **always-on rolling diagnostic buffer** of a configured recent
//   duration at low cost, holding trace events, logs, tick summaries, and health transitions.
//
//   Capture SHALL be **triggerable automatically** by declared conditions — a frame or tick
//   exceeding a threshold, a GPU overrun, an assertion, a health transition to critical — freezing
//   the window before and after the event and writing a capture artefact.
//
//   Manual capture SHALL also be available, and both SHALL produce the same artefact format.
//
//   A profiler SHALL NOT be required to be attached beforehand for a hitch to be diagnosable.
//
// THE SENTENCE THIS FILE EXISTS FOR is the last one. A hitch that happens once, on a player's
// machine, at three in the morning, cannot be caught by a profiler somebody attached afterwards.
// So the buffer is always on, holds only what fits, and discards the past continuously — and the
// only thing a trigger does is stop discarding for a moment and write what is held.
//
// HOW IT IS BUILT, AND WHY IT COSTS THE PRODUCER NOTHING. The trace's per-thread rings and its
// emission path are untouched: a producer still writes into its own ring and returns. The rolling
// buffer is a CONSUMER — it subscribes through `TraceConfig::observer` and copies each drained
// record into a fixed arena. The trace is opened with no path, so the always-on state writes no
// file at all; a capture artefact is written by a `TraceWriter` of its own, from the held records,
// at the moment something asks for one. That is what makes "always on at low cost" and "the same
// artefact format" both true, rather than one at the expense of the other.
//
// WHAT IS BOUNDED, AND WHAT HAPPENS WHEN IT IS NOT ENOUGH. The arena is a fixed number of bytes
// chosen at open and never grown — "no unbounded growth in any always-on buffer" is a forbidden
// pattern, not a guideline. Records leave it for two different reasons and the difference matters:
// falling out of the configured WINDOW is the buffer working, and being overwritten because the
// window did not fit in the arena is the buffer failing. The second is counted separately AND
// reported to the health model as `DiagnosticsLoss`, because a capture that silently holds two
// seconds when it was configured for ten is exactly the kind of gap the loss policy exists to make
// visible.

#include <cy/core/diagnostics/health.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy::diag {

/// Why a capture was written. It is recorded in the artefact's identity, so the file says what
/// asked for it rather than leaving a reader to infer it from what is in it.
enum class CaptureTrigger : u8 {
    /// Somebody asked. "Manual capture SHALL also be available."
    Manual = 0,
    FrameBudgetOverrun = 1,
    TickBudgetOverrun = 2,
    GpuBudgetOverrun = 3,
    Assertion = 4,
    HealthCritical = 5,
};

const char* capture_trigger_name(CaptureTrigger trigger) noexcept;

struct RollingConfig {
    /// How much recent history to hold. Records older than this leave the buffer.
    u64 window_ns = 5'000'000'000ULL;
    /// How much to keep recording AFTER a trigger before the artefact is written. "Freezing the
    /// window before and after the event" is two numbers, not one.
    u64 post_trigger_ns = 250'000'000ULL;
    /// The hard bound. Never grown, never exceeded; when the window does not fit, the buffer says
    /// so through `RollingStats::records_overwritten` and the health model.
    u32 arena_bytes = 4u << 20;
    /// What a capture artefact this buffer writes may contain.
    ExportPolicy policy = ExportPolicy::local();
    /// Where artefacts are written. Null uses the working directory, which is what a test wants and
    /// what a shipping build never does.
    const char* directory = nullptr;
    /// Recorded in every artefact this buffer writes.
    const char* build_identity = "";
    /// Drain on the trace's background consumer. False leaves every drain to `trace_flush()`, which
    /// is what a test wants and what makes the buffer's contents a function of the calls made.
    bool consumer_thread = true;
    u32 drain_interval_ms = 10;

    // --- The declared conditions
    // ------------------------------------------------------------------
    //
    // Zero means "do not trigger on this", so a build enables what it wants to pay for rather than
    // inheriting a threshold somebody else chose.

    /// A frame longer than this triggers a capture. `rolling_note_frame()` is what measures it.
    u64 frame_budget_ns = 0;
    /// A tick longer than this triggers a capture.
    u64 tick_budget_ns = 0;
    /// A GPU frame longer than this triggers a capture.
    u64 gpu_budget_ns = 0;
    /// A health condition transitioning to Critical triggers a capture.
    bool on_health_critical = true;
    /// A failed assertion triggers a capture. The bridge calls `capture_on_assertion()`.
    bool on_assertion = true;

    /// The most artefacts one session may write, so a pathological session does not fill a disk.
    u32 max_captures = 8;
    /// The shortest interval between two captures. A hitch that recurs every frame should produce
    /// one artefact and a count, not four hundred files.
    u64 cooldown_ns = 1'000'000'000ULL;
};

struct RollingStats {
    u64 records_held = 0;
    u64 bytes_held = 0;
    /// Left the buffer because they fell out of the configured window. The buffer working.
    u64 records_aged_out = 0;
    /// Left the buffer because the arena wrapped onto them. The buffer too small for the window,
    /// which is a different fact and is reported as one.
    u64 records_overwritten = 0;
    u64 captures_written = 0;
    /// Triggers refused by the cooldown or by `max_captures`, so a suppressed capture is a number
    /// rather than a silence.
    u64 triggers_suppressed = 0;
    u64 oldest_ns = 0;
    u64 newest_ns = 0;
    /// True between a trigger and the artefact being written.
    bool capture_pending = false;
};

/// Open the trace in rolling mode and start holding a window. Fails if a trace is already open.
///
/// This opens the ONE trace — there is no second transport — with no artefact path, so nothing is
/// written until something triggers a capture.
Expected<u32, cy::Error> rolling_open(const RollingConfig& config) noexcept;

/// Stop holding, close the trace, and release the arena. Any pending capture is written first, so a
/// session that ends between a trigger and its post-window still leaves the artefact behind.
void rolling_close() noexcept;

bool rolling_is_open() noexcept;

RollingStats rolling_stats() noexcept;

/// Ask for a capture. Returns the number of the artefact written or pending; fails when the buffer
/// is not open, when the cooldown has not elapsed, or when `max_captures` is reached — and a
/// refusal is counted in `RollingStats::triggers_suppressed` rather than being silent.
///
/// The artefact is not written here: recording continues for `post_trigger_ns` so the window after
/// the event is in it too. `rolling_poll()` is what completes it.
Expected<u32, cy::Error> capture_trigger(CaptureTrigger trigger, u64 detail) noexcept;

/// Complete a pending capture whose post-trigger window has elapsed. Cheap and idempotent: with no
/// capture pending it is one relaxed load. Called by the drain consumer on every pass, and callable
/// by a caller that drives the trace by hand.
void rolling_poll() noexcept;

/// The path of the last artefact written, or "" when none has been. Stable until the next capture.
const char* last_capture_path() noexcept;

// --- The declared conditions, as the calls that measure them -------------------------------------

/// Report a frame's duration. Triggers when `frame_budget_ns` is set and exceeded, and reports the
/// overrun to the health model either way, so the condition is visible without a capture.
///
/// Each of these three also calls `rolling_poll()`, so a frame boundary is the pump that completes
/// a capture whose post-trigger window has elapsed and the buffer needs no thread of its own.
void rolling_note_frame(u64 frame_index, u64 duration_ns) noexcept;
void rolling_note_tick(u64 tick_index, u64 duration_ns) noexcept;
void rolling_note_gpu(u64 frame_index, u64 duration_ns) noexcept;

/// Called by the assertion bridge. Separate from `capture_trigger()` because a failed assertion is
/// on its way to aborting the process: this one completes the artefact immediately rather than
/// waiting for a post-trigger window that will never arrive.
void capture_on_assertion() noexcept;

}  // namespace cy::diag
