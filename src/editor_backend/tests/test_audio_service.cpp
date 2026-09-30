// SPDX-License-Identifier: MIT
// The editor's audio tools, engine side (#29): what the server holds after the editor's requests.
//
// Every case below drives the service through the C ABI's `service_*` entries — the seam the
// window runtime's bridge forwards `audio.*` requests to — and then reads the ANSWER OUT OF THE
// SERVER: a bus's gain in the graph, a voice the mixer is playing, a level the mix produced. The
// editor-side suites assert what the editor shows; these assert that it is true.
//
// THE WIRE, FROM BOTH SIDES. `data/audio_mixer_v1.cymixer`, `data/audio_cue_v1.cycue` and
// `data/audio_cue_preview_v1.wire` are what the Rust editor's encoders write (its tests compare
// them byte for byte). This suite submits exactly those bytes. `data/audio_state_v1.wire` is what
// the engine answers; the Rust suites decode it and replay it to an MCP client as the runtime's
// reply. Regenerate it with `CY_UPDATE_AUDIO_WIRE=1` after a deliberate change to the reply.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/editor/audio_authoring.h>
#include <cy/editor/audio_service.h>
#include <cy/editor/material_service.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u8;
using cy::usize;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CY_REQUIRE(input.good());
    const auto size = static_cast<std::size_t>(std::filesystem::file_size(path));
    std::string text(size, '\0');
    CY_REQUIRE(input.read(text.data(), static_cast<std::streamsize>(size)).good());
    return text;
}

std::filesystem::path wire(std::string_view name) {
    return std::filesystem::path(CY_AUDIO_WIRE_DIR) / name;
}

std::vector<u8> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

void append_u32(std::vector<u8>& bytes, u32 value) {
    for (u32 index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<u8>((value >> (index * 8)) & 0xffU));
    }
}

void append_f32(std::vector<u8>& bytes, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

void append_text(std::vector<u8>& bytes, std::string_view value) {
    append_u32(bytes, static_cast<u32>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

/// A cue-preview payload, for cases that need a placement the fixture does not have.
std::vector<u8> preview_payload(std::string_view name, std::string_view cue, bool spatial,
                                cy::Vec3 position) {
    std::vector<u8> bytes;
    append_text(bytes, name);
    append_text(bytes, cue);
    bytes.push_back(spatial ? 1 : 0);
    for (f32 lane : {position.x, position.y, position.z, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F}) {
        append_f32(bytes, lane);
    }
    append_f32(bytes, 1.0F);
    append_f32(bytes, 50.0F);
    append_text(bytes, "inverse");
    return bytes;
}

/// The state reply, decoded the way the editor decodes it.
struct BusState {
    std::string name;
    std::string output;
    f32 volume = 0.0F;
    bool mute = false;
    bool solo = false;
    bool bypass = false;
    bool audible = false;
    std::vector<std::pair<std::string, f32>> sends;
    std::vector<std::string> effects;
    f32 peak = 0.0F;
    f32 rms = 0.0F;
};

struct State {
    std::string backend;
    u32 sample_rate = 0;
    u32 active_voices = 0;
    u32 virtual_voices = 0;
    u32 blocks_mixed = 0;
    bool playing = false;
    u32 play_voices = 0;
    std::vector<BusState> buses;
    std::vector<std::string> cues;
    bool preview = false;
    std::string preview_cue;
    bool preview_playing = false;
    f32 distance = 0.0F;
    f32 gain = 0.0F;
    f32 left = 0.0F;
    f32 right = 0.0F;

    [[nodiscard]] const BusState* bus(std::string_view wanted) const {
        for (const BusState& state : buses) {
            if (state.name == wanted) {
                return &state;
            }
        }
        return nullptr;
    }
};

class Cursor {
public:
    Cursor(const u8* bytes, usize size) : bytes_(bytes), size_(size) {}

    u32 u32v() {
        CY_REQUIRE(at_ + 4 <= size_);
        u32 value = 0;
        for (usize byte = 0; byte < 4; ++byte) {
            value |= static_cast<u32>(bytes_[at_ + byte]) << (byte * 8);
        }
        at_ += 4;
        return value;
    }
    bool flag() {
        CY_REQUIRE(at_ < size_);
        return bytes_[at_++] != 0;
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string text() {
        const u32 length = u32v();
        CY_REQUIRE(at_ + length <= size_);
        std::string value(reinterpret_cast<const char*>(bytes_ + at_), length);
        at_ += length;
        return value;
    }
    [[nodiscard]] bool finished() const { return at_ == size_; }

private:
    const u8* bytes_;
    usize size_;
    usize at_ = 0;
};

BusState decode_bus(Cursor& cursor) {
    BusState bus;
    bus.name = cursor.text();
    bus.output = cursor.text();
    bus.volume = cursor.f32v();
    bus.mute = cursor.flag();
    bus.solo = cursor.flag();
    bus.bypass = cursor.flag();
    bus.audible = cursor.flag();
    for (u32 send = cursor.u32v(); send > 0; --send) {
        std::string target = cursor.text();
        bus.sends.emplace_back(std::move(target), cursor.f32v());
    }
    for (u32 effect = cursor.u32v(); effect > 0; --effect) {
        bus.effects.push_back(cursor.text());
        (void)cursor.f32v();
        (void)cursor.f32v();
        (void)cursor.flag();
    }
    bus.peak = cursor.f32v();
    bus.rms = cursor.f32v();
    return bus;
}

State decode_state(const u8* bytes, usize size) {
    Cursor cursor(bytes, size);
    State state;
    CY_REQUIRE_EQ(cursor.u32v(), 1U);
    state.backend = cursor.text();
    state.sample_rate = cursor.u32v();
    state.active_voices = cursor.u32v();
    state.virtual_voices = cursor.u32v();
    state.blocks_mixed = cursor.u32v();
    state.playing = cursor.flag();
    state.play_voices = cursor.u32v();
    for (u32 bus = cursor.u32v(); bus > 0; --bus) {
        state.buses.push_back(decode_bus(cursor));
    }
    for (u32 cue = cursor.u32v(); cue > 0; --cue) {
        state.cues.push_back(cursor.text());
    }
    state.preview = cursor.flag();
    if (state.preview) {
        state.preview_cue = cursor.text();
        state.preview_playing = cursor.flag();
        state.distance = cursor.f32v();
        state.gain = cursor.f32v();
        state.left = cursor.f32v();
        state.right = cursor.f32v();
    }
    CY_CHECK(cursor.finished());
    return state;
}

/// One service session over the C ABI, with the host's audio server behind it.
struct Service {
    cy::editor::AudioAuthoring audio{allocator()};
    cy::editor::MaterialService service{allocator()};
    cy::abi::Host host{allocator()};
    const CyInterface* api = nullptr;
    CyServiceSession session = nullptr;
    cy::u64 request = 1;

    Service() {
        CY_REQUIRE(audio.initialize());
        service.set_audio(&audio);
        host.bind_editor_service(&service);
        api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
        CY_REQUIRE(api != nullptr);
        CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    }

    ~Service() { api->service_close(&host, session); }
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    /// Submit and poll one request, keeping a copy of the event's payload.
    std::pair<u32, std::vector<u8>> call(const char* operation, const std::vector<u8>& payload) {
        const CyServiceRequest submitted{
            sizeof(CyServiceRequest), 1, request++, operation, payload.data(), payload.size()};
        CY_REQUIRE_EQ(api->service_submit(&host, session, &submitted), CY_RESULT_OK);
        CyServiceEvent event{};
        bool present = false;
        CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
        CY_REQUIRE(present);
        return {event.kind, {event.payload, event.payload + event.payload_size}};
    }

    State state(const char* operation, const std::vector<u8>& payload) {
        const auto [kind, bytes] = call(operation, payload);
        CY_REQUIRE_EQ(kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
        return decode_state(bytes.data(), bytes.size());
    }

    State advance(f32 seconds) {
        std::vector<u8> payload;
        append_f32(payload, seconds);
        return state("audio.state.get", payload);
    }
};

std::string failure_code(const std::vector<u8>& payload) {
    Cursor cursor(payload.data(), payload.size());
    CY_REQUIRE_EQ(cursor.u32v(), 1U);
    return cursor.text();
}

/// Rewrite a committed reply under `CY_UPDATE_AUDIO_WIRE=1`, then read back what is committed.
std::string committed_reply(std::string_view name, const std::vector<u8>& reply) {
    const char* update = std::getenv("CY_UPDATE_AUDIO_WIRE");
    if (update != nullptr && std::string_view(update) == "1") {
        std::ofstream(wire(name), std::ios::binary)
            .write(reinterpret_cast<const char*>(reply.data()),
                   static_cast<std::streamsize>(reply.size()));
    }
    return read_file(wire(name));
}

constexpr std::string_view kTone = "cycue 1\nclip tone:440:0.5\nbus Music\nvolume 1\n";

bool near(f32 left, f32 right, f32 tolerance = 1e-4F) {
    return std::fabs(left - right) <= tolerance;
}

}  // namespace

CY_TEST_CASE("editor audio: a mixer is refused whole when it would cycle, dangle or overload") {
    const auto refused = [](std::string_view text) {
        return !cy::editor::parse_mixer(text).has_value();
    };
    constexpr std::string_view kMaster = "cymixer 1\nbus Master - 1 0 0 0\n";
    CY_CHECK_FALSE(refused(kMaster));
    CY_CHECK(refused("cymixer 2\nbus Master - 1 0 0 0\n"));
    CY_CHECK(refused("cymixer 1\nbus Music Master 1 0 0 0\nbus Master - 1 0 0 0\n"));
    CY_CHECK(refused(std::string(kMaster) + "bus Music Nowhere 1 0 0 0\n"));
    CY_CHECK(refused(std::string(kMaster) + "bus Music Master 5 0 0 0\n"));
    CY_CHECK(
        refused(std::string(kMaster) + "bus Music Master 1 0 0 0\nbus Music Master 1 0 0 0\n"));
    CY_CHECK(
        refused(std::string(kMaster) + "bus Music Master 1 0 0 0\neffect Music reverb 0 0 0\n"));
    // A send back up the chain is the cycle the bus graph could only refuse half-way through.
    CY_CHECK(
        refused(std::string(kMaster) + "bus A Master 1 0 0 0\nbus B A 1 0 0 0\nsend A B 0.5\n"));
    CY_CHECK_FALSE(
        refused(std::string(kMaster) + "bus A Master 1 0 0 0\nbus B A 1 0 0 0\nsend B A 0.5\n"));

    Service service;
    const auto [kind, payload] = service.call(
        "audio.mixer.apply",
        bytes_of(std::string(kMaster) + "bus A Master 1 0 0 0\nbus B A 1 0 0 0\nsend A B 0.5\n"));
    CY_CHECK_EQ(kind, static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(failure_code(payload), "audio.mixer");
    CY_CHECK_EQ(service.audio.server().buses().size(), 1U);
}

CY_TEST_CASE("editor audio: an asset's numbers are plain decimals, read whole") {
    const auto volume = [](std::string_view number) -> std::optional<f32> {
        const auto cue = cy::editor::parse_cue("cycue 1\nclip tone:440:0.5\nvolume " +
                                               std::string(number) + "\n");
        return cue.has_value() ? std::optional<f32>(cue->volume) : std::nullopt;
    };
    CY_CHECK_EQ(volume("0.25").value_or(-1.0F), 0.25F);
    CY_CHECK_EQ(volume("2").value_or(-1.0F), 2.0F);
    CY_CHECK_EQ(volume("2.5e-1").value_or(-1.0F), 0.25F);
    CY_CHECK_EQ(volume("-0").value_or(-1.0F), 0.0F);
    for (const std::string_view malformed : {"", ".", "+1", " 1", "1 ", "1x", "0x1", "inf", "nan",
                                             "1e", "1,5", "1e999", "1e-50", "1-2", "-"}) {
        CY_CHECK_FALSE(volume(malformed).has_value());
    }

    const auto mixer = cy::editor::parse_mixer("cymixer 1\nbus Master - 0.75 0 0 0\n");
    CY_REQUIRE(mixer.has_value());
    CY_CHECK_EQ(mixer->buses.front().volume, 0.75F);
}

CY_TEST_CASE("editor audio: the editor's mixer sets the engine's gains, routes and effect chain") {
    Service service;
    const State applied =
        service.state("audio.mixer.apply", bytes_of(read_file(wire("audio_mixer_v1.cymixer"))));
    CY_REQUIRE_EQ(applied.buses.size(), 4U);

    const cy::audio::BusGraph& graph = service.audio.server().buses();
    const cy::audio::BusHandle music = service.audio.bus("Music");
    const cy::audio::BusHandle sfx = service.audio.bus("SFX");
    const cy::audio::BusHandle reverb = service.audio.bus("Reverb");
    CY_REQUIRE(graph.alive(music));
    CY_REQUIRE(graph.alive(sfx));
    CY_CHECK(graph.compiled());
    CY_CHECK_EQ(graph.description(music)->volume, 0.5F);
    CY_CHECK_EQ(graph.description(music)->output, graph.master());
    CY_REQUIRE_EQ(graph.sends(sfx).size(), 1U);
    CY_CHECK_EQ(graph.sends(sfx)[0].target, reverb);
    CY_CHECK_EQ(graph.sends(sfx)[0].level, 0.3F);
    CY_REQUIRE_EQ(graph.effects(sfx).size(), 1U);
    CY_CHECK_EQ(graph.effects(sfx)[0].kind, cy::audio::EffectKind::LowPass);
    CY_REQUIRE_EQ(graph.effects(graph.master()).size(), 1U);
    CY_CHECK_EQ(graph.effects(graph.master())[0].kind, cy::audio::EffectKind::Limiter);
    // And the reply says what the graph holds, not what was sent.
    CY_CHECK_EQ(applied.bus("Music")->volume, 0.5F);
    CY_CHECK_EQ(applied.bus("SFX")->sends.at(0).first, "Reverb");
    CY_CHECK_EQ(applied.bus("SFX")->effects.at(0), "low-pass");

    // The gain is the mixer's: a full-scale tone at half volume peaks at half on Master.
    (void)service.state("audio.cue.preview",
                        preview_payload("audio/cues/tone.cycue", kTone, false, {}));
    const State loud = service.advance(0.1F);
    const f32 loud_peak = loud.bus("Music")->peak;
    CY_CHECK_GT(loud_peak, 0.2F);
    CY_CHECK_LT(loud_peak, 0.26F);
    CY_CHECK(near(loud.bus("Master")->peak, loud_peak, 1e-3F));

    // Re-applying keeps every bus that keeps its name: the voice goes on, now at a quarter.
    std::string quieter = read_file(wire("audio_mixer_v1.cymixer"));
    const usize line = quieter.find("bus Music Master 0.5");
    CY_REQUIRE(line != std::string::npos);
    quieter.replace(line, std::string_view("bus Music Master 0.5").size(), "bus Music Master 0.25");
    (void)service.state("audio.mixer.apply", bytes_of(quieter));
    CY_CHECK_EQ(service.audio.bus("Music"), music);
    const State quiet = service.advance(0.1F);
    CY_CHECK_EQ(quiet.active_voices, 1U);
    CY_CHECK(near(quiet.bus("Music")->peak, loud_peak * 0.5F, 2e-3F));

    // Removing the bus stops what played on it rather than leaving it routed nowhere.
    (void)service.state("audio.mixer.apply",
                        bytes_of("cymixer 1\nbus Master - 1 0 0 0\nbus SFX Master 1 0 0 0\n"));
    CY_CHECK_FALSE(graph.alive(music));
    const State removed = service.advance(0.05F);
    CY_CHECK_EQ(removed.active_voices, 0U);
    CY_CHECK_EQ(removed.buses.size(), 2U);
}

CY_TEST_CASE(
    "editor audio: previewing a cue starts an engine voice with the engine's attenuation") {
    Service service;
    (void)service.state("audio.mixer.apply", bytes_of(read_file(wire("audio_mixer_v1.cymixer"))));
    const std::string preview = read_file(wire("audio_cue_preview_v1.wire"));
    const State started = service.state("audio.cue.preview", bytes_of(preview));
    CY_CHECK(started.preview);
    CY_CHECK_EQ(started.preview_cue, "audio/cues/ping.cycue");
    CY_CHECK(started.preview_playing);
    CY_CHECK_EQ(started.cues, std::vector<std::string>{"audio/cues/ping.cycue"});
    // Three metres to the listener's right, inverse from one metre: a third, all in the right ear.
    CY_CHECK(near(started.distance, 3.0F));
    CY_CHECK(near(started.gain, 1.0F / 3.0F));
    CY_CHECK_GT(started.right, 0.99F);
    CY_CHECK_LT(started.left, 0.01F);

    const State mixed = service.advance(0.1F);
    CY_CHECK_EQ(mixed.active_voices, 1U);
    CY_CHECK_GT(mixed.bus("SFX")->peak, 0.0F);
    CY_CHECK_GT(mixed.bus("Reverb")->peak, 0.0F);

    // The nearer source is louder: the same cue at one metre mixes above the one at three.
    const State near_source =
        service.state("audio.cue.preview", preview_payload("audio/cues/ping.cycue",
                                                           read_file(wire("audio_cue_v1.cycue")),
                                                           true, {1.0F, 0.0F, 0.0F}));
    CY_CHECK(near(near_source.gain, 1.0F));
    const State near_mix = service.advance(0.1F);
    CY_CHECK_GT(near_mix.bus("SFX")->peak, mixed.bus("SFX")->peak * 2.0F);

    // Past the silence radius the engine's curve is zero, and so is the mix.
    const State beyond =
        service.state("audio.cue.preview", preview_payload("audio/cues/ping.cycue",
                                                           read_file(wire("audio_cue_v1.cycue")),
                                                           true, {60.0F, 0.0F, 0.0F}));
    CY_CHECK_EQ(beyond.gain, 0.0F);
    CY_CHECK(beyond.preview_playing);
    (void)service.advance(0.05F);
    CY_CHECK_EQ(service.advance(0.05F).bus("SFX")->peak, 0.0F);

    const State stopped = service.state("audio.preview.stop", {});
    CY_CHECK_FALSE(stopped.preview_playing);
    CY_CHECK_EQ(service.advance(0.05F).active_voices, 0U);

    // The committed reply the editor's suites decode is this engine's answer to that request.
    Service fixture;
    (void)fixture.state("audio.mixer.apply", bytes_of(read_file(wire("audio_mixer_v1.cymixer"))));
    (void)fixture.state("audio.cue.preview", bytes_of(preview));
    std::vector<u8> advance;
    append_f32(advance, 0.1F);
    const auto [kind, reply] = fixture.call("audio.state.get", advance);
    CY_REQUIRE_EQ(kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    const std::string committed = committed_reply("audio_state_v1.wire", reply);
    const State expected =
        decode_state(reinterpret_cast<const u8*>(committed.data()), committed.size());
    const State actual = decode_state(reply.data(), reply.size());
    CY_CHECK_EQ(actual.backend, expected.backend);
    CY_CHECK_EQ(actual.active_voices, expected.active_voices);
    CY_CHECK_EQ(actual.cues, expected.cues);
    CY_REQUIRE_EQ(actual.buses.size(), expected.buses.size());
    for (usize index = 0; index < actual.buses.size(); ++index) {
        CY_CHECK_EQ(actual.buses[index].name, expected.buses[index].name);
        CY_CHECK_EQ(actual.buses[index].output, expected.buses[index].output);
        CY_CHECK_EQ(actual.buses[index].volume, expected.buses[index].volume);
        CY_CHECK(near(actual.buses[index].peak, expected.buses[index].peak, 1e-3F));
    }
    CY_CHECK(near(actual.gain, expected.gain));
}

CY_TEST_CASE("editor audio: without an audio server every audio operation is refused by name") {
    cy::abi::Host host(allocator());
    cy::editor::MaterialService service(allocator());
    host.bind_editor_service(&service);
    const CyInterface* api = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(api != nullptr);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(api->service_open(&host, &session), CY_RESULT_OK);
    const CyServiceRequest request{sizeof(CyServiceRequest), 1, 1, "audio.state.get", nullptr, 0};
    CY_REQUIRE_EQ(api->service_submit(&host, session, &request), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(api->service_poll(&host, session, &event, &present), CY_RESULT_OK);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(failure_code({event.payload, event.payload + event.payload_size}),
                "audio.unavailable");
    api->service_close(&host, session);
}

CY_TEST_CASE("editor audio: the vocabulary the mixer editor offers is the server's") {
    Service service;
    const auto [kind, payload] = service.call("audio.capabilities.get", {});
    CY_REQUIRE_EQ(kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    Cursor cursor(payload.data(), payload.size());
    CY_CHECK_EQ(cursor.u32v(), 1U);
    CY_CHECK_EQ(cursor.text(), "null");
    CY_CHECK_EQ(cursor.u32v(), cy::editor::AudioAuthoring::kSampleRate);
    CY_CHECK_EQ(cursor.u32v(), 32U);
    CY_CHECK_EQ(cursor.u32v(), cy::audio::BusGraph::kMaxSends);
    CY_CHECK_EQ(cursor.u32v(), cy::audio::BusGraph::kMaxEffects);
    std::vector<std::string> effects;
    for (u32 count = cursor.u32v(); count > 0; --count) {
        effects.push_back(cursor.text());
        (void)cursor.text();
        (void)cursor.text();
        (void)cursor.f32v();
        (void)cursor.f32v();
    }
    CY_CHECK_EQ(effects, (std::vector<std::string>{"gain", "low-pass", "high-pass", "limiter"}));
    for (const std::string& effect : effects) {
        CY_CHECK_NE(cy::editor::effect_kind_named(effect), cy::audio::EffectKind::Count);
    }
    std::vector<std::string> models;
    for (u32 count = cursor.u32v(); count > 0; --count) {
        models.push_back(cursor.text());
    }
    CY_CHECK_EQ(models,
                (std::vector<std::string>{"inverse", "inverse-square", "linear", "logarithmic"}));
    CY_CHECK(cursor.finished());
    // No mixing goes into this reply, so it is compared byte for byte.
    CY_CHECK(committed_reply("audio_capabilities_v1.wire", payload) ==
             std::string(payload.begin(), payload.end()));
}

namespace {

/// A project on disk with one cue and a world with two sources, one of them silent at Play.
struct PlayProject {
    std::filesystem::path root;

    PlayProject() {
        root = std::filesystem::path(CY_TEST_BINARY_DIR) / "editor-audio-play";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "audio/cues");
        std::filesystem::create_directories(root / "game/audio");
        std::ofstream(root / "audio/cues/hum.cycue")
            << "cycue 1\nclip tone:220:0.5\nbus Master\nlooping 1\n";
        std::ofstream(root / "audio/cues/boom.cycue") << "cycue 1\nclip tone:80:0.2\nbus Master\n";
        std::ofstream(root / "game/audio/mixer.cymixer")
            << "cymixer 1\nbus Master - 1 0 0 0\nbus Ambience Master 0.25 0 0 0\n";
        std::ofstream(root / "audio/cues/wind.cycue")
            << "cycue 1\nclip tone:300:0.5\nbus Ambience\nlooping 1\n";
    }
    ~PlayProject() { std::filesystem::remove_all(root); }
    PlayProject(const PlayProject&) = delete;
    PlayProject& operator=(const PlayProject&) = delete;
};

constexpr std::string_view kWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "cy::audio::AudioSource"
  field 4 text "cue" ""
  field 5 float "min_distance" ""
  field 6 float "max_distance" ""
  field 7 text "attenuation" ""
  field 8 bool "autoplay" ""
  field 9 bool "enabled" ""
node 0 - "audio" "Generator"
  component 1
    field 1 0 0 0 1
    field 2 2 0 0
    field 3 1 1 1
  component 2
    field 4 "audio/cues/hum.cycue"
    field 5 1
    field 6 30
    field 7 "linear"
    field 8 true
    field 9 true
node 1 - "audio" "Quiet"
  component 1
    field 1 0 0 0 1
    field 2 0 0 5
    field 3 1 1 1
  component 2
    field 4 "audio/cues/hum.cycue"
    field 5 2
    field 6 12
    field 7 "inverse"
    field 8 false
    field 9 true
)";

/// Read a world and resolve it against the engine's scene types, as the runtime's world is.
void load_world(std::string_view text, cy::scene::serialization::World& world) {
    CY_REQUIRE(cy::scene::serialization::read_world(text, "worlds/audio.cyworld", world));
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_scene_types(registry));
    cy::scene::serialization::AuthoringSchema schema(allocator());
    CY_REQUIRE(cy::scene::serialization::build_authoring_schema(registry, schema));
    CY_REQUIRE(cy::scene::serialization::resolve_against(world, schema));
}

}  // namespace

CY_TEST_CASE("editor audio: the world's sources are read with their attenuation radii") {
    cy::scene::serialization::World world(allocator());
    load_world(kWorld, world);
    const auto sources = cy::editor::read_emitters(world);
    CY_REQUIRE(sources.has_value());
    CY_REQUIRE_EQ(sources->size(), 2U);
    CY_CHECK_EQ((*sources)[0].position.x, 2.0F);
    CY_CHECK_EQ((*sources)[0].max_distance, 30.0F);
    CY_CHECK_EQ((*sources)[0].model, cy::audio::AttenuationModel::Linear);
    CY_CHECK_EQ((*sources)[1].min_distance, 2.0F);
    CY_CHECK_FALSE((*sources)[1].autoplay);

    std::string inverted(kWorld);
    const usize field = inverted.find("field 6 30");
    CY_REQUIRE(field != std::string::npos);
    inverted.replace(field, std::string_view("field 6 30").size(), "field 6 0.5");
    cy::scene::serialization::World broken(allocator());
    load_world(inverted, broken);
    CY_CHECK_FALSE(cy::editor::read_emitters(broken).has_value());
}

CY_TEST_CASE(
    "editor audio: Play starts the autoplay sources, Swift finds every cue, Stop ends it") {
    PlayProject project;
    cy::editor::AudioAuthoring audio(allocator());
    CY_REQUIRE(audio.initialize());
    audio.set_project(project.root.string());
    cy::scene::serialization::World world(allocator());
    load_world(kWorld, world);

    CY_REQUIRE(audio.start_play(world));
    CY_CHECK(audio.playing());
    CY_CHECK_EQ(audio.play_voices(), 1U);
    audio.set_listener({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    audio.pump(0.1F);
    CY_CHECK_EQ(audio.server().statistics().active_voices, 1U);
    const cy::u32 master = audio.server().buses().index_of(audio.server().buses().master());
    CY_CHECK_GT(audio.server().bus_levels()[master].peak, 0.1F);

    // The reply the editor's suites decode as "Play is sounding".
    cy::Array<u8> encoded(allocator());
    CY_REQUIRE(audio.encode_state(encoded));
    const std::vector<u8> reply(encoded.begin(), encoded.end());
    const std::string committed = committed_reply("audio_state_play_v1.wire", reply);
    const State expected =
        decode_state(reinterpret_cast<const u8*>(committed.data()), committed.size());
    const State actual = decode_state(reply.data(), reply.size());
    CY_CHECK(actual.playing);
    CY_CHECK_EQ(actual.play_voices, expected.play_voices);
    CY_CHECK_EQ(actual.active_voices, expected.active_voices);
    std::vector<std::string> actual_cues = actual.cues;
    std::vector<std::string> expected_cues = expected.cues;
    std::ranges::sort(actual_cues);
    std::ranges::sort(expected_cues);
    CY_CHECK_EQ(actual_cues, expected_cues);
    CY_CHECK(near(actual.bus("Master")->peak, expected.bus("Master")->peak, 1e-3F));

    // ABI 1.3's `audio_find_cue` during Play: a cue no source references is still there by name.
    CyAudioCue boom = 0;
    CY_CHECK_EQ(audio.adapter().find_cue("boom", boom), CY_RESULT_OK);
    CyAudioPlay play{};
    play.struct_size = sizeof(CyAudioPlay);
    play.cue = boom;
    CyAudioVoice voice = 0;
    CY_REQUIRE_EQ(audio.adapter().play(play, voice), CY_RESULT_OK);
    CY_CHECK(audio.adapter().playing(voice));
    audio.pump(0.02F);
    CY_CHECK_EQ(audio.server().statistics().active_voices, 2U);

    // Play mixed through the project's own mixer, which nothing had sent: its bus is there.
    CY_CHECK(audio.server().buses().alive(audio.bus("Ambience")));
    CY_CHECK_EQ(audio.server().buses().description(audio.bus("Ambience"))->volume, 0.25F);

    // The looping source is still playing half a second in; Stop ends it.
    audio.pump(0.6F);
    CY_CHECK_EQ(audio.play_voices(), 1U);
    audio.stop_play();
    CY_CHECK_FALSE(audio.playing());
    audio.pump(0.05F);
    CY_CHECK_EQ(audio.server().statistics().active_voices, 0U);
}

CY_TEST_CASE("editor audio: a WAV clip is read from the project, and a wrong rate is refused") {
    const std::filesystem::path root =
        std::filesystem::path(CY_TEST_BINARY_DIR) / "editor-audio-wav";
    std::filesystem::create_directories(root / "sounds");
    const auto write_wav = [&](const char* name, u32 rate) {
        std::vector<u8> bytes;
        const auto chunk = [&](std::string_view id) {
            bytes.insert(bytes.end(), id.begin(), id.end());
        };
        const u32 frames = 4800;
        chunk("RIFF");
        append_u32(bytes, 36 + (frames * 2));
        chunk("WAVE");
        chunk("fmt ");
        append_u32(bytes, 16);
        bytes.insert(bytes.end(), {1, 0, 1, 0});  // PCM, mono
        append_u32(bytes, rate);
        append_u32(bytes, rate * 2);
        bytes.insert(bytes.end(), {2, 0, 16, 0});
        chunk("data");
        append_u32(bytes, frames * 2);
        for (u32 frame = 0; frame < frames; ++frame) {
            bytes.insert(bytes.end(), {0x00, 0x40});  // 16384: half scale
        }
        std::ofstream(root / "sounds" / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    };
    write_wav("half.wav", 48000);
    write_wav("slow.wav", 22050);

    cy::editor::AudioAuthoring audio(allocator());
    CY_REQUIRE(audio.initialize());
    audio.set_project(root.string());
    CY_REQUIRE(audio.load_cue("sounds/half.cycue", "cycue 1\nclip sounds/half.wav\n"));
    CY_REQUIRE(audio.preview("sounds/half.cycue", {}));
    // Two blocks: the first ramps the voice's gain up from silence, the second is at full gain.
    audio.pump(0.01F);
    const cy::u32 master = audio.server().buses().index_of(audio.server().buses().master());
    CY_CHECK(near(audio.server().bus_levels()[master].peak, 0.5F, 1e-3F));
    CY_CHECK_FALSE(audio.load_cue("sounds/slow.cycue", "cycue 1\nclip sounds/slow.wav\n"));
    CY_CHECK_FALSE(audio.load_cue("sounds/out.cycue", "cycue 1\nclip ../outside.wav\n"));
    std::filesystem::remove_all(root);
}
