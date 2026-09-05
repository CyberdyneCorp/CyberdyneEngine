// TASK 4.1.4 — fixed-tick sampling and buffering. The milestone's subtle requirement.
//
// ================================================================================================
// READ THIS BEFORE CHANGING ANYTHING IN user.cpp's RESOLUTION
// ================================================================================================
//
// design.md §5: "A button pressed and released **between** two ticks must still be observable as
// both a press and a release by the tick that follows. This is the requirement most likely to be
// implemented as 'sample the current state each tick', which works in every manual test and loses
// inputs precisely when the frame rate is uneven — which is when players notice."
//
// The first case below is that test, and it was written before the resolution was. The reason it is
// worth a file of its own is that the *wrong* implementation passes every other case in this
// module: hold a key and read it, press a key and read it next tick, drive an axis — all identical
// under both implementations. Only a transition pair inside one window separates them, and only if
// the test never reads the device between the two events.
//
// `THE NEGATIVE CONTROL` at the bottom is the other half. It asserts, on the same events, that the
// level-sampling answer really is different — so a future change that quietly made both
// implementations agree would be caught rather than making this file's green meaningless.

#include "fixture.h"

using namespace cy::input_test;
using cy::f32;
using cy::u32;

namespace {

/// One digital action bound to one key, one user, one keyboard. The smallest arrangement in which
/// the question can be asked at all.
struct TickFixture : Fixture {
    [[nodiscard]] bool build() noexcept {
        if (!start()) {
            return false;
        }
        fire = declare("Fire", 1, ActionValueType::Digital, true);
        jump = declare("Jump", 2, ActionValueType::Digital, true, 0.15F);
        move = declare("Move", 3, ActionValueType::Axis2, true);
        keyboard = attach(DeviceKind::Keyboard);
        if (keyboard.is_null() || fire == kInvalidAction) {
            return false;
        }
        MappingContext context(allocator());
        context.set_name(cy::Name::intern("gameplay"));
        if (!context.add(simple(fire, key_control(Key::Space))).has_value()) {
            return false;
        }
        if (!context.add(simple(jump, key_control(Key::Enter), TriggerKind::Pressed)).has_value()) {
            return false;
        }
        if (!context.add(wasd(move, Key::A, Key::D, Key::S, Key::W)).has_value()) {
            return false;
        }
        return install(std::move(context));
    }

    ActionId fire = kInvalidAction;
    ActionId jump = kInvalidAction;
    ActionId move = kInvalidAction;
    DeviceId keyboard;
};

}  // namespace

CY_TEST_CASE("input: a press and a release between two ticks are both observed by the next tick") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());

    // Tick 1 establishes the resting state: nothing pressed, nothing transitioned.
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    CY_REQUIRE_FALSE(fixture.server.user(0).action_state(fixture.fire).pressed());

    // THE EVENTS THAT MATTER. Both happen strictly between tick 1 and tick 2, and the key is back
    // up before the tick runs. A resolver that read the device's current level at tick 2 would see
    // "not pressed" and report nothing at all.
    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);
    fixture.release(fixture.keyboard, key_control(Key::Space), 14 * kMs);

    fixture.server.resolve_tick(2, 26 * kMs, 1.0F / 60.0F);

    const ActionState& state = fixture.server.user(0).action_state(fixture.fire);
    CY_CHECK(state.just_pressed());
    CY_CHECK(state.just_released());
    CY_CHECK_EQ(state.press_count, 1);
    CY_CHECK_EQ(state.release_count, 1);
    // And the level is where the events left it, which is *up*. Both facts are true at once, which
    // is the whole point: the transitions are counted, the level is sampled.
    CY_CHECK_FALSE(state.pressed());
}

CY_TEST_CASE("input: several presses inside one window are all counted") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    // An uneven frame — one long frame containing three taps. This is what a hitch looks like from
    // the input system's side, and it is exactly when a level-sampling resolver loses input.
    for (int index = 0; index < 3; ++index) {
        const cy::Nanoseconds base = (12 + (index * 4)) * kMs;
        fixture.press(fixture.keyboard, key_control(Key::Space), base);
        fixture.release(fixture.keyboard, key_control(Key::Space), base + kMs);
    }
    fixture.server.resolve_tick(2, 40 * kMs, 1.0F / 60.0F);

    const ActionState& state = fixture.server.user(0).action_state(fixture.fire);
    CY_CHECK_EQ(state.press_count, 3);
    CY_CHECK_EQ(state.release_count, 3);
    CY_CHECK_FALSE(state.pressed());
}

CY_TEST_CASE("input: events are resolved in timestamp order, not arrival order") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    // Two backends, or a remote source, can deliver out of order. The resolution sorts by
    // `(timestamp, sequence)` — see EventBuffer — so the release cannot be applied before the press
    // and leave the action stuck down.
    fixture.release(fixture.keyboard, key_control(Key::Space), 18 * kMs);
    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);

    fixture.server.resolve_tick(2, 26 * kMs, 1.0F / 60.0F);
    const ActionState& state = fixture.server.user(0).action_state(fixture.fire);
    CY_CHECK_EQ(state.press_count, 1);
    CY_CHECK_EQ(state.release_count, 1);
    CY_CHECK_FALSE(state.pressed());
}

CY_TEST_CASE("input: a tick with no events reports no transitions and keeps the level") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);
    fixture.server.resolve_tick(1, 16 * kMs, 1.0F / 60.0F);
    CY_REQUIRE(fixture.server.user(0).action_state(fixture.fire).just_pressed());

    // Ten ticks later the key is still down. The level survives; the edge does not repeat. This is
    // `input-and-actions`' "An axis does not spam" for a digital control.
    for (cy::u64 tick = 2; tick <= 11; ++tick) {
        fixture.server.resolve_tick(tick, static_cast<cy::Nanoseconds>(tick) * 16 * kMs,
                                    1.0F / 60.0F);
        const ActionState& state = fixture.server.user(0).action_state(fixture.fire);
        CY_CHECK(state.pressed());
        CY_CHECK_FALSE(state.just_pressed());
        CY_CHECK_EQ(state.press_count, 0);
    }
}

CY_TEST_CASE("input: one command frame per tick carries the button edges and the axes") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);
    fixture.release(fixture.keyboard, key_control(Key::Space), 14 * kMs);
    fixture.press(fixture.keyboard, key_control(Key::D), 15 * kMs);
    fixture.server.resolve_tick(2, 26 * kMs, 1.0F / 60.0F);

    const CommandFrame& frame = fixture.server.user(0).command_frame();
    CY_CHECK_EQ(frame.tick, 2);
    CY_CHECK_EQ(frame.user, 0);
    // BOTH EDGE MASKS, ON ONE FRAME, FOR A BUTTON THAT IS NOT DOWN. A frame carrying only the level
    // could not express this, and a replay reconstructed from level-only frames would lose exactly
    // the inputs that were hardest to make. See frame.h.
    CY_CHECK_NE(frame.just_pressed & 1U, 0U);
    CY_CHECK_NE(frame.just_released & 1U, 0U);
    CY_CHECK_EQ(frame.pressed & 1U, 0U);
    CY_CHECK_NEAR(frame.axes[0].x, 1.0F, 1e-5F);

    // The hash is a value identity: the same intent twice is the same number, and a different
    // intent is a different one.
    CommandFrame copy = frame;
    CY_CHECK_EQ(copy.hash(), frame.hash());
    copy.just_pressed = 0;
    CY_CHECK_NE(copy.hash(), frame.hash());
}

CY_TEST_CASE("input: a buffered action survives its window and is consumed exactly once") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    // `Jump` declares a 150 ms buffer. The press lands at 12 ms; the game does not "land" until
    // 100 ms later, which is the grace period `input-and-actions` provides the mechanism for.
    fixture.press(fixture.keyboard, key_control(Key::Enter), 12 * kMs);
    fixture.server.resolve_tick(2, 16 * kMs, 1.0F / 60.0F);
    CY_REQUIRE(fixture.server.user(0).action_state(fixture.jump).triggered());

    CY_CHECK(fixture.server.user(0).consume_buffered(fixture.jump, 100 * kMs));
    // Exactly once. Two systems both "checking" would otherwise both get the jump.
    CY_CHECK_FALSE(fixture.server.user(0).consume_buffered(fixture.jump, 101 * kMs));
}

CY_TEST_CASE("input: a buffered action expires outside its window") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);
    fixture.press(fixture.keyboard, key_control(Key::Enter), 12 * kMs);
    fixture.server.resolve_tick(2, 16 * kMs, 1.0F / 60.0F);

    // 400 ms later, well past the declared 150 ms. A buffer that never expired would be a jump the
    // player asked for half a second ago arriving as a surprise.
    CY_CHECK_FALSE(fixture.server.user(0).consume_buffered(fixture.jump, 400 * kMs));
}

CY_TEST_CASE("input: an action with no buffer window declared buffers nothing") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);
    fixture.server.resolve_tick(1, 16 * kMs, 1.0F / 60.0F);
    CY_REQUIRE(fixture.server.user(0).action_state(fixture.fire).triggered());
    CY_CHECK_FALSE(fixture.server.user(0).consume_buffered(fixture.fire, 17 * kMs));
}

// ================================================================================================
// THE NEGATIVE CONTROL
// ================================================================================================
//
// The case at the top of this file is only meaningful if the implementation it rules out would
// really have failed it. This case computes what "sample the current state each tick" would have
// answered, from the same events, and asserts that the two answers differ.
//
// Without it, a future refactor that made the accumulating path degenerate into level sampling
// would leave every assertion above still passing on a resolver that had lost the property.

CY_TEST_CASE("input: the level-sampling answer really is different, on these very events") {
    TickFixture fixture;
    CY_REQUIRE(fixture.build());
    fixture.server.resolve_tick(1, 10 * kMs, 1.0F / 60.0F);

    fixture.press(fixture.keyboard, key_control(Key::Space), 12 * kMs);
    fixture.release(fixture.keyboard, key_control(Key::Space), 14 * kMs);
    fixture.server.resolve_tick(2, 26 * kMs, 1.0F / 60.0F);

    // What a level-sampling resolver would have read at tick 2: the device's control state after
    // every event was applied. It is zero, because the key came back up.
    const DeviceRecord* record = fixture.server.devices().find(fixture.keyboard);
    CY_REQUIRE(record != nullptr);
    const f32 level_at_tick = record->controls[static_cast<cy::u16>(Key::Space)];
    CY_CHECK_EQ(level_at_tick, 0.0F);

    // And what this resolver reports. The two disagree, which is the property under test: had they
    // agreed, the case at the top of the file would have been vacuous.
    const ActionState& state = fixture.server.user(0).action_state(fixture.fire);
    CY_CHECK_EQ(state.press_count, 1);
    CY_CHECK_NE(static_cast<f32>(state.press_count), level_at_tick);
}
