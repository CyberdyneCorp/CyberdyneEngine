// The input user: the resolved table, the trigger state machines, and the per-tick resolution that
// design.md §5 is about. Tasks 4.1.1 to 4.1.6.
//
// ================================================================================================
// THE RESOLUTION, IN ONE PARAGRAPH, BECAUSE IT IS THE MILESTONE'S SUBTLE REQUIREMENT
// ================================================================================================
//
// `begin_tick()` clears the per-tick half of every action record — the flags and the two transition
// counts — and keeps the level half. `observe()` is then called once per accumulated event, **in
// `(timestamp, sequence)` order**, and each call re-evaluates only the bindings that read the
// control the event touched. Every rising edge increments `press_count` and every falling edge
// increments `release_count`, so a press and a release inside one window leave `1` and `1` behind
// even though the level ends where it started. `finish_tick()` then advances the triggers that
// depend on elapsed time rather than on an event — a hold reaching its duration, a pulse repeating
// — and builds the command frame.
//
// The implementation this replaces is one line: read the device's current state per tick. It is
// simpler, it passes every manual test, and it reports neither the press nor the release above.

#include <cy/servers/input/user.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace cy::input {
namespace {

constexpr u16 kNoBinding = 0xFFFFU;
constexpr u8 kNoFrameSlot = 0xFFU;
constexpr u8 kFrameButtonFlag = 0x80U;

/// The keyboard controls a sticky-modifier setting latches.
[[nodiscard]] bool is_modifier_key(Control control) noexcept {
    if (control.kind != DeviceKind::Keyboard) {
        return false;
    }
    switch (static_cast<Key>(control.code)) {
        case Key::LeftShift:
        case Key::RightShift:
        case Key::LeftControl:
        case Key::RightControl:
        case Key::LeftAlt:
        case Key::RightAlt:
            return true;
        default:
            return false;
    }
}

/// `single_stick`: the right stick's controls are read from the left stick, so a player using one
/// hand can still aim. Applied at gather, inside the pipeline, which is what makes it
/// unbypassable — see `AccessibilitySettings`.
[[nodiscard]] Control fold_single_stick(Control control) noexcept {
    if (control.kind != DeviceKind::Gamepad) {
        return control;
    }
    if (control.code == static_cast<u16>(GamepadControl::RightStickX)) {
        return gamepad_control(GamepadControl::LeftStickX);
    }
    if (control.code == static_cast<u16>(GamepadControl::RightStickY)) {
        return gamepad_control(GamepadControl::LeftStickY);
    }
    return control;
}

[[nodiscard]] bool trigger_needs_time(TriggerKind kind) noexcept {
    return kind == TriggerKind::Hold || kind == TriggerKind::Tap ||
           kind == TriggerKind::DoubleTap || kind == TriggerKind::Pulse ||
           kind == TriggerKind::Sequence;
}

}  // namespace

const char* focus_layer_name(FocusLayer layer) noexcept {
    switch (layer) {
        case FocusLayer::OperatingSystem:
            return "OperatingSystem";
        case FocusLayer::Editor:
            return "Editor";
        case FocusLayer::Interface:
            return "Interface";
        case FocusLayer::Gameplay:
            return "Gameplay";
        case FocusLayer::Count:
            break;
    }
    return "Gameplay";
}

InputUser::InputUser(Allocator& allocator, u32 id) noexcept
    : id_(id),
      states_(allocator),
      claim_(allocator),
      resolved_(allocator),
      runtime_(allocator),
      control_index_(allocator),
      sequence_bindings_(allocator),
      outcomes_(allocator),
      frame_slot_(allocator),
      buffer_window_ns_(allocator),
      settings_(allocator),
      states_named_(allocator),
      profile_(allocator),
      trace_(allocator) {}

f32 InputUser::actuation_of(Vec3 value) noexcept {
    return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
}

Status InputUser::configure(const ActionRegistry& actions) noexcept {
    const u32 count = actions.count();
    if (Status sized = states_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = claim_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = outcomes_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = frame_slot_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = buffer_window_ns_.resize(count); !sized) {
        return sized;
    }

    // Command-frame slots, assigned in declaration order. A digital action takes a bit in the three
    // masks; everything else takes an axis. Both are capped, and an action that does not fit is
    // *dropped from the frame* rather than silently reusing a slot: two actions on one bit is a
    // replay that plays back the wrong intent, which is worse than an action that is not predicted.
    u8 buttons = 0;
    u8 axes = 0;
    for (u32 action = 0; action < count; ++action) {
        frame_slot_[action] = kNoFrameSlot;
        const ActionDeclaration& declaration = actions.at(action);
        buffer_window_ns_[action] =
            static_cast<Nanoseconds>(declaration.buffer_window_seconds * 1e9F);
        if (!declaration.in_command_frame) {
            continue;
        }
        if (declaration.type == ActionValueType::Digital) {
            if (buttons < kMaxFrameButtons) {
                frame_slot_[action] = static_cast<u8>(kFrameButtonFlag | buttons);
                ++buttons;
            }
        } else if (axes < kMaxFrameAxes) {
            frame_slot_[action] = axes;
            ++axes;
        }
    }
    frame_axis_count_ = axes;
    frame_out_ = CommandFrame{};
    frame_out_.user = id_;
    frame_out_.axis_count = axes;
    return ok();
}

// --- Devices -------------------------------------------------------------------------------------

void InputUser::note_device_assigned(DeviceId device, DeviceKind kind) noexcept {
    for (u32 index = 0; index < device_count_; ++index) {
        if (devices_[index] == device) {
            return;
        }
    }
    if (device_count_ == kMaxDevices) {
        return;
    }
    devices_[device_count_] = device;
    device_kind_[device_count_] = kind;
    ++device_count_;
    device_kinds_ = static_cast<u16>(device_kinds_ | (1U << static_cast<u16>(kind)));
}

void InputUser::note_device_unassigned(DeviceId device) noexcept {
    for (u32 index = 0; index < device_count_; ++index) {
        if (devices_[index] != device) {
            continue;
        }
        for (u32 shift = index + 1; shift < device_count_; ++shift) {
            devices_[shift - 1] = devices_[shift];
            device_kind_[shift - 1] = device_kind_[shift];
        }
        --device_count_;
        break;
    }
    // The mask is recomputed rather than cleared: two gamepads means losing one leaves the class
    // present, and a mask that said otherwise would make every gamepad binding report
    // `DeviceUnassigned` while a controller was still plugged in.
    device_kinds_ = 0;
    for (u32 index = 0; index < device_count_; ++index) {
        device_kinds_ =
            static_cast<u16>(device_kinds_ | (1U << static_cast<u16>(device_kind_[index])));
    }
    if (preferred_ == device) {
        preferred_ = DeviceId{};
    }
}

// --- Contexts ------------------------------------------------------------------------------------

Expected<u32, Error> InputUser::push_context(ContextHandle context, i32 priority, FocusLayer layer,
                                             bool pass_through) noexcept {
    if (context_count_ == kMaxContexts) {
        return fail(ErrorCode::OutOfRange, "input: the context stack is full");
    }
    for (u32 index = 0; index < context_count_; ++index) {
        if (contexts_[index].context == context) {
            return fail(ErrorCode::AlreadyExists,
                        "input: that context is already on this user's stack");
        }
    }
    ContextStackEntry entry;
    entry.context = context;
    entry.priority = priority;
    entry.sequence = context_sequence_++;
    entry.active = true;
    contexts_[context_count_] = entry;
    // The layer and the pass-through flag are per *activation*, not per context asset: the same
    // bindings pushed by a menu and by gameplay are two activations with different focus.
    layers_[context_count_] = layer;
    pass_through_[context_count_] = pass_through;
    const u32 slot = context_count_++;
    dirty_ = true;
    return slot;
}

bool InputUser::pop_context(ContextHandle context) noexcept {
    for (u32 index = 0; index < context_count_; ++index) {
        if (contexts_[index].context != context) {
            continue;
        }
        for (u32 shift = index + 1; shift < context_count_; ++shift) {
            contexts_[shift - 1] = contexts_[shift];
            layers_[shift - 1] = layers_[shift];
            pass_through_[shift - 1] = pass_through_[shift];
        }
        --context_count_;
        dirty_ = true;
        return true;
    }
    return false;
}

// --- Settings and states ---------------------------------------------------------------------

void InputUser::set_accessibility(const AccessibilitySettings& settings) noexcept {
    accessibility_ = settings;
    force_full_pass_ = true;
}

void InputUser::set_focus(FocusLayer layer) noexcept {
    focus_ = layer;
    force_full_pass_ = true;
}

void InputUser::set_text_entry_active(bool active) noexcept {
    text_entry_ = active;
    force_full_pass_ = true;
}

void InputUser::set_reference_frame(const ReferenceFrame& frame) noexcept {
    frame_ = frame;
    force_full_pass_ = true;
}

Status InputUser::set_setting(Name key, f32 value) noexcept {
    force_full_pass_ = true;
    for (auto& setting : settings_) {
        if (setting.key == key) {
            setting.value = value;
            return ok();
        }
    }
    return settings_.push_back(NamedFloat{key, value});
}

f32 InputUser::setting(Name key, f32 fallback) const noexcept {
    for (auto setting : settings_) {
        if (setting.key == key) {
            return setting.value;
        }
    }
    return fallback;
}

Status InputUser::set_state(Name key, bool active) noexcept {
    force_full_pass_ = true;
    for (auto& index : states_named_) {
        if (index.key == key) {
            index.value = active ? 1.0F : 0.0F;
            return ok();
        }
    }
    return states_named_.push_back(NamedFloat{key, active ? 1.0F : 0.0F});
}

bool InputUser::state(Name key) const noexcept {
    for (auto index : states_named_) {
        if (index.key == key) {
            return index.value != 0.0F;
        }
    }
    return false;
}

// --- The resolved table --------------------------------------------------------------------------

void InputUser::order_contexts(u32* order) const noexcept {
    // The stack in priority order: higher priority first, and within one priority the most recently
    // pushed first, so the arrangement is total rather than dependent on insertion order in an
    // array. `order` holds stack slots, not handles: the inspector reports the slot.
    CY_ASSERT_MSG(context_count_ <= kMaxContexts, "the context stack never exceeds kMaxContexts");
    for (u32 index = 0; index < context_count_ && index < kMaxContexts; ++index) {
        order[index] = index;
    }
    const auto before = [this](u32 a, u32 b) {
        if (contexts_[a].priority != contexts_[b].priority) {
            return contexts_[a].priority > contexts_[b].priority;
        }
        return contexts_[a].sequence > contexts_[b].sequence;
    };
    // An insertion sort, written out, over at most `kMaxContexts` (16) entries.
    //
    // Two reasons, and the second is the one worth recording. At this size an insertion sort *is*
    // the fastest thing — it is what libstdc++'s `std::sort` degenerates to below its threshold —
    // so nothing is lost. And the static analyser cannot prove that a two-key comparator over an
    // array of indices is a strict weak ordering, so it reports libstdc++'s unguarded insertion
    // loop as reading before the start of `order`. That is a false positive, but silencing it with
    // a suppression would silence a real out-of-bounds report in the same function forever.
    for (u32 index = 1; index < context_count_ && index < kMaxContexts; ++index) {
        const u32 value = order[index];
        u32 slot = index;
        while (slot > 0 && before(value, order[slot - 1])) {
            order[slot] = order[slot - 1];
            --slot;
        }
        order[slot] = value;
    }
}

Status InputUser::collect_bindings(const MappingContext* const* contexts, u32 context_capacity,
                                   const u32* order) noexcept {
    for (u32 position = 0; position < context_count_; ++position) {
        const u32 slot = order[position];
        const ContextStackEntry& entry = contexts_[slot];
        if (!entry.active || entry.context.index() >= context_capacity) {
            continue;
        }
        const MappingContext* context = contexts[entry.context.index()];
        if (context == nullptr) {
            continue;
        }
        for (u32 index = 0; index < context->binding_count(); ++index) {
            ResolvedBinding resolved;
            resolved.binding = context->binding(index);
            resolved.context = entry.context;
            resolved.context_slot = static_cast<u16>(slot);
            resolved.layer = layers_[slot];
            resolved.pass_through = pass_through_[slot];
            if (Status pushed = resolved_.push_back(resolved); !pushed) {
                return pushed;
            }
            if (Status pushed = runtime_.push_back(BindingRuntime{}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

void InputUser::apply_overrides(const ActionRegistry& actions) noexcept {
    // The player's overrides, applied into the copy. The authored context is never touched — see
    // binding.h — which is what makes "a game ships new default bindings and player overrides still
    // apply over them" true rather than aspirational.
    //
    // `slot` addresses one *component* of one binding: `ordinal * kMaxComponents + component`,
    // where `ordinal` counts the action's bindings within that scheme in table order. Addressing a
    // component rather than a binding is what makes `input-and-actions`' accessibility requirement
    // — "full remapping including composite elements" — expressible: a player may rebind the 'A' of
    // WASD without rebinding the other three.
    u16 ordinal[static_cast<usize>(SchemeKind::Count)] = {};
    for (auto& entry : resolved_) {
        Binding& binding = entry.binding;
        if (binding.action >= actions.count()) {
            continue;
        }
        const ActionStableId stable = actions.at(binding.action).stable_id;
        for (u8 component = 0; component < binding.component_count; ++component) {
            const SchemeKind scheme = scheme_of(binding.components[component].control.kind);
            const auto scheme_index = static_cast<usize>(scheme);
            const auto slot = static_cast<u8>((ordinal[scheme_index] * kMaxComponents) + component);
            if (const BindingOverride* override_entry = profile_.find(stable, scheme, slot);
                override_entry != nullptr) {
                binding.components[component].control = override_entry->control;
            }
        }
        ++ordinal[static_cast<usize>(scheme_of(binding.components[0].control.kind))];
    }
}

void InputUser::compute_shadow_chain() noexcept {
    // For each binding, the nearest higher-priority binding of the same action that consumes it.
    // Computed here, once, rather than searched per event.
    for (usize index = 0; index < resolved_.size(); ++index) {
        resolved_[index].shadow_by = kNoBinding;
        for (usize above = index; above-- > 0;) {
            if (resolved_[above].binding.action == resolved_[index].binding.action &&
                resolved_[above].binding.consume) {
                resolved_[index].shadow_by = static_cast<u16>(above);
                break;
            }
        }
    }
}

Status InputUser::build_control_index() noexcept {
    // `(control key, binding)`, sorted, so an event finds its bindings by binary search. This is
    // what "process only controls that changed" means in practice.
    for (usize index = 0; index < resolved_.size(); ++index) {
        const Binding& binding = resolved_[index].binding;
        const bool is_sequence =
            binding.kind == BindingKind::Sequence || binding.trigger.kind == TriggerKind::Sequence;
        if (is_sequence) {
            // A sequence is evaluated for *every* event rather than only for its own controls, so
            // it goes on its own list. See `observe`.
            if (Status pushed = sequence_bindings_.push_back(static_cast<u16>(index)); !pushed) {
                return pushed;
            }
        }
        for (u8 component = 0; component < binding.component_count; ++component) {
            const Control control = binding.components[component].control;
            if (!control.is_valid()) {
                continue;
            }
            if (Status pushed = control_index_.push_back(
                    ControlIndexEntry{control.key(), static_cast<u16>(index)});
                !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(control_index_, [](const ControlIndexEntry& a, const ControlIndexEntry& b) {
        return a.control_key != b.control_key ? a.control_key < b.control_key
                                              : a.binding < b.binding;
    });
    return ok();
}

Status InputUser::rebuild(const ActionRegistry& actions, const MappingContext* const* contexts,
                          u32 context_capacity) noexcept {
    // FIVE PASSES, IN THIS ORDER, EACH ONE A FUNCTION. Written inline this was a 56-point function,
    // and the passes are genuinely independent — the reader who wants to know how an override is
    // addressed should not have to scroll past the shadow chain to find out.
    resolved_.clear();
    runtime_.clear();
    control_index_.clear();
    sequence_bindings_.clear();
    force_full_pass_ = true;

    // Zero-initialised rather than left indeterminate: `push_context` refuses past `kMaxContexts`,
    // but the static analyser cannot see that invariant across two functions and reports the read
    // as uninitialised. Sixteen stores on a path that runs when the stack changes is the right
    // price for not having to teach a reader which of the two is true.
    u32 order[kMaxContexts] = {};
    order_contexts(order);
    if (Status collected = collect_bindings(contexts, context_capacity, order); !collected) {
        return collected;
    }
    apply_overrides(actions);
    compute_shadow_chain();
    if (Status indexed = build_control_index(); !indexed) {
        return indexed;
    }
    dirty_ = false;
    return ok();
}

bool InputUser::binding_active(const ResolvedBinding& entry) const noexcept {
    // Focus routing. A context whose layer is not the user's current focus is suppressed unless it
    // declares pass-through — `input-and-actions`' "Typing does not move the player": a text field
    // takes `Interface` focus and every `Gameplay` context stops producing.
    if (entry.layer != focus_ && !entry.pass_through) {
        return false;
    }
    if (text_entry_ && entry.layer == FocusLayer::Gameplay && !entry.pass_through) {
        return false;
    }
    return true;
}

bool InputUser::shadowed(u32 index, u16& by) const noexcept {
    u16 candidate = resolved_[index].shadow_by;
    while (candidate != kNoBinding) {
        if (binding_active(resolved_[candidate])) {
            by = candidate;
            return true;
        }
        candidate = resolved_[candidate].shadow_by;
    }
    by = kNoBinding;
    return false;
}

// --- Gathering and modifiers ---------------------------------------------------------------------

Vec3 InputUser::gather(const Binding& binding, const DeviceRegistry& registry) const noexcept {
    Vec3 value;
    bool chord_complete = binding.kind == BindingKind::Chord;
    for (u8 component = 0; component < binding.component_count; ++component) {
        Control control = binding.components[component].control;
        if (!control.is_valid()) {
            continue;
        }
        if (accessibility_.single_stick) {
            control = fold_single_stick(control);
        }
        // The device the control belongs to: the first assigned device of that class that reports
        // a non-zero level. A user with two gamepads reads whichever is being moved, which is what
        // a single-player binding means; a game that wants them apart binds them to two users,
        // which is what users are for.
        f32 raw = control_level(control, registry);
        if (accessibility_.sticky_modifiers && is_modifier_key(control) && sticky_ == control) {
            raw = 1.0F;
        }
        if (binding.kind == BindingKind::Chord) {
            chord_complete =
                chord_complete && std::fabs(raw) >= binding.trigger.actuation_threshold;
            continue;
        }
        const Vec3 weight = binding.components[component].weight;
        value = Vec3{value.x + (raw * weight.x), value.y + (raw * weight.y),
                     value.z + (raw * weight.z)};
    }
    if (binding.kind == BindingKind::Chord) {
        return Vec3{chord_complete ? 1.0F : 0.0F, 0.0F, 0.0F};
    }
    if (binding.kind == BindingKind::Radial) {
        const f32 size = actuation_of(value);
        if (size > 1.0F) {
            value = Vec3{value.x / size, value.y / size, value.z / size};
        }
    }
    return value;
}

f32 InputUser::control_level(Control control, const DeviceRegistry& registry) const noexcept {
    for (u32 index = 0; index < device_count_; ++index) {
        if (device_kind_[index] != control.kind) {
            continue;
        }
        const f32 level = registry.control_value(devices_[index], control);
        if (level != 0.0F) {
            return level;
        }
    }
    return 0.0F;
}

f32 InputUser::advance_sequence(u32 index, Control changed, Nanoseconds timestamp,
                                const DeviceRegistry& registry) noexcept {
    BindingRuntime& runtime = runtime_[index];
    const Binding& binding = resolved_[index].binding;
    if (!changed.is_valid()) {
        return 0.0F;
    }
    const auto window = static_cast<Nanoseconds>(binding.trigger.duration_seconds * 1e9F);
    if (runtime.sequence_index != 0 && window > 0 &&
        timestamp - runtime.sequence_started > window) {
        runtime.sequence_index = 0;
    }
    if (std::fabs(control_level(changed, registry)) < binding.trigger.actuation_threshold) {
        return 0.0F;
    }
    if (changed == binding.components[runtime.sequence_index].control) {
        if (runtime.sequence_index == 0) {
            runtime.sequence_started = timestamp;
        }
        ++runtime.sequence_index;
        if (runtime.sequence_index >= binding.component_count) {
            runtime.sequence_index = 0;
            return 1.0F;
        }
        return 0.0F;
    }
    // A control outside the sequence, actuated, resets it. Otherwise "A then B" would accept
    // "A, Q, B", which is not a sequence.
    for (u8 component = 0; component < binding.component_count; ++component) {
        if (binding.components[component].control == changed) {
            return 0.0F;
        }
    }
    runtime.sequence_index = 0;
    return 0.0F;
}

Vec3 InputUser::apply_modifier(const Modifier& modifier, Vec3 value) const noexcept {
    switch (modifier.kind) {
        case ModifierKind::None:
        case ModifierKind::Count:
            return value;
        case ModifierKind::ReferenceFrame: {
            // X along `right`, Y along `forward`. Three vectors the caller supplied; no camera, no
            // renderer, no world — see binding.h's `ReferenceFrame`.
            const Vec3 right = frame_.right;
            const Vec3 forward = frame_.forward;
            return Vec3{(right.x * value.x) + (forward.x * value.y),
                        (right.y * value.x) + (forward.y * value.y),
                        (right.z * value.x) + (forward.z * value.y)};
        }
        case ModifierKind::SettingScale: {
            const f32 scale = setting(modifier.key, modifier.fallback);
            return Vec3{value.x * scale, value.y * scale, value.z * scale};
        }
        case ModifierKind::Negate:
            return Vec3{-value.x, -value.y, -value.z};
        case ModifierKind::ActiveInState:
            return state(modifier.key) ? value : Vec3{};
    }
    return value;
}

// --- The trigger state machines --------------------------------------------------------------

// --- The trigger state machines ------------------------------------------------------------------
//
// ONE FUNCTION PER TRIGGER KIND, and a `TriggerStep` carrying what they all read.
//
// Written as a single switch this was one 104-point function — far past even the band this project
// allows a systems state machine (`CLAUDE.md`: 25-35), and the reason is worth naming: the ten
// cases share nothing but their inputs, so a reader looking for what `Hold` does has to scroll past
// nine machines that are not it. Split, each one is three to eight points and fits on a screen, and
// the dispatcher below is a table.
//
// Every one of them returns the flags it *adds*. None of them touches the action record, the claim,
// or the shared level bits — `publish()` owns those, and keeping the split there is what lets a
// trigger be read in isolation.

namespace {

/// What every trigger step reads. `was` and `now` are the actuation on either side of this step,
/// already compared against the threshold, because every kind needs them and computing them nine
/// times is nine chances to compare against the wrong number.
struct TriggerStep {
    BindingRuntime* runtime = nullptr;
    const TriggerSpec* trigger = nullptr;
    Nanoseconds timestamp = 0;
    Nanoseconds duration = 0;
    f32 actuation = 0.0F;
    bool was = false;
    bool now = false;
    /// `AccessibilitySettings`, passed rather than reached for: a trigger step that could see the
    /// whole user could see the world, and these two flags are all it needs.
    bool hold_to_toggle = false;
    bool repeat_assist = false;
};

[[nodiscard]] constexpr u16 flag_of(ActionFlag flag) noexcept {
    return static_cast<u16>(flag);
}

/// Enter `phase` and record the trigger. Returns the `Triggered` flag, so a caller reads
/// `flags |= fire(step, phase)` and cannot fire without recording it.
[[nodiscard]] u16 fire(const TriggerStep& step, TriggerPhase phase) noexcept {
    step.runtime->phase = phase;
    step.runtime->last_trigger = step.timestamp;
    return flag_of(ActionFlag::Triggered);
}

/// `Down` and `Chord`: active for as long as the control is actuated.
[[nodiscard]] u16 step_down(const TriggerStep& step) noexcept {
    if (step.now) {
        return fire(step, TriggerPhase::Triggered);
    }
    step.runtime->phase = step.was ? TriggerPhase::Completed : TriggerPhase::Idle;
    return step.was ? flag_of(ActionFlag::Completed) : 0U;
}

[[nodiscard]] u16 step_pressed(const TriggerStep& step) noexcept {
    if (step.now && !step.was) {
        return static_cast<u16>(fire(step, TriggerPhase::Triggered) | flag_of(ActionFlag::Started));
    }
    step.runtime->phase = step.now ? TriggerPhase::Ongoing : TriggerPhase::Idle;
    return 0;
}

[[nodiscard]] u16 step_released(const TriggerStep& step) noexcept {
    if (!step.now && step.was) {
        return static_cast<u16>(fire(step, TriggerPhase::Triggered) |
                                flag_of(ActionFlag::Completed));
    }
    step.runtime->phase = step.now ? TriggerPhase::Ongoing : TriggerPhase::Idle;
    return 0;
}

/// `hold_to_toggle`: one press starts the hold, the next ends it. Applied to every hold action with
/// no per-action implementation, which is `input-and-actions`' accessibility scenario.
[[nodiscard]] u16 step_hold_toggle(const TriggerStep& step) noexcept {
    u16 flags = 0;
    if (step.now && !step.was) {
        step.runtime->toggled = !step.runtime->toggled;
        if (step.runtime->toggled) {
            step.runtime->started_at = step.timestamp;
            step.runtime->phase = TriggerPhase::Started;
            flags = flag_of(ActionFlag::Started);
        } else {
            step.runtime->phase = TriggerPhase::Completed;
            flags = flag_of(ActionFlag::Completed);
        }
    }
    if (step.runtime->toggled) {
        flags = static_cast<u16>(flags | fire(step, TriggerPhase::Triggered));
    }
    return flags;
}

/// THE SCENARIO THIS EXISTS FOR: "a player begins a hold and releases before the threshold — the
/// action SHALL report started then cancelled, and SHALL NOT report triggered." A hold that is
/// abandoned and one that completes are different events, and a game that cannot tell them apart
/// cannot draw the ring that fills.
[[nodiscard]] u16 step_hold(const TriggerStep& step) noexcept {
    if (step.hold_to_toggle) {
        return step_hold_toggle(step);
    }
    if (step.now && !step.was) {
        step.runtime->started_at = step.timestamp;
        step.runtime->phase = TriggerPhase::Started;
        return flag_of(ActionFlag::Started);
    }
    if (step.now) {
        if (step.runtime->phase == TriggerPhase::Triggered) {
            return 0;
        }
        if (step.timestamp - step.runtime->started_at >= step.duration) {
            return fire(step, TriggerPhase::Triggered);
        }
        step.runtime->phase = TriggerPhase::Ongoing;
        return 0;
    }
    if (!step.was) {
        return 0;
    }
    const bool completed = step.runtime->phase == TriggerPhase::Triggered;
    step.runtime->phase = completed ? TriggerPhase::Completed : TriggerPhase::Cancelled;
    return flag_of(completed ? ActionFlag::Completed : ActionFlag::Cancelled);
}

[[nodiscard]] u16 step_tap(const TriggerStep& step) noexcept {
    if (step.now && !step.was) {
        step.runtime->started_at = step.timestamp;
        step.runtime->phase = TriggerPhase::Started;
        return flag_of(ActionFlag::Started);
    }
    if (step.now || !step.was) {
        return 0;
    }
    if (step.timestamp - step.runtime->started_at <= step.duration) {
        return static_cast<u16>(fire(step, TriggerPhase::Triggered) |
                                flag_of(ActionFlag::Completed));
    }
    step.runtime->phase = TriggerPhase::Cancelled;
    return flag_of(ActionFlag::Cancelled);
}

[[nodiscard]] u16 step_double_tap(const TriggerStep& step) noexcept {
    if (step.now && !step.was) {
        step.runtime->started_at = step.timestamp;
        return flag_of(ActionFlag::Started);
    }
    if (step.now || !step.was) {
        return 0;
    }
    const bool quick = step.timestamp - step.runtime->started_at <= step.duration;
    const bool near_previous = step.runtime->last_release != 0 &&
                               step.timestamp - step.runtime->last_release <= step.duration;
    step.runtime->last_release = step.timestamp;
    if (!quick || !near_previous) {
        step.runtime->phase = TriggerPhase::Ongoing;
        return 0;
    }
    // The pair is consumed, so a third tap starts a new one rather than completing a second.
    step.runtime->last_release = 0;
    return static_cast<u16>(fire(step, TriggerPhase::Triggered) | flag_of(ActionFlag::Completed));
}

/// A threshold is a **crossing**, not a level: held past it, it does not fire again.
[[nodiscard]] u16 step_threshold(const TriggerStep& step) noexcept {
    const bool over = step.actuation >= step.trigger->threshold;
    if (over && step.runtime->actuation < step.trigger->threshold) {
        return fire(step, TriggerPhase::Triggered);
    }
    step.runtime->phase = over ? TriggerPhase::Ongoing : TriggerPhase::Idle;
    return 0;
}

/// The sequence's own progress is kept by `InputUser::advance_sequence()`, which is the only place
/// that knows which control the event touched. By the time it reaches here, `actuation` is 1
/// exactly when the sequence completed.
[[nodiscard]] u16 step_sequence(const TriggerStep& step) noexcept {
    if (step.now) {
        return static_cast<u16>(fire(step, TriggerPhase::Triggered) |
                                flag_of(ActionFlag::Completed));
    }
    step.runtime->phase =
        step.runtime->sequence_index != 0 ? TriggerPhase::Ongoing : TriggerPhase::Idle;
    return 0;
}

[[nodiscard]] u16 step_pulse(const TriggerStep& step) noexcept {
    if (step.now && !step.was) {
        step.runtime->started_at = step.timestamp;
        return static_cast<u16>(fire(step, TriggerPhase::Triggered) | flag_of(ActionFlag::Started));
    }
    if (step.now && step.repeat_assist) {
        // Repeated-press assistance: one press is enough, the repeat does not have to be performed.
        // The action stays triggered rather than pulsing.
        return fire(step, TriggerPhase::Triggered);
    }
    if (step.now) {
        if (step.duration > 0 && step.timestamp - step.runtime->last_trigger >= step.duration) {
            return fire(step, TriggerPhase::Triggered);
        }
        step.runtime->phase = TriggerPhase::Ongoing;
        return 0;
    }
    if (!step.was) {
        return 0;
    }
    step.runtime->phase = TriggerPhase::Completed;
    return flag_of(ActionFlag::Completed);
}

/// The dispatcher. A table rather than a machine — every case is one call.
[[nodiscard]] u16 step_trigger(const TriggerStep& step) noexcept {
    switch (step.trigger->kind) {
        case TriggerKind::Down:
        case TriggerKind::Chord:
            return step_down(step);
        case TriggerKind::Pressed:
            return step_pressed(step);
        case TriggerKind::Released:
            return step_released(step);
        case TriggerKind::Hold:
            return step_hold(step);
        case TriggerKind::Tap:
            return step_tap(step);
        case TriggerKind::DoubleTap:
            return step_double_tap(step);
        case TriggerKind::Threshold:
            return step_threshold(step);
        case TriggerKind::Sequence:
            return step_sequence(step);
        case TriggerKind::Pulse:
            return step_pulse(step);
        case TriggerKind::Count:
            break;
    }
    return 0;
}

}  // namespace

u16 InputUser::advance_trigger(u32 index, f32 actuation, Nanoseconds timestamp) noexcept {
    BindingRuntime& runtime = runtime_[index];
    const TriggerSpec& trigger = resolved_[index].binding.trigger;

    TriggerStep step;
    step.runtime = &runtime;
    step.trigger = &trigger;
    step.timestamp = timestamp;
    step.duration = static_cast<Nanoseconds>(trigger.duration_seconds * 1e9F);
    step.actuation = actuation;
    step.was = runtime.actuation >= trigger.actuation_threshold;
    step.now = actuation >= trigger.actuation_threshold;
    step.hold_to_toggle = accessibility_.hold_to_toggle;
    step.repeat_assist = accessibility_.repeated_press_assistance;

    // The level and the two edges, which every kind reports identically. Computed here so that no
    // trigger step can forget one, and so that "pressed" means the same thing for all ten.
    u16 flags = 0;
    if (step.now) {
        flags = static_cast<u16>(flags | flag_of(ActionFlag::Pressed));
    }
    if (step.now && !step.was) {
        flags = static_cast<u16>(flags | flag_of(ActionFlag::JustPressed));
    }
    if (!step.now && step.was) {
        flags = static_cast<u16>(flags | flag_of(ActionFlag::JustReleased));
    }

    flags = static_cast<u16>(flags | step_trigger(step));
    // Last, so that a step comparing against the *previous* actuation — `Threshold` does — sees it.
    runtime.actuation = actuation;
    return flags;
}

// --- Evaluation ------------------------------------------------------------------------------

void InputUser::evaluate_binding(u32 index, Nanoseconds timestamp, const DeviceRegistry& registry,
                                 EventSource source, Control changed) noexcept {
    ResolvedBinding& entry = resolved_[index];
    const Binding& binding = entry.binding;
    if (binding.action >= states_.size()) {
        return;
    }
    if (!binding_active(entry)) {
        if (outcomes_[binding.action] == ActionOutcome::NoBindingInActiveContext) {
            outcomes_[binding.action] = ActionOutcome::SuppressedByFocus;
        }
        return;
    }
    // A binding naming a device class this user holds none of is not an error — it is a binding
    // whose scheme is inactive — but it is the answer to "why did nothing happen".
    if (binding.components[0].control.is_valid() &&
        (device_kinds_ & (1U << static_cast<u16>(binding.components[0].control.kind))) == 0) {
        if (outcomes_[binding.action] == ActionOutcome::NoBindingInActiveContext) {
            outcomes_[binding.action] = ActionOutcome::DeviceUnassigned;
        }
        return;
    }
    u16 shadowing = kNoBinding;
    if (shadowed(index, shadowing)) {
        outcomes_[binding.action] = ActionOutcome::ConsumedByHigherContext;
        if (ActionTrace* trace = trace_.action(binding.action); trace != nullptr) {
            trace->outcome = ActionOutcome::ConsumedByHigherContext;
            trace->consumed_by = resolved_[shadowing].context;
        }
        return;
    }

    Vec3 raw = gather(binding, registry);

    // A sequence advances on the event that touched one of its components, in order. It is the one
    // trigger whose state is about *which* control moved rather than about the level, which is why
    // it is handled here and not inside `advance_trigger`.
    BindingRuntime& runtime = runtime_[index];
    if (binding.kind == BindingKind::Sequence || binding.trigger.kind == TriggerKind::Sequence) {
        raw = Vec3{advance_sequence(index, changed, timestamp, registry), 0.0F, 0.0F};
    }

    // The accessibility settings, folded into a *copy* of the chain. Inside the pipeline, which is
    // what makes them unbypassable — see `AccessibilitySettings`.
    Processor chain[kMaxProcessors];
    const u8 chain_length = binding.processor_count;
    for (u8 slot = 0; slot < chain_length && slot < kMaxProcessors; ++slot) {
        chain[slot] = binding.processors[slot];
        if (chain[slot].kind == ProcessorKind::DeadZone ||
            chain[slot].kind == ProcessorKind::RadialDeadZone) {
            chain[slot].a *= accessibility_.dead_zone_scale;
        } else if (chain[slot].kind == ProcessorKind::Sensitivity) {
            chain[slot].a = math::clamp(chain[slot].a, -accessibility_.sensitivity_limit,
                                        accessibility_.sensitivity_limit);
        }
    }

    ActionTrace* trace = trace_.action(binding.action);
    if (trace != nullptr) {
        trace->raw = raw;
        trace->binding = static_cast<u16>(index);
        trace->context = entry.context;
        trace->context_slot = entry.context_slot;
        trace->stage_count = chain_length;
        // Per-stage values, recorded one processor at a time so the inspector can say which stage
        // zeroed a value. Only while tracing is enabled — see diagnostics.h for the cost.
        Vec3 staged = raw;
        for (u8 slot = 0; slot < chain_length && slot < kMaxProcessors; ++slot) {
            ProcessorState scratch = runtime.processors;
            staged = evaluate_processors(&chain[slot], 1, staged, scratch, last_delta_);
            trace->stages[slot] = staged;
        }
    }

    Vec3 value = evaluate_processors(chain, chain_length, raw, runtime.processors, last_delta_);
    value = apply_modifier(binding.modifier, value);
    value = Vec3{value.x * accessibility_.input_scale, value.y * accessibility_.input_scale,
                 value.z * accessibility_.input_scale};
    runtime.value = value;

    const f32 actuation = actuation_of(value);
    const u16 flags = advance_trigger(index, actuation, timestamp);
    publish(index, flags, timestamp, source);

    if (trace != nullptr) {
        trace->final_value = value;
        trace->phase = runtime.phase;
        trace->outcome = outcomes_[binding.action];
    }
}

void InputUser::publish(u32 index, u16 flags, Nanoseconds timestamp, EventSource source) noexcept {
    const ResolvedBinding& entry = resolved_[index];
    const ActionId action = entry.binding.action;
    ActionState& state = states_[action];
    BindingRuntime& runtime = runtime_[index];
    const bool actuated = has_flag(flags, ActionFlag::Pressed);

    // Transition counts accumulate for every eligible binding: a press on *any* of the action's
    // active bindings is a press of the action. This is the half design.md §5 is about — see the
    // file header.
    if (has_flag(flags, ActionFlag::JustPressed)) {
        ++state.press_count;
        state.last_transition = timestamp;
        state.actuated_since = timestamp;
    }
    if (has_flag(flags, ActionFlag::JustReleased)) {
        ++state.release_count;
        state.last_transition = timestamp;
    }

    // The arbitration, and the one clause that is easy to leave out: `index == claim.binding` — the
    // *same* binding re-evaluated later in the tick always supersedes its own earlier value.
    // Without it, a WASD binding that saw 'W' and then 'D' would keep the value it had after 'W',
    // and two opposing keys pressed in one window would never cancel.
    ClaimRecord& claim = claim_[action];
    const bool better = claim.binding == kNoBinding || index == claim.binding ||
                        (actuated && !claim.actuated) ||
                        (actuated == claim.actuated && index < claim.binding);
    if (better) {
        claim.binding = static_cast<u16>(index);
        claim.actuated = actuated;
        state.value = ActionValue{runtime.value, Quat{}};
        state.phase = runtime.phase;
        state.binding = static_cast<u16>(index);
        state.context_slot = entry.context_slot;
        // `Pressed` is a **level at the end of the tick**, not an edge, so it is assigned from the
        // winner rather than accumulated. Folding it in with the edges is the defect that makes a
        // press-and-release inside one window report the button as still down — which would have
        // quietly undone the whole of design.md §5 while every edge assertion still passed.
        state.flags =
            static_cast<u16>(actuated ? (state.flags | static_cast<u16>(ActionFlag::Pressed))
                                      : (state.flags & ~static_cast<u16>(ActionFlag::Pressed)));
    }
    // The edges accumulate; the level does not.
    state.flags = static_cast<u16>(state.flags | (flags & ~static_cast<u16>(ActionFlag::Pressed)));
    if (source != EventSource::Physical) {
        state.flags = static_cast<u16>(state.flags | static_cast<u16>(ActionFlag::Synthetic));
    }

    if (has_flag(flags, ActionFlag::Triggered)) {
        outcomes_[action] = ActionOutcome::Triggered;
        if (buffer_window_ns_[action] > 0) {
            state.buffered_until = timestamp + buffer_window_ns_[action];
            state.buffer_consumed = false;
        }
    } else if (outcomes_[action] != ActionOutcome::Triggered) {
        outcomes_[action] =
            actuated ? ActionOutcome::TriggerConditionsUnmet : ActionOutcome::BelowThreshold;
    }
}

// --- The tick --------------------------------------------------------------------------------

void InputUser::classify_idle(u32 index) noexcept {
    const ResolvedBinding& entry = resolved_[index];
    const ActionId action = entry.binding.action;
    if (action >= outcomes_.size() ||
        outcomes_[action] != ActionOutcome::NoBindingInActiveContext) {
        return;
    }
    u16 shadowing = kNoBinding;
    if (!binding_active(entry)) {
        outcomes_[action] = ActionOutcome::SuppressedByFocus;
    } else if (entry.binding.components[0].control.is_valid() &&
               (device_kinds_ &
                (1U << static_cast<u16>(entry.binding.components[0].control.kind))) == 0) {
        outcomes_[action] = ActionOutcome::DeviceUnassigned;
    } else if (shadowed(index, shadowing)) {
        outcomes_[action] = ActionOutcome::ConsumedByHigherContext;
    } else {
        outcomes_[action] = ActionOutcome::BelowThreshold;
    }
}

void InputUser::begin_tick(u64 tick) noexcept {
    tick_ = tick;
    trace_.begin_tick(tick);
    for (usize action = 0; action < states_.size(); ++action) {
        ActionState& state = states_[action];
        // The level half is kept; the per-tick half is cleared. `previous_value` is what makes an
        // edge query answerable without a second record.
        state.previous_value = state.value;
        state.flags = 0;
        state.press_count = 0;
        state.release_count = 0;
        claim_[action] = ClaimRecord{};
        outcomes_[action] = ActionOutcome::NoBindingInActiveContext;
    }
}

void InputUser::observe(const DeviceEvent& event, const DeviceRegistry& registry) noexcept {
    trace_.record_event(event);
    if (scheme_.observe(event.control.kind, event.value, event.timestamp)) {
        // The scheme changed. Nothing to rebuild — prompts are the interface's concern and it reads
        // `scheme().active()` — but the preferred device follows the last meaningful input.
    }
    if (event.value != 0.0F) {
        preferred_ = event.device;
        if (accessibility_.sticky_modifiers) {
            sticky_ = is_modifier_key(event.control) ? event.control : Control{};
        }
    }

    // Only the bindings that read this control. Binary search over the sorted index; the scan is
    // over the equal range, which is one or two entries in practice.
    const u32 key = event.control.key();
    // `std::ranges::lower_bound` with a heterogeneous comparator needs a projection, and the
    // projection *is* the comparison here: the index is sorted by `control_key`, so projecting each
    // entry onto that member and comparing with `std::less` says the same thing more directly than
    // a two-type predicate would.
    auto* const lower = std::ranges::lower_bound(control_index_, key, std::less<>{},
                                                 &ControlIndexEntry::control_key);
    for (auto* it = lower; it != control_index_.end() && it->control_key == key; ++it) {
        const Binding& binding = resolved_[it->binding].binding;
        if (binding.kind == BindingKind::Sequence ||
            binding.trigger.kind == TriggerKind::Sequence) {
            continue;  // handled below, for every event rather than only for its own controls
        }
        evaluate_binding(it->binding, event.timestamp, registry, event.source, event.control);
    }
    for (const u16 sequence_binding : sequence_bindings_) {
        evaluate_binding(sequence_binding, event.timestamp, registry, event.source, event.control);
    }
}

void InputUser::finish_tick(Nanoseconds now, f32 delta_seconds,
                            const DeviceRegistry& registry) noexcept {
    last_delta_ = delta_seconds;
    // The time-dependent half. A hold reaches its duration and a pulse repeats with no event to
    // provoke them, so every binding that is either mid-phase or has a time-based trigger is
    // re-evaluated at the tick's own timestamp. A binding that is idle with a level-based trigger
    // is skipped, which is `input-and-actions`' "process only controls that changed or bindings
    // that are active".
    for (usize index = 0; index < resolved_.size(); ++index) {
        const BindingRuntime& runtime = runtime_[index];
        const TriggerKind kind = resolved_[index].binding.trigger.kind;
        if (!force_full_pass_ && runtime.phase == TriggerPhase::Idle && runtime.actuation == 0.0F &&
            !trigger_needs_time(kind)) {
            // Skipped, but still explained. An action nobody touched this tick has a reason it
            // produced nothing, and the inspector is opened precisely on the ticks where nothing
            // happened.
            classify_idle(static_cast<u32>(index));
            continue;
        }
        evaluate_binding(static_cast<u32>(index), now, registry, EventSource::Physical, Control{});
    }
    force_full_pass_ = false;

    for (usize action = 0; action < states_.size(); ++action) {
        ActionState& state = states_[action];
        if (!state.buffer_consumed && state.buffered_until != 0 && now > state.buffered_until) {
            state.buffer_consumed = true;
        }
        if (trace_.enabled()) {
            if (ActionTrace* trace = trace_.action(static_cast<ActionId>(action));
                trace != nullptr) {
                trace->outcome = outcomes_[action];
                trace->phase = state.phase;
            }
        }
    }
    build_command_frame();
}

bool InputUser::consume_buffered(ActionId action, Nanoseconds now) noexcept {
    if (action >= states_.size()) {
        return false;
    }
    ActionState& state = states_[action];
    if (state.buffer_consumed || state.buffered_until == 0 || now > state.buffered_until) {
        return false;
    }
    state.buffer_consumed = true;
    return true;
}

ActionOutcome InputUser::outcome(ActionId action) const noexcept {
    if (action >= outcomes_.size()) {
        return ActionOutcome::NoBindingInActiveContext;
    }
    return outcomes_[action];
}

void InputUser::build_command_frame() noexcept {
    frame_out_ = CommandFrame{};
    frame_out_.tick = tick_;
    frame_out_.user = id_;
    frame_out_.axis_count = frame_axis_count_;
    for (usize action = 0; action < states_.size(); ++action) {
        const u8 slot = frame_slot_[action];
        if (slot == kNoFrameSlot) {
            continue;
        }
        const ActionState& state = states_[action];
        if ((slot & kFrameButtonFlag) != 0) {
            const u32 bit = 1U << static_cast<u32>(slot & 0x1FU);
            if (state.pressed()) {
                frame_out_.pressed |= bit;
            }
            if (state.just_pressed()) {
                frame_out_.just_pressed |= bit;
            }
            if (state.just_released()) {
                frame_out_.just_released |= bit;
            }
        } else if (slot < kMaxFrameAxes) {
            frame_out_.axes[slot] = Vec2{state.value.axis.x, state.value.axis.y};
        }
    }
}

}  // namespace cy::input
