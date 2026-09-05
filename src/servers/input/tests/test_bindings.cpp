// TASK 4.1.2 — actions and value types, mapping contexts, bindings and composites.
//
// The cases are `input-and-actions`' scenarios under "Actions and value types", "Mapping contexts"
// and "Bindings and composites". Two of them are about arrangements that only fail once a second
// context exists, which is why they are written now rather than when a menu is built:
//
//   * "Unwinding is order-independent" — contexts removed in a different order than they were
//     added. An index-based stack passes this test right up to the day a modal opens over an
//     inventory, and then it silently pops the wrong one.
//   * "A modal over an inventory over a vehicle" — consumption stops the lower context acting,
//     and the *reason* is reportable rather than the action simply being quiet.

#include "fixture.h"

using namespace cy::input_test;
using cy::f32;

CY_TEST_CASE("input: an action is identified by a stable id, not by its name") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 17, ActionValueType::Axis2);
    CY_REQUIRE_NE(move, kInvalidAction);

    // The dense id is an index; the stable id is the identity. A profile stores the second.
    CY_CHECK_EQ(fixture.server.actions().find(ActionStableId{17}), move);
    CY_CHECK_EQ(fixture.server.actions().at(move).stable_id.value, 17);

    // Two actions cannot share one identity: a stored override would resolve differently depending
    // on registration order.
    ActionDeclaration duplicate;
    duplicate.name = cy::Name::intern("Walk");
    duplicate.stable_id = ActionStableId{17};
    CY_CHECK_FALSE(fixture.server.actions().declare(duplicate).has_value());

    // And zero is the null identity rather than the first action's.
    ActionDeclaration unnamed;
    unnamed.name = cy::Name::intern("Nothing");
    CY_CHECK_FALSE(fixture.server.actions().declare(unnamed).has_value());
}

CY_TEST_CASE("input: four keys produce one two-dimensional action") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Axis2);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    CY_REQUIRE(!keyboard.is_null());
    MappingContext context(allocator());
    CY_REQUIRE(context.add(wasd(move, Key::A, Key::D, Key::S, Key::W)).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::W), kMs);
    fixture.press(keyboard, key_control(Key::D), 2 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    // The consumer reads one Vec2 and never learns that four keys produced it, which is the
    // requirement's "without gameplay knowing".
    const ActionValue value = fixture.server.user(0).action_state(move).value;
    CY_CHECK_NEAR(value.axis.x, 1.0F, 1e-5F);
    CY_CHECK_NEAR(value.axis.y, 1.0F, 1e-5F);
}

CY_TEST_CASE("input: opposing keys cancel and a released key returns the axis to rest") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Axis2);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(wasd(move, Key::A, Key::D, Key::S, Key::W)).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::A), kMs);
    fixture.press(keyboard, key_control(Key::D), 2 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.x, 0.0F, 1e-5F);

    fixture.release(keyboard, key_control(Key::A), 11 * kMs);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.x, 1.0F, 1e-5F);
}

CY_TEST_CASE("input: a modal over gameplay consumes the action, and says so") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId confirm = fixture.declare("Confirm", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    MappingContext gameplay(allocator());
    Binding low = simple(confirm, key_control(Key::Enter));
    low.consume = true;
    CY_REQUIRE(gameplay.add(low).has_value());
    CY_REQUIRE(fixture.install(std::move(gameplay), 0, 0));

    // A second context at a higher priority, binding the same action to a different key, and
    // consuming it. The gameplay binding must not act — even on its own key.
    MappingContext modal(allocator());
    Binding high = simple(confirm, key_control(Key::Space));
    high.consume = true;
    CY_REQUIRE(modal.add(high).has_value());
    auto registered = fixture.server.register_context(std::move(modal));
    CY_REQUIRE(registered.has_value());
    CY_REQUIRE(fixture.server.user(0).push_context(*registered, 100).has_value());

    fixture.press(keyboard, key_control(Key::Enter), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    CY_CHECK_FALSE(fixture.server.user(0).action_state(confirm).pressed());
    // And the *reason* is reportable, which is `input-and-actions`' "Why did nothing happen".
    CY_CHECK_EQ(fixture.server.user(0).outcome(confirm), ActionOutcome::ConsumedByHigherContext);
}

CY_TEST_CASE("input: a context that does not consume lets a lower one augment it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 1, ActionValueType::Scalar);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    MappingContext gameplay(allocator());
    CY_REQUIRE(gameplay.add(simple(move, key_control(Key::D))).has_value());
    CY_REQUIRE(fixture.install(std::move(gameplay), 0, 0));

    MappingContext overlay(allocator());
    Binding high = simple(move, key_control(Key::Right));
    high.consume = false;  // augment, not override
    CY_REQUIRE(overlay.add(high).has_value());
    auto registered = fixture.server.register_context(std::move(overlay));
    CY_REQUIRE(registered.has_value());
    CY_REQUIRE(fixture.server.user(0).push_context(*registered, 100).has_value());

    // The lower context's key still drives the action: the higher one is idle and does not write
    // zero over it. That arbitration is `ClaimRecord`'s rule — prefer an actuated binding.
    fixture.press(keyboard, key_control(Key::D), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(move).pressed());
}

CY_TEST_CASE("input: contexts are removed by handle, in any order") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId confirm = fixture.declare("Confirm", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    MappingContext vehicle(allocator());
    CY_REQUIRE(vehicle.add(simple(confirm, key_control(Key::Enter))).has_value());
    CY_REQUIRE(fixture.install(std::move(vehicle), 0, 0));
    const ContextHandle vehicle_handle = fixture.installed;

    MappingContext inventory(allocator());
    CY_REQUIRE(inventory.add(simple(confirm, key_control(Key::Tab))).has_value());
    auto inventory_handle = fixture.server.register_context(std::move(inventory));
    CY_REQUIRE(inventory_handle.has_value());
    CY_REQUIRE(fixture.server.user(0).push_context(*inventory_handle, 10).has_value());

    MappingContext modal(allocator());
    CY_REQUIRE(modal.add(simple(confirm, key_control(Key::Space))).has_value());
    auto modal_handle = fixture.server.register_context(std::move(modal));
    CY_REQUIRE(modal_handle.has_value());
    CY_REQUIRE(fixture.server.user(0).push_context(*modal_handle, 20).has_value());
    CY_REQUIRE_EQ(fixture.server.user(0).context_count(), 3);

    // Removed in the *opposite* order from an index-based stack's assumption: the middle one first.
    CY_CHECK(fixture.server.user(0).pop_context(*inventory_handle));
    CY_CHECK(fixture.server.user(0).pop_context(vehicle_handle));
    CY_CHECK_EQ(fixture.server.user(0).context_count(), 1);

    // What remains is the modal, and its key is the one that works.
    fixture.press(keyboard, key_control(Key::Space), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(confirm).pressed());

    // Unwinding twice is not a crash: an interface that already closed a dialogue is allowed to
    // try.
    CY_CHECK_FALSE(fixture.server.user(0).pop_context(vehicle_handle));
}

CY_TEST_CASE("input: a chord is one binding, not two button states") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId sprint = fixture.declare("Sprint", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    Binding chord;
    chord.action = sprint;
    chord.kind = BindingKind::Chord;
    chord.component_count = 2;
    chord.components[0].control = key_control(Key::LeftShift);
    chord.components[1].control = key_control(Key::W);
    chord.trigger.kind = TriggerKind::Chord;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(chord).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    fixture.press(keyboard, key_control(Key::LeftShift), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(sprint).pressed());

    fixture.press(keyboard, key_control(Key::W), 11 * kMs);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(sprint).pressed());

    fixture.release(keyboard, key_control(Key::LeftShift), 21 * kMs);
    fixture.server.resolve_tick(3, 30 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(sprint).pressed());
}

CY_TEST_CASE("input: a sequence is a trigger, and an intruding control resets it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId combo = fixture.declare("Combo", 1, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    Binding sequence;
    sequence.action = combo;
    sequence.kind = BindingKind::Sequence;
    sequence.component_count = 2;
    sequence.components[0].control = key_control(Key::A);
    sequence.components[1].control = key_control(Key::B);
    sequence.trigger.kind = TriggerKind::Sequence;
    sequence.trigger.duration_seconds = 0.5F;
    MappingContext context(allocator());
    CY_REQUIRE(context.add(sequence).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // A, then Q, then B. Not the sequence: the intruder resets it.
    fixture.press(keyboard, key_control(Key::A), kMs);
    fixture.press(keyboard, key_control(Key::Q), 2 * kMs);
    fixture.press(keyboard, key_control(Key::B), 3 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.user(0).action_state(combo).triggered());

    // A then B, in order and in time.
    fixture.release(keyboard, key_control(Key::A), 11 * kMs);
    fixture.release(keyboard, key_control(Key::B), 12 * kMs);
    fixture.release(keyboard, key_control(Key::Q), 13 * kMs);
    fixture.press(keyboard, key_control(Key::A), 14 * kMs);
    fixture.press(keyboard, key_control(Key::B), 16 * kMs);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(combo).triggered());
}

CY_TEST_CASE("input: a control path is parsed at cook time and its inverse recovers it") {
    // The evaluation path never sees a string. This is the only function that reads one, it is in a
    // namespace named for when it runs, and nothing under src/servers/input/src/ calls it outside
    // this test and a rebinding flow. See types.h.
    const Control key = cook::parse_control_path("keyboard/w");
    CY_CHECK_EQ(key.kind, DeviceKind::Keyboard);
    CY_CHECK_EQ(key.code, static_cast<cy::u16>(Key::W));
    CY_CHECK(cy::input::cook::parse_control_path("gamepad/leftStickX") ==
             gamepad_control(GamepadControl::LeftStickX));
    CY_CHECK(cy::input::cook::parse_control_path("mouse/moveX") ==
             mouse_control(MouseControl::MoveX));

    CY_CHECK_FALSE(cook::parse_control_path("keyboard").is_valid());
    CY_CHECK_FALSE(cook::parse_control_path("keyboard/nonexistent").is_valid());
    CY_CHECK_FALSE(cook::parse_control_path("nosuchdevice/w").is_valid());
}
