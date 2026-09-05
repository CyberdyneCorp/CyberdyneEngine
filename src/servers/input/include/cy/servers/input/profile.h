#pragma once
// Rebinding profiles: player overrides applied over immutable authored bindings. Task 4.1.5.
//
// `input-and-actions` — "Rebinding and profiles": runtime rebinding goes through an explicit flow —
// begin a rebind for an action, listen for an eligible control, apply the conflict policy, store
// the result. "**Authored binding assets SHALL be immutable at runtime.** Player changes SHALL be
// stored as overrides in a profile, applied over the defaults, so that content updates do not
// conflict with customisation." Overrides are per device scheme.
//
// --- WHY AN OVERRIDE IS A THREE-PART KEY ---------------------------------------------------------
//
// `{action stable id, scheme, slot}`. Not "binding index": an index into a shipped context is
// exactly the thing a content update invalidates, and the scenario the requirement names — "a game
// ships new default bindings, player overrides SHALL still apply over them" — is the case where the
// index moved. The stable id survives a rename and a reorder; the scheme keeps the player's gamepad
// and keyboard choices apart; the slot distinguishes the primary binding of an action from its
// alternate.
//
// --- WHY THE FLOW IS AN OBJECT
// --------------------------------------------------------------------
//
// Rebinding is a *state machine with a cancel*, not a function call: it waits for a control, it
// must reject controls that belong to another scheme or that the interface uses to cancel, and it
// has to report the conflict rather than resolve it silently. A `RebindOperation` makes each of
// those a step a caller can see, and makes the "cancel" case a method rather than a special value.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/input/action.h>
#include <cy/servers/input/binding.h>
#include <cy/servers/input/event.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// What happens when a rebind duplicates an existing binding. Declared per project and per context;
/// never decided by the rebinding code.
enum class ConflictPolicy : u8 {
    /// Refuse the new binding and report the conflict.
    Reject = 0,
    /// Give the new control to this action and the old control to the action that had it.
    Swap,
    /// Two actions on one control. Legitimate — a "use" and a "reload" on one key in different
    /// contexts — and therefore offered rather than prevented.
    Allow,
    /// Take the control away from whoever had it and leave them unbound.
    UnbindPrevious,
    /// Report the conflict and let the interface ask. The operation stays open.
    Ask,
};

const char* conflict_policy_name(ConflictPolicy policy) noexcept;

/// One player change. `control` invalid means "unbound", which is a legitimate override and the
/// thing `UnbindPrevious` produces.
struct BindingOverride {
    ActionStableId action;
    SchemeKind scheme = SchemeKind::KeyboardMouse;
    /// Which of the action's bindings within that scheme. Zero is the primary.
    u8 slot = 0;
    Control control;
};

/// A player's stored changes. Serialised with a save; never merged into a context.
class BindingProfile {
public:
    explicit BindingProfile(Allocator& allocator) noexcept : overrides_(allocator) {}

    /// Store an override, replacing one with the same key.
    [[nodiscard]] Status set(const BindingOverride& override_entry) noexcept;
    void clear(ActionStableId action, SchemeKind scheme, u8 slot) noexcept;
    void clear_all() noexcept { overrides_.clear(); }

    /// The control the player chose, or null when they made no choice for this key.
    [[nodiscard]] const BindingOverride* find(ActionStableId action, SchemeKind scheme,
                                              u8 slot) const noexcept;

    /// Which action, if any, currently holds `control` in this scheme. The conflict check.
    [[nodiscard]] const BindingOverride* holder_of(Control control,
                                                   SchemeKind scheme) const noexcept;

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(overrides_.size()); }
    [[nodiscard]] const BindingOverride& at(u32 index) const noexcept { return overrides_[index]; }

private:
    Array<BindingOverride> overrides_;
};

/// Why a rebind did not complete.
enum class RebindStatus : u8 {
    /// Waiting for an eligible control.
    Listening = 0,
    Applied,
    /// A control was offered and it duplicates an existing binding; `conflict_action` names the
    /// holder. Under `Ask` the operation is still open.
    Conflict,
    Cancelled,
};

const char* rebind_status_name(RebindStatus status) noexcept;

/// The explicit rebinding flow.
///
/// Not thread-safe, not reentrant, and deliberately owned by the caller rather than by the server:
/// two simultaneous rebinds are two objects, and an interface that started one and forgot it drops
/// the object rather than leaving the server in listening mode.
class RebindOperation {
public:
    RebindOperation(ActionStableId action, SchemeKind scheme, u8 slot,
                    ConflictPolicy policy) noexcept
        : action_(action), scheme_(scheme), slot_(slot), policy_(policy) {}

    /// Offer one device event. Returns the status after considering it.
    ///
    /// Controls from another scheme are ignored rather than refused — a player pressing a gamepad
    /// button while rebinding a key has not made a mistake worth an error, they have pressed the
    /// wrong thing — and the operation stays open. `cancel_control` is what closes it.
    RebindStatus offer(const DeviceEvent& event, const BindingProfile& profile) noexcept;

    /// Write the result into `profile`, resolving the conflict under the policy.
    ///
    /// Fails when the status is not `Applied` or `Conflict`, so a caller that skipped the flow gets
    /// an error rather than an override from nowhere.
    [[nodiscard]] Status apply(BindingProfile& profile) noexcept;

    void cancel() noexcept { status_ = RebindStatus::Cancelled; }
    void set_cancel_control(Control control) noexcept { cancel_control_ = control; }

    [[nodiscard]] RebindStatus status() const noexcept { return status_; }
    [[nodiscard]] Control candidate() const noexcept { return candidate_; }
    /// Valid only when the status is `Conflict`.
    [[nodiscard]] ActionStableId conflict_action() const noexcept { return conflict_action_; }

private:
    ActionStableId action_;
    SchemeKind scheme_;
    u8 slot_;
    ConflictPolicy policy_;
    RebindStatus status_ = RebindStatus::Listening;
    Control candidate_;
    Control cancel_control_;
    ActionStableId conflict_action_;
    u8 conflict_slot_ = 0;
};

}  // namespace cy::input
