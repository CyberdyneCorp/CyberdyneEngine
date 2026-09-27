// SPDX-License-Identifier: MIT
// `integration.game_backend_audio`: ABI 1.3's `audio_*` entries over a real `AudioServer`, through
// the interface table, with the `AudioAdapter` bound on a host. `add-swift-game-api`.
//
// OWNER: implementer C. Every case runs over the server's null backend, which is driven — a case
// mixes exactly the frames it names — so "has the voice ended" is a fact the case controls rather
// than a race with an audio thread.
//
// The two claims design.md asks this suite for: a STALE voice handle — one whose slot a later voice
// reused — is not stopped by `audio_stop`, and a RESIMULATED `audio_play` starts nothing.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

namespace {

using namespace cy;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

constexpr u32 kRate = 48000;
constexpr u32 kBlock = 120;

/// A server over the null backend, one constant clip named as the cue "explosion", a "Music" bus,
/// a scene to attach to, and the adapter bound on a host.
struct AudioWorld {
    audio::NullAudioBackend device;
    audio::AudioServer server{allocator()};
    Array<f32> samples{allocator()};
    Array<f32> output{allocator()};
    ecs::World world{system_allocator(MemoryDomain::World)};
    scene::SceneTree tree{world};
    abi::Host host{system_allocator(MemoryDomain::Scripting)};
    game_backend::AudioAdapter adapter{server, allocator(), &tree};
    audio::ClipHandle clip;
    audio::BusHandle music;

    explicit AudioWorld(u32 voice_capacity = 8) {
        audio::AudioBackendConfig device_config;
        device_config.requested.sample_rate = kRate;
        device_config.requested.layout = audio::ChannelLayout::Stereo;
        device_config.requested.buffer_frames = kBlock;
        CY_REQUIRE(device.initialize(device_config).has_value());
        audio::AudioServerConfig config;
        config.requested = device_config.requested;
        config.block_frames = kBlock;
        config.voice_capacity = voice_capacity;
        CY_REQUIRE(server.configure(config).has_value());
        CY_REQUIRE(server.initialize_with(device).has_value());
        CY_REQUIRE(output.resize(static_cast<usize>(kRate) * 2).has_value());

        CY_REQUIRE(samples.resize(kRate).has_value());  // one second
        for (f32& sample : samples) {
            sample = 0.25F;
        }
        audio::ClipDescription description;
        description.name = Name::intern("explosion.pcm");
        description.samples = samples.data();
        description.frame_count = static_cast<u32>(samples.size());
        description.sample_rate = kRate;
        const auto created = server.create_clip(description);
        CY_REQUIRE(created.has_value());
        clip = *created;
        CY_REQUIRE(adapter.add_cue(Name::intern("explosion"), clip).has_value());

        audio::BusDescription bus;
        bus.name = Name::intern("Music");
        const auto made = server.buses().create(bus);
        CY_REQUIRE(made.has_value());
        music = *made;
        CY_REQUIRE(server.buses().compile().has_value());

        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        game_backend::bind(host, &adapter);
    }

    /// Mix enough for a stop's fade to finish, then let the server reap what ended.
    void settle() {
        CY_REQUIRE_EQ(device.advance(output.data(), kBlock * 100), kBlock * 100);
        server.update(1.0F / 60.0F);
    }

    CyAudioVoice play(CyAudioPlay request = {}) {
        request.struct_size = sizeof(CyAudioPlay);
        request.cue = clip.bits();
        CyAudioVoice voice = 0;
        CY_REQUIRE_EQ(table().audio_play(&host, &request, &voice), CY_RESULT_OK);
        return voice;
    }
};

}  // namespace

CY_TEST_CASE("audio backend: cues and buses resolve by name to live handles") {
    AudioWorld fixture;
    CyAudioCue cue = 0;
    CY_REQUIRE_EQ(table().audio_find_cue(&fixture.host, "explosion", &cue), CY_RESULT_OK);
    CY_CHECK_EQ(cue, fixture.clip.bits());
    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "never-interned-cue", &cue),
                CY_RESULT_NOT_FOUND);

    CyAudioBus bus = 0;
    CY_REQUIRE_EQ(table().audio_find_bus(&fixture.host, "Music", &bus), CY_RESULT_OK);
    CY_CHECK_EQ(bus, fixture.music.bits());
    CY_REQUIRE_EQ(table().audio_find_bus(&fixture.host, "Master", &bus), CY_RESULT_OK);
    CY_CHECK_EQ(bus, fixture.server.buses().master().bits());
    CY_CHECK_EQ(table().audio_find_bus(&fixture.host, "explosion", &bus), CY_RESULT_NOT_FOUND);

    // A cue whose clip was released is gone, not pointing at whatever reuses the slot.
    fixture.server.destroy_clip(fixture.clip);
    fixture.settle();
    CY_CHECK_EQ(table().audio_find_cue(&fixture.host, "explosion", &cue), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("audio backend: a played voice plays, and a stop with no fade ends it") {
    AudioWorld fixture;
    CyAudioPlay request{};
    request.bus = fixture.music.bits();
    request.flags = CY_AUDIO_PLAY_LOOP;
    const CyAudioVoice voice = fixture.play(request);
    CY_REQUIRE_NE(voice, 0U);
    CY_CHECK(table().audio_voice_playing(&fixture.host, voice));

    CY_CHECK_EQ(table().audio_stop(&fixture.host, voice, 0.0F), CY_RESULT_OK);
    fixture.settle();
    CY_CHECK_FALSE(table().audio_voice_playing(&fixture.host, voice));
    // Idempotent: stopping what already ended is OK.
    CY_CHECK_EQ(table().audio_stop(&fixture.host, voice, 0.0F), CY_RESULT_OK);
}

CY_TEST_CASE("audio backend: a stale voice handle does not stop the voice that reused its slot") {
    AudioWorld fixture;
    const CyAudioVoice first = fixture.play();
    CY_REQUIRE_EQ(table().audio_stop(&fixture.host, first, 0.0F), CY_RESULT_OK);
    fixture.settle();
    CY_REQUIRE_FALSE(table().audio_voice_playing(&fixture.host, first));

    CyAudioPlay looping{};
    looping.flags = CY_AUDIO_PLAY_LOOP;
    const CyAudioVoice second = fixture.play(looping);
    // The same slot, a later generation: exactly the case a raw index would get wrong.
    CY_REQUIRE_EQ(audio::VoiceHandle::from_bits(second).index(),
                  audio::VoiceHandle::from_bits(first).index());
    CY_REQUIRE_NE(second, first);

    CY_CHECK_EQ(table().audio_stop(&fixture.host, first, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(table().audio_stop(&fixture.host, first, 0.5F), CY_RESULT_OK);
    fixture.adapter.update(1.0F);
    fixture.settle();
    CY_CHECK(table().audio_voice_playing(&fixture.host, second));
}

CY_TEST_CASE("audio backend: a resimulated audio_play starts nothing") {
    AudioWorld fixture;
    fixture.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    fixture.host.game.clock.flags = CY_TIME_RESIMULATING;
    CyAudioPlay request{};
    request.struct_size = sizeof(CyAudioPlay);
    request.cue = fixture.clip.bits();
    CyAudioVoice voice = 99;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_OK);
    CY_CHECK_EQ(voice, 0U);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 0U);

    // The same call on the live tick plays.
    fixture.host.game.clock.flags = 0;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_OK);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 1U);
}

CY_TEST_CASE("audio backend: a fade-out stops the voice only once the fade has run") {
    AudioWorld fixture;
    CyAudioPlay looping{};
    looping.flags = CY_AUDIO_PLAY_LOOP;
    const CyAudioVoice voice = fixture.play(looping);
    CY_REQUIRE_EQ(table().audio_stop(&fixture.host, voice, 0.2F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.adapter.tracked_voices(), 1U);

    fixture.adapter.update(0.1F);
    fixture.settle();
    CY_CHECK(table().audio_voice_playing(&fixture.host, voice));

    fixture.adapter.update(0.1F);
    fixture.settle();
    CY_CHECK_FALSE(table().audio_voice_playing(&fixture.host, voice));
    CY_CHECK_EQ(fixture.adapter.tracked_voices(), 0U);
}

CY_TEST_CASE("audio backend: a bus volume ramps over its fade, and a zero fade sets it at once") {
    AudioWorld fixture;
    const CyAudioBus music = fixture.music.bits();
    CY_REQUIRE_EQ(table().audio_set_bus_volume(&fixture.host, music, 0.0F, 1.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.server.buses().description(fixture.music)->volume, 1.0F);
    fixture.adapter.update(0.5F);
    CY_CHECK_NEAR(fixture.server.buses().description(fixture.music)->volume, 0.5F, 1e-6F);
    fixture.adapter.update(0.5F);
    CY_CHECK_EQ(fixture.server.buses().description(fixture.music)->volume, 0.0F);

    CY_REQUIRE_EQ(table().audio_set_bus_volume(&fixture.host, music, 0.75F, 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.server.buses().description(fixture.music)->volume, 0.75F);

    CY_CHECK_EQ(table().audio_set_bus_volume(&fixture.host, 0x7777'0000'0003ULL, 1.0F, 0.0F),
                CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("audio backend: an attached voice follows a live node and refuses a dead one") {
    AudioWorld fixture;
    const auto node = fixture.tree.create_node(Name::intern("Tank"), fixture.tree.root());
    CY_REQUIRE(node.has_value());
    CyAudioPlay attached{};
    attached.flags = CY_AUDIO_PLAY_ATTACH | CY_AUDIO_PLAY_LOOP;
    attached.attach_to = abi::to_abi(node->entity());
    const CyAudioVoice voice = fixture.play(attached);
    CY_CHECK(table().audio_voice_playing(&fixture.host, voice));
    CY_CHECK_EQ(fixture.adapter.tracked_voices(), 1U);
    fixture.adapter.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.adapter.tracked_voices(), 1U);

    // The entity dies: the voice keeps playing where it was and is no longer followed.
    CY_REQUIRE(fixture.tree.destroy_node(*node).has_value());
    fixture.adapter.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.adapter.tracked_voices(), 0U);
    CY_CHECK(table().audio_voice_playing(&fixture.host, voice));

    attached.struct_size = sizeof(CyAudioPlay);
    attached.cue = fixture.clip.bits();
    CY_CHECK_EQ(table().audio_play(&fixture.host, &attached, nullptr), CY_RESULT_NOT_FOUND);
    abi::clear_last_error();
}

CY_TEST_CASE("audio backend: with no free voice, play is OK with a voice that is not playing") {
    AudioWorld fixture(2);
    CyAudioPlay looping{};
    looping.flags = CY_AUDIO_PLAY_LOOP;
    CY_REQUIRE_NE(fixture.play(looping), 0U);
    CY_REQUIRE_NE(fixture.play(looping), 0U);
    const CyAudioVoice lost = fixture.play(looping);
    CY_CHECK_EQ(lost, 0U);
    CY_CHECK_FALSE(table().audio_voice_playing(&fixture.host, lost));
}

CY_TEST_CASE("audio backend: a stale cue or bus in a request is NOT_FOUND and plays nothing") {
    AudioWorld fixture;
    CyAudioPlay request{};
    request.struct_size = sizeof(CyAudioPlay);
    request.cue = 0;
    CyAudioVoice voice = 5;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(voice, 5U);
    request.cue = fixture.clip.bits();
    request.bus = 0x7777'0000'0003ULL;
    CY_CHECK_EQ(table().audio_play(&fixture.host, &request, &voice), CY_RESULT_NOT_FOUND);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 0U);
    abi::clear_last_error();
}
