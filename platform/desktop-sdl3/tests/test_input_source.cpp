// The SDL3 input event source, driven by real SDL events. Task 4.1.1.
//
// ================================================================================================
// WHY THIS TEST CAN EXIST AT ALL, AND WHY IT MATTERS THAT IT DOES
// ================================================================================================
//
// SDL's *event* subsystem needs no display, no window and no video driver. So this suite brings up
// `SDL_INIT_EVENTS` alone, pushes real `SDL_Event`s with `SDL_PushEvent`, and reads what the input
// server received — an end-to-end test of the platform boundary that runs headless, in CI, with no
// GPU and no X server.
//
// That is worth the file, because the boundary is where design.md §5's requirement is *lost* rather
// than where it is implemented. `input-and-actions` requires the platform layer to deliver
// timestamped events **without coalescing transitions**; information destroyed here cannot be
// recovered by any amount of care above. The last case pushes four transitions in one window and
// asserts that four arrive — the exact thing a backend that folded a press and its release into
// "the key is up" would fail.
//
// This file is under platform/, so it may include SDL. Nothing above platform/ may, and
// tools/layercheck/ fails the build over it.

#include <SDL3/SDL.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/platform/sdl3_input_source.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u64;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// SDL's event subsystem, brought up for the duration of one case and taken down after it.
///
/// Per case rather than once for the suite: `attach()` connects a keyboard and a mouse to the
/// server it is given, and a subsystem shared across cases would let one case's watch see another
/// case's events.
struct SdlEvents {
    SdlEvents() noexcept { initialised = SDL_InitSubSystem(SDL_INIT_EVENTS); }
    ~SdlEvents() {
        if (initialised) {
            SDL_QuitSubSystem(SDL_INIT_EVENTS);
        }
    }

    SdlEvents(const SdlEvents&) = delete;
    SdlEvents& operator=(const SdlEvents&) = delete;

    bool initialised = false;
};

/// Push a key transition with an explicit timestamp. SDL stamps an event whose timestamp is zero
/// with the current time, and the whole point of the assertions below is that the *given* time
/// survives — so it is never left at zero.
void push_key(SDL_Scancode scancode, bool down, u64 timestamp, bool repeat = false) noexcept {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = timestamp;
    event.key.scancode = scancode;
    event.key.down = down;
    event.key.repeat = repeat;
    (void)SDL_PushEvent(&event);
}

struct Fixture {
    Fixture() noexcept : server(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        cy::input::InputServerConfig config;
        config.event_capacity = 64;
        return server.configure(config).has_value() && server.initialize().has_value();
    }

    cy::input::InputServer server;
    cy::Sdl3InputSource source;
};

}  // namespace

CY_TEST_CASE("sdl3 input: attaching without the event subsystem is refused, not a crash") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    // No `SdlEvents` here: the subsystem is down. The refusal names the fix rather than the
    // symptom, because "input does nothing" is otherwise a very long afternoon.
    const cy::Status refused = fixture.source.attach(fixture.server);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unavailable);
    CY_CHECK_FALSE(fixture.source.attached());
}

CY_TEST_CASE("sdl3 input: attaching registers a keyboard and a mouse, unassigned") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());

    CY_CHECK(fixture.source.attached());
    CY_CHECK_FALSE(fixture.source.keyboard().is_null());
    CY_CHECK_FALSE(fixture.source.mouse().is_null());

    // ASSIGNED TO NOBODY. `input-and-actions` requires device assignment to be **explicit**, and a
    // platform layer that handed the keyboard to player one would be exactly the assumption the
    // requirement forbids. The caller decides.
    const cy::input::DeviceRecord* keyboard =
        fixture.server.devices().find(fixture.source.keyboard());
    CY_REQUIRE(keyboard != nullptr);
    CY_CHECK_EQ(keyboard->user, cy::input::kNoUser);
    CY_CHECK_EQ(keyboard->description.kind, cy::input::DeviceKind::Keyboard);

    fixture.source.detach();
    CY_CHECK_FALSE(fixture.source.attached());
}

CY_TEST_CASE("sdl3 input: a key transition arrives with its own timestamp and control") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());

    push_key(SDL_SCANCODE_W, true, 12345);
    CY_REQUIRE_EQ(fixture.server.pending().size(), 1);
    const cy::input::DeviceEvent& event = fixture.server.pending()[0];
    // The engine's own key numbering, never SDL's scancode: `input-and-actions` forbids gameplay
    // depending on platform key codes, and the way to make that structural is for the translation
    // to happen here and nowhere else.
    CY_CHECK(event.control == cy::input::key_control(cy::input::Key::W));
    CY_CHECK_EQ(event.value, 1.0F);
    // The time the platform observed it, not the time it was processed.
    CY_CHECK_EQ(event.timestamp, 12345);
    CY_CHECK_EQ(event.source, cy::input::EventSource::Physical);
    CY_CHECK(event.device == fixture.source.keyboard());

    fixture.source.detach();
}

CY_TEST_CASE("sdl3 input: a key repeat is not a transition and is not forwarded") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());

    push_key(SDL_SCANCODE_A, true, 100);
    const u64 forwarded = fixture.source.forwarded();
    const u64 ignored = fixture.source.ignored();

    // The operating system's auto-repeat. The key never came up, so forwarding this would report a
    // press the player did not make. Repetition is `TriggerKind::Pulse`'s job, at the game's rate.
    push_key(SDL_SCANCODE_A, true, 200, /*repeat=*/true);
    CY_CHECK_EQ(fixture.source.forwarded(), forwarded);
    CY_CHECK_EQ(fixture.source.ignored(), ignored + 1);
    CY_CHECK_EQ(fixture.server.pending().size(), 1);

    fixture.source.detach();
}

CY_TEST_CASE("sdl3 input: four transitions in one window arrive as four events") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());

    // THE PLATFORM HALF OF design.md §5. Two presses and two releases, all inside one accumulation
    // window, all on one key. A backend that coalesced transitions — folded these into "the key is
    // up" — would destroy the information here, and no amount of care in the action layer could
    // recover it. Four in, four out.
    push_key(SDL_SCANCODE_SPACE, true, 10);
    push_key(SDL_SCANCODE_SPACE, false, 20);
    push_key(SDL_SCANCODE_SPACE, true, 30);
    push_key(SDL_SCANCODE_SPACE, false, 40);

    CY_REQUIRE_EQ(fixture.server.pending().size(), 4);
    const float expected_values[] = {1.0F, 0.0F, 1.0F, 0.0F};
    const cy::Nanoseconds expected_times[] = {10, 20, 30, 40};
    for (u32 index = 0; index < 4; ++index) {
        CY_CHECK_EQ(fixture.server.pending()[index].value, expected_values[index]);
        CY_CHECK_EQ(fixture.server.pending()[index].timestamp, expected_times[index]);
        // And each carries a distinct sequence number, so `(timestamp, sequence)` is a total order
        // even when two events share an instant.
        CY_CHECK_EQ(fixture.server.pending()[index].sequence, index);
    }

    fixture.source.detach();
}

CY_TEST_CASE("sdl3 input: a mouse motion produces a delta and a position, separately") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());

    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = 777;
    event.motion.x = 100.0F;
    event.motion.y = 50.0F;
    event.motion.xrel = 4.0F;
    event.motion.yrel = -3.0F;
    (void)SDL_PushEvent(&event);

    // Four controls, because a displacement and a position are different *interpretations* — one
    // must never be scaled by frame time and the other is a position. See `Interpretation`.
    CY_REQUIRE_EQ(fixture.server.pending().size(), 4);
    CY_CHECK(fixture.server.pending()[0].control ==
             cy::input::mouse_control(cy::input::MouseControl::MoveX));
    CY_CHECK_EQ(fixture.server.pending()[0].value, 4.0F);
    // SDL's Y grows downward; the engine's does not, so the sign is flipped here and nowhere else.
    CY_CHECK(fixture.server.pending()[1].control ==
             cy::input::mouse_control(cy::input::MouseControl::MoveY));
    CY_CHECK_EQ(fixture.server.pending()[1].value, 3.0F);
    CY_CHECK(fixture.server.pending()[2].control ==
             cy::input::mouse_control(cy::input::MouseControl::PositionX));
    CY_CHECK_EQ(fixture.server.pending()[2].value, 100.0F);

    fixture.source.detach();
}

CY_TEST_CASE("sdl3 input: detaching stops the watch") {
    const SdlEvents sdl;
    CY_REQUIRE(sdl.initialised);
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.source.attach(fixture.server).has_value());
    push_key(SDL_SCANCODE_Q, true, 1);
    CY_REQUIRE_EQ(fixture.server.pending().size(), 1);

    fixture.source.detach();
    push_key(SDL_SCANCODE_Q, false, 2);
    // Nothing more arrived: a source that kept forwarding after detachment would be a dangling
    // callback into an `InputServer` the caller believes it has finished with.
    CY_CHECK_EQ(fixture.server.pending().size(), 1);
}
