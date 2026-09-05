#pragma once
// The input inspector: why an action did not trigger, what each processor did to the value, the
// event trace, and the latency view. Task 4.1.6.
//
// `input-and-actions` — "Input diagnostics": the inspector shows, per user, assigned devices,
// active scheme, the context stack with priorities, and for each action "its raw value, its value
// after each processor, its final value, its trigger phase, and **which binding produced it**". It
// must be able to answer **why an action did not trigger**, an event trace records device events
// with timestamps, and a latency view shows the path from device event to presented frame.
//
// --- WHY THE REASON IS AN ENUMERATOR AND NOT A STRING
// ---------------------------------------------
//
// "Why did nothing happen" has six answers and they are the six the requirement lists. Written as
// formatted text they are unsearchable, untestable and untranslatable; written as an enumerator
// with the naming data beside it, a test can assert that a consumed action reports
// `ConsumedByHigherContext` and names the context that consumed it. The interface formats it; the
// engine decides it. That is the same rule `gameplay-framework` applies to command validation, and
// for the same reason: one decision, four consumers, no disagreement.
//
// --- WHAT TRACING COSTS, STATED PLAINLY
// -----------------------------------------------------------
//
// Per-stage values are recorded **only while tracing is enabled**, and enabling it allocates the
// storage. `input-and-actions` requires evaluation to allocate nothing per frame; a trace buffer
// that existed unconditionally would be a per-frame write to memory nobody reads, and one sized for
// a thousand actions is not free. The evaluation path therefore branches on one bool, which is a
// predictable branch, and writes nothing when it is false.

#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/input/action.h>
#include <cy/servers/input/binding.h>
#include <cy/servers/input/event.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// Why an action did not trigger. The requirement's list, in its order, plus `Triggered` for the
/// case where it did — so that one field answers the question either way and a caller does not have
/// to combine a bool and a reason.
enum class ActionOutcome : u8 {
    Triggered = 0,
    /// The action is not bound in any context currently on the stack.
    NoBindingInActiveContext,
    /// A binding exists and produced a value below the trigger's actuation threshold.
    BelowThreshold,
    /// A higher-priority context binds this action and consumes it.
    ConsumedByHigherContext,
    /// The binding is bound and actuated, but the trigger's own conditions are not met — a hold
    /// that has not reached its duration, a sequence part-way through.
    TriggerConditionsUnmet,
    /// The context holding the binding does not match the user's focus layer.
    SuppressedByFocus,
    /// The binding names a device class this user has none of.
    DeviceUnassigned,
    Count,
};

const char* action_outcome_name(ActionOutcome outcome) noexcept;

/// One action's evaluation, recorded for the inspector.
struct ActionTrace {
    ActionId action = kInvalidAction;
    ActionOutcome outcome = ActionOutcome::NoBindingInActiveContext;
    /// The binding that produced the final value, or `0xFFFF`.
    u16 binding = 0xFFFFU;
    /// Where that binding came from. `0xFFFF` when nothing produced a value.
    u16 context_slot = 0xFFFFU;
    ContextHandle context;
    /// Before any processor.
    Vec3 raw;
    /// After each processor in the chain, in order. `stage_count` says how many are meaningful.
    Vec3 stages[kMaxProcessors];
    u8 stage_count = 0;
    /// After the modifier — the value the action received.
    Vec3 final_value;
    TriggerPhase phase = TriggerPhase::Idle;
    /// Set when `outcome` is `ConsumedByHigherContext`: the context that took it.
    ContextHandle consumed_by;
};

/// The stages a single input travels, for the latency view.
///
/// Recorded as four timestamps rather than three durations: durations cannot be recombined and a
/// missing stage would be indistinguishable from a zero one.
struct LatencySample {
    /// When the platform observed the device event.
    Nanoseconds device_event = 0;
    /// When the action layer resolved it.
    Nanoseconds action_evaluated = 0;
    /// When the simulation tick that consumed it ran.
    Nanoseconds tick_consumed = 0;
    /// When the frame showing its result was presented. Filled in by the renderer, which is why it
    /// may be zero here — the input system does not present frames and does not pretend to know.
    Nanoseconds frame_presented = 0;
    u64 tick = 0;

    [[nodiscard]] constexpr Nanoseconds to_action() const noexcept {
        return action_evaluated - device_event;
    }
    [[nodiscard]] constexpr Nanoseconds to_tick() const noexcept {
        return tick_consumed - device_event;
    }
    [[nodiscard]] constexpr Nanoseconds to_frame() const noexcept {
        return frame_presented == 0 ? 0 : frame_presented - device_event;
    }
};

/// The per-user recording. Off by default; enabling it allocates once. See the header comment.
class InputTrace {
public:
    explicit InputTrace(Allocator& allocator) noexcept
        : actions_(allocator), events_(allocator), latency_(allocator) {}

    /// Turn recording on and size the buffers. `action_count` comes from the registry, so a trace
    /// cannot be sized for fewer actions than exist.
    [[nodiscard]] Status enable(u32 action_count, u32 event_capacity) noexcept;
    void disable() noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// Start a tick's recording. Resets the per-tick action traces to "not bound", which is the
    /// correct default: an action nothing touched this tick was not bound in an active context.
    void begin_tick(u64 tick) noexcept;

    [[nodiscard]] ActionTrace* action(ActionId action) noexcept;
    [[nodiscard]] const ActionTrace* action(ActionId action) const noexcept;

    void record_event(const DeviceEvent& event) noexcept;
    void record_latency(const LatencySample& sample) noexcept;

    [[nodiscard]] const Array<DeviceEvent>& events() const noexcept { return events_; }
    [[nodiscard]] const Array<LatencySample>& latency() const noexcept { return latency_; }
    [[nodiscard]] u64 tick() const noexcept { return tick_; }

private:
    Array<ActionTrace> actions_;
    /// A ring: the trace is a window on recent history, not a log that grows until the process
    /// runs out of memory. `event_capacity` is the window.
    Array<DeviceEvent> events_;
    Array<LatencySample> latency_;
    usize event_capacity_ = 0;
    usize event_write_ = 0;
    u64 tick_ = 0;
    bool enabled_ = false;
};

}  // namespace cy::input
