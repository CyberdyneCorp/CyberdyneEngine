#pragma once
// Normalised, timestamped device events, and the buffer that accumulates them between ticks.
// Tasks 4.1.1 and 4.1.4.
//
// ================================================================================================
// THE ONE THING THIS FILE EXISTS TO PREVENT
// ================================================================================================
//
// `input-and-actions` — "Fixed-tick sampling": input consumed by fixed-step simulation SHALL be
// resolved from **timestamped events accumulated between ticks**, not from whichever value happened
// to be current when the tick began. "A press and release occurring within one frame SHALL still be
// observed by the tick."
//
// design.md §5 names the defect precisely: the implementation everybody writes is "sample the
// current state each tick", it passes every manual test, and it loses inputs *exactly* when the
// frame rate is uneven — which is when players notice and when nobody can reproduce it.
//
// The mechanism here is the whole answer, and it is three rules:
//
//   1. The platform layer **does not coalesce transitions**. Two events with the same control and
//      the same value are still two events. A backend that folded a press and a release into "the
//      key is up" would have destroyed the information before this layer saw it, and no amount of
//      care above could recover it. `push()` therefore never compares against the previous event.
//   2. Every event carries the timestamp the platform *observed* it at, not the time it was
//      processed. Latency analysis, fixed-tick resolution and replay all need the first.
//   3. Resolution walks the accumulated events **in order** and applies each one, so an action's
//      per-tick record counts transitions rather than sampling a level.
//
// A `sequence` number accompanies the timestamp because two events can share a timestamp — a
// backend that reads a whole gamepad state in one go stamps them all identically — and the order
// within that instant is still the order the device reported. Sorting by `(timestamp, sequence)` is
// therefore a total order and a stable one, which is what replay needs.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/servers/input/types.h>

namespace cy::input {

CY_HANDLE_TAG(InputDevice);
/// The stable identifier a device event carries. Generational, so an event from a controller that
/// has since been unplugged and replaced cannot be attributed to its successor.
using DeviceId = Handle<InputDeviceTag>;

/// One normalised device event.
///
/// 32 bytes, trivially copyable, no pointer. A replay writes these to a file and reads them back
/// without a marshalling step, and the event trace holds a span of them rather than a list of
/// allocations.
struct DeviceEvent {
    /// When the platform layer observed it. Monotonic nanoseconds — never the processing time.
    Nanoseconds timestamp = 0;
    DeviceId device;
    Control control;
    /// The normalised value. Digital controls report 0 or 1; axes report [-1, 1]; a delta reports
    /// the displacement since the previous event, in the control's own units.
    f32 value = 0.0F;
    EventSource source = EventSource::Physical;
    /// The order within one timestamp. Assigned by the buffer, never by the backend, so two
    /// backends feeding one buffer still produce a total order.
    u32 sequence = 0;

    [[nodiscard]] constexpr bool before(const DeviceEvent& other) const noexcept {
        return timestamp != other.timestamp ? timestamp < other.timestamp
                                            : sequence < other.sequence;
    }
};

static_assert(sizeof(DeviceEvent) <= 40, "a device event is a small value: keep it one");

/// The accumulation window between two resolutions.
///
/// Single-producer, single-consumer by construction: the platform layer pushes on the thread that
/// pumps the platform, and the tick resolves on the tick thread, at the quiesced boundary the
/// runtime already has. It is deliberately not a lock-free multi-producer queue — the honest shape
/// for a subsystem whose consumer is one fixed point in the frame.
///
/// ON OVERFLOW. A full buffer **drops the newest event and counts it**, and the count is reported
/// rather than being an internal statistic: an input that was discarded is a defect the player will
/// feel, and a silent drop is the same class of bug as the coalescing this file exists to forbid.
/// The newest is dropped rather than the oldest because the oldest carries the press whose release
/// the newest would have been, and half a transition pair leaves an action stuck down. Neither
/// choice is good; the capacity is what makes the choice not matter, and `dropped()` is what says
/// when it did.
class EventBuffer {
public:
    explicit EventBuffer(Allocator& allocator) noexcept : events_(allocator) {}

    /// Size the window. Called once at configure; never in the evaluation path.
    [[nodiscard]] Status reserve(usize capacity) noexcept;

    /// Append an event, stamping it with the next sequence number.
    ///
    /// **Never coalesces.** Two identical events in a row are two events; see the header comment.
    void push(const DeviceEvent& event) noexcept;

    /// The events accumulated since the last `clear()`, in `(timestamp, sequence)` order.
    ///
    /// Already sorted when pushes were monotonic, which they are for a single backend. `sort()`
    /// exists for the case they are not — two backends, or a remote source whose events arrive out
    /// of order — and the resolver calls it, so ordering is a property of resolution rather than an
    /// assumption about producers.
    void sort() noexcept;

    [[nodiscard]] const DeviceEvent* data() const noexcept { return events_.data(); }
    [[nodiscard]] usize size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] const DeviceEvent& operator[](usize index) const noexcept {
        return events_[index];
    }

    /// Begin a new accumulation window. The sequence counter is **not** reset: it is a tiebreaker
    /// within a timestamp, and restarting it would make two windows' events compare equal in a
    /// trace that spans both.
    void clear() noexcept { events_.clear(); }

    /// How many events were discarded for want of capacity since the last `take_dropped()`.
    [[nodiscard]] u32 dropped() const noexcept { return dropped_; }
    [[nodiscard]] u32 take_dropped() noexcept {
        const u32 count = dropped_;
        dropped_ = 0;
        return count;
    }

    [[nodiscard]] usize capacity() const noexcept { return capacity_; }

private:
    Array<DeviceEvent> events_;
    usize capacity_ = 0;
    u32 next_sequence_ = 0;
    u32 dropped_ = 0;
};

}  // namespace cy::input
