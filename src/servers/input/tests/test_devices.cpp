// TASK 4.1.1 — input users, device ownership and the device lifecycle.
//
// Every case here is one of `input-and-actions`' scenarios under "Input users and device ownership"
// and "Device lifecycle". The two worth naming, because they are the ones a device-indexed design
// cannot pass at all:
//
//   * "Identity is not the device" — a user with no devices is still a user, and its gameplay
//     participant is unaffected;
//   * "Reconnection restores the pairing" — a controller that ran out of battery comes back to the
//     same player, rather than shuffling everybody up one index.

#include "fixture.h"

using namespace cy::input_test;
using cy::u32;

namespace {

[[nodiscard]] u32 count_lifecycle(const cy::Array<DeviceLifecycleEvent>& events,
                                  DeviceLifecycle kind) noexcept {
    u32 found = 0;
    for (const auto& event : events) {
        found += event.kind == kind ? 1U : 0U;
    }
    return found;
}

}  // namespace

CY_TEST_CASE("input: the server initialises with no devices at all") {
    // `input-and-actions` — "A server has no input devices": absence of a device backend SHALL NOT
    // prevent initialisation. Nothing below connects anything.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_CHECK_EQ(fixture.server.devices().count(), 0);
    CY_CHECK_EQ(fixture.server.user_count(), 1);
    fixture.server.resolve_tick(1, kMs, 1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.last_event_count(), 0);
}

CY_TEST_CASE("input: two players on one machine are two users with their own devices") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    const DeviceId first = fixture.attach(DeviceKind::Gamepad, 0, "pad-a");
    const DeviceId second = fixture.attach(DeviceKind::Gamepad, 1, "pad-b");
    CY_REQUIRE(!first.is_null());
    CY_REQUIRE(!second.is_null());

    CY_CHECK_EQ(fixture.server.user(0).device_count(), 1);
    CY_CHECK_EQ(fixture.server.user(1).device_count(), 1);
    CY_CHECK(fixture.server.user(0).device(0) == first);
    CY_CHECK(fixture.server.user(1).device(0) == second);
}

CY_TEST_CASE("input: a user survives losing its device") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad, 1, "pad-b");
    CY_REQUIRE(!pad.is_null());

    fixture.server.devices().disconnect(pad, 5 * kMs);
    fixture.server.pump_device_lifecycle();

    // The user is still user 1. It has no device, which is a user awaiting one — not a lost player,
    // and emphatically not a renumbering of every user above it.
    CY_CHECK_EQ(fixture.server.user_count(), 2);
    CY_CHECK_EQ(fixture.server.user(1).device_count(), 0);
    CY_CHECK_EQ(fixture.server.user(1).id(), 1);
}

CY_TEST_CASE("input: a reconnecting controller returns to the same user") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad, 1, "pad-b");
    CY_REQUIRE(!pad.is_null());
    fixture.server.devices().disconnect(pad, 5 * kMs);
    fixture.server.pump_device_lifecycle();

    // The same hardware identifier comes back. Under the default `HoldAndAwait` policy the pairing
    // is restored, and the stale `DeviceId` from before is *not* the new one.
    DeviceDescription description;
    description.kind = DeviceKind::Gamepad;
    description.hardware_id = cy::Name::intern("pad-b");
    auto reconnected = fixture.server.devices().connect(description, 9 * kMs);
    CY_REQUIRE(reconnected.has_value());
    CY_CHECK_NE(reconnected->bits(), pad.bits());
    CY_CHECK_EQ(
        count_lifecycle(fixture.server.devices().lifecycle_events(), DeviceLifecycle::Reconnected),
        1);

    fixture.server.pump_device_lifecycle();
    CY_CHECK_EQ(fixture.server.user(1).device_count(), 1);
    CY_CHECK(fixture.server.user(1).device(0) == *reconnected);
}

CY_TEST_CASE("input: a stale device id resolves to nothing after a disconnection") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad, 0, "pad-a");
    CY_REQUIRE(fixture.server.devices().find(pad) != nullptr);
    fixture.server.devices().disconnect(pad, 5 * kMs);
    // The generation moved. An event still carrying the old id cannot be attributed to whatever
    // takes the slot next — which is the whole reason `DeviceId` is generational.
    CY_CHECK(fixture.server.devices().find(pad) == nullptr);
}

CY_TEST_CASE("input: the disconnect policy is declared, and ReassignToAnother is not the default") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_CHECK_EQ(fixture.server.devices().disconnect_policy(), DisconnectPolicy::HoldAndAwait);

    const DeviceId held = fixture.attach(DeviceKind::Gamepad, 0, "pad-a");
    DeviceDescription spare;
    spare.kind = DeviceKind::Gamepad;
    spare.hardware_id = cy::Name::intern("pad-spare");
    auto unassigned = fixture.server.devices().connect(spare, 0);
    CY_REQUIRE(unassigned.has_value());

    fixture.server.devices().set_disconnect_policy(DisconnectPolicy::ReassignToAnother);
    fixture.server.devices().disconnect(held, 5 * kMs);
    fixture.server.pump_device_lifecycle();

    // Under the declared policy the spare is handed over. Under the default it would not have been,
    // and the point of the requirement is that which of the two happens is written down.
    CY_CHECK_EQ(fixture.server.user(0).device_count(), 1);
    CY_CHECK(fixture.server.user(0).device(0) == *unassigned);
}

CY_TEST_CASE("input: keyboard and mouse ownership follows the declared policy") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    DeviceDescription keyboard;
    keyboard.kind = DeviceKind::Keyboard;
    keyboard.hardware_id = cy::Name::intern("kbd");
    auto connected = fixture.server.devices().connect(keyboard, 0);
    CY_REQUIRE(connected.has_value());

    bool shared = false;
    // Exclusive, and the user is *stated* rather than assumed to be player one.
    fixture.server.devices().set_keyboard_mouse_policy(KeyboardMousePolicy::Exclusive, 1, 1);
    CY_CHECK_EQ(fixture.server.devices().route(*connected, shared), 1);
    CY_CHECK_FALSE(shared);

    // Shared routes to every user, which is not a user index — `route` says so by returning
    // `kNoUser` with the flag set rather than by returning "0 and also everyone else".
    fixture.server.devices().set_keyboard_mouse_policy(KeyboardMousePolicy::Shared, 0, 0);
    CY_CHECK_EQ(fixture.server.devices().route(*connected, shared), kNoUser);
    CY_CHECK(shared);
}

CY_TEST_CASE("input: a capability change and a low battery are surfaced as events") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad, 0, "pad-a");
    fixture.server.devices().clear_lifecycle_events();

    CY_REQUIRE(
        fixture.server.devices()
            .set_capabilities(pad, DeviceCapability::Rumble | DeviceCapability::Battery, 10 * kMs)
            .has_value());
    CY_REQUIRE(fixture.server.devices().set_battery(pad, 0.5F, 11 * kMs).has_value());
    CY_REQUIRE(fixture.server.devices().set_battery(pad, 0.05F, 12 * kMs).has_value());
    // Still low on the next report, and it does not fire again: the event is the crossing, not the
    // state, so the interface does not have to de-duplicate it.
    CY_REQUIRE(fixture.server.devices().set_battery(pad, 0.04F, 13 * kMs).has_value());

    const cy::Array<DeviceLifecycleEvent>& events = fixture.server.devices().lifecycle_events();
    CY_CHECK_EQ(count_lifecycle(events, DeviceLifecycle::CapabilityChanged), 1);
    CY_CHECK_EQ(count_lifecycle(events, DeviceLifecycle::BatteryLow), 1);
}

CY_TEST_CASE("input: a device cannot belong to two users") {
    Fixture fixture;
    CY_REQUIRE(fixture.start(2));
    const DeviceId pad = fixture.attach(DeviceKind::Gamepad, 0, "pad-a");
    CY_REQUIRE(!pad.is_null());
    // Sharing one physical device is `KeyboardMousePolicy::Shared`, answered at routing. Two owners
    // of one record would make "whose device is this" have two answers.
    CY_CHECK_FALSE(fixture.server.assign(pad, 1, kMs).has_value());
}

CY_TEST_CASE("input: synthetic injection is marked, and a shipping build can refuse it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const ActionId fire = fixture.declare("Fire", 1, ActionValueType::Digital);
    auto virtual_device = fixture.server.create_virtual_device(cy::Name::intern("test"), 0);
    CY_REQUIRE(virtual_device.has_value());
    CY_REQUIRE(fixture.server.assign(*virtual_device, 0, 0).has_value());

    MappingContext context(allocator());
    // A virtual device's controls are the class it is standing in for; the binding names the
    // control, and the *event* carries the device.
    Binding binding = simple(fire, Control{DeviceKind::Virtual, 1});
    CY_REQUIRE(context.add(binding).has_value());
    CY_REQUIRE(fixture.install(std::move(context)));

    CY_REQUIRE(fixture.server
                   .inject(*virtual_device, Control{DeviceKind::Virtual, 1}, 1.0F, 5 * kMs,
                           EventSource::Synthetic)
                   .has_value());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    const ActionState& state = fixture.server.user(0).action_state(fire);
    CY_CHECK(state.pressed());
    // Marked, so a diagnostic and an anti-cheat policy can tell it from a physical press.
    CY_CHECK(has_flag(state.flags, ActionFlag::Synthetic));

    // "Physical" is not something injection may claim.
    CY_CHECK_FALSE(fixture.server
                       .inject(*virtual_device, Control{DeviceKind::Virtual, 1}, 1.0F, 6 * kMs,
                               EventSource::Physical)
                       .has_value());
}

CY_TEST_CASE("input: a build with synthetic input disabled refuses injection loudly") {
    Fixture fixture;
    InputServerConfig config;
    config.allow_synthetic = false;
    CY_REQUIRE(fixture.server.configure(config).has_value());
    CY_REQUIRE(fixture.server.initialize().has_value());
    auto virtual_device = fixture.server.create_virtual_device(cy::Name::intern("test"), 0);
    CY_REQUIRE(virtual_device.has_value());

    const cy::Status refused = fixture.server.inject(
        *virtual_device, Control{DeviceKind::Virtual, 1}, 1.0F, 5 * kMs, EventSource::Synthetic);
    CY_REQUIRE_FALSE(refused.has_value());
    // An error rather than a silent drop: an automation harness in a shipping build must fail, not
    // report green over a game nothing was driving.
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::PermissionDenied);
}

CY_TEST_CASE("input: text is a separate stream from key actions") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    TextEvent event;
    event.timestamp = kMs;
    event.user = 0;
    event.text[0] = 'a';
    event.length = 1;
    fixture.server.submit_text(event);

    // `input-and-actions` — "Text entry is separate": composed text arrives on its own stream and
    // is never reconstructed from key presses. The two streams do not share a queue, which is what
    // makes that structural.
    CY_CHECK_EQ(fixture.server.text().size(), 1);
    CY_CHECK_EQ(fixture.server.pending().size(), 0);
}
