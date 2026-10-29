// TASK 4.1.5 — control schemes, device detection and rebinding.
//
// The scheme cases exist as a pair on purpose. `input-and-actions` requires "hysteresis **and** a
// significance threshold", and they answer different questions:
//
//   * significance — "was this input meaningful?" A stick resting at 0.03 is not.
//   * hysteresis — "is this a change of intent?" One brush of the mouse is not.
//
// Written as one guard, only one of the two failures gets fixed, and the other reappears the first
// time a player leaves a controller on the desk. There is a case for each, and one that shows a
// single significant sample is not enough on its own.

#include "fixture.h"

using namespace cy::input_test;
using cy::f32;

CY_TEST_CASE("input: a noisy idle stick does not change the active scheme") {
    SchemeDetector detector;
    detector.set_active(SchemeKind::KeyboardMouse, 0);
    detector.set_significance(0.2F);
    detector.set_hysteresis(0.25F);

    // Drift, reported for a full second, all of it below the threshold. Not evidence of anything,
    // and discarded before it can even become a candidate.
    for (int index = 0; index < 60; ++index) {
        CY_CHECK_FALSE(detector.observe(DeviceKind::Gamepad, 0.05F,
                                        static_cast<cy::Nanoseconds>(index) * 16 * kMs));
    }
    CY_CHECK_EQ(detector.active(), SchemeKind::KeyboardMouse);
}

CY_TEST_CASE("input: one significant sample is not yet a change of scheme") {
    SchemeDetector detector;
    detector.set_active(SchemeKind::Gamepad, 0);
    detector.set_significance(0.2F);
    detector.set_hysteresis(0.25F);

    // A player using a controller brushes the mouse once. The prompts must not flip mid-sentence.
    CY_CHECK_FALSE(detector.observe(DeviceKind::Mouse, 1.0F, 10 * kMs));
    CY_CHECK_EQ(detector.active(), SchemeKind::Gamepad);
    CY_CHECK_EQ(detector.candidate(), SchemeKind::KeyboardMouse);
}

CY_TEST_CASE("input: sustained input on another scheme changes it, and prompts follow") {
    SchemeDetector detector;
    detector.set_active(SchemeKind::Gamepad, 0);
    detector.set_significance(0.2F);
    detector.set_hysteresis(0.25F);

    CY_CHECK_FALSE(detector.observe(DeviceKind::Mouse, 1.0F, 10 * kMs));
    // Still leading 300 ms later, past the dwell time.
    CY_CHECK(detector.observe(DeviceKind::Mouse, 1.0F, 320 * kMs));
    CY_CHECK_EQ(detector.active(), SchemeKind::KeyboardMouse);
    CY_CHECK_EQ(detector.changed_at(), 320 * kMs);
}

CY_TEST_CASE("input: activity on the active scheme resets the challenger's dwell") {
    SchemeDetector detector;
    detector.set_active(SchemeKind::Gamepad, 0);
    detector.set_significance(0.2F);
    detector.set_hysteresis(0.25F);

    // A stray mouse nudge at 10 ms, then the player keeps using the controller. Without the reset,
    // the nudge's dwell time would accumulate across a whole minute of controller play and flip the
    // prompts long after the mouse stopped.
    CY_CHECK_FALSE(detector.observe(DeviceKind::Mouse, 1.0F, 10 * kMs));
    CY_CHECK_FALSE(detector.observe(DeviceKind::Gamepad, 0.9F, 100 * kMs));
    CY_CHECK_FALSE(detector.observe(DeviceKind::Mouse, 1.0F, 500 * kMs));
    CY_CHECK_EQ(detector.active(), SchemeKind::Gamepad);
}

CY_TEST_CASE("input: a scheme declares the devices it requires") {
    const ControlScheme gamepad = builtin_scheme(SchemeKind::Gamepad);
    CY_CHECK(gamepad.satisfied_by(device_mask(DeviceKind::Gamepad)));
    CY_CHECK_FALSE(gamepad.satisfied_by(device_mask(DeviceKind::Keyboard)));

    // The mouse is optional for keyboard-and-mouse on purpose: a keyboard-only player is still on
    // that scheme, and a scheme requiring both would report "unsatisfied" on a machine that can
    // obviously play.
    const ControlScheme desktop = builtin_scheme(SchemeKind::KeyboardMouse);
    CY_CHECK(desktop.satisfied_by(device_mask(DeviceKind::Keyboard)));
}

CY_TEST_CASE("input: a rebind stores an override and leaves the authored binding alone") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 7, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);

    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::Space))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // The flow: begin, listen, apply.
    RebindOperation rebind(ActionStableId{7}, SchemeKind::KeyboardMouse, 0, ConflictPolicy::Reject);
    DeviceEvent offered;
    offered.device = keyboard;
    offered.control = key_control(Key::F);
    offered.value = 1.0F;
    offered.timestamp = kMs;
    CY_CHECK_EQ(rebind.offer(offered, fixture.server.user(0).profile()), RebindStatus::Applied);
    CY_REQUIRE(rebind.apply(fixture.server.user(0).profile()).has_value());
    fixture.server.user(0).invalidate();

    // The player's key works.
    fixture.press(keyboard, key_control(Key::F), 2 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK(fixture.server.user(0).action_state(fire).pressed());

    // THE SHIPPED ASSET IS UNMODIFIED. This is the requirement's "an update does not lose
    // customisation": the override is applied into the resolved copy, never into the context.
    const MappingContext* stored = fixture.server.context(fixture.installed);
    CY_REQUIRE(stored != nullptr);
    CY_CHECK(stored->binding(0).components[0].control == key_control(Key::Space));
}

CY_TEST_CASE("input: a rebind ignores a control from another scheme and honours cancel") {
    BindingProfile profile(allocator());
    RebindOperation rebind(ActionStableId{7}, SchemeKind::KeyboardMouse, 0, ConflictPolicy::Reject);
    rebind.set_cancel_control(key_control(Key::Escape));

    DeviceEvent from_pad;
    from_pad.control = gamepad_control(GamepadControl::South);
    from_pad.value = 1.0F;
    // Not a mistake worth an error — the player pressed the wrong thing — so the flow stays open.
    CY_CHECK_EQ(rebind.offer(from_pad, profile), RebindStatus::Listening);

    DeviceEvent cancel;
    cancel.control = key_control(Key::Escape);
    cancel.value = 1.0F;
    CY_CHECK_EQ(rebind.offer(cancel, profile), RebindStatus::Cancelled);
    // And a cancelled flow refuses to write anything.
    CY_CHECK_FALSE(rebind.apply(profile).has_value());
}

CY_TEST_CASE("input: a rebind conflict is resolved by the declared policy, not silently") {
    BindingProfile profile(allocator());
    CY_REQUIRE(profile
                   .set(BindingOverride{ActionStableId{1}, SchemeKind::KeyboardMouse, 0,
                                        key_control(Key::F)})
                   .has_value());

    DeviceEvent duplicate;
    duplicate.control = key_control(Key::F);
    duplicate.value = 1.0F;

    // Reject: refused, and the refusal says why.
    RebindOperation rejecting(ActionStableId{2}, SchemeKind::KeyboardMouse, 0,
                              ConflictPolicy::Reject);
    CY_CHECK_EQ(rejecting.offer(duplicate, profile), RebindStatus::Conflict);
    CY_CHECK_EQ(rejecting.conflict_action().value, 1);
    CY_CHECK_FALSE(rejecting.apply(profile).has_value());

    // Ask: the operation stays open and the interface decides. A policy that says "ask" and then
    // picks for the player is not asking.
    RebindOperation asking(ActionStableId{2}, SchemeKind::KeyboardMouse, 0, ConflictPolicy::Ask);
    CY_CHECK_EQ(asking.offer(duplicate, profile), RebindStatus::Conflict);
    CY_CHECK_FALSE(asking.apply(profile).has_value());

    // UnbindPrevious: the holder is left unbound and the new action takes the key.
    RebindOperation unbinding(ActionStableId{2}, SchemeKind::KeyboardMouse, 0,
                              ConflictPolicy::UnbindPrevious);
    CY_CHECK_EQ(unbinding.offer(duplicate, profile), RebindStatus::Conflict);
    CY_REQUIRE(unbinding.apply(profile).has_value());
    const BindingOverride* previous = profile.find(ActionStableId{1}, SchemeKind::KeyboardMouse, 0);
    CY_REQUIRE(previous != nullptr);
    CY_CHECK_FALSE(previous->control.is_valid());
    const BindingOverride* current = profile.find(ActionStableId{2}, SchemeKind::KeyboardMouse, 0);
    CY_REQUIRE(current != nullptr);
    CY_CHECK(current->control == key_control(Key::F));
}

CY_TEST_CASE("input: overrides are per scheme, so gamepad and keyboard are rebound apart") {
    BindingProfile profile(allocator());
    CY_REQUIRE(profile
                   .set(BindingOverride{ActionStableId{1}, SchemeKind::KeyboardMouse, 0,
                                        key_control(Key::F)})
                   .has_value());
    CY_REQUIRE(profile
                   .set(BindingOverride{ActionStableId{1}, SchemeKind::Gamepad, 0,
                                        gamepad_control(GamepadControl::West)})
                   .has_value());

    CY_CHECK_EQ(profile.count(), 2);
    // The same control on two schemes is not a conflict: they are different keys in the profile.
    CY_CHECK(profile.holder_of(key_control(Key::F), SchemeKind::Gamepad) == nullptr);
    CY_CHECK(profile.holder_of(key_control(Key::F), SchemeKind::KeyboardMouse) != nullptr);
}

CY_TEST_CASE("input: a composite element can be rebound on its own") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId move = fixture.declare("Move", 9, ActionValueType::Axis2);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(wasd(move, Key::A, Key::D, Key::S, Key::W)).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    // Component 2 is "down" — the 'S' of WASD. Rebound to the down arrow, with the other three
    // untouched, which is `input-and-actions`' "full remapping including composite elements".
    CY_REQUIRE(fixture.server.user(0)
                   .profile()
                   .set(BindingOverride{ActionStableId{9}, SchemeKind::KeyboardMouse, 2,
                                        key_control(Key::Down)})
                   .has_value());
    fixture.server.user(0).invalidate();

    fixture.press(keyboard, key_control(Key::Down), kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.y, -1.0F, 1e-5F);

    // And the key it replaced does nothing now.
    fixture.release(keyboard, key_control(Key::Down), 11 * kMs);
    fixture.press(keyboard, key_control(Key::S), 12 * kMs);
    fixture.server.resolve_tick(2, 20 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.y, 0.0F, 1e-5F);

    // The other three keys still work: the override addressed one component, not the binding.
    fixture.press(keyboard, key_control(Key::W), 21 * kMs);
    fixture.server.resolve_tick(3, 30 * kMs, 1.0F / 60.0F);
    CY_CHECK_NEAR(fixture.server.user(0).action_state(move).value.axis.y, 1.0F, 1e-5F);
}

CY_TEST_CASE("input: any action of a context is rebound at its own slot, not the first action's") {
    // Regression: an override's slot counted the bindings of every action before it in the scheme,
    // so a second action's primary binding was slot 4, not 0, and a rebinding menu's override for
    // it — stored at slot 0, as `BindingOverride` documents — never applied.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId jump = fixture.declare("Jump", 1, ActionValueType::Digital);
    const ActionId fire = fixture.declare("Fire", 2, ActionValueType::Digital);
    const DeviceId keyboard = fixture.attach(DeviceKind::Keyboard);
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad);
    MappingContext context(allocator());
    CY_REQUIRE(context.add(simple(jump, key_control(Key::Space))).has_value());
    CY_REQUIRE(context.add(simple(jump, gamepad_control(GamepadControl::South))).has_value());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::F))).has_value());
    CY_REQUIRE(context.add(simple(fire, key_control(Key::Enter))).has_value());
    CY_REQUIRE(context.add(simple(fire, gamepad_control(GamepadControl::West))).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    InputUser& user = fixture.server.user(0);
    CY_REQUIRE(user.profile()
                   .set(BindingOverride{ActionStableId{2}, SchemeKind::KeyboardMouse, 0,
                                        key_control(Key::G)})
                   .has_value());
    CY_REQUIRE(user.profile()
                   .set(BindingOverride{ActionStableId{2}, SchemeKind::Gamepad, 0,
                                        gamepad_control(GamepadControl::North)})
                   .has_value());
    user.invalidate();
    CY_REQUIRE(fixture.server.refresh_user(0).has_value());

    // The table in effect, as a menu reads it: Fire's primary key is G, its second still Enter.
    Control control;
    CY_REQUIRE(user.binding_control(fire, SchemeKind::KeyboardMouse, 0, control));
    CY_CHECK(control == key_control(Key::G));
    CY_REQUIRE(user.binding_control(fire, SchemeKind::KeyboardMouse, kMaxComponents, control));
    CY_CHECK(control == key_control(Key::Enter));
    CY_REQUIRE(user.binding_control(fire, SchemeKind::Gamepad, 0, control));
    CY_CHECK(control == gamepad_control(GamepadControl::North));
    CY_REQUIRE(user.binding_control(jump, SchemeKind::KeyboardMouse, 0, control));
    CY_CHECK(control == key_control(Key::Space));
    CY_CHECK_FALSE(user.binding_control(jump, SchemeKind::KeyboardMouse, kMaxComponents, control));

    // And who holds a control, authored bindings included.
    ActionId holder = kInvalidAction;
    cy::u8 slot = 0xFF;
    CY_REQUIRE(
        user.holder_of(key_control(Key::Enter), SchemeKind::KeyboardMouse, jump, holder, slot));
    CY_CHECK_EQ(holder, fire);
    CY_CHECK_EQ(slot, kMaxComponents);
    CY_CHECK_FALSE(
        user.holder_of(key_control(Key::F), SchemeKind::KeyboardMouse, jump, holder, slot));

    // The player's G fires; the authored F no longer does.
    fixture.press(keyboard, key_control(Key::G), 2 * kMs);
    fixture.press(pad, gamepad_control(GamepadControl::North), 3 * kMs);
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_CHECK(user.action_state(fire).pressed());
    CY_CHECK_FALSE(user.action_state(jump).pressed());
}

CY_TEST_CASE("input: a control held in another context is no conflict for a rebind") {
    // Regression: the conflict check searched every context on the stack, so rebinding a game's
    // Jump to a gamepad's north button "swapped" it with the interface's own north-button action —
    // two contexts binding one control is how a menu and a game share a gamepad, not a conflict.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId jump = fixture.declare("Jump", 1, ActionValueType::Digital);
    const ActionId inventory = fixture.declare("Inventory", 2, ActionValueType::Digital);
    const ActionId menu = fixture.declare("Menu", 3, ActionValueType::Digital);
    MappingContext game(allocator());
    CY_REQUIRE(game.add(simple(jump, gamepad_control(GamepadControl::South))).has_value());
    CY_REQUIRE(game.add(simple(inventory, gamepad_control(GamepadControl::West))).has_value());
    MappingContext interface(allocator());
    CY_REQUIRE(interface.add(simple(menu, gamepad_control(GamepadControl::North))).has_value());
    CY_REQUIRE(fixture.install(std::move(game)));
    auto registered = fixture.server.register_context(std::move(interface));
    CY_REQUIRE(registered.has_value());
    InputUser& user = fixture.server.user(0);
    CY_REQUIRE(user.push_context(*registered, 100, FocusLayer::Interface).has_value());
    CY_REQUIRE(fixture.server.refresh_user(0).has_value());

    ActionId holder = kInvalidAction;
    cy::u8 slot = 0xFF;
    CY_CHECK_FALSE(user.holder_of(gamepad_control(GamepadControl::North), SchemeKind::Gamepad, jump,
                                  holder, slot));
    // Within the game's own context the check still finds the holder.
    CY_REQUIRE(user.holder_of(gamepad_control(GamepadControl::West), SchemeKind::Gamepad, jump,
                              holder, slot));
    CY_CHECK_EQ(holder, inventory);
    CY_CHECK_EQ(slot, 0);
    // And the interface's control is found from the interface's action.
    CY_REQUIRE(user.holder_of(gamepad_control(GamepadControl::North), SchemeKind::Gamepad, menu,
                              holder, slot));
    CY_CHECK_EQ(holder, menu);
}
