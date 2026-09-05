// TASK 4.1.6 — input diagnostics: why an action did not trigger, the per-processor trace, the event
// trace and the latency view.
//
// `input-and-actions` — "Why did nothing happen": the inspector "SHALL state the reason, naming the
// context, binding, or trigger responsible". The requirement lists six reasons and each is a case
// here, because a diagnostic that can only answer some of the question is one a developer stops
// trusting and then stops opening.

#include "fixture.h"

using namespace cy::input_test;
using cy::f32;
using cy::u32;

CY_TEST_CASE("input: an unbound action reports that nothing active binds it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 1, ActionValueType::Digital);
    const ActionId unbound = fixture.declare("Reload", 2, ActionValueType::Digital);
    CY_REQUIRE(!fixture.attach(DeviceKind::Keyboard).is_null());
    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::Space))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.user(0).outcome(unbound), ActionOutcome::NoBindingInActiveContext);
    CY_CHECK_EQ(fixture.server.user(0).outcome(fire), ActionOutcome::BelowThreshold);
}

CY_TEST_CASE("input: a binding whose device class is absent says so") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 1, ActionValueType::Digital);
    // A keyboard is attached; the binding names a gamepad control.
    CY_REQUIRE(!fixture.attach(DeviceKind::Keyboard).is_null());
    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(fire, gamepad_control(GamepadControl::South))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    // Not an error — a binding whose scheme is inactive — but it is the answer to the question.
    CY_CHECK_EQ(fixture.server.user(0).outcome(fire), ActionOutcome::DeviceUnassigned);
}

CY_TEST_CASE("input: a hold that has not reached its duration reports conditions unmet") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId interact = fixture.declare("Interact", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    Binding binding = simple(interact, key_control(Key::E), TriggerKind::Hold);
    binding.trigger.duration_seconds = 1.0F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::E), 10 * kMs);
    fixture.server.resolve_tick(1, 20 * kMs, 1.0F / 60.0F);
    // The control is actuated and the action has not fired: the trigger is the reason, and the
    // inspector says which of the six it is rather than leaving the developer to guess.
    CY_CHECK_EQ(fixture.server.user(0).outcome(interact), ActionOutcome::TriggerConditionsUnmet);
}

CY_TEST_CASE("input: the trace records the raw value, every processor stage and the final value") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId look = fixture.declare("Look", 1, ActionValueType::Scalar);
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad);
    Binding binding = simple(look, gamepad_control(GamepadControl::LeftStickX));
    binding.trigger.actuation_threshold = 0.01F;
    binding.processor_count = 2;
    binding.processors[0] = Processor{ProcessorKind::DeadZone, 0.2F, 0.0F};
    binding.processors[1] = Processor{ProcessorKind::Sensitivity, 2.0F, 0.0F};
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    CY_REQUIRE(
        fixture.server.user(0).trace().enable(fixture.server.actions().count(), 64).has_value());

    fixture.press(pad, gamepad_control(GamepadControl::LeftStickX), kMs, 0.6F);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    const ActionTrace* trace = fixture.server.user(0).trace().action(look);
    CY_REQUIRE(trace != nullptr);
    CY_CHECK_NEAR(trace->raw.x, 0.6F, 1e-5F);
    CY_CHECK_EQ(trace->stage_count, 2);
    // After the dead zone: (0.6 - 0.2) / 0.8 = 0.5. After sensitivity: 1.0. Being able to see the
    // intermediate is the difference between "the value is wrong" and "the dead zone ate it".
    CY_CHECK_NEAR(trace->stages[0].x, 0.5F, 1e-5F);
    CY_CHECK_NEAR(trace->stages[1].x, 1.0F, 1e-5F);
    CY_CHECK_NEAR(trace->final_value.x, 1.0F, 1e-5F);
    // And which binding produced it, which is the clause a value alone cannot answer.
    CY_CHECK_EQ(trace->binding, 0);
    CY_CHECK(trace->context == fixture.installed);
}

CY_TEST_CASE("input: the trace names the context that consumed an action") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId confirm = fixture.declare("Confirm", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext gameplay(allocator());
    CY_REQUIRE(gameplay.add(simple(confirm, key_control(Key::Enter))).has_value());
    CY_REQUIRE(fixture.install(std::move(gameplay)));

    MappingContext modal(allocator());
    CY_REQUIRE(modal.add(simple(confirm, key_control(Key::Space))).has_value());
    auto modal_handle = fixture.server.register_context(std::move(modal));
    CY_REQUIRE(modal_handle.has_value());
    CY_REQUIRE(fixture.server.user(0).push_context(*modal_handle, 50).has_value());
    CY_REQUIRE(
        fixture.server.user(0).trace().enable(fixture.server.actions().count(), 64).has_value());

    fixture.press(keyboard, key_control(Key::Enter), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    const ActionTrace* trace = fixture.server.user(0).trace().action(confirm);
    CY_REQUIRE(trace != nullptr);
    CY_CHECK_EQ(trace->outcome, ActionOutcome::ConsumedByHigherContext);
    // Naming the context is the requirement: "the inspector SHALL state the reason, naming the
    // context, binding, or trigger responsible".
    CY_CHECK(trace->consumed_by == *modal_handle);
}

CY_TEST_CASE("input: the event trace records device events with their timestamps") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::Space))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));
    CY_REQUIRE(
        fixture.server.user(0).trace().enable(fixture.server.actions().count(), 8).has_value());

    fixture.press(keyboard, key_control(Key::Space), 3 * kMs);
    fixture.release(keyboard, key_control(Key::Space), 5 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    const cy::Array<DeviceEvent>& events = fixture.server.user(0).trace().events();
    CY_REQUIRE_EQ(events.size(), 2);
    // The timestamp the platform observed, not the time it was processed — which is what makes the
    // latency view and a faithful replay possible at all.
    CY_CHECK_EQ(events[0].timestamp, 3 * kMs);
    CY_CHECK_EQ(events[1].timestamp, 5 * kMs);
}

CY_TEST_CASE("input: the event trace is a window, not a log that grows without bound") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.declare("Fire", 1, ActionValueType::Digital) != kInvalidAction);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(fixture.install(std::move(context)));
    CY_REQUIRE(
        fixture.server.user(0).trace().enable(fixture.server.actions().count(), 4).has_value());

    for (int index = 0; index < 20; ++index) {
        fixture.press(keyboard, key_control(Key::Space),
                      static_cast<cy::Nanoseconds>(index + 1) * kMs);
    }
    fixture.server.resolve_tick(1, 100 * kMs, 1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.user(0).trace().events().size(), 4);
}

CY_TEST_CASE("input: the latency view keeps the stages apart") {
    LatencySample sample;
    sample.device_event = 1 * kMs;
    sample.action_evaluated = 3 * kMs;
    sample.tick_consumed = 8 * kMs;
    sample.frame_presented = 0;

    CY_CHECK_EQ(sample.to_action(), 2 * kMs);
    CY_CHECK_EQ(sample.to_tick(), 7 * kMs);
    // The renderer fills in the last stage. Zero means "not yet known" rather than "no latency",
    // which is why the sample holds four timestamps rather than three durations.
    CY_CHECK_EQ(sample.to_frame(), 0);
    sample.frame_presented = 20 * kMs;
    CY_CHECK_EQ(sample.to_frame(), 19 * kMs);
}

CY_TEST_CASE("input: tracing is off by default and records nothing when disabled") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::Space))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    CY_CHECK_FALSE(fixture.server.user(0).trace().enabled());
    fixture.press(keyboard, key_control(Key::Space), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    // The evaluation path branches on one bool and writes nothing. `input-and-actions` requires
    // evaluation to allocate nothing per frame, and a trace buffer that existed unconditionally
    // would be a per-frame write to memory nobody reads.
    CY_CHECK(fixture.server.user(0).trace().action(fire) == nullptr);
    CY_CHECK_EQ(fixture.server.user(0).trace().events().size(), 0);
    // The action still resolved: tracing is an observer, not a participant.
    CY_CHECK(fixture.server.user(0).action_state(fire).pressed());
}

CY_TEST_CASE("input: a full accumulation window reports what it dropped") {
    Fixture fixture;
    InputServerConfig config;
    config.event_capacity = 4;
    CY_REQUIRE(fixture.server.configure(config).has_value());
    CY_REQUIRE(fixture.server.initialize().has_value());
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    CY_REQUIRE(fixture.server.finalize_declarations().has_value());

    for (int index = 0; index < 10; ++index) {
        fixture.press(keyboard, key_control(Key::Space),
                      static_cast<cy::Nanoseconds>(index + 1) * kMs);
    }
    fixture.server.resolve_tick(1, 100 * kMs, 1.0F / 60.0F);

    // Six dropped, and *reported*. A dropped input is a defect a player feels; a silent drop is the
    // same class of bug as coalescing a press with its release.
    CY_CHECK_EQ(fixture.server.last_event_count(), 4);
    CY_CHECK_EQ(fixture.server.dropped_events(), 6);
}

CY_TEST_CASE("input: every enumerator has a spelling a diagnostic can print") {
    // A `*_name()` that returned null or an empty string would make a report unreadable exactly
    // when it is being read — while something is going wrong.
    for (u32 index = 0; index < static_cast<u32>(ActionOutcome::Count); ++index) {
        CY_CHECK_NE(action_outcome_name(static_cast<ActionOutcome>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(TriggerPhase::Count); ++index) {
        CY_CHECK_NE(trigger_phase_name(static_cast<TriggerPhase>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(TriggerKind::Count); ++index) {
        CY_CHECK_NE(trigger_kind_name(static_cast<TriggerKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(DeviceKind::Count); ++index) {
        CY_CHECK_NE(device_kind_name(static_cast<DeviceKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(ProcessorKind::Count); ++index) {
        CY_CHECK_NE(processor_kind_name(static_cast<ProcessorKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(ModifierKind::Count); ++index) {
        CY_CHECK_NE(modifier_kind_name(static_cast<ModifierKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(BindingKind::Count); ++index) {
        CY_CHECK_NE(binding_kind_name(static_cast<BindingKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(DeviceLifecycle::Count); ++index) {
        CY_CHECK_NE(device_lifecycle_name(static_cast<DeviceLifecycle>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(SchemeKind::Count); ++index) {
        CY_CHECK_NE(scheme_kind_name(static_cast<SchemeKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(EventSource::Count); ++index) {
        CY_CHECK_NE(event_source_name(static_cast<EventSource>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(FocusLayer::Count); ++index) {
        CY_CHECK_NE(focus_layer_name(static_cast<FocusLayer>(index))[0], '\0');
    }
    CY_CHECK_NE(interpretation_name(Interpretation::Delta)[0], '\0');
    CY_CHECK_NE(disconnect_policy_name(DisconnectPolicy::Pause)[0], '\0');
    CY_CHECK_NE(keyboard_mouse_policy_name(KeyboardMousePolicy::Split)[0], '\0');
    CY_CHECK_NE(conflict_policy_name(ConflictPolicy::Swap)[0], '\0');
    CY_CHECK_NE(rebind_status_name(RebindStatus::Applied)[0], '\0');
    CY_CHECK_NE(action_value_type_name(ActionValueType::Pose)[0], '\0');
}
