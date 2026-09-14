// The three play modes, driven. M11.b tasks 0.5 and 3.1, and the `play-mode-round-trip` criterion.
//
// ================================================================================================
// WHAT THIS SUITE IS FOR, AND WHAT IT WOULD HAVE TO LOOK LIKE TO PASS VACUOUSLY
// ================================================================================================
//
// `editor-architecture` and `live-editing` both name three play modes, and until M11.b no code in
// this tree named any of them: `grep -rniI 'SeparateProcess|RemoteDevice|InEditor' src/ editor/
// tools/` returned nothing across five milestones of work built on top of those two rows. The
// obvious way to close that grep is to write the three words down, and the obvious way to close a
// "the modes work" criterion is to drive one of them three times.
//
// So this suite is written against both temptations:
//
//   * **The same command stream drives all three.** One world, one sequence of calls, three modes,
//     and the SIMULATED RESULT is compared between them. If a mode were a second world model — the
//     spike's refutation — the three results would differ, and if a mode were quietly the same mode
//     the comparison would pass for the wrong reason, which is why the refusal below is checked
//     too.
//
//   * **The refusal is exercised in both directions.** `RemoteDevice` is unavailable on this build
//     because nothing encodes a frame, and the case asks for it and watches it refuse BY NAME with
//     no session started. It then flips the one support bit that makes it available and watches the
//     same mode run. A refusal that could not be made to stop refusing would be a constant wearing
//     a check's clothes.
//
//   * **A capability is queried rather than tried.** `RemoteDevice` declares `step_frame == false`,
//     and the case checks that the query says so AND that the call refuses — two facts, because a
//     declaration nothing enforces is a comment.

#include <cy/gameplay/play/mode.h>
#include <cy/gameplay/play/session.h>
#include <cy/test/test.h>

#include "editor_play_fixture.h"

#include <cstring>
#include <string>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::gameplay;
using namespace cy::test_editor;

namespace {

/// The support a mode needs in order to be available. `RemoteDevice` is the only one that needs
/// anything this build does not have, and this is where the case says which bits those are rather
/// than asserting the answer.
[[nodiscard]] PlayModeSupport support_for(PlayMode mode) noexcept {
    PlayModeSupport support = play_mode_support();
    if (mode == PlayMode::RemoteDevice) {
        // The two facts `mode.cpp` refuses on. Flipped here so that the SAME code path that refuses
        // on this build is the one that runs when they are true: a test that reached past
        // `availability_of` to force the mode would prove nothing about the refusal.
        support.frame_encoder = true;
        support.remote_runtime = true;
    }
    return support;
}

/// One world, driven through one command stream, in one mode. Returns the sphere's height after the
/// stream — the simulated result the three modes are compared on.
struct Driven {
    bool entered = false;
    bool stepped_tick = false;
    bool stepped_frame = false;
    bool step_frame_refused = false;
    u64 ticks = 0;
    u64 stepped_ticks = 0;
    f32 height = 0.0F;
    bool restored_exactly = false;
};

/// THE COMMAND STREAM. Identical for all three modes, by construction: the mode is an argument and
/// nothing else in this function reads it.
///
/// The frame step is attempted AFTER the compared state is captured, and deliberately. It is the
/// one capability that differs between the modes, so folding it into the compared prefix would make
/// the three modes run different numbers of ticks and the comparison would be measuring the
/// capability difference rather than the world model. The attempt is still made and still recorded
/// — it is the refusal the next case is about — it simply happens where it cannot contaminate the
/// comparison.
[[nodiscard]] Driven drive(PlayMode mode, cy::physics::PhysicsServer* server) {
    Driven result;
    Authored authored;
    if (!authored.started) {
        return result;
    }

    PlayConfiguration configuration = configuration_over(server, authored.schema);
    configuration.mode = mode;
    configuration.support = support_for(mode);

    PlaySession session(allocator(), authored.world);
    if (!session.enter(configuration)) {
        return result;
    }
    result.entered = true;

    for (u32 tick = 0; tick < 20; ++tick) {
        if (!session.tick()) {
            return result;
        }
    }
    if (!session.pause()) {
        return result;
    }
    result.stepped_tick = session.step_tick().has_value();
    if (!session.resume()) {
        return result;
    }
    for (u32 tick = 0; tick < 9; ++tick) {
        if (!session.tick()) {
            return result;
        }
    }

    // The compared state: thirty ticks of simulation in every mode, and the sphere's height.
    result.ticks = session.report().ticks;
    result.stepped_ticks = session.report().stepped_ticks;
    cy::Transform placement;
    if (ser::transform_of(authored.world, authored.world.nodes()[1], placement)) {
        result.height = placement.translation.y;
    }

    if (!session.pause()) {
        return result;
    }
    const cy::Status framed = session.step_frame();
    result.stepped_frame = framed.has_value();
    result.step_frame_refused = !framed.has_value();

    if (!session.stop()) {
        return result;
    }
    result.restored_exactly = session.report().restored_exactly;
    return result;
}

}  // namespace

CY_TEST_CASE("a world plays in each of the three modes, and the same command stream drives them") {
    Reference physics;
    CY_REQUIRE(physics.ready());

    const Driven in_editor = drive(PlayMode::InEditor, physics.server());
    const Driven separate = drive(PlayMode::SeparateProcess, physics.server());
    const Driven remote = drive(PlayMode::RemoteDevice, physics.server());

    CY_REQUIRE(in_editor.entered);
    CY_REQUIRE(separate.entered);
    CY_REQUIRE(remote.entered);

    // ONE WORLD MODEL, THREE TRANSPORTS — the spike's question, answered in the tree rather than in
    // a design document. The same authored world, driven by the same calls, simulates to the same
    // place in all three modes. A second world model could not produce this by accident.
    CY_CHECK_EQ(in_editor.ticks, separate.ticks);
    CY_CHECK_EQ(in_editor.ticks, remote.ticks);
    CY_CHECK_NEAR(separate.height, in_editor.height, 0.000001);
    CY_CHECK_NEAR(remote.height, in_editor.height, 0.000001);
    // And it is a world that actually moved, so the comparison is over a simulation rather than
    // over three copies of the initial placement.
    CY_CHECK_LT(in_editor.height, 4.0F);

    // Entering and leaving play leaves the authoring document exactly as it was, in every mode.
    CY_CHECK(in_editor.restored_exactly);
    CY_CHECK(separate.restored_exactly);
    CY_CHECK(remote.restored_exactly);

    // STEPPING, AND THE ONE CAPABILITY THAT DIFFERS BY MODE. Both local modes step a frame; the
    // remote one declares it cannot, and refuses rather than doing something else.
    CY_CHECK(in_editor.stepped_tick);
    CY_CHECK(separate.stepped_tick);
    CY_CHECK(remote.stepped_tick);
    CY_CHECK(in_editor.stepped_frame);
    CY_CHECK(separate.stepped_frame);
    CY_CHECK(remote.step_frame_refused);

    // A stepped tick is counted as a step, which is how a reader of a report tells a stepped
    // session from a running one. One tick step each, at the point the comparison is taken.
    CY_CHECK_EQ(in_editor.stepped_ticks, 1U);
    CY_CHECK_EQ(separate.stepped_ticks, 1U);
    CY_CHECK_EQ(remote.stepped_ticks, 1U);
    CY_CHECK_EQ(in_editor.ticks, 30U);
}

CY_TEST_CASE("a play mode that is not available refuses by name and starts nothing") {
    Reference physics;
    CY_REQUIRE(physics.ready());
    Authored authored;
    CY_REQUIRE(authored.started);

    // The support this build actually has. `RemoteDevice` is unavailable here because nothing in
    // the tree encodes a frame — `EncodedStream` is declared on both sides of the viewport
    // transport and implemented on neither.
    const PlayModeSupport real = play_mode_support();
    CY_REQUIRE_FALSE(real.frame_encoder);

    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.mode = PlayMode::RemoteDevice;
    configuration.support = real;

    const std::string before = bytes_of(authored.world);
    PlaySession session(allocator(), authored.world);
    const cy::Status entered = session.enter(configuration);
    CY_REQUIRE_FALSE(entered.has_value());

    // IT REFUSES BY NAME. The message names the mode, so a person reading a log knows which request
    // was refused and why — and it names the missing part rather than saying "unsupported".
    const std::string reason = entered.error().message;
    CY_CHECK(reason.find("remote-device") != std::string::npos);
    CY_CHECK(reason.find("encoder") != std::string::npos);

    // AND IT STARTS NOTHING. This is the half that catches a silent fallback: a request that
    // "refused" and left a session running in another mode would pass the message check above.
    CY_CHECK(session.state() == PlayState::Editing);
    CY_CHECK(session.world() == nullptr);
    CY_CHECK_EQ(bytes_of(authored.world), before);
}

CY_TEST_CASE("every play mode the engine claims is driveable or declared absent with a rung") {
    // `specs/live-editing/` (M11.b): *"a mode that is neither driveable through the live bridge nor
    // declared absent with a rung SHALL fail the check, naming the mode"*. A check over the LIST,
    // so that a fourth mode added to the enumeration and forgotten fails here.
    PlayModeDescriptor rows[kPlayModeCount];
    describe_play_modes(play_mode_support(), rows);

    u32 available = 0;
    u32 absent_with_a_rung = 0;
    for (const PlayModeDescriptor& row : rows) {
        CY_REQUIRE(row.name != nullptr);
        CY_CHECK(std::strlen(row.name) > 0);
        // Every row says why, in both directions, so a reader of an acceptance and a reader of a
        // refusal are reading the same field.
        CY_REQUIRE(row.availability.reason != nullptr);
        CY_CHECK(std::strlen(row.availability.reason) > 0);
        if (row.availability.available) {
            ++available;
            CY_CHECK_EQ(std::strlen(row.availability.due), 0U);
        } else {
            // The rung at which it is due, so that "not yet" is a recorded decision rather than the
            // absence of a check.
            CY_REQUIRE(row.availability.due != nullptr);
            CY_CHECK(std::strlen(row.availability.due) > 0);
            ++absent_with_a_rung;
        }
        // The mode's own spelling round-trips, which is what the protocol carries.
        const cy::Expected<PlayMode, cy::Error> parsed = play_mode_of(row.name);
        CY_REQUIRE(parsed.has_value());
        CY_CHECK(*parsed == row.mode);
    }
    CY_CHECK_EQ(available + absent_with_a_rung, kPlayModeCount);
    // On this build: two driveable, one absent. Written as a number rather than as "at least one"
    // so that a mode quietly becoming unavailable is a failure rather than a smaller total.
    CY_CHECK_EQ(available, 2U);
    CY_CHECK_EQ(absent_with_a_rung, 1U);

    // A word this build does not know is refused rather than read as the closest mode.
    CY_CHECK_FALSE(play_mode_of("console").has_value());
    CY_CHECK_FALSE(play_mode_of("").has_value());
}

CY_TEST_CASE("a mode's capabilities are queried rather than discovered by trying") {
    // `live-editing` requires stepping *"in every mode where the runtime permits"*, and M11.b's
    // delta makes "where the runtime permits" answerable by a query. Both halves are checked: the
    // query says no, AND the call refuses. Either alone would be satisfied by the other being
    // wrong.
    CY_CHECK(capabilities_of(PlayMode::InEditor).step_frame);
    CY_CHECK(capabilities_of(PlayMode::SeparateProcess).step_frame);
    CY_CHECK_FALSE(capabilities_of(PlayMode::RemoteDevice).step_frame);
    CY_CHECK(capabilities_of(PlayMode::RemoteDevice).step_tick);

    // Standalone behaviour is honest: the two modes whose point is isolation say so, and the one
    // whose point is iteration speed does not claim it.
    CY_CHECK_FALSE(capabilities_of(PlayMode::InEditor).isolated_state);
    CY_CHECK(capabilities_of(PlayMode::SeparateProcess).isolated_state);
    CY_CHECK(capabilities_of(PlayMode::RemoteDevice).isolated_state);

    Reference physics;
    CY_REQUIRE(physics.ready());
    Authored authored;
    CY_REQUIRE(authored.started);

    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.mode = PlayMode::RemoteDevice;
    configuration.support = support_for(PlayMode::RemoteDevice);
    PlaySession session(allocator(), authored.world);
    CY_REQUIRE(session.enter(configuration).has_value());
    CY_REQUIRE(session.pause().has_value());

    const cy::Status framed = session.step_frame();
    CY_REQUIRE_FALSE(framed.has_value());
    CY_CHECK(framed.error().code == cy::ErrorCode::Unsupported);
    // The tick step is a message rather than a picture, so the remote mode keeps it.
    CY_CHECK(session.step_tick().has_value());
    CY_CHECK_EQ(session.report().stepped_ticks, 1U);
    CY_CHECK_EQ(session.report().stepped_frames, 0U);
    CY_REQUIRE(session.stop().has_value());
}

CY_TEST_CASE("a frame step is as many ticks as the two rates say, not one") {
    Reference physics;
    CY_REQUIRE(physics.ready());
    Authored authored;
    CY_REQUIRE(authored.started);

    // 60 Hz ticks against 30 Hz frames: a frame is two ticks. A `step_frame` that ran one would be
    // a frame step in name only, and at the default 60/60 it would look correct.
    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.rate = cy::determinism::TickRate{60, 1};
    configuration.frame_rate = cy::determinism::TickRate{30, 1};
    PlaySession session(allocator(), authored.world);
    CY_REQUIRE(session.enter(configuration).has_value());
    CY_CHECK_EQ(session.ticks_per_frame(), 2U);

    CY_REQUIRE(session.pause().has_value());
    const u64 before = session.report().ticks;
    CY_REQUIRE(session.step_frame().has_value());
    CY_CHECK_EQ(session.report().ticks - before, 2U);
    CY_CHECK_EQ(session.report().stepped_frames, 1U);
    CY_CHECK_EQ(session.report().stepped_ticks, 2U);

    // A step into a RUNNING simulation is one extra tick rather than a step, and is refused.
    CY_REQUIRE(session.resume().has_value());
    CY_CHECK_FALSE(session.step_tick().has_value());
    CY_CHECK_FALSE(session.step_frame().has_value());
    CY_REQUIRE(session.stop().has_value());
}
