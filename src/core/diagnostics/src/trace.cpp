// The trace: thread registration, the emission path, and the drain.
//
// The emission path is the part of this file that matters. It is, in order: one relaxed load to see
// whether a trace is open, one thread-local pointer read, one clock read, one bounds check inside
// the producer's own ring, and a memcpy of a record it composed on its stack. No allocation, no
// string work, no lock, no call into another subsystem. A producer that is refused by the loss
// policy increments one relaxed counter and returns; it never blocks on the consumer.

#include <cy/core/diagnostics/trace.h>

#include <cy/core/diagnostics/bridge.h>

#include "internal.h"
#include "lifetime.h"
#include "ring.h"
#include "tracy_sink.h"
#include "writer.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace cy::diag {
namespace {

using format::FieldRecord;
using format::RecordBody;
using format::RecordHeader;

constexpr u32 align_up(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/// A thread's buffer and its identity in the artefact. Slots are never freed: a thread that exits
/// returns its slot to a free list for the next one, so a process that creates and destroys many
/// threads does not grow a buffer per thread, and a consumer never holds a pointer that a producer
/// can invalidate.
struct ThreadSlot {
    ThreadRing ring;
    u32 index = 0;
    std::atomic<bool> in_use{true};
};

struct System {
    std::atomic<bool> open{false};
    std::atomic<u64> events_emitted{0};
    std::atomic<u64> last_frame{0};

    std::mutex consumer_mutex;  // held by whoever is draining: the thread, or a caller of flush()
    std::mutex slots_mutex;     // held only when a thread claims or releases a slot
    std::vector<ThreadSlot*> slots;

    TraceWriter writer;
    TraceConfig config;
    TraceId id = 0;

    std::thread drainer;
    std::atomic<bool> stop{false};

    NameId loss_name = kInvalidName;
    NameId frame_name = kInvalidName;
    NameId tick_name = kInvalidName;
    CategoryId trace_category = kInvalidCategory;
};

System& system() noexcept {
    static System instance;
    return instance;
}

thread_local ThreadSlot* t_slot = nullptr;

/// Returned to the free list when the thread ends, so the slot outlives the thread that used it.
struct SlotRelease {
    ~SlotRelease() {
        if (t_slot != nullptr) {
            t_slot->in_use.store(false, std::memory_order_release);
            t_slot = nullptr;
        }
    }
};
thread_local SlotRelease t_slot_release;

ThreadSlot* claim_slot(System& sys) noexcept {
    const std::lock_guard<std::mutex> guard(sys.slots_mutex);
    for (ThreadSlot* slot : sys.slots) {
        bool expected = false;
        if (slot->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return slot;
        }
    }
    auto* slot = new (std::nothrow) ThreadSlot();
    if (slot == nullptr) {
        return nullptr;
    }
    if (!slot->ring.initialize(sys.config.buffer_bytes_per_thread)) {
        delete slot;
        return nullptr;
    }
    slot->index = static_cast<u32>(sys.slots.size());
    sys.slots.push_back(slot);
    // The slot outlives the thread that claimed it, and `sys` is a function-local static whose
    // vector is destroyed before the leak detector looks. Declared for the same reason its ring is:
    // this is pooling, and the detector should say so rather than report it every run.
    declare_process_lifetime(slot);
    return slot;
}

ThreadSlot* current_slot(System& sys) noexcept {
    if (t_slot != nullptr) {
        return t_slot;
    }
    t_slot = claim_slot(sys);
    (void)&t_slot_release;  // the guard exists for its destructor
    return t_slot;
}

u32 record_bytes(u32 field_count, u32 text_bytes) noexcept {
    return align_up(
        format::kRecordFixedBytes + (field_count * u32{sizeof(FieldRecord)}) + text_bytes,
        format::kRecordAlignment);
}

/// Compose one record in place. The caller has already reserved exactly `record_bytes()` bytes.
void compose(u8* out, EventKind kind, Channel channel, NameId name, CategoryId category, u64 a,
             u64 b, const FieldValue* fields, u32 field_count, u32 text_bytes) noexcept {
    RecordHeader header{};
    header.size = static_cast<u16>(record_bytes(field_count, text_bytes));
    header.kind = static_cast<u8>(kind);
    header.channel = static_cast<u8>(channel);
    header.name = name;
    std::memcpy(out, &header, sizeof(header));

    RecordBody body{};
    body.timestamp_ns = monotonic_now_ns();
    body.category = category;
    body.field_count = static_cast<u16>(field_count);
    body.text_bytes = static_cast<u16>(text_bytes);
    body.a = a;
    body.b = b;
    std::memcpy(out + sizeof(header), &body, sizeof(body));

    u8* field_bytes = out + format::kRecordFixedBytes;
    u8* text = field_bytes + (field_count * sizeof(FieldRecord));
    u32 text_offset = 0;
    for (u32 index = 0; index < field_count; ++index) {
        const FieldValue& value = fields[index];
        FieldRecord record{};
        record.field = value.field;
        if (value.text != nullptr) {
            const u32 length = (text_offset + value.text_length <= text_bytes)
                                   ? value.text_length
                                   : (text_bytes - text_offset);
            std::memcpy(text + text_offset, value.text, length);
            record.text_offset = static_cast<u16>(text_offset);
            record.bits = length;
            text_offset += length;
        } else {
            record.bits = value.bits;
        }
        std::memcpy(field_bytes + (index * sizeof(FieldRecord)), &record, sizeof(record));
    }
}

/// The bytes a call's text fields will occupy, clamped to what one record may hold.
u32 text_budget(const FieldValue* fields, u32 field_count) noexcept {
    u32 total = 0;
    for (u32 index = 0; index < field_count; ++index) {
        if (fields[index].text != nullptr) {
            total += fields[index].text_length;
        }
    }
    return (total > kMaxTextBytesPerRecord) ? kMaxTextBytesPerRecord : total;
}

/// Append a record the consumer synthesised — a Loss marker — through the same path a producer's
/// record takes, so the artefact has one record shape and one reader.
void append_synthetic(TraceWriter& writer, EventKind kind, NameId name, CategoryId category, u64 a,
                      u64 b) noexcept {
    alignas(8) u8 buffer[format::kRecordFixedBytes];
    compose(buffer, kind, Channel::Critical, name, category, a, b, nullptr, 0, 0);
    writer.append_record(buffer, sizeof(buffer));
}

void drain_slot(System& sys, ThreadSlot* slot) noexcept {
    sys.writer.begin_thread_chunk(slot->index);
    slot->ring.drain(
        [&sys](const u8* record, u32 size) { sys.writer.append_record(record, size); });
    for (u32 channel = 0; channel < kChannelCount; ++channel) {
        const u64 dropped = slot->ring.take_drops(channel);
        if (dropped != 0) {
            // Recorded twice: on the timeline, where the gap is, and in the LOSS chunk, where a
            // reader looks for the total. A capture that lost events says so.
            append_synthetic(sys.writer, EventKind::Loss, sys.loss_name, sys.trace_category,
                             channel, dropped);
            sys.writer.record_loss(slot->index, static_cast<Channel>(channel),
                                   format::LossReason::BufferPressure, dropped);
        }
    }
    const u64 oversized = slot->ring.take_oversized();
    if (oversized != 0) {
        append_synthetic(sys.writer, EventKind::Loss, sys.loss_name, sys.trace_category,
                         kChannelCount, oversized);
        sys.writer.record_loss(slot->index, Channel::Critical, format::LossReason::RecordTooLarge,
                               oversized);
    }
    sys.writer.end_thread_chunk();
}

/// One pass over every slot. The consumer mutex serialises the drain thread against a caller of
/// trace_flush(), so each ring still has exactly one consumer at a time.
void drain_all(System& sys) noexcept {
    const std::lock_guard<std::mutex> guard(sys.consumer_mutex);
    if (!sys.writer.is_open()) {
        return;
    }
    std::vector<ThreadSlot*> slots;
    {
        const std::lock_guard<std::mutex> slot_guard(sys.slots_mutex);
        slots = sys.slots;
    }
    for (ThreadSlot* slot : slots) {
        drain_slot(sys, slot);
    }
    sys.writer.flush();
}

void drain_loop(System& sys) noexcept {
    while (!sys.stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sys.config.drain_interval_ms));
        drain_all(sys);
    }
}

}  // namespace

// --- Field values --------------------------------------------------------------------------------

FieldValue field_u64(FieldId field, u64 value) noexcept {
    return FieldValue{field, value, nullptr, 0};
}

FieldValue field_i64(FieldId field, i64 value) noexcept {
    return FieldValue{field, static_cast<u64>(value), nullptr, 0};
}

FieldValue field_f64(FieldId field, f64 value) noexcept {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return FieldValue{field, bits, nullptr, 0};
}

FieldValue field_bool(FieldId field, bool value) noexcept {
    return FieldValue{field, value ? 1u : 0u, nullptr, 0};
}

FieldValue field_text(FieldId field, const char* text, u32 length) noexcept {
    const u32 clamped = (length > kMaxTextBytesPerRecord) ? kMaxTextBytesPerRecord : length;
    return FieldValue{field, clamped, (text != nullptr) ? text : "", clamped};
}

// --- Lifecycle -----------------------------------------------------------------------------------

Expected<TraceId, cy::Error> trace_open(const TraceConfig& config) noexcept {
    System& sys = system();
    if (sys.open.load(std::memory_order_acquire)) {
        return fail(ErrorCode::AlreadyExists, "a trace is already open");
    }
    if (config.path == nullptr || config.path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "TraceConfig::path is required");
    }

    sys.config = config;
    sys.loss_name = register_name("trace.loss");
    sys.frame_name = register_name("frame");
    sys.tick_name = register_name("tick");
    sys.trace_category = register_category("trace");

    const TraceId id = monotonic_now_ns() ^ (static_cast<u64>(registry_stats().names) << 48);
    Expected<TraceId, cy::Error> opened = sys.writer.open(config, id);
    if (!opened) {
        return opened;
    }
    sys.id = id;
    sys.events_emitted.store(0, std::memory_order_relaxed);
    sys.stop.store(false, std::memory_order_release);
    sys.open.store(true, std::memory_order_release);
    // From here, base's assertion failures and its diagnostic sink land on this timeline rather
    // than on standard error. Both are restored by trace_close().
    install_assertion_bridge();
    install_diagnostic_bridge();
    if (config.consumer_thread) {
        sys.drainer = std::thread([&sys] { drain_loop(sys); });
    }
    return id;
}

void trace_flush() noexcept {
    drain_all(system());
}

bool trace_is_open() noexcept {
    return system().open.load(std::memory_order_relaxed);
}

TraceStats trace_stats() noexcept {
    System& sys = system();
    const std::lock_guard<std::mutex> guard(sys.consumer_mutex);
    TraceStats stats = sys.writer.stats();
    stats.events_emitted = sys.events_emitted.load(std::memory_order_relaxed);
    const std::lock_guard<std::mutex> slot_guard(sys.slots_mutex);
    stats.threads = static_cast<u32>(sys.slots.size());
    return stats;
}

Expected<TraceStats, cy::Error> trace_close() noexcept {
    System& sys = system();
    if (!sys.open.load(std::memory_order_acquire)) {
        return fail(ErrorCode::Unavailable, "no trace is open");
    }
    sys.open.store(false, std::memory_order_release);
    sys.stop.store(true, std::memory_order_release);
    uninstall_assertion_bridge();
    uninstall_diagnostic_bridge();
    if (sys.drainer.joinable()) {
        sys.drainer.join();
    }
    drain_all(sys);

    const std::lock_guard<std::mutex> guard(sys.consumer_mutex);
    const u64 emitted = sys.events_emitted.load(std::memory_order_relaxed);
    u32 threads = 0;
    {
        const std::lock_guard<std::mutex> slot_guard(sys.slots_mutex);
        threads = static_cast<u32>(sys.slots.size());
    }
    Expected<TraceStats, cy::Error> closed = sys.writer.close();
    if (!closed) {
        return closed;
    }
    TraceStats stats = closed.value();
    stats.events_emitted = emitted;
    stats.threads = threads;
    return stats;
}

// --- Emission ------------------------------------------------------------------------------------

void trace_emit(EventKind kind, Channel channel, NameId name, CategoryId category, u64 a, u64 b,
                const FieldValue* fields, u32 field_count) noexcept {
    System& sys = system();
    if (!sys.open.load(std::memory_order_relaxed)) {
        return;
    }
    ThreadSlot* slot = current_slot(sys);
    if (slot == nullptr) {
        return;
    }
    const u32 count = (field_count > kMaxFieldsPerRecord) ? kMaxFieldsPerRecord : field_count;
    const u32 text_bytes = text_budget(fields, count);
    u8* out = slot->ring.reserve(record_bytes(count, text_bytes), channel);
    if (out == nullptr) {
        return;  // refused by the loss policy, and counted by it
    }
    compose(out, kind, channel, name, category, a, b, fields, count, text_bytes);
    slot->ring.commit();
    sys.events_emitted.fetch_add(1, std::memory_order_relaxed);
    tracy_publish(kind, name, category, a, b);
}

void trace_instant(NameId name, CategoryId category, Channel channel, const FieldValue* fields,
                   u32 field_count) noexcept {
    trace_emit(EventKind::Instant, channel, name, category, 0, 0, fields, field_count);
}

void trace_counter(NameId name, CategoryId category, Channel channel, u64 value) noexcept {
    trace_emit(EventKind::Counter, channel, name, category, value, 0, nullptr, 0);
}

void trace_counter_f64(NameId name, CategoryId category, Channel channel, f64 value) noexcept {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    trace_emit(EventKind::Counter, channel, name, category, bits, 1, nullptr, 0);
}

void trace_scope_begin(NameId name, CategoryId category, Channel channel) noexcept {
    trace_emit(EventKind::ScopeBegin, channel, name, category, 0, 0, nullptr, 0);
}

void trace_scope_end(NameId name, CategoryId category, Channel channel) noexcept {
    trace_emit(EventKind::ScopeEnd, channel, name, category, 0, 0, nullptr, 0);
}

void trace_flow_begin(NameId name, CategoryId category, u64 flow_id) noexcept {
    trace_emit(EventKind::FlowBegin, Channel::Important, name, category, flow_id, 0, nullptr, 0);
}

void trace_flow_end(NameId name, CategoryId category, u64 flow_id) noexcept {
    trace_emit(EventKind::FlowEnd, Channel::Important, name, category, flow_id, 0, nullptr, 0);
}

void trace_tick_begin(u64 tick_index) noexcept {
    System& sys = system();
    trace_emit(EventKind::TickBegin, Channel::Critical, sys.tick_name, sys.trace_category,
               tick_index, 0, nullptr, 0);
}

void trace_tick_end(u64 tick_index) noexcept {
    System& sys = system();
    trace_emit(EventKind::TickEnd, Channel::Critical, sys.tick_name, sys.trace_category, tick_index,
               0, nullptr, 0);
}

void trace_state_hash(NameId name, u64 hash) noexcept {
    trace_emit(EventKind::StateHash, Channel::Critical, name, system().trace_category, hash, 0,
               nullptr, 0);
}

void trace_frame_begin(u64 frame_index) noexcept {
    System& sys = system();
    sys.last_frame.store(frame_index, std::memory_order_relaxed);
    trace_emit(EventKind::FrameBegin, Channel::Critical, sys.frame_name, sys.trace_category,
               frame_index, 0, nullptr, 0);
}

void trace_frame_end(u64 frame_index) noexcept {
    System& sys = system();
    trace_emit(EventKind::FrameEnd, Channel::Critical, sys.frame_name, sys.trace_category,
               frame_index, 0, nullptr, 0);
}

u64 trace_last_frame() noexcept {
    return system().last_frame.load(std::memory_order_relaxed);
}

// --- Self-measurement ----------------------------------------------------------------------------

namespace {

CY_TRACE_NAME(measurement_name, "diagnostics.measure")
CY_TRACE_CATEGORY(measurement_category, "diagnostics")
CY_TRACE_FIELD(measure_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(measure_value, f64, cy::Privacy::Public)

f64 mean_ns(u64 elapsed_ns, u32 samples) noexcept {
    return (samples == 0) ? 0.0 : static_cast<f64>(elapsed_ns) / static_cast<f64>(samples);
}

}  // namespace

EmissionCost measure_emission_cost(u32 samples) noexcept {
    EmissionCost cost{};
    cost.samples = samples;
    cost.trace_was_open = trace_is_open();
    if (samples == 0) {
        return cost;
    }
    const NameId name = measurement_name();
    const CategoryId category = measurement_category();

    u64 start = monotonic_now_ns();
    for (u32 index = 0; index < samples; ++index) {
        trace_instant(name, category, Channel::Verbose);
    }
    cost.instant_ns = mean_ns(monotonic_now_ns() - start, samples);

    const FieldValue fields[] = {field_u64(measure_index(), 1), field_f64(measure_value(), 2.0)};
    start = monotonic_now_ns();
    for (u32 index = 0; index < samples; ++index) {
        trace_instant(name, category, Channel::Verbose, fields, 2);
    }
    cost.instant_fields_ns = mean_ns(monotonic_now_ns() - start, samples);

    start = monotonic_now_ns();
    for (u32 index = 0; index < samples; ++index) {
        trace_scope_begin(name, category, Channel::Verbose);
        trace_scope_end(name, category, Channel::Verbose);
    }
    cost.scope_pair_ns = mean_ns(monotonic_now_ns() - start, samples);
    return cost;
}

}  // namespace cy::diag
