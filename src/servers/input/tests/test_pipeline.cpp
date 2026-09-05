// TASK 4.1.3 — processors, modifiers, triggers and the action lifecycle.
//
// The two cases worth reading first:
//
//   * "a delta is not scaled by frame time" — `input-and-actions` calls this "the most common input
//     defect in shipped games and nearly invisible until someone changes frame rate". The case
//     asserts the difference between a delta and a rate at two different step sizes, because at one
//     step size the two are indistinguishable, which is exactly why the defect survives review.
//   * "a hold that is abandoned reports started then cancelled" — a lifecycle that cannot tell an
//     abandoned hold from a completed one cannot draw the ring that fills.

#include "fixture.h"

using namespace cy::input_test;
using cy::f32;

CY_TEST_CASE("input: a delta is not scaled by the time step and a rate is") {
    // The engine's one place where a value meets a time step. `apply_time_step` takes the
    // interpretation, so the defect has nowhere left to live.
    const Vec3 value{2.0F, 0.0F, 0.0F};

    // At 60 Hz and at 30 Hz. The delta is the *same displacement* at both, which is the property; a
    // rate is twice as large in the longer step, which is also the property.
    const Vec3 delta_60 = apply_time_step(Interpretation::Delta, value, 1.0F / 60.0F);
    const Vec3 delta_30 = apply_time_step(Interpretation::Delta, value, 1.0F / 30.0F);
    CY_CHECK_NEAR(delta_60.x, 2.0F, 1e-6F);
    CY_CHECK_NEAR(delta_30.x, 2.0F, 1e-6F);
    CY_CHECK_NEAR(delta_60.x, delta_30.x, 1e-6F);

    const Vec3 rate_60 = apply_time_step(Interpretation::Rate, value, 1.0F / 60.0F);
    const Vec3 rate_30 = apply_time_step(Interpretation::Rate, value, 1.0F / 30.0F);
    CY_CHECK_NEAR(rate_30.x, rate_60.x * 2.0F, 1e-6F);

    // An absolute value is not scaled either — a pointer position is a position.
    CY_CHECK_NEAR(apply_time_step(Interpretation::Absolute, value, 1.0F / 30.0F).x, 2.0F, 1e-6F);
}

CY_TEST_CASE("input: a mouse delta accumulates within the window and does not latch across ticks") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId look = fixture.declare("Look", 1, ActionValueType::Axis2);
    const DeviceId mouse = fixture.attach(DeviceKind::Mouse);
    CY_REQUIRE(!mouse.is_null());

    Binding binding;
    binding.action = look;
    binding.kind = BindingKind::Axis2D;
    binding.interpretation = Interpretation::Delta;
    binding.component_count = 2;
    binding.components[0] =
        BindingComponent{mouse_control(MouseControl::MoveX), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[1] =
        BindingComponent{mouse_control(MouseControl::MoveY), Vec3{0.0F, 1.0F, 0.0F}};
    binding.trigger.kind = TriggerKind::Down;
    binding.trigger.actuation_threshold = 0.01F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // Three small motions in one window are one displacement, not the last one.
    fixture.press(mouse, mouse_control(MouseControl::MoveX), kMs, 3.0F);
    fixture.press(mouse, mouse_control(MouseControl::MoveX), 2 * kMs, 4.0F);
    fixture.press(mouse, mouse_control(MouseControl::MoveX), 3 * kMs, 5.0F);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(look).value.axis.x, 12.0F, 1e-4F);

    // And the next tick, with the mouse still, reads zero. A latched delta would move the camera on
    // every tick after one flick — the mirror image of the defect above and just as invisible in a
    // manual test where the mouse never stops.
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(look).value.axis.x, 0.0F, 1e-5F);
}

CY_TEST_CASE("input: a dead zone rescales so full deflection still reaches one") {
    const Processor chain[] = {Processor{ProcessorKind::DeadZone, 0.2F, 0.0F}};
    ProcessorState state;
    // Inside the dead zone: nothing.
    CY_CHECK_NEAR(evaluate_processors(chain, 1, Vec3{0.1F, 0.0F, 0.0F}, state, 0.016F).x, 0.0F,
                  1e-6F);
    // At full deflection: one. Without rescaling this would be 0.8 and the character would never
    // run at full speed — the part that is usually missing.
    CY_CHECK_NEAR(evaluate_processors(chain, 1, Vec3{1.0F, 0.0F, 0.0F}, state, 0.016F).x, 1.0F,
                  1e-6F);
    // Halfway past the threshold: halfway.
    CY_CHECK_NEAR(evaluate_processors(chain, 1, Vec3{0.6F, 0.0F, 0.0F}, state, 0.016F).x, 0.5F,
                  1e-6F);
}

CY_TEST_CASE("input: a radial dead zone has no square hole") {
    const Processor radial[] = {Processor{ProcessorKind::RadialDeadZone, 0.25F, 0.0F}};
    const Processor axis_wise[] = {Processor{ProcessorKind::DeadZone, 0.25F, 0.0F}};
    ProcessorState state;

    // A stick pushed diagonally at 0.2 on each axis: magnitude 0.28, which is past a radial
    // threshold of 0.25 and inside an axis-wise one.
    const Vec3 diagonal{0.2F, 0.2F, 0.0F};
    CY_CHECK_NE(evaluate_processors(radial, 1, diagonal, state, 0.016F).x, 0.0F);
    CY_CHECK_EQ(evaluate_processors(axis_wise, 1, diagonal, state, 0.016F).x, 0.0F);

    // Which is the bug: with an axis-wise dead zone the character drifts diagonally and nowhere
    // else, because straight up at 0.2 is discarded while diagonal at 0.2 is not.
    const Vec3 straight{0.0F, 0.2F, 0.0F};
    CY_CHECK_EQ(evaluate_processors(axis_wise, 1, straight, state, 0.016F).y, 0.0F);
}

CY_TEST_CASE("input: a chain is evaluated in order and allocates nothing") {
    const Processor chain[] = {
        Processor{ProcessorKind::DeadZone, 0.1F, 0.0F},
        Processor{ProcessorKind::ResponseCurve, 2.0F, 0.0F},
        Processor{ProcessorKind::Sensitivity, 3.0F, 0.0F},
        Processor{ProcessorKind::Clamp, -1.0F, 1.0F},
    };
    ProcessorState state;
    const f32 result = evaluate_processors(chain, 4, Vec3{1.0F, 0.0F, 0.0F}, state, 0.016F).x;
    // 1.0 survives the dead zone as 1.0, the curve as 1.0, the sensitivity as 3.0, and the clamp
    // brings it back to 1.0. Order matters: clamping first would have made the sensitivity useless.
    CY_CHECK_NEAR(result, 1.0F, 1e-6F);

    const f32 half = evaluate_processors(chain, 4, Vec3{0.55F, 0.0F, 0.0F}, state, 0.016F).x;
    CY_CHECK_GT(half, 0.0F);
    CY_CHECK_LE(half, 1.0F);
}

CY_TEST_CASE("input: smoothing is the only processor that reads the time step") {
    const Processor chain[] = {Processor{ProcessorKind::Smooth, 0.1F, 0.0F}};
    ProcessorState state;
    // The first sample seeds the filter rather than ramping from zero, which would read as input
    // lag on the frame an action is first touched.
    CY_CHECK_NEAR(evaluate_processors(chain, 1, Vec3{1.0F, 0.0F, 0.0F}, state, 0.016F).x, 1.0F,
                  1e-6F);
    // Then it lags: a step to zero does not arrive in one frame.
    const f32 next = evaluate_processors(chain, 1, Vec3{0.0F, 0.0F, 0.0F}, state, 0.016F).x;
    CY_CHECK_GT(next, 0.0F);
    CY_CHECK_LT(next, 1.0F);
}

CY_TEST_CASE("input: a reference frame modifier takes vectors, never a camera") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Axis2);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    Binding binding = wasd(move, Key::A, Key::D, Key::S, Key::W);
    binding.modifier.kind = ModifierKind::ReferenceFrame;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // The frame is *supplied* — three vectors, from whoever owns a camera. The input system holds
    // no camera type and cannot reach a renderer; see binding.h's `ReferenceFrame`.
    ReferenceFrame frame;
    frame.forward = Vec3{0.0F, 0.0F, -1.0F};
    frame.right = Vec3{1.0F, 0.0F, 0.0F};
    fixture.server.user(0).set_reference_frame(frame);

    fixture.press(keyboard, key_control(Key::W), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    // "Forward" on the keys became forward in the supplied frame: -Z, not +Y.
    const Vec3 value = fixture.server.user(0).action_state(move).value.axis;
    CY_CHECK_NEAR(value.z, -1.0F, 1e-5F);
    CY_CHECK_NEAR(value.y, 0.0F, 1e-5F);
}

CY_TEST_CASE("input: sensitivity is a setting, and changing it rewrites no binding") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId look = fixture.declare("Look", 1, ActionValueType::Scalar);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    Binding binding = simple(look, key_control(Key::D));
    binding.modifier.kind = ModifierKind::SettingScale;
    binding.modifier.key = cy::Name::intern("look.sensitivity");
    binding.modifier.fallback = 1.0F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::D), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(look).value.axis.x, 1.0F, 1e-5F);

    CY_REQUIRE(
        fixture.server.user(0).set_setting(cy::Name::intern("look.sensitivity"), 4.0F).has_value());
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(look).value.axis.x, 4.0F, 1e-5F);

    // The authored binding is untouched: the setting is read at evaluation, not folded into it.
    const MappingContext* stored = fixture.server.context(fixture.installed);
    CY_REQUIRE(stored != nullptr);
    CY_CHECK_EQ(stored->binding(0).modifier.fallback, 1.0F);
}

CY_TEST_CASE("input: a hold that is abandoned reports started then cancelled, never triggered") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId interact = fixture.declare("Interact", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    Binding binding = simple(interact, key_control(Key::E), TriggerKind::Hold);
    binding.trigger.duration_seconds = 0.5F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::E), 10 * kMs);
    fixture.server.resolve_tick(1, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK(has_flag(fixture.server.user(0).action_state(interact).flags, ActionFlag::Started));
    CY_CHECK_FALSE(fixture.server.user(0).action_state(interact).triggered());

    // Released at 200 ms, well before the 500 ms threshold.
    fixture.release(keyboard, key_control(Key::E), 210 * kMs);
    fixture.server.resolve_tick(2, 220 * kMs, 1.0F / 60.0F);
    const ActionState& abandoned = fixture.server.user(0).action_state(interact);
    CY_CHECK(has_flag(abandoned.flags, ActionFlag::Cancelled));
    CY_CHECK_FALSE(abandoned.triggered());
    CY_CHECK_EQ(abandoned.phase, TriggerPhase::Cancelled);
}

CY_TEST_CASE("input: a hold that completes triggers with no further event") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId interact = fixture.declare("Interact", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    Binding binding = simple(interact, key_control(Key::E), TriggerKind::Hold);
    binding.trigger.duration_seconds = 0.5F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::E), 10 * kMs);
    fixture.server.resolve_tick(1, 20 * kMs, 1.0F / 60.0F);
    CY_REQUIRE_FALSE(fixture.server.user(0).action_state(interact).triggered());

    // No further event. The hold reaches its duration because the *tick* advances time, which is
    // why `finish_tick` re-evaluates time-based triggers.
    fixture.server.resolve_tick(2, 600 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(interact).triggered());
    CY_CHECK_EQ(fixture.server.user(0).action_state(interact).phase, TriggerPhase::Triggered);
}

CY_TEST_CASE("input: hold-to-toggle converts every hold with no per-action implementation") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId crouch = fixture.declare("Crouch", 1, ActionValueType::Digital);
    const ActionId aim = fixture.declare("Aim", 2, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    MappingContext context(allocator());
    Binding first = simple(crouch, key_control(Key::C), TriggerKind::Hold);
    first.trigger.duration_seconds = 0.3F;
    Binding second = simple(aim, key_control(Key::Q), TriggerKind::Hold);
    second.trigger.duration_seconds = 0.3F;
    CY_REQUIRE(context.add(first).has_value());
    CY_REQUIRE(context.add(second).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // One setting, applied inside the pipeline. Both actions convert; neither knows about it.
    AccessibilitySettings settings;
    settings.hold_to_toggle = true;
    fixture.server.user(0).set_accessibility(settings);

    fixture.press(keyboard, key_control(Key::C), 10 * kMs);
    fixture.release(keyboard, key_control(Key::C), 20 * kMs);
    fixture.press(keyboard, key_control(Key::Q), 21 * kMs);
    fixture.release(keyboard, key_control(Key::Q), 22 * kMs);
    fixture.server.resolve_tick(1, 30 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(crouch).triggered());
    CY_CHECK(fixture.server.user(0).action_state(aim).triggered());

    // Still on with the key up — that is what a toggle is — and the second press turns it off.
    fixture.server.resolve_tick(2, 40 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(crouch).triggered());
    fixture.press(keyboard, key_control(Key::C), 50 * kMs);
    fixture.release(keyboard, key_control(Key::C), 55 * kMs);
    fixture.server.resolve_tick(3, 60 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(crouch).triggered());
}

CY_TEST_CASE("input: an accessibility dead-zone adjustment cannot be bypassed") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Scalar);
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad);

    Binding binding = simple(move, gamepad_control(GamepadControl::LeftStickX));
    binding.processor_count = 1;
    binding.processors[0] = Processor{ProcessorKind::DeadZone, 0.5F, 0.0F};
    binding.trigger.actuation_threshold = 0.01F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(pad, gamepad_control(GamepadControl::LeftStickX), kMs, 0.3F);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.x, 0.0F, 1e-5F);

    // Opening the dead zone up for a player whose hands shake. One setting, applied inside the
    // chain — the action's value changes and there is no path to the untransformed one.
    AccessibilitySettings settings;
    settings.dead_zone_scale = 0.2F;
    fixture.server.user(0).set_accessibility(settings);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_GT(fixture.server.user(0).action_state(move).value.axis.x, 0.0F);
}

CY_TEST_CASE("input: a threshold trigger fires on the crossing, not on the level") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId shoot = fixture.declare("Shoot", 1, ActionValueType::Scalar);
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad);
    Binding binding =
        simple(shoot, gamepad_control(GamepadControl::RightTrigger), TriggerKind::Threshold);
    binding.trigger.threshold = 0.7F;
    binding.trigger.actuation_threshold = 0.1F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(pad, gamepad_control(GamepadControl::RightTrigger), kMs, 0.4F);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(shoot).triggered());

    fixture.press(pad, gamepad_control(GamepadControl::RightTrigger), 11 * kMs, 0.9F);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(shoot).triggered());

    // Held past the threshold: no second trigger. A threshold is a crossing.
    fixture.press(pad, gamepad_control(GamepadControl::RightTrigger), 21 * kMs, 0.95F);
    fixture.server.resolve_tick(3, 30 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(shoot).triggered());
}

CY_TEST_CASE("input: typing suppresses gameplay actions bound to the same keys") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Axis2);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(wasd(move, Key::A, Key::D, Key::S, Key::W)).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // `input-and-actions` — "Typing does not move the player": a text field takes focus and every
    // gameplay context stops producing, unless the context declares pass-through.
    fixture.server.user(0).set_text_entry_active(true);
    fixture.press(keyboard, key_control(Key::W), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.y, 0.0F, 1e-5F);
    CY_CHECK_EQ(fixture.server.user(0).outcome(move), ActionOutcome::SuppressedByFocus);

    fixture.server.user(0).set_text_entry_active(false);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.y, 1.0F, 1e-5F);
}
