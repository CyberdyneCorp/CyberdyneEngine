#pragma once
// One trace, many producers.
//
// `diagnostics-profiling-and-crash` — "One trace, many producers": a single transport and schema
// into which every subsystem emits. A capability's diagnostics requirement says what it emits and
// what question that answers; this file says how it is transported, buffered and captured. A
// subsystem that defines its own event format, buffer or capture file is a defect against that
// requirement, and there is nothing here to stop it except that everything it could want is here.
//
// The shape is fixed now because M1 through M6 land on it: archetype and chunk churn, residency and
// streaming decisions, tick boundaries and determinism hashes are new EventKinds and new classified
// fields on this timeline, not new timelines. `a` and `b` carry a kind's machine-word payload;
// structured fields carry everything named.
//
// Emission allocates nothing, formats nothing, hashes nothing, and takes no lock: a producer writes
// into its own thread's bounded ring and returns. Under pressure it drops by channel priority and
// counts what it dropped, because silent loss is a forbidden pattern.

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy::diag {

/// What a record is. The M0 set covers the transport requirement's list; a later milestone adds
/// kinds rather than adding a transport.
enum class EventKind : u8 {
    Padding = 0,  // ring filler, never written to an artefact
    ScopeBegin = 1,
    ScopeEnd = 2,
    Instant = 3,
    Counter = 4,
    FlowBegin = 5,
    FlowEnd = 6,
    Allocation = 7,
    Free = 8,
    TaskBegin = 9,
    TaskEnd = 10,
    GpuBegin = 11,
    GpuEnd = 12,
    IoRequest = 13,
    IoComplete = 14,
    NetworkEvent = 15,
    TickBegin = 16,
    TickEnd = 17,
    StateHash = 18,
    Breadcrumb = 19,
    Log = 20,
    Loss = 21,
    FrameBegin = 22,
    FrameEnd = 23,
};

/// A channel's priority, which is the whole of the loss policy: under buffer pressure the sampled
/// and verbose channels are refused first, and critical records — breadcrumbs, tick boundaries,
/// task lifecycle, loss itself — are refused last.
enum class Channel : u8 {
    Critical = 0,
    Important = 1,
    Verbose = 2,
    Sampled = 3,
};

inline constexpr u32 kChannelCount = 4;

/// A log record's severity. It lives here rather than in log.h because a log is a record on this
/// timeline — the level is the Log kind's `a` word — not a second mechanism beside it.
enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Fatal = 5,
    Off = 6,
};

/// One structured value as a producer passes it. `text` is read during the call and copied into the
/// thread's own buffer; nothing is retained.
struct FieldValue {
    FieldId field = kInvalidField;
    u64 bits = 0;
    const char* text = nullptr;
    u32 text_length = 0;
};

FieldValue field_u64(FieldId field, u64 value) noexcept;
FieldValue field_i64(FieldId field, i64 value) noexcept;
FieldValue field_f64(FieldId field, f64 value) noexcept;
FieldValue field_bool(FieldId field, bool value) noexcept;
/// A dynamic string. Truncated to the record limit rather than allocating; truncation is visible in
/// the artefact because the length written is the length kept.
FieldValue field_text(FieldId field, const char* text, u32 length) noexcept;

/// The most that fits in one record, so a producer cannot make a record the ring cannot hold.
inline constexpr u32 kMaxFieldsPerRecord = 16;
inline constexpr u32 kMaxTextBytesPerRecord = 512;

using TraceId = u64;

struct TraceConfig {
    /// Where the artefact is written. Required.
    const char* path = nullptr;
    /// Per-thread ring size. Rounded up to a power of two; the loss policy is expressed as fill
    /// fractions of it.
    u32 buffer_bytes_per_thread = 1u << 16;
    /// What classifications this artefact may contain. The writer redacts everything above it.
    ExportPolicy policy = ExportPolicy::local();
    /// Drain on a background consumer. False keeps everything on the caller's flush, which is what
    /// a test wants.
    bool consumer_thread = true;
    u32 drain_interval_ms = 10;
    /// Records at or above this level are formatted to standard error by the consumer — at
    /// presentation, never at emission. Off silences it.
    LogLevel console_level = LogLevel::Off;
    /// Recorded in the artefact's metadata so a capture identifies the build that wrote it.
    const char* build_identity = nullptr;
};

struct TraceStats {
    TraceId trace_id = 0;
    u64 events_emitted = 0;
    u64 events_written = 0;
    u64 bytes_written = 0;
    u64 dropped[kChannelCount] = {0, 0, 0, 0};
    u64 redacted_fields = 0;
    u64 unclassified_fields = 0;
    u32 threads = 0;
};

/// Open the one trace. Fails if one is already open, if the path is missing, or if the file cannot
/// be created.
Expected<TraceId, cy::Error> trace_open(const TraceConfig& config) noexcept;

/// Drain every thread's buffer into the artefact now. Safe to call while the consumer thread runs.
void trace_flush() noexcept;

/// Drain, write the metadata, loss and index chunks, and close. Returns what the trace recorded.
Expected<TraceStats, cy::Error> trace_close() noexcept;

/// True between a successful open and a close. Emission is a load of this and a return when false.
bool trace_is_open() noexcept;

/// What the trace has recorded so far, without closing it.
TraceStats trace_stats() noexcept;

// --- Emission ------------------------------------------------------------------------------------

void trace_emit(EventKind kind, Channel channel, NameId name, CategoryId category, u64 a, u64 b,
                const FieldValue* fields, u32 field_count) noexcept;

void trace_instant(NameId name, CategoryId category, Channel channel,
                   const FieldValue* fields = nullptr, u32 field_count = 0) noexcept;
void trace_counter(NameId name, CategoryId category, Channel channel, u64 value) noexcept;
void trace_counter_f64(NameId name, CategoryId category, Channel channel, f64 value) noexcept;
void trace_scope_begin(NameId name, CategoryId category, Channel channel) noexcept;
void trace_scope_end(NameId name, CategoryId category, Channel channel) noexcept;
void trace_flow_begin(NameId name, CategoryId category, u64 flow_id) noexcept;
void trace_flow_end(NameId name, CategoryId category, u64 flow_id) noexcept;
/// A simulation tick boundary. Critical: it is what a capture is read against.
void trace_tick_begin(u64 tick_index) noexcept;
void trace_tick_end(u64 tick_index) noexcept;
/// A determinism hash for a tick. The M6 replay comparison reads these; M0 only transports them.
void trace_state_hash(NameId name, u64 hash) noexcept;
/// A frame boundary. The frame index is also held for the crash report, which names the last frame
/// the process reached.
void trace_frame_begin(u64 frame_index) noexcept;
void trace_frame_end(u64 frame_index) noexcept;
/// The last frame index passed to trace_frame_begin(). Readable from a signal handler.
u64 trace_last_frame() noexcept;

/// A scope, ended by its destructor. The name is registered once at the declaration site.
class ScopedTrace {
public:
    ScopedTrace(NameId name, CategoryId category, Channel channel) noexcept
        : name_(name), category_(category), channel_(channel) {
        trace_scope_begin(name_, category_, channel_);
    }
    ~ScopedTrace() noexcept { trace_scope_end(name_, category_, channel_); }

    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;
    ScopedTrace(ScopedTrace&&) = delete;
    ScopedTrace& operator=(ScopedTrace&&) = delete;

private:
    NameId name_;
    CategoryId category_;
    Channel channel_;
};

// --- Self-measurement ----------------------------------------------------------------------------

/// The cost of the engine's own diagnostics, so the budget claim in
/// `diagnostics-profiling-and-crash` is a number rather than an assertion. Runs `samples` emissions
/// of each shape and reports the mean.
struct EmissionCost {
    u64 samples = 0;
    f64 instant_ns = 0.0;         // one Instant, no fields
    f64 instant_fields_ns = 0.0;  // one Instant with two fields
    f64 scope_pair_ns = 0.0;      // a begin and its end
    /// Whether a trace was open while measuring. Called with one closed, the same figures are the
    /// cost of diagnostics that are compiled in and turned off — which is the shipping number.
    bool trace_was_open = false;
};

EmissionCost measure_emission_cost(u32 samples) noexcept;

}  // namespace cy::diag

#define CY_DIAG_CONCAT_(a, b) a##b
#define CY_DIAG_CONCAT(a, b) CY_DIAG_CONCAT_(a, b)

/// Time a lexical scope. The name is a literal, registered once on first entry.
#define CY_TRACE_SCOPE(literal, category, channel)                                   \
    static const ::cy::diag::NameId CY_DIAG_CONCAT(cy_trace_scope_name_, __LINE__) = \
        ::cy::diag::register_name(literal);                                          \
    const ::cy::diag::ScopedTrace CY_DIAG_CONCAT(cy_trace_scope_, __LINE__)(         \
        CY_DIAG_CONCAT(cy_trace_scope_name_, __LINE__), (category), (channel))
