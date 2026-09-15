// The play modes, driven — one of them in a process this suite launches. M11.b tasks 0.5 and 3.1,
// and the `play-mode-round-trip` and `separate-process-is-a-second-process` criteria.
//
// ================================================================================================
// WHAT THIS SUITE USED TO BE, AND WHY THAT WAS NOT A CHECK
// ================================================================================================
//
// Until the repair round that built `cy/gameplay/play/launcher.h`, this suite drove "a world in
// each of the three modes" by building THREE `PlaySession`s IN ONE PROCESS, passing a different
// `PlayMode` to each, and comparing the three results. It passed. It could not have failed:
//
//   * three copies of the same in-process simulation agree whatever the mode argument said, so the
//     comparison measured nothing about locality;
//   * `PlayModeSupport::runtime_launcher` was a literal `true` beside a comment reading "this build
//     carries no launcher yet", so the availability it gated was a constant;
//   * `grep -rn 'fork\|exec\|posix_spawn\|CreateProcess' src/gameplay/play/` returned four string
//     literals and no code — deleting separate-process play entirely would have left this suite
//     green.
//
// M11.b's gate said so in those terms. The repair is in three parts and this file is the third:
// `launcher.h` launches and supervises a real second process, `driver.h` gives both localities ONE
// command vocabulary, and this suite drives the same stream through both and compares the two
// PROCESSES' results.
//
// ================================================================================================
// WHAT MAKES THE SEPARATE-PROCESS LEG UNFAKEABLE
// ================================================================================================
//
// Three facts, and no in-process stand-in can produce all three:
//
//   1. `RuntimeProcess::launch` asks the operating system for the child's identifier and the CHILD
//      reports its own in the handshake. They must agree, or the launch fails. A local object
//      answering the protocol would have to produce an identifier the operating system agrees
//      belongs to a process this one spawned.
//   2. The identifier is not this process's. The case asserts that too, because a launcher that
//      re-executed nothing and answered from here would satisfy (1) trivially.
//   3. `PlaySession::enter` REFUSES `SeparateProcess` unless `support.hosted_runtime_process` says
//      this process is the launched one — which only `cy_play_runtime_host` sets. The shape this
//      suite used to have is now a refusal, and a case below drives it and watches it refuse.
//
// ================================================================================================
// AND WHAT REMOTE-DEVICE HONESTLY IS HERE
// ================================================================================================
//
// Unavailable, declared absent, with the rung it is due at — and this suite does NOT claim a remote
// runtime ran. Nothing in this tree encodes a frame: `EncodedStream` is declared on both sides of
// the viewport transport and implemented on neither. The cases below exercise the refusal in both
// directions and the capability table that says a frame step is the one thing a remote transport
// cannot honour. They do not simulate a console and call it a mode.

#include <cy/core/assets/file.h>
#include <cy/gameplay/play/driver.h>
#include <cy/gameplay/play/launcher.h>
#include <cy/gameplay/play/mode.h>
#include <cy/gameplay/play/session.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/test/test.h>

#include "editor_play_fixture.h"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using cy::f32;
using cy::i64;
using cy::u32;
using cy::u64;
using namespace cy::gameplay;
using namespace cy::test_editor;

namespace {

/// This process's own identifier. The separate-process leg is only a separate process if the thing
/// that answered is not this.
[[nodiscard]] i64 own_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<i64>(::GetCurrentProcessId());
#else
    return static_cast<i64>(::getpid());
#endif
}

/// The platform, which is the only thing in the tree that can start a process. One per case: an
/// `Sdl3Platform` holds the process table the launcher's handles index into.
class PlatformFixture {
public:
    PlatformFixture() noexcept {
        static char program[] = "cy_test_integration_editor_play";
        static char* argv[] = {program, nullptr};
        ready_ = platform_.initialise(1, argv).has_value();
    }
    ~PlatformFixture() {
        if (ready_) {
            platform_.shutdown();
        }
    }

    PlatformFixture(const PlatformFixture&) = delete;
    PlatformFixture& operator=(const PlatformFixture&) = delete;
    PlatformFixture(PlatformFixture&&) = delete;
    PlatformFixture& operator=(PlatformFixture&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] cy::Sdl3Platform& platform() noexcept { return platform_; }

private:
    cy::Sdl3Platform platform_;
    bool ready_ = false;
};

/// The authored world, on disk, for the second process to read. A path relative to the suite's
/// working directory, which ctest sets to the suite's own build directory.
[[nodiscard]] bool write_world(const std::string& text, const char* path) {
    return cy::assets::fs::write_atomic(path, text.data(), text.size()).has_value();
}

/// One run of THE command stream, and what it produced.
struct Driven {
    bool entered = false;
    bool stepped_tick = false;
    bool stepped_frame = false;
    bool step_frame_refused = false;
    bool stopped = false;
    u64 ticks = 0;
    u64 stepped_ticks = 0;
    u32 entities = 0;
    u32 bodies = 0;
    /// The sphere's height after the stream, as its exact bits. A decimal comparison between two
    /// processes would be "agrees to within printing precision", which is a weaker claim than the
    /// one being made and would hide a divergence of one unit in the last place.
    u32 height_bits = 0;
    bool restored_exactly = false;
};

[[nodiscard]] u32 bits_of(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// The authored identity of the sphere — the second node of the fixture world, the one that falls.
///
/// Read out of the document rather than written down, because an identity is `fnv1a_128` over the
/// ASSET PATH and the node's ordinal (see `worldfile.h`), not an index. Both processes must derive
/// it from the same asset path or they hold two different sets of identities and every read across
/// the bridge refuses — which is what the first run of this case did.
[[nodiscard]] u64 sphere_identity(const ser::World& world) noexcept {
    return world.nodes().size() > 1 ? world.nodes()[1].identity : 0;
}

/// THE COMMAND STREAM. One function, one sequence of calls, and the only thing that varies between
/// runs is which `PlayDriver` receives them — which is the requirement restated as code: *"All three
/// SHALL be driven through the same live bridge interface. Locality SHALL be an optimisation of
/// transport, not a different architecture."*
[[nodiscard]] Driven drive(PlayDriver& driver, u64 sphere) {
    Driven result;
    if (!driver.enter()) {
        return result;
    }
    result.entered = true;

    for (u32 tick = 0; tick < 20; ++tick) {
        if (!driver.tick()) {
            return result;
        }
    }
    if (!driver.pause()) {
        return result;
    }
    result.stepped_tick = driver.step_tick().has_value();
    if (!driver.resume()) {
        return result;
    }
    for (u32 tick = 0; tick < 9; ++tick) {
        if (!driver.tick()) {
            return result;
        }
    }

    // The compared state: thirty ticks of simulation, and where the sphere got to.
    const cy::Expected<PlayObservation, cy::Error> observed = driver.observe();
    if (!observed) {
        return result;
    }
    result.ticks = observed->ticks;
    result.stepped_ticks = observed->stepped_ticks;
    result.entities = observed->entities;
    result.bodies = observed->bodies;

    const cy::Expected<f32, cy::Error> height = driver.translation_y(sphere);
    if (!height) {
        return result;
    }
    result.height_bits = bits_of(*height);

    // The frame step is attempted AFTER the compared state is captured, and deliberately: it is the
    // one capability that differs between transports, so folding it into the compared prefix would
    // make the two localities run different numbers of ticks and the comparison would be measuring
    // the capability difference rather than the world model.
    if (!driver.pause()) {
        return result;
    }
    const cy::Status framed = driver.step_frame();
    result.stepped_frame = framed.has_value();
    result.step_frame_refused = !framed.has_value();

    if (!driver.stop()) {
        return result;
    }
    result.stopped = true;
    const cy::Expected<PlayObservation, cy::Error> after = driver.observe();
    if (after) {
        result.restored_exactly = after->restored_exactly;
    }
    return result;
}

}  // namespace

CY_TEST_CASE("a world plays in the editor's process and in a second one, driven by one stream") {
    PlatformFixture host;
    CY_REQUIRE(host.ready());
    Reference physics;
    CY_REQUIRE(physics.ready());

    // The launcher is MEASURED. If `cy_play_runtime_host` is not beside this binary the mode is
    // unavailable, and this case says so by name rather than passing over an absent feature.
    CY_REQUIRE(runtime_launcher_available(host.platform()));

    // --- The editor's own process ---------------------------------------------------------------
    Authored authored;
    CY_REQUIRE(authored.started);
    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.mode = PlayMode::InEditor;
    configuration.support = play_mode_support(host.platform());

    const u64 sphere = sphere_identity(authored.world);
    CY_REQUIRE(sphere != 0);

    PlaySession session(allocator(), authored.world);
    LocalPlayDriver local(session, configuration, authored.world);
    const Driven in_editor = drive(local, sphere);

    // --- A second runtime process, launched here ------------------------------------------------
    constexpr const char* kWorldFile = "editor_play_separate_process.cyworld";
    CY_REQUIRE(write_world(authored.text, kWorldFile));

    RuntimeLaunchRequest request;
    request.world_path = kWorldFile;
    // The path the EDITOR opens this world by, which is what every node identity is derived from.
    // The file on disk is a transport detail; `kAssetPath` is the document.
    request.asset_path = kAssetPath;
    request.body_capacity = 64;

    RuntimeProcess child(allocator());
    const cy::Status launched = child.launch(host.platform(), request);
    // The reason, in the log, before the assertion: a launch that failed is the one failure a
    // reader of this suite most needs the words for, and "REQUIRE(false)" says nothing about which
    // of the binary, the world, the handshake or the identifier check refused.
    if (!launched) {
        CY_TEST_MESSAGE(std::string(launched.error().message));
    }
    CY_REQUIRE(launched.has_value());

    // IT IS A SECOND PROCESS. The operating system's identifier for the child is not this one's,
    // and the child's own report of it agrees with what the operating system told this process —
    // which `launch` already required, and which is restated here because it is the assertion the
    // whole leg rests on.
    CY_CHECK(child.observed_process_id() != own_process_id());
    CY_CHECK_EQ(child.reported_process_id(), child.observed_process_id());
    CY_CHECK(child.running());

    ProcessPlayDriver remote_process(child, allocator());
    CY_CHECK(remote_process.mode() == PlayMode::SeparateProcess);
    const Driven separate = drive(remote_process, sphere);

    const cy::Expected<cy::i32, cy::Error> exit_code = child.shutdown();
    CY_REQUIRE(exit_code.has_value());
    // A child asked to leave exits zero. A child that crashed, was killed, or died mid-session does
    // not, and the difference is what distinguishes a run from a launch.
    CY_CHECK_EQ(*exit_code, 0);

    // --- ONE WORLD MODEL, TWO PROCESSES ---------------------------------------------------------
    //
    // The spike's question — does the hosted runtime carry the modes without a second world model —
    // answered in the tree. The same authored world, driven by the same calls, simulates to the
    // same place in a process that shares nothing with this one but the bytes of the world file.
    CY_REQUIRE(in_editor.entered);
    CY_REQUIRE(separate.entered);
    CY_CHECK_EQ(in_editor.ticks, separate.ticks);
    CY_CHECK_EQ(in_editor.ticks, 30U);
    CY_CHECK_EQ(in_editor.stepped_ticks, separate.stepped_ticks);
    CY_CHECK_EQ(in_editor.stepped_ticks, 1U);
    CY_CHECK_EQ(in_editor.entities, separate.entities);
    CY_CHECK_EQ(in_editor.bodies, separate.bodies);
    // Bit for bit, not "near". Two processes running the same compiled simulation over the same
    // bytes produce the same float; anything weaker would let a real divergence through.
    CY_CHECK_EQ(in_editor.height_bits, separate.height_bits);
    // And it is a world that actually moved, so the agreement is over a simulation rather than over
    // two copies of the initial placement.
    f32 fell = 0.0F;
    std::memcpy(&fell, &in_editor.height_bits, sizeof(fell));
    CY_CHECK_LT(fell, 4.0F);

    // Both localities step a frame, and entering and leaving play leaves the document exactly as it
    // was in both.
    CY_CHECK(in_editor.stepped_tick);
    CY_CHECK(separate.stepped_tick);
    CY_CHECK(in_editor.stepped_frame);
    CY_CHECK(separate.stepped_frame);
    CY_CHECK(in_editor.stopped);
    CY_CHECK(separate.stopped);
    CY_CHECK(in_editor.restored_exactly);
    CY_CHECK(separate.restored_exactly);

    (void)cy::assets::fs::remove_file(kWorldFile);
}

CY_TEST_CASE("separate-process play refuses when there is no runtime host to launch") {
    PlatformFixture host;
    CY_REQUIRE(host.ready());
    Authored authored;
    CY_REQUIRE(authored.started);

    constexpr const char* kWorldFile = "editor_play_missing_host.cyworld";
    CY_REQUIRE(write_world(authored.text, kWorldFile));

    // A binary that is not there. This is what a tree whose runtime host was never built, was
    // deleted, or was renamed looks like to the launcher, and the answer has to be a refusal that
    // names the missing part rather than a mode that silently becomes the in-editor one.
    RuntimeLaunchRequest request;
    request.world_path = kWorldFile;
    request.binary = "cy_play_runtime_host_that_is_not_there";

    RuntimeProcess child(allocator());
    const cy::Status launched = child.launch(host.platform(), request);
    CY_REQUIRE_FALSE(launched.has_value());
    CY_CHECK(launched.error().code == cy::ErrorCode::NotFound);
    const std::string reason = launched.error().message;
    CY_CHECK(reason.find("separate-process") != std::string::npos);
    CY_CHECK_FALSE(child.running());

    // And nothing may be driven through a driver whose process never started.
    ProcessPlayDriver driver(child, allocator());
    CY_CHECK_FALSE(driver.enter().has_value());
    CY_CHECK_FALSE(driver.tick().has_value());
    CY_CHECK_FALSE(driver.observe().has_value());

    (void)cy::assets::fs::remove_file(kWorldFile);
}

CY_TEST_CASE("a play session in the launching process cannot call itself separate-process") {
    // THE SHAPE THIS SUITE USED TO HAVE, NOW A REFUSAL. Three `PlaySession`s in one process with
    // three different `PlayMode`s was how M11.b claimed the modes worked; it is the in-editor world
    // under another name, and `PlaySession::enter` now says so.
    PlatformFixture host;
    CY_REQUIRE(host.ready());
    Reference physics;
    CY_REQUIRE(physics.ready());
    Authored authored;
    CY_REQUIRE(authored.started);

    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.mode = PlayMode::SeparateProcess;
    configuration.support = play_mode_support(host.platform());
    // The editor's side of the question is YES — this build can launch a runtime host — which is
    // exactly why the session's side has to be asked separately.
    CY_REQUIRE(configuration.support.runtime_launcher);
    CY_REQUIRE_FALSE(configuration.support.hosted_runtime_process);
    CY_CHECK(availability_of(PlayMode::SeparateProcess, configuration.support).available);

    const std::string before = bytes_of(authored.world);
    PlaySession session(allocator(), authored.world);
    const cy::Status entered = session.enter(configuration);
    CY_REQUIRE_FALSE(entered.has_value());
    const std::string reason = entered.error().message;
    CY_CHECK(reason.find("separate-process") != std::string::npos);
    CY_CHECK(reason.find("ProcessPlayDriver") != std::string::npos);
    // And it started nothing, which is the half that catches a refusal that left a session running.
    CY_CHECK(session.state() == PlayState::Editing);
    CY_CHECK(session.world() == nullptr);
    CY_CHECK_EQ(bytes_of(authored.world), before);

    // The same configuration inside the launched process is accepted, and that is the one bit that
    // differs. `cy_play_runtime_host` is the only thing in the tree that sets it.
    configuration.support.hosted_runtime_process = true;
    PlaySession hosted(allocator(), authored.world);
    CY_REQUIRE(hosted.enter(configuration).has_value());
    CY_CHECK(hosted.mode() == PlayMode::SeparateProcess);
    CY_REQUIRE(hosted.stop().has_value());
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
    PlatformFixture host;
    CY_REQUIRE(host.ready());

    // MEASURED against the platform, which is what a host does. The no-argument overload answers
    // `runtime_launcher = false`, because a caller with no platform cannot start a process — and a
    // build that answered otherwise would be the literal `true` this rung's gate refused.
    CY_CHECK_FALSE(play_mode_support().runtime_launcher);
    const PlayModeSupport support = play_mode_support(host.platform());
    CY_REQUIRE(support.runtime_launcher);

    PlayModeDescriptor rows[kPlayModeCount];
    describe_play_modes(support, rows);

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

    // And the mode that is absent is the one with no transport, not the one with no launcher: the
    // launcher exists now, and a build that lost it would fail the REQUIRE above rather than
    // quietly reporting a different absent mode here.
    CY_CHECK_FALSE(rows[static_cast<u32>(PlayMode::RemoteDevice)].availability.available);
    CY_CHECK(rows[static_cast<u32>(PlayMode::SeparateProcess)].availability.available);

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

    // The remote transport, made available by flipping the two bits `mode.cpp` refuses on. This is
    // the refusal exercised in the other direction — a refusal that cannot be made to stop refusing
    // is a constant wearing a check's clothes — and it is NOT a claim that a remote runtime ran:
    // there is no machine at the other end, and the case asserts only what the capability table
    // says a transport can be asked to do.
    PlayConfiguration configuration = configuration_over(physics.server(), authored.schema);
    configuration.mode = PlayMode::RemoteDevice;
    configuration.support.frame_encoder = true;
    configuration.support.remote_runtime = true;
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
