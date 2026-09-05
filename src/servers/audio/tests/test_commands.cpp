// The lock-free command queue. Task 4.3.5.
//
// `audio` — "Thread-safe state handoff": "commands SHALL be enqueued to a lock-free queue consumed
// at the start of each callback". The cases here are the ring's behaviour at its edges — full,
// empty, and wrapped — because those are where a ring buffer is wrong, and because a queue that
// silently dropped a `Stop` would leave a sound playing forever with nothing to point at.

#include <cy/servers/audio/commands.h>
#include <cy/test/test.h>

#include <type_traits>

using namespace cy::audio;

namespace {

[[nodiscard]] AudioCommand play_of(cy::u32 slot) {
    AudioCommand command;
    command.kind = CommandKind::Play;
    command.voice = VoiceHandle::from_slot(slot, 1);
    return command;
}

}  // namespace

CY_TEST_CASE("a command is a POD, because it crosses a thread boundary in a ring buffer") {
    static_assert(std::is_trivially_copyable_v<AudioCommand>);
    static_assert(std::is_trivially_destructible_v<AudioCommand>);
    CY_CHECK(true);
}

CY_TEST_CASE("an empty queue pops nothing") {
    CommandQueue<8> queue;
    AudioCommand command;
    CY_CHECK_FALSE(queue.pop(command));
    CY_CHECK_EQ(queue.approximate_size(), 0U);
}

CY_TEST_CASE("commands come out in the order they went in") {
    CommandQueue<8> queue;
    for (cy::u32 i = 0; i < 5; ++i) {
        CY_CHECK(queue.push(play_of(i)));
    }
    CY_CHECK_EQ(queue.approximate_size(), 5U);
    for (cy::u32 i = 0; i < 5; ++i) {
        AudioCommand command;
        CY_REQUIRE(queue.pop(command));
        CY_CHECK_EQ(command.voice.index(), i);
    }
    AudioCommand command;
    CY_CHECK_FALSE(queue.pop(command));
}

CY_TEST_CASE("a full queue refuses rather than overwriting") {
    // Reported, never silent: a dropped `Stop` leaves a sound playing forever, and the caller
    // counts the refusal into the diagnostics.
    CommandQueue<4> queue;
    CY_CHECK_EQ(CommandQueue<4>::capacity(), 3U);
    for (cy::u32 i = 0; i < 3; ++i) {
        CY_CHECK(queue.push(play_of(i)));
    }
    CY_CHECK_FALSE(queue.push(play_of(99)));

    // And the three that were taken are intact — a full queue does not corrupt the ones it holds.
    for (cy::u32 i = 0; i < 3; ++i) {
        AudioCommand command;
        CY_REQUIRE(queue.pop(command));
        CY_CHECK_EQ(command.voice.index(), i);
    }
}

CY_TEST_CASE("the ring wraps without losing or duplicating a command") {
    CommandQueue<4> queue;
    cy::u32 pushed = 0;
    cy::u32 popped = 0;
    // Ten laps of a three-deep ring: every wrap of the index is exercised.
    for (cy::u32 round = 0; round < 10; ++round) {
        while (queue.push(play_of(pushed))) {
            ++pushed;
        }
        AudioCommand command;
        while (queue.pop(command)) {
            CY_CHECK_EQ(command.voice.index(), popped);
            ++popped;
        }
    }
    CY_CHECK_EQ(pushed, popped);
    CY_CHECK_EQ(pushed, 30U);
}

CY_TEST_CASE("a queue interleaving pushes and pops keeps its order") {
    CommandQueue<8> queue;
    CY_CHECK(queue.push(play_of(0)));
    CY_CHECK(queue.push(play_of(1)));

    AudioCommand command;
    CY_REQUIRE(queue.pop(command));
    CY_CHECK_EQ(command.voice.index(), 0U);

    CY_CHECK(queue.push(play_of(2)));
    CY_REQUIRE(queue.pop(command));
    CY_CHECK_EQ(command.voice.index(), 1U);
    CY_REQUIRE(queue.pop(command));
    CY_CHECK_EQ(command.voice.index(), 2U);
}
