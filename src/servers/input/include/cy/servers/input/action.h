#pragma once
// Actions: their declarations, their stable identity, and the per-user state record. Tasks 4.1.2
// and 4.1.3.
//
// `input-and-actions` — "Actions and value types": an action is a semantic, device-independent
// input with a declared value type; actions are identified by **stable identifiers cooked from
// authored names**; "Strings SHALL NOT be the runtime identity of an action".
//
// --- TWO NUMBERS, AND WHY BOTH ARE NEEDED --------------------------------------------------------
//
// `ActionStableId` is the persistent identity — assigned once when the action is authored, recorded
// alongside the rest of the project's identity (`core-type-system`'s manifest), and never derived
// from the name. It is what a saved rebinding profile stores and what a cooked binding refers to,
// which is what makes the specification's scenario true: rename the action and every existing
// binding and profile still resolves.
//
// `ActionId` is the dense runtime index into this registry's arrays. It is *not* persistent, is
// never written to a file, and exists because the evaluation path indexes a per-user array a
// thousand times a tick and a persistent identifier would make that a lookup.
//
// Conflating the two is the mistake this comment exists to prevent: an engine that persists the
// dense index has a save file that breaks when an action is inserted, and an engine that indexes
// arrays by the persistent id has a sparse array with a thousand holes.
//
// --- WHY THE STATE RECORD LOOKS LIKE THIS --------------------------------------------------------
//
// "Action state SHALL be maintained as compact per-user records holding current value, previous
// value, transition flags, trigger phase, and the time of the last transition", and "Continuous
// actions SHALL NOT generate an event or an allocation per frame."
//
// So there is no event object here and no list. There is one POD per (user, action), written in
// place by the tick resolver and read by sampling. The transition *counts* are the part that makes
// design.md §5's requirement work: a press and a release inside one tick are two counts on one
// record, not two events that a sampling implementation would never have created.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// The persistent identity of an action. Opaque, authored, never derived from the name.
///
/// Zero is null, exactly as `reflect::TypeId` is, so a default-constructed declaration is invalid
/// rather than being action number zero's.
struct ActionStableId {
    u32 value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ActionStableId a, ActionStableId b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(ActionStableId a, ActionStableId b) noexcept {
        return !(a == b);
    }
};

/// The dense runtime index. Not persistent — see the header comment.
using ActionId = u32;
inline constexpr ActionId kInvalidAction = 0xFFFFFFFFU;

/// What an action is, as authored.
struct ActionDeclaration {
    /// For diagnostics, the editor and the cook. Never consulted in the evaluation path.
    Name name;
    ActionStableId stable_id;
    ActionValueType type = ActionValueType::Digital;
    /// `input-and-actions` — "Input buffering": a triggered action may remain valid for a declared
    /// window "so that an input arriving slightly early is not discarded". Zero disables it.
    ///
    /// The mechanism is here; the semantics — coyote time, cancel windows, queued attacks — are
    /// gameplay's, which is why this is a duration and not a policy.
    f32 buffer_window_seconds = 0.0F;
    /// Whether this action is part of the per-tick command frame, and at which slot. See
    /// `frame.h`: a command frame is a compact record for prediction and replay, not every action
    /// the project declares.
    bool in_command_frame = false;
};

/// The transition flags on a state record. A tick's worth, cleared at the start of each resolution.
enum class ActionFlag : u16 {
    None = 0,
    /// Actuated at the end of the tick.
    Pressed = 1U << 0U,
    /// Went from not actuated to actuated at least once during the tick.
    JustPressed = 1U << 1U,
    /// Went from actuated to not actuated at least once during the tick.
    JustReleased = 1U << 2U,
    /// The trigger fired during the tick.
    Triggered = 1U << 3U,
    /// The trigger began during the tick.
    Started = 1U << 4U,
    /// The trigger completed during the tick.
    Completed = 1U << 5U,
    /// The trigger was abandoned during the tick.
    Cancelled = 1U << 6U,
    /// The value came from a synthetic, remote or replay source. Carried so that a diagnostic can
    /// say so and a shipping build can refuse it.
    Synthetic = 1U << 7U,
};

[[nodiscard]] constexpr u16 operator|(ActionFlag a, ActionFlag b) noexcept {
    return static_cast<u16>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr bool has_flag(u16 flags, ActionFlag flag) noexcept {
    return (flags & static_cast<u16>(flag)) != 0;
}

/// One action's state for one user. Written by the tick resolver, read by everything else.
///
/// `press_count` and `release_count` are the two fields a "sample the current state" implementation
/// cannot produce and the reason this record exists in this shape. See design.md §5.
struct ActionState {
    ActionValue value;
    ActionValue previous_value;
    TriggerPhase phase = TriggerPhase::Idle;
    u16 flags = 0;
    /// Transitions observed **during the resolved tick**, not since boot. A press and a release in
    /// the same tick is `1` and `1`; a key held across ten ticks is `0` and `0` on nine of them.
    u16 press_count = 0;
    u16 release_count = 0;
    /// The binding that produced the value, for `input-and-actions`' "which binding produced it".
    /// `0xFFFF` when nothing did.
    u16 binding = 0xFFFFU;
    /// The context stack slot the winning binding came from. `0xFFFF` when nothing did.
    u16 context_slot = 0xFFFFU;
    /// When the value last changed actuation, on the platform clock.
    Nanoseconds last_transition = 0;
    /// When the current actuation began; `held_seconds()` is derived from it rather than
    /// accumulated, so a paused tick cannot inflate it.
    Nanoseconds actuated_since = 0;
    /// The buffered intent's expiry, and whether it has been taken. See `ActionDeclaration`.
    Nanoseconds buffered_until = 0;
    bool buffer_consumed = true;

    [[nodiscard]] constexpr bool pressed() const noexcept {
        return has_flag(flags, ActionFlag::Pressed);
    }
    [[nodiscard]] constexpr bool just_pressed() const noexcept {
        return has_flag(flags, ActionFlag::JustPressed);
    }
    [[nodiscard]] constexpr bool just_released() const noexcept {
        return has_flag(flags, ActionFlag::JustReleased);
    }
    [[nodiscard]] constexpr bool triggered() const noexcept {
        return has_flag(flags, ActionFlag::Triggered);
    }
    [[nodiscard]] f32 held_seconds(Nanoseconds now) const noexcept {
        if (!pressed() || actuated_since == 0) {
            return 0.0F;
        }
        return static_cast<f32>(now - actuated_since) * 1e-9F;
    }
};

static_assert(sizeof(ActionState) <= 128, "the per-user, per-action record stays compact");

/// Every action the project declares, and the two lookups over them.
///
/// Registration happens at load; `find()` by name is a cook-time and diagnostic path. The
/// evaluation path uses `ActionId` and touches neither.
class ActionRegistry {
public:
    explicit ActionRegistry(Allocator& allocator) noexcept : declarations_(allocator) {}

    /// Declare an action. Refuses a duplicate stable id — two actions with one identity would make
    /// a profile's override ambiguous — and refuses a null one.
    [[nodiscard]] Expected<ActionId, Error> declare(const ActionDeclaration& declaration) noexcept;

    [[nodiscard]] ActionId find(ActionStableId stable_id) const noexcept;
    /// By name. Cook time, editor and diagnostics only; never the evaluation path.
    [[nodiscard]] ActionId find(Name name) const noexcept;

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(declarations_.size()); }
    [[nodiscard]] const ActionDeclaration& at(ActionId action) const noexcept {
        return declarations_[action];
    }

private:
    Array<ActionDeclaration> declarations_;
};

}  // namespace cy::input
