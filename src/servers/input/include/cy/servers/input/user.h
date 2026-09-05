#pragma once
// The input user: devices, a context stack, a profile, a scheme, a focus state, and the action
// records the tick resolver writes. Tasks 4.1.1 to 4.1.6.
//
// ================================================================================================
// WHY THE USER IS THE UNIT AND NOT THE DEVICE
// ================================================================================================
//
// `input-and-actions`: "Input SHALL be organised around **input users**, each owning: a set of
// assigned devices, a context stack, a rebinding profile, a preferred device, and a focus state."
// And: "An input user SHALL NOT be identified by a device index; a user without a device is a user
// awaiting one, not a lost player."
//
// Every one of those clauses is a bug that has shipped in real games. Identify the player by the
// controller index and a flat battery renumbers everybody. Keep the context stack globally and two
// players cannot be in different menus. Keep the rebinding profile globally and the second player
// inherits the first's keys. The structure here makes each of them unrepresentable rather than
// discouraged: there is no global context stack to reach for.
//
// ================================================================================================
// THE RESOLVED TABLE, AND WHY IT IS REBUILT RATHER THAN WALKED
// ================================================================================================
//
// A user's context stack changes when a menu opens. Its profile changes when the player rebinds.
// Neither happens per frame. So the *evaluation* path does not walk the stack, does not consult the
// profile and does not test priorities: it reads a flat `ResolvedBinding` table, sorted by priority
// and indexed by control, that `rebuild()` produces whenever the stack or the profile changes.
//
// This is what makes `input-and-actions`' performance requirement achievable — "allocate nothing
// per frame, take no global lock, and process only controls that changed or bindings that are
// active" — and it is also what keeps the authored contexts immutable: an override is applied into
// the *copy* in the table, never into the context it came from.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/input/action.h>
#include <cy/servers/input/binding.h>
#include <cy/servers/input/device.h>
#include <cy/servers/input/diagnostics.h>
#include <cy/servers/input/frame.h>
#include <cy/servers/input/profile.h>
#include <cy/servers/input/scheme.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// The declared focus layers, outermost first.
///
/// `input-and-actions` — "Interface routing and focus": focus is "routed through declared layers —
/// operating system focus, editor focus, interface focus, gameplay focus — with a declared policy
/// at each boundary". A context declares the layer it belongs to; a context whose layer is below
/// the user's current focus is suppressed unless it declares pass-through.
enum class FocusLayer : u8 {
    OperatingSystem = 0,
    Editor,
    Interface,
    Gameplay,
    Count,
};

const char* focus_layer_name(FocusLayer layer) noexcept;

/// `input-and-actions` — "Accessibility": these are "first-class capabilities of the input model,
/// not per-game implementations", and they "apply as processors and modifiers within the standard
/// pipeline, so that they work for every action without gameplay changes".
///
/// The consequence worth naming: because they are applied inside the pipeline, a gameplay system
/// reading the action gets the transformed value and there is no path to the untransformed one.
/// That is the requirement "Gameplay SHALL NOT be able to bypass accessibility transformations by
/// reading device state directly" being kept by construction rather than by review — and it is a
/// second, independent reason raw device access is not the gameplay path.
struct AccessibilitySettings {
    /// Every `Hold` trigger becomes a toggle: one press starts it, the next ends it. Applied to
    /// every hold action with no per-action implementation, which is the scenario.
    bool hold_to_toggle = false;
    /// A `Pulse` trigger fires once per press instead of repeating, so a player who cannot press
    /// repeatedly is not excluded.
    bool repeated_press_assistance = false;
    /// A modifier control latches until the next non-modifier actuation.
    bool sticky_modifiers = false;
    /// Read the right stick's controls from the left stick, for one-handed play.
    bool single_stick = false;
    /// Multiplies every dead-zone processor's threshold. Below 1 opens the dead zone up for a
    /// player whose hands shake; above 1 closes it for a worn stick.
    f32 dead_zone_scale = 1.0F;
    /// An upper bound on any `Sensitivity` processor, so a setting cannot exceed what a player can
    /// control.
    f32 sensitivity_limit = 100.0F;
    /// Multiplies every value the pipeline produces. Distinct from sensitivity: it applies to
    /// digital-derived composites too.
    f32 input_scale = 1.0F;
};

/// A binding as the evaluator sees it: the authored binding with the player's override applied, and
/// where it came from.
struct ResolvedBinding {
    Binding binding;
    ContextHandle context;
    /// Position in the stack, 0 being the highest priority. What the inspector reports.
    u16 context_slot = 0;
    FocusLayer layer = FocusLayer::Gameplay;
    bool pass_through = false;
    /// The nearest **higher-priority** binding of the same action that consumes it, or `0xFFFF`.
    ///
    /// Computed once at `rebuild()` rather than searched per event, and stored as a chain rather
    /// than a bool because whether a shadow actually applies depends on the *current* focus: a
    /// modal that consumes `Confirm` shadows the gameplay binding only while the modal's focus
    /// layer is the user's. Walking the chain skips the consumers that are currently suppressed,
    /// which a bool computed at rebuild could not do.
    u16 shadow_by = 0xFFFFU;
};

/// The mutable half of one binding: the processor chain's state and the trigger's state machine.
///
/// Kept beside the table rather than inside `Binding` so that `Binding` stays a value a cook can
/// write and a `memcpy` can copy — see binding.h.
struct BindingRuntime {
    ProcessorState processors;
    TriggerPhase phase = TriggerPhase::Idle;
    Vec3 value;
    /// The magnitude last seen, so that a rising edge is a comparison rather than a search.
    f32 actuation = 0.0F;
    Nanoseconds started_at = 0;
    Nanoseconds last_trigger = 0;
    Nanoseconds last_release = 0;
    /// `Tap`/`DoubleTap` bookkeeping.
    u8 tap_count = 0;
    /// `Sequence` bookkeeping: how many components have been actuated in order so far.
    u8 sequence_index = 0;
    Nanoseconds sequence_started = 0;
    /// `hold_to_toggle`'s latch.
    bool toggled = false;
};

/// One local player's whole input state.
///
/// Not thread-safe. Resolution happens at the runtime's quiesced tick boundary, which is the same
/// place `runtime::Simulation` commits; two threads resolving one user would be two threads writing
/// one action record.
class InputUser {
public:
    static constexpr u32 kMaxContexts = 16;
    static constexpr u32 kMaxDevices = 8;

    InputUser(Allocator& allocator, u32 id) noexcept;

    InputUser(const InputUser&) = delete;
    InputUser& operator=(const InputUser&) = delete;

    [[nodiscard]] u32 id() const noexcept { return id_; }

    /// Size the per-action storage. Called after every action is declared and before the first
    /// resolution; the only allocation on this object's hot path, and it happens once.
    [[nodiscard]] Status configure(const ActionRegistry& actions) noexcept;

    // --- Devices -------------------------------------------------------------------------------
    //
    // The registry owns assignment; the user caches the list so that the evaluation path does not
    // search the registry per event. `note_device_assigned` and `note_device_unassigned` are how
    // the registry tells it.

    void note_device_assigned(DeviceId device, DeviceKind kind) noexcept;
    void note_device_unassigned(DeviceId device) noexcept;
    [[nodiscard]] u32 device_count() const noexcept { return device_count_; }
    [[nodiscard]] DeviceId device(u32 index) const noexcept { return devices_[index]; }
    /// The device classes this user holds, as a mask. What `ControlScheme::satisfied_by` takes and
    /// what `DeviceUnassigned` is decided from.
    [[nodiscard]] u16 device_kinds() const noexcept { return device_kinds_; }

    /// The device the player used last, which is what an interface calls "the preferred device"
    /// when it has to pick one. Null until something has been pressed.
    [[nodiscard]] DeviceId preferred_device() const noexcept { return preferred_; }

    // --- Contexts ------------------------------------------------------------------------------

    /// Push a context. Returns its stack slot for a diagnostic; **removal is by handle**, never by
    /// this number.
    [[nodiscard]] Expected<u32, Error> push_context(ContextHandle context, i32 priority,
                                                    FocusLayer layer = FocusLayer::Gameplay,
                                                    bool pass_through = false) noexcept;
    /// Remove by handle, wherever it sits. Removing one that is not on the stack is a no-op and
    /// returns false — an interface unwinding twice is not a crash.
    bool pop_context(ContextHandle context) noexcept;
    [[nodiscard]] u32 context_count() const noexcept { return context_count_; }
    [[nodiscard]] const ContextStackEntry& context_at(u32 slot) const noexcept {
        return contexts_[slot];
    }

    // --- Profile, schemes, focus, settings -----------------------------------------------------

    [[nodiscard]] BindingProfile& profile() noexcept { return profile_; }
    [[nodiscard]] const BindingProfile& profile() const noexcept { return profile_; }
    /// Call after changing the profile; the resolved table is rebuilt on the next resolution.
    void invalidate() noexcept { dirty_ = true; }

    [[nodiscard]] SchemeDetector& scheme() noexcept { return scheme_; }
    [[nodiscard]] const SchemeDetector& scheme() const noexcept { return scheme_; }

    void set_focus(FocusLayer layer) noexcept;
    [[nodiscard]] FocusLayer focus() const noexcept { return focus_; }
    /// While true, gameplay actions bound to controls the text field uses are suppressed unless
    /// their context declares pass-through — `input-and-actions`' "Typing does not move the
    /// player".
    void set_text_entry_active(bool active) noexcept;
    [[nodiscard]] bool text_entry_active() const noexcept { return text_entry_; }

    void set_reference_frame(const ReferenceFrame& frame) noexcept;
    [[nodiscard]] const ReferenceFrame& reference_frame() const noexcept { return frame_; }

    /// A named player setting, for `ModifierKind::SettingScale`. Changing one does not rewrite a
    /// binding, which is the requirement's second scenario.
    [[nodiscard]] Status set_setting(Name key, f32 value) noexcept;
    [[nodiscard]] f32 setting(Name key, f32 fallback) const noexcept;

    /// A named state, for `ModifierKind::ActiveInState`.
    [[nodiscard]] Status set_state(Name key, bool active) noexcept;
    [[nodiscard]] bool state(Name key) const noexcept;

    /// Accessibility is set, not mutated in place.
    ///
    /// The reason is not style. The evaluation path deliberately touches only the bindings whose
    /// controls changed, so a setting altered through a reference would not be seen until the
    /// player happened to move the control again — a dead-zone slider that appears not to work
    /// until you wiggle the stick. Every setter that can change what an idle binding would produce
    /// marks the next resolution as a full pass, and there is no accessor that could bypass that.
    void set_accessibility(const AccessibilitySettings& settings) noexcept;
    [[nodiscard]] const AccessibilitySettings& accessibility() const noexcept {
        return accessibility_;
    }

    // --- Action state --------------------------------------------------------------------------

    [[nodiscard]] const ActionState& action_state(ActionId action) const noexcept {
        return states_[action];
    }
    [[nodiscard]] u32 action_count() const noexcept { return static_cast<u32>(states_.size()); }

    /// Take a buffered intent, once. Returns false when there is none or it has already been taken.
    ///
    /// `input-and-actions` — "buffered intent SHALL be consumable exactly once". Consumption is a
    /// mutation, which is why this is not on the const state record: two systems both "checking"
    /// a buffered jump would both get it if reading were free.
    [[nodiscard]] bool consume_buffered(ActionId action, Nanoseconds now) noexcept;

    [[nodiscard]] InputTrace& trace() noexcept { return trace_; }
    [[nodiscard]] const InputTrace& trace() const noexcept { return trace_; }

    /// The command frame produced by the last resolution.
    [[nodiscard]] const CommandFrame& command_frame() const noexcept { return frame_out_; }

    // --- Resolution ------------------------------------------------------------------------------
    //
    // Driven by `InputServer`, which owns the event buffer and the registry. Three phases, in this
    // order, and the order is the requirement: begin (clear the per-tick record), observe every
    // accumulated event **in timestamp order**, then finish (advance time-based triggers, compute
    // the command frame).

    void begin_tick(u64 tick) noexcept;
    /// Apply one event. `registry` supplies the raw control values a composite reads.
    void observe(const DeviceEvent& event, const DeviceRegistry& registry) noexcept;
    void finish_tick(Nanoseconds now, f32 delta_seconds, const DeviceRegistry& registry) noexcept;

    /// Rebuild the resolved table from the stack and the profile. Called by resolution when dirty;
    /// exposed because a test and the inspector both want to look at the table without a tick.
    ///
    /// `contexts` is indexed by `ContextHandle::index()` — the server's own storage, handed in
    /// rather than reached for, because layer 2 has no registry to look one up in and a user that
    /// held a pointer back to its server would be the ambient global this design avoids.
    [[nodiscard]] Status rebuild(const ActionRegistry& actions,
                                 const MappingContext* const* contexts,
                                 u32 context_capacity) noexcept;

    [[nodiscard]] u32 resolved_count() const noexcept { return static_cast<u32>(resolved_.size()); }
    [[nodiscard]] const ResolvedBinding& resolved(u32 index) const noexcept {
        return resolved_[index];
    }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    /// Which action the inspector should blame, and why. Valid after `finish_tick`.
    [[nodiscard]] ActionOutcome outcome(ActionId action) const noexcept;

private:
    struct NamedFloat {
        Name key;
        f32 value = 0.0F;
    };
    /// Which binding currently owns an action's value within the tick being resolved.
    ///
    /// The arbitration rule, stated once: **prefer an actuated binding; among equally actuated
    /// ones, prefer the higher priority.** Without the first half a high-priority binding that is
    /// idle would write zero over a lower-priority one the player is actually holding, which is
    /// the "augment" case of `input-and-actions`' context stack — a second device bound to the
    /// same action in a lower context has to be able to drive it.
    struct ClaimRecord {
        u16 binding = 0xFFFFU;
        bool actuated = false;
    };
    /// `(control key, binding index)`, sorted, so an event finds its bindings by binary search
    /// rather than by scanning the table. Rebuilt with the table.
    struct ControlIndexEntry {
        u32 control_key = 0;
        u16 binding = 0;
    };

    /// Evaluate one binding. `changed` is the control whose event provoked this, or an invalid
    /// control at the end-of-tick pass — `Sequence` is the only trigger that needs to know which,
    /// because "in order" is a statement about events and not about levels.
    void evaluate_binding(u32 index, Nanoseconds timestamp, const DeviceRegistry& registry,
                          EventSource source, Control changed) noexcept;
    /// The level of one control across this user's devices, which is what a composite reads and
    /// what a sequence checks the provoking event against.
    [[nodiscard]] f32 control_level(Control control, const DeviceRegistry& registry) const noexcept;
    /// The `Sequence` trigger's progress, which is the one thing that depends on *which* control an
    /// event touched rather than on a level. Returns the actuation to feed the trigger.
    [[nodiscard]] f32 advance_sequence(u32 index, Control changed, Nanoseconds timestamp,
                                       const DeviceRegistry& registry) noexcept;
    /// Why an action produced nothing, for a binding the evaluation path skipped. Answering it for
    /// the skipped bindings too is what makes `input-and-actions`' "why did nothing happen" have an
    /// answer on a tick where nothing happened — which is every tick a developer opens the
    /// inspector on.
    void classify_idle(u32 index) noexcept;
    // `rebuild()`'s five passes, one function each. They are genuinely independent of one another
    // and each is readable on its own, which the 56-point function they were extracted from was
    // not.
    void order_contexts(u32* order) const noexcept;
    [[nodiscard]] Status collect_bindings(const MappingContext* const* contexts,
                                          u32 context_capacity, const u32* order) noexcept;
    void apply_overrides(const ActionRegistry& actions) noexcept;
    void compute_shadow_chain() noexcept;
    [[nodiscard]] Status build_control_index() noexcept;
    /// The trigger state machine for one binding. Returns the flags to fold into the action.
    u16 advance_trigger(u32 index, f32 actuation, Nanoseconds timestamp) noexcept;
    void publish(u32 index, u16 flags, Nanoseconds timestamp, EventSource source) noexcept;
    [[nodiscard]] bool binding_active(const ResolvedBinding& entry) const noexcept;
    [[nodiscard]] Vec3 gather(const Binding& binding,
                              const DeviceRegistry& registry) const noexcept;
    [[nodiscard]] Vec3 apply_modifier(const Modifier& modifier, Vec3 value) const noexcept;
    void build_command_frame() noexcept;
    /// True when a higher-priority active binding of the same action consumes it, so this one must
    /// not act at all. Walks `ResolvedBinding::shadow_by`.
    [[nodiscard]] bool shadowed(u32 index, u16& by) const noexcept;
    /// The magnitude a trigger compares against its actuation threshold.
    [[nodiscard]] static f32 actuation_of(Vec3 value) noexcept;

    u32 id_;
    Array<ActionState> states_;
    /// The winning binding per action for the tick being resolved. See `ClaimRecord`.
    Array<ClaimRecord> claim_;
    Array<ResolvedBinding> resolved_;
    Array<BindingRuntime> runtime_;
    Array<ControlIndexEntry> control_index_;
    /// The bindings whose trigger is `Sequence`. They are evaluated for **every** event rather than
    /// only for their own controls, because "A then B" has to reject "A, Q, B" and the Q is not one
    /// of the binding's controls. The cost is paid by sequence bindings alone, which is why they
    /// are a separate list rather than a flag the indexed path would have to test.
    Array<u16> sequence_bindings_;
    Array<ActionOutcome> outcomes_;
    /// Which frame slot each action occupies, or 0xFF. Built by `configure`.
    Array<u8> frame_slot_;
    /// Each action's buffer window in nanoseconds, copied out of the declaration by `configure` so
    /// that the publish path does not hold a pointer back into the registry.
    Array<Nanoseconds> buffer_window_ns_;
    Array<NamedFloat> settings_;
    Array<NamedFloat> states_named_;

    ContextStackEntry contexts_[kMaxContexts];
    /// The focus layer and the pass-through flag are per **activation**, not per context asset: one
    /// set of bindings pushed by a menu and by gameplay is two activations with different focus, so
    /// they live beside the stack rather than inside `MappingContext`.
    FocusLayer layers_[kMaxContexts] = {};
    bool pass_through_[kMaxContexts] = {};
    u32 context_count_ = 0;
    u32 context_sequence_ = 0;

    DeviceId devices_[kMaxDevices];
    DeviceKind device_kind_[kMaxDevices] = {};
    u32 device_count_ = 0;
    u16 device_kinds_ = 0;
    DeviceId preferred_;

    BindingProfile profile_;
    SchemeDetector scheme_;
    InputTrace trace_;
    AccessibilitySettings accessibility_;
    ReferenceFrame frame_;
    FocusLayer focus_ = FocusLayer::Gameplay;
    bool text_entry_ = false;
    bool dirty_ = true;
    /// Set by every change that can alter what an *idle* binding produces — a setting, a state, the
    /// reference frame, focus, accessibility. The next `finish_tick` then re-evaluates every
    /// binding once instead of only the ones whose controls moved. See `set_accessibility`.
    bool force_full_pass_ = true;
    u64 tick_ = 0;
    /// The last fixed step, for `ProcessorKind::Smooth`. It never scales a value — that is
    /// `Interpretation`'s job and `apply_time_step()`'s — and the name says so.
    f32 last_delta_ = 0.0F;
    CommandFrame frame_out_;
    u8 frame_axis_count_ = 0;
    /// The sticky-modifier latch: the control currently held for the player.
    Control sticky_;
};

}  // namespace cy::input
