// Playback: the lifecycle, seeking, skipping, capture and restore, the preload plan, and the
// exclusive group. `integration` rather than `unit` — every case here builds a program and drives a
// player, which is more than a millisecond's work on a loaded machine.

#include "fixture.h"

#include <cy/sequencing/player.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

constexpr u32 kLampBinding = 10;
constexpr u32 kCameraBinding = 11;
constexpr u32 kLightTrack = 100;
constexpr u32 kCommandTrack = 200;
constexpr u32 kCameraTrack = 300;
constexpr u32 kCutTrack = 400;
constexpr u32 kUnlockEvent = 900;

/// A mission cinematic in the SIMULATION domain: a light that must be put back, a shot that takes
/// the camera, a cut, a command that unlocks a door, and an event that is confirmed-only.
[[nodiscard]] SequenceSource mission_sequence() noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("mission");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.domain = ClockDomain::Simulation;
    source.skip = SkipPolicy::ApplyRequiredOutcomes;
    source.duration = SequenceTime::from_frame(96);
    source.accessibility.skippable = true;
    (void)source.bindings.push_back(binding_of(kLampBinding, "lamp", BindingKind::Entity));
    (void)source.bindings.push_back(binding_of(kCameraBinding, "shot", BindingKind::Camera));

    Track light = track_of("lamp", TrackKind::Property, kLightTrack, kLampBinding);
    light.subsystem = SubsystemId::Light;
    Section lit = section_over(0, 48, 1000);
    lit.completion = CompletionPolicy::Restore;
    lit.asset = 0xA55E7;
    lit.pre_roll = SequenceTime::from_frame(12);
    Channel intensity = scalar_channel("intensity", 1);
    add_key(intensity, 0, 0.0F);
    add_key(intensity, 48, 4.0F);
    (void)lit.channels.push_back(std::move(intensity));
    (void)light.sections.push_back(std::move(lit));
    (void)source.tracks.push_back(std::move(light));

    Track shot = track_of("shot A", TrackKind::Camera, kCameraTrack, kCameraBinding);
    Section wide = section_over(0, 72, 3000);
    wide.priority = 100;
    wide.blend_in_seconds = 1.5F;
    wide.framing_binding = kLampBinding;
    (void)shot.sections.push_back(std::move(wide));
    (void)source.tracks.push_back(std::move(shot));

    Track cut = track_of("cut to B", TrackKind::CameraCut, kCutTrack, kCameraBinding);
    Section moment = section_over(72, 73, 4000);
    moment.pre_roll = SequenceTime::from_frame(48);  // announced two seconds ahead
    (void)cut.sections.push_back(std::move(moment));
    (void)source.tracks.push_back(std::move(cut));

    Track unlock = track_of("unlock", TrackKind::GameplayCommand, kCommandTrack, kLampBinding);
    unlock.authority = AuthorityClass::AuthoritativeGameplay;
    unlock.command_stable_id = 4242;
    Section fire = section_over(60, 61, 2000);
    fire.command_payload_size = 4;
    fire.command_payload[0] = 7;
    (void)unlock.sections.push_back(std::move(fire));
    EventDeclaration granted;
    granted.time = SequenceTime::from_frame(60);
    granted.type = Name::intern("door_unlocked");
    granted.policy = SideEffectPolicy::ConfirmedOnly;
    granted.stable_id = kUnlockEvent;
    (void)unlock.events.push_back(granted);
    (void)source.tracks.push_back(std::move(unlock));

    (void)source.required_outcomes.push_back(RequiredOutcome{kCommandTrack, kUnlockEvent});
    (void)source.required_outcomes.push_back(RequiredOutcome{kCommandTrack, 0});

    MarkerDeclaration marker;
    marker.time = SequenceTime::from_frame(36);
    marker.name = Name::intern("beat");
    (void)source.markers.push_back(marker);
    return source;
}

/// Everything a case needs, compiled once.
struct Fixture {
    explicit Fixture(Allocator& alloc) noexcept
        : registry(alloc), program(alloc), report(alloc), batches(alloc) {}

    [[nodiscard]] Status build(const SequenceSource& source) noexcept {
        if (Status built = registry.build(); !built) {
            return built;
        }
        SequenceCompiler compiler(allocator(), registry.registry);
        return compiler.compile(source, CompileOptions{}, program, report);
    }

    [[nodiscard]] static PlayRequest request(Span<const BindingResolution> bindings) noexcept {
        PlayRequest play;
        play.bindings = bindings;
        return play;
    }

    Registry registry;
    Program program;
    CompileReport report;
    DispatchBatches batches;
};

[[nodiscard]] BindingResolution resolved(u32 stable_id, u64 target) noexcept {
    return BindingResolution{stable_id, target, true};
}

[[nodiscard]] u32 count_values(const DispatchBatches& batches, SubsystemId subsystem) noexcept {
    u32 count = 0;
    for (const ValueRequest& request : batches.values.span()) {
        count += (request.subsystem == subsystem) ? 1U : 0U;
    }
    return count;
}

}  // namespace

CY_TEST_CASE("sequence_playback: a shot drives values, a camera and a command") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);

    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    // One second of playback at 24 frames per second.
    for (i32 frame = 0; frame < 24; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    const Expected<InstanceStatus, Error> status = player.status(instance.value());
    CY_REQUIRE(status);
    CY_CHECK_EQ(status.value().state, InstanceState::Playing);
    CY_CHECK_EQ(status.value().time.frame(), 24);

    CY_CHECK_EQ(count_values(fixture.batches, SubsystemId::Light), 1U);

    // TWO camera requests, and the second is the point of the first: the shot, and the cut's
    // ANNOUNCEMENT, which is due at this instant because the cut declares a two-second pre-roll.
    CY_REQUIRE_EQ(fixture.batches.cameras.size(), 2U);
    const CameraRequest* shot = nullptr;
    for (const CameraRequest& request : fixture.batches.cameras.span()) {
        if (!request.cut) {
            shot = &request;
        }
    }
    CY_REQUIRE(shot != nullptr);
    // THE CAMERA REQUEST NAMES A RIG AND A BLEND, AND CARRIES NO POSE. There is no field for one.
    CY_CHECK_EQ(shot->rig, 900U);
    CY_CHECK_EQ(shot->priority, 100);
    CY_CHECK_NEAR(shot->blend_in.duration_seconds, 1.5F, 1e-5F);
    CY_CHECK(shot->has_framing_target);
    CY_CHECK_EQ(shot->framing_target, 500U);
}

CY_TEST_CASE("sequence_playback: play and seek agree") {
    // "**WHEN** a seekable track is played to a time and separately seeked to it **THEN** the
    // resulting state SHALL be equivalent."
    Fixture played(allocator());
    CY_REQUIRE(played.build(mission_sequence()));
    Fixture seeked(allocator());
    CY_REQUIRE(seeked.build(mission_sequence()));

    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};

    SequencePlayer first(allocator(), played.registry.registry, PlaybackScope::World, 0);
    const Expected<InstanceId, Error> playing =
        first.create(played.program, played.request(bindings));
    CY_REQUIRE(playing);
    CY_REQUIRE(first.play(playing.value()));
    for (i32 frame = 0; frame < 36; ++frame) {
        played.batches.clear();
        CY_REQUIRE(first.advance(41666667, played.batches));
    }

    SequencePlayer second(allocator(), seeked.registry.registry, PlaybackScope::World, 0);
    const Expected<InstanceId, Error> jumping =
        second.create(seeked.program, seeked.request(bindings));
    CY_REQUIRE(jumping);
    CY_REQUIRE(second.play(jumping.value()));
    CY_REQUIRE(second.seek(jumping.value(), SequenceTime::from_frame(36), SeekMode::Runtime,
                           seeked.batches));
    seeked.batches.clear();
    CY_REQUIRE(second.step_to(jumping.value(), SequenceTime::from_frame(36), seeked.batches));

    CY_REQUIRE_EQ(played.batches.values.size(), seeked.batches.values.size());
    for (usize index = 0; index < played.batches.values.size(); ++index) {
        CY_CHECK_NEAR(played.batches.values[index].value.components[0],
                      seeked.batches.values[index].value.components[0], 1e-5F);
    }
    CY_CHECK_EQ(first.status(playing.value()).value().time.ticks(),
                second.status(jumping.value()).value().time.ticks());
}

CY_TEST_CASE("sequence_playback: scrubbing does not fire mission events") {
    // "**WHEN** an author scrubs a timeline in preview mode **THEN** irreversible gameplay events
    // SHALL be suppressed."
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    CY_REQUIRE(player.seek(instance.value(), SequenceTime::from_frame(90), SeekMode::Preview,
                           fixture.batches));
    for (const EventRequest& event : fixture.batches.events.span()) {
        CY_CHECK_NE(event.type, Name::intern("door_unlocked"));
    }

    // The same jump at run time DOES fire it, which is what proves the suppression is the mode.
    fixture.batches.clear();
    CY_REQUIRE(player.seek(instance.value(), SequenceTime::from_frame(0), SeekMode::Replay,
                           fixture.batches));
    fixture.batches.clear();
    CY_REQUIRE(player.seek(instance.value(), SequenceTime::from_frame(90), SeekMode::Runtime,
                           fixture.batches));
    bool fired = false;
    for (const EventRequest& event : fixture.batches.events.span()) {
        fired = fired || event.type == Name::intern("door_unlocked");
    }
    CY_CHECK(fired);
}

CY_TEST_CASE("sequence_playback: seeking crosses a whole timeline in one step") {
    // "Seeking SHALL locate the target time through the index ... **without replaying the sequence
    // from its start**." What is observable is that ONE seek to the end emits the whole crossed set
    // once, rather than a frame's worth of values per intervening frame.
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    CY_REQUIRE(player.seek(instance.value(), SequenceTime::from_frame(90), SeekMode::Runtime,
                           fixture.batches));
    // At frame 90 the light section has ended and the shot has too: what is emitted is the state at
    // 90, not 90 frames of it.
    CY_CHECK_LE(fixture.batches.values.size(), 2U);
    CY_CHECK_EQ(fixture.batches.events.size(), 2U);  // the unlock and the marker
}

CY_TEST_CASE("sequence_playback: the door still opens when the cutscene is skipped") {
    // "**WHEN** a player skips a cutscene that would have unlocked a door **THEN** the required
    // outcome SHALL be applied and the door SHALL be unlocked."
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    // Skip from the very beginning: the command section was never entered by playing.
    CY_REQUIRE(player.skip(instance.value(), fixture.batches));

    bool unlocked = false;
    for (const EventRequest& event : fixture.batches.events.span()) {
        if (event.type == Name::intern("door_unlocked")) {
            unlocked = true;
            CY_CHECK(event.applied_by_skip);
        }
    }
    CY_CHECK(unlocked);
    bool commanded = false;
    for (const CommandRequest& command : fixture.batches.commands.span()) {
        commanded = commanded || command.command_stable_id == 4242;
    }
    CY_CHECK(commanded);
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Completed);
}

CY_TEST_CASE("sequence_playback: a light returns to its value, and interruption is not special") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);

    fixture.registry.host.current = ChannelValue::scalar(0.75F);  // the world's own value
    CY_REQUIRE(player.play(instance.value()));
    for (i32 frame = 0; frame < 24; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    // ONLY WHAT THE SEQUENCE TOUCHED: one property captured, not an object snapshot.
    CY_CHECK_EQ(fixture.registry.host.captured.size(), 1U);
    CY_CHECK_EQ(fixture.registry.host.restored.size(), 0U);

    // Stopped mid-playback: the completion policy applies exactly as it would at the end.
    fixture.batches.clear();
    CY_REQUIRE(player.stop(instance.value(), fixture.batches));
    CY_REQUIRE_EQ(fixture.registry.host.restored.size(), 1U);
    CY_CHECK_NEAR(fixture.registry.host.current.components[0], 0.75F, 1e-5F);
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Stopped);
}

CY_TEST_CASE("sequence_playback: a missing requirement is explicit") {
    // "**WHEN** a required binding cannot be resolved **THEN** the sequence SHALL fail to start and
    // name the binding."
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          BindingResolution{kCameraBinding, 0, false}};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_CHECK_FALSE(player.play(instance.value()));

    const Expected<InstanceStatus, Error> status = player.status(instance.value());
    CY_REQUIRE(status);
    CY_CHECK_EQ(status.value().state, InstanceState::Failed);
    CY_CHECK_EQ(status.value().failure, FailureReason::UnresolvedBinding);
    CY_CHECK_EQ(status.value().failure_detail, kCameraBinding);
}

CY_TEST_CASE("sequence_playback: an editor preview cannot drive the simulation clock") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer preview(allocator(), fixture.registry.registry, PlaybackScope::EditorPreview, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        preview.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_CHECK_FALSE(preview.play(instance.value()));
    CY_CHECK_EQ(preview.status(instance.value()).value().failure, FailureReason::IncompatibleClock);
}

CY_TEST_CASE("sequence_playback: a shot is warm before it starts, and a miss is reported") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);

    // Prepared, not played: the guarantee a critical cinematic needs.
    CY_REQUIRE(player.prepare(instance.value()));
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Preparing);

    Array<PreloadRequest> requests(allocator());
    CY_REQUIRE(player.publish_preload(SequenceTime::from_frame(48), requests));
    CY_REQUIRE_EQ(requests.size(), 1U);
    CY_CHECK_EQ(requests[0].asset, 0xA55E7U);
    // Ahead of need: the deadline is the pre-roll, which is before the section starts.
    CY_CHECK_EQ(requests[0].lead_ticks, SequenceTime::from_frame(-12).ticks());

    CY_REQUIRE(player.report_preload_miss(instance.value(), 0xA55E7, 0xFA11BAC));
    CY_REQUIRE_EQ(player.preload_misses().size(), 1U);
    CY_CHECK_EQ(player.preload_misses()[0].substituted, 0xFA11BACU);

    CY_REQUIRE(player.asset_ready(instance.value(), 0xA55E7));
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Ready);
}

CY_TEST_CASE("sequence_playback: a cut is announced before it happens") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    // The cut is at frame 72 with a 48-frame pre-roll, so the announcement is due at frame 24.
    bool announced = false;
    f32 lead = 0.0F;
    for (i32 frame = 0; frame < 30; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
        for (const CameraRequest& request : fixture.batches.cameras.span()) {
            if (request.cut && request.anticipated) {
                announced = true;
                lead = request.cut_lead_seconds;
            }
        }
    }
    CY_CHECK(announced);
    CY_CHECK_GT(lead, 1.0F);  // about two seconds of lead at 24 frames per second

    Array<FutureShot> shots(allocator());
    CY_REQUIRE(player.publish_future_shots(SequenceTime::from_frame(96), shots));
    bool future_cut = false;
    for (const FutureShot& shot : shots.span()) {
        future_cut = future_cut || shot.cut;
    }
    CY_CHECK(future_cut);
}

CY_TEST_CASE("sequence_playback: pre-roll prepares") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    PlayRequest request = fixture.request(bindings);
    request.start = SequenceTime::from_frame(-20);  // inside the light section's pre-roll
    const Expected<InstanceId, Error> instance = player.create(fixture.program, request);
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));
    // Forward to inside the light section's twelve-frame pre-roll, which begins at frame -12.
    for (i32 frame = 0; frame < 10; ++frame) {
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    CY_CHECK_LT(player.status(instance.value()).value().time.frame(), 0);

    CY_REQUIRE(fixture.batches.prepares.size() >= 1U);
    CY_CHECK_EQ(fixture.batches.prepares[0].asset, 0xA55E7U);
    CY_CHECK_GT(fixture.batches.prepares[0].lead_ticks, 0);
    // Prepared but NOT evaluated: no value is driven before the section's own range.
    CY_CHECK_EQ(count_values(fixture.batches, SubsystemId::Light), 0U);
}

CY_TEST_CASE("sequence_playback: starting one sequence in an exclusive group suspends the other") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};

    PlayRequest first = fixture.request(bindings);
    first.exclusive_group = Name::intern("cinematic");
    const Expected<InstanceId, Error> a = player.create(fixture.program, first);
    CY_REQUIRE(a);
    CY_REQUIRE(player.play(a.value()));

    PlayRequest second = fixture.request(bindings);
    second.exclusive_group = Name::intern("cinematic");
    const Expected<InstanceId, Error> b = player.create(fixture.program, second);
    CY_REQUIRE(b);
    CY_REQUIRE(player.play(b.value()));

    CY_CHECK(player.status(a.value()).value().suspended);
    CY_CHECK_FALSE(player.status(b.value()).value().suspended);

    // And the suspended one does not advance.
    const i64 before = player.status(a.value()).value().time.ticks();
    CY_REQUIRE(player.advance(41666667, fixture.batches));
    CY_CHECK_EQ(player.status(a.value()).value().time.ticks(), before);
}

CY_TEST_CASE("sequence_playback: jumping to a marker lands on the marker") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    CY_REQUIRE(player.jump_to_marker(instance.value(), Name::intern("beat"), SeekMode::Runtime,
                                     fixture.batches));
    CY_CHECK_EQ(player.status(instance.value()).value().time.frame(), 36);
    CY_CHECK_FALSE(player.jump_to_marker(instance.value(), Name::intern("nowhere"),
                                         SeekMode::Runtime, fixture.batches));
}

CY_TEST_CASE("sequence_playback: a hundred instances share one program") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    for (u32 index = 0; index < 100; ++index) {
        const Expected<InstanceId, Error> instance =
            player.create(fixture.program, fixture.request(bindings));
        CY_REQUIRE(instance);
        CY_REQUIRE(player.play(instance.value()));
    }
    CY_CHECK_EQ(player.live_instances(), 100U);
    CY_REQUIRE(player.advance(41666667, fixture.batches));
    // A hundred instances, one program, and a hundred contributions to arbitrate.
    CY_CHECK_EQ(fixture.batches.cameras.size(), 100U);
    ArbitrationReport report(allocator());
    CY_REQUIRE(arbitrate(fixture.batches, report));
    // One resolved value per (target, property) whatever the number of contributors.
    CY_CHECK_EQ(report.values.size(), 1U);
    CY_CHECK_EQ(report.values[0].contribution_count, 100U);
}

CY_TEST_CASE("sequence_playback: reverse playback runs the timeline backwards") {
    // "Reverse playback SHALL be supported where semantics permit: channels evaluate in reverse."
    // The clock runs backwards and the channel is evaluated at the instant that produces; nothing
    // asks a subsystem to run backwards, which the specification forbids.
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));
    for (i32 frame = 0; frame < 24; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    f32 forward_value = 0.0F;
    for (const ValueRequest& request : fixture.batches.values.span()) {
        if (request.subsystem == SubsystemId::Light) {
            forward_value = request.value.components[0];
        }
    }
    CY_CHECK_GT(forward_value, 0.0F);

    CY_REQUIRE(player.set_rate(instance.value(), PlayRate{-1, 1}));
    for (i32 frame = 0; frame < 12; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    // Within a tick of frame 12, and NOT exactly on it: 41,666,667 ns is a hair more than a
    // twenty-fourth of a second, so twelve of them backwards overshoot by the same hair that
    // twenty-four of them forwards overshot by, and the flooring lands a tick short. The exactness
    // that matters is that nothing accumulates — see test_time.cpp's reverse case, which asserts
    // that the same accumulator returns to zero.
    const i64 ticks = player.status(instance.value()).value().time.ticks();
    CY_CHECK_GT(ticks, SequenceTime::from_frame(11, 900).ticks());
    CY_CHECK_LE(ticks, SequenceTime::from_frame(12).ticks());
    f32 reversed_value = forward_value;
    for (const ValueRequest& request : fixture.batches.values.span()) {
        if (request.subsystem == SubsystemId::Light) {
            reversed_value = request.value.components[0];
        }
    }
    // The light was rising, so running the clock backwards lowers it.
    CY_CHECK_LT(reversed_value, forward_value);
}

CY_TEST_CASE("sequence_playback: menus over a paused game") {
    // "Pausing SHALL be defined per concern: pausing a sequence, pausing simulation, and pausing
    // presentation are distinct, and an interface sequence SHALL be able to continue while gameplay
    // is paused." Two SERVICES is what makes that structural: the player is scoped, not global, so
    // pausing one cannot pause the other.
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer world(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    SequencePlayer interface_service(allocator(), fixture.registry.registry, PlaybackScope::Session,
                                     0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> gameplay =
        world.create(fixture.program, fixture.request(bindings));
    const Expected<InstanceId, Error> menu =
        interface_service.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(gameplay);
    CY_REQUIRE(menu);
    CY_REQUIRE(world.play(gameplay.value()));
    CY_REQUIRE(interface_service.play(menu.value()));
    CY_REQUIRE(world.pause(gameplay.value()));

    for (i32 frame = 0; frame < 12; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(world.advance(41666667, fixture.batches));
        CY_REQUIRE(interface_service.advance(41666667, fixture.batches));
    }
    CY_CHECK_EQ(world.status(gameplay.value()).value().time.frame(), 0);
    CY_CHECK_EQ(interface_service.status(menu.value()).value().time.frame(), 12);
}

CY_TEST_CASE("sequence_playback: slow motion is scoped to the domain it declares") {
    // "**WHEN** a sequence slows the action **THEN** it SHALL scale the declared domain, and the
    // interface SHALL remain at full speed unless also declared." The scale is a REQUEST in the
    // batch: this module owns no clock, and a sequence that scaled one itself would be the parallel
    // path the specification's first requirement forbids.
    SequenceSource source = mission_sequence();
    Track scale = track_of("slow motion", TrackKind::TimeScale, 500, 0);
    scale.time_scale_domain = 1;  // simulation
    Section slow = section_over(0, 48, 5000);
    Channel amount = scalar_channel("scale", 1);
    add_key(amount, 0, 1.0F);
    add_key(amount, 48, 0.25F);
    (void)slow.channels.push_back(std::move(amount));
    (void)scale.sections.push_back(std::move(slow));
    (void)source.tracks.push_back(std::move(scale));

    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(source));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));
    for (i32 frame = 0; frame < 24; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    CY_REQUIRE_EQ(fixture.batches.time_scales.size(), 1U);
    CY_CHECK_EQ(fixture.batches.time_scales[0].domain, 1U);
    CY_CHECK_LT(fixture.batches.time_scales[0].scale, 1.0F);
    CY_CHECK_GT(fixture.batches.time_scales[0].scale, 0.25F);
}

CY_TEST_CASE("sequence_playback: nothing is orphaned, and a handover is explicit") {
    // "**WHEN** a sequence is stopped abruptly **THEN** content it spawned SHALL be released
    // according to its declared lifetime", and "**WHEN** a sequence spawns something intended to
    // outlive it **THEN** ownership SHALL be transferred explicitly to an owning system."
    SequenceSource source = mission_sequence();
    // A short-lived effect on the light track's section, and a persistent one on the camera's.
    source.tracks[0].sections[0].spawn_template = 0xEFFEC7;
    source.tracks[0].sections[0].spawn_lifetime = 0;  // Section
    source.tracks[1].sections[0].spawn_template = 0xB0A12D;
    source.tracks[1].sections[0].spawn_lifetime = 2;  // Persistent

    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(source));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    u32 spawned = 0;
    for (i32 frame = 0; frame < 2; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
        spawned += static_cast<u32>(fixture.batches.spawns.size());
    }
    CY_CHECK_EQ(spawned, 2U);

    // Past the light section's end: its section-lifetime content is released, the camera's is not.
    u32 section_releases = 0;
    for (i32 frame = 0; frame < 50; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
        for (const ReleaseRequest& release : fixture.batches.releases.span()) {
            section_releases += (release.spawned == 0xEFFEC7) ? 1U : 0U;
            CY_CHECK_NE(release.spawned, 0xB0A12DU);
        }
    }
    CY_CHECK_EQ(section_releases, 1U);

    // Stopped abruptly: the persistent one is HANDED OVER rather than released.
    fixture.batches.clear();
    CY_REQUIRE(player.stop(instance.value(), fixture.batches));
    bool handed_over = false;
    for (const ReleaseRequest& release : fixture.batches.releases.span()) {
        if (release.spawned == 0xB0A12D) {
            handed_over = true;
            CY_CHECK(release.handover);
            CY_CHECK_EQ(release.lifetime, SpawnLifetime::Persistent);
        }
    }
    CY_CHECK(handed_over);
}

CY_TEST_CASE("sequence_playback: teardown under load releases everything it allocated") {
    // TEARDOWN UNDER LOAD, which is where a pool of instances usually leaks: a hundred instances
    // mid-playback, each with captures taken, active sets filled and preload plans published, and
    // then the service is destroyed WITHOUT anything being stopped first.
    //
    // What makes this a test rather than an exercise is the sanitizer: under
    // `just test-sanitize --sanitizer address --tests integration.sequencing_playback` a leaked
    // instance, a double free of one, or a use of a freed instance is a failure. Without it the
    // case still asserts the half that is visible from inside — that a stopped instance's captures
    // are restored and that a destroyed player does not keep the program alive.
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    {
        SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
        for (u32 index = 0; index < 100; ++index) {
            const Expected<InstanceId, Error> instance =
                player.create(fixture.program, fixture.request(bindings));
            CY_REQUIRE(instance);
            CY_REQUIRE(player.prepare(instance.value()));
            CY_REQUIRE(player.play(instance.value()));
            if ((index % 3) == 0) {
                // A third of them are seeked into the middle of the cinematic, so their captures
                // and active sets are non-empty when the player goes.
                CY_REQUIRE(player.seek(instance.value(), SequenceTime::from_frame(30),
                                       SeekMode::Runtime, fixture.batches));
            }
        }
        for (i32 frame = 0; frame < 8; ++frame) {
            fixture.batches.clear();
            CY_REQUIRE(player.advance(41666667, fixture.batches));
        }
        Array<PreloadRequest> requests(allocator());
        CY_REQUIRE(player.publish_preload(SequenceTime::from_frame(96), requests));
        CY_CHECK_EQ(requests.size(), 100U);
        CY_CHECK_EQ(player.live_instances(), 100U);
        // Destroyed here, with every instance still playing.
    }

    // A second service over the same program: the program outlived the player, which is what
    // "many playing instances SHALL share one immutable program" requires of the ownership.
    SequencePlayer again(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const Expected<InstanceId, Error> instance =
        again.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(again.play(instance.value()));
    fixture.batches.clear();
    CY_REQUIRE(again.advance(41666667, fixture.batches));
    CY_CHECK_GT(fixture.batches.values.size(), 0U);
}

CY_TEST_CASE("sequence_playback: a stale handle is refused rather than followed") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    const InstanceId stale =
        InstanceId::from_slot(instance.value().index(), instance.value().generation() + 1);
    CY_CHECK_FALSE(player.play(stale));
    CY_CHECK_FALSE(player.status(stale));
    CY_CHECK_FALSE(player.seek(stale, SequenceTime{}, SeekMode::Runtime, fixture.batches));
}

CY_TEST_CASE("sequence_playback: looping does not drift") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    PlayRequest request = fixture.request(bindings);
    request.mode = PlaybackMode::Loop;
    const Expected<InstanceId, Error> instance = player.create(fixture.program, request);
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    // Ten laps of a four-second sequence, one 24-frame-per-second frame at a time.
    for (i32 frame = 0; frame < 960; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    const i64 ticks = player.status(instance.value()).value().time.ticks();
    CY_CHECK_GE(ticks, 0);
    CY_CHECK_LE(ticks, fixture.program.duration().ticks());
    // 960 advances of 41,666,667 ns is 39,999,999,  ns short of exactly 960 frames — the exact
    // answer is 960,000 ticks minus nothing, wrapped ten times through a 96-frame sequence, so the
    // instant is frame 0 of the eleventh lap plus the tick the rounding produced.
    CY_CHECK_EQ(ticks % kTicksPerFrame, 0);
}

CY_TEST_CASE("sequence_playback: a completed sequence stops evaluating") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));
    for (i32 frame = 0; frame < 120; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Completed);
    fixture.batches.clear();
    CY_REQUIRE(player.advance(41666667, fixture.batches));
    CY_CHECK_EQ(fixture.batches.values.size(), 0U);
    CY_CHECK_EQ(fixture.batches.cameras.size(), 0U);
}

CY_TEST_CASE("sequence_playback: fast-forward is distinct from skipping") {
    Fixture fixture(allocator());
    CY_REQUIRE(fixture.build(mission_sequence()));
    SequencePlayer player(allocator(), fixture.registry.registry, PlaybackScope::World, 0);
    const BindingResolution bindings[] = {resolved(kLampBinding, 500),
                                          resolved(kCameraBinding, 900)};
    const Expected<InstanceId, Error> instance =
        player.create(fixture.program, fixture.request(bindings));
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));
    CY_REQUIRE(player.set_fast_forward(instance.value(), true));
    CY_CHECK(player.fast_forwarding(instance.value()));

    for (i32 frame = 0; frame < 6; ++frame) {
        fixture.batches.clear();
        CY_REQUIRE(player.advance(41666667, fixture.batches));
    }
    // Still PLAYING, four times as fast — not completed, and its values are still evaluated.
    CY_CHECK_EQ(player.status(instance.value()).value().state, InstanceState::Playing);
    CY_CHECK_EQ(player.status(instance.value()).value().time.frame(), 24);
    CY_CHECK_GT(fixture.batches.values.size(), 0U);
}
