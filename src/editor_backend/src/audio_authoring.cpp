// SPDX-License-Identifier: MIT
// The engine half of the editor's audio tools. See cy/editor/audio_authoring.h.

#include <cy/editor/audio_authoring.h>

#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/scene/serialization/worldfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <sstream>
#include <utility>

namespace cy::editor {
namespace {

namespace ser = scene::serialization;

constexpr usize kMaxNameLength = 48;
constexpr usize kMaxBuses = 32;
constexpr usize kMaxCues = 256;
constexpr f32 kMaxBusVolume = 4.0F;
constexpr u32 kToneChannels = 1;

// --- Text
// -----------------------------------------------------------------------------------------

/// Split one line on single spaces. Assets are written by the editor, so a doubled space is a
/// malformed file rather than something to tolerate.
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    usize start = 0;
    while (start <= line.size()) {
        const usize end = std::min(line.find(' ', start), line.size());
        out.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    usize start = 0;
    while (start < text.size()) {
        const usize end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            out.push_back(line);
        }
        start = end + 1;
    }
    return out;
}

/// A plain decimal: optional minus, digits, point and exponent. Apple's libc++ has no
/// floating-point `std::from_chars`, so the digits go through `strtof`, which would otherwise also
/// take leading blanks, a plus sign, hex, `inf` and `nan` that the asset format never writes. A
/// value too large or too small for an `f32` is refused, as `from_chars` would.
bool parse_float(std::string_view word, f32& out) noexcept {
    constexpr usize kCapacity = 32;
    if (word.empty() || word.size() >= kCapacity || word.front() == '+' ||
        word.find_first_not_of("0123456789+-.eE") != std::string_view::npos) {
        return false;
    }
    std::array<char, kCapacity> terminated{};
    std::memcpy(terminated.data(), word.data(), word.size());
    char* end = nullptr;
    errno = 0;
    const f32 value = std::strtof(terminated.data(), &end);
    if (end != terminated.data() + word.size() || errno == ERANGE || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_flag(std::string_view word, bool& out) noexcept {
    if (word == "0" || word == "1") {
        out = word == "1";
        return true;
    }
    return false;
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxNameLength) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.';
    });
}

Error refuse(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message};
}

// --- The mixer asset
// ------------------------------------------------------------------------------

MixerBus* bus_named(Mixer& mixer, std::string_view name) noexcept {
    for (MixerBus& bus : mixer.buses) {
        if (bus.name == name) {
            return &bus;
        }
    }
    return nullptr;
}

const MixerBus* bus_named(const Mixer& mixer, std::string_view name) noexcept {
    for (const MixerBus& bus : mixer.buses) {
        if (bus.name == name) {
            return &bus;
        }
    }
    return nullptr;
}

Status parse_bus_line(const std::vector<std::string_view>& word, Mixer& mixer) noexcept {
    if (word.size() != 7) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: a bus line is `bus <name> <output> <volume> <mute> <solo> "
                    "<bypass>`");
    }
    MixerBus bus;
    bus.name = std::string(word[1]);
    if (!valid_name(word[1]) || bus_named(mixer, word[1]) != nullptr) {
        return fail(ErrorCode::InvalidArgument, "audio mixer: a bus name is invalid or repeated");
    }
    bus.output = word[2] == "-" ? std::string() : std::string(word[2]);
    if (!parse_float(word[3], bus.volume) || bus.volume < 0.0F || bus.volume > kMaxBusVolume ||
        !parse_flag(word[4], bus.mute) || !parse_flag(word[5], bus.solo) ||
        !parse_flag(word[6], bus.bypass)) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: a bus volume must be 0 to 4 and its flags 0 or 1");
    }
    mixer.buses.push_back(std::move(bus));
    return ok();
}

Status parse_send_line(const std::vector<std::string_view>& word, Mixer& mixer) noexcept {
    if (word.size() != 4) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: a send line is `send <from> <to> <level>`");
    }
    MixerBus* from = bus_named(mixer, word[1]);
    MixerSend send{std::string(word[2]), 0.0F};
    if (from == nullptr || !parse_float(word[3], send.level) || send.level < 0.0F ||
        send.level > 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: a send names a declared bus and a level from 0 to 1");
    }
    if (from->sends.size() >= audio::BusGraph::kMaxSends) {
        return fail(ErrorCode::InvalidArgument, "audio mixer: a bus has at most four sends");
    }
    from->sends.push_back(std::move(send));
    return ok();
}

Status parse_effect_line(const std::vector<std::string_view>& word, Mixer& mixer) noexcept {
    if (word.size() != 6) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: an effect line is `effect <bus> <kind> <a> <b> <bypass>`");
    }
    MixerBus* bus = bus_named(mixer, word[1]);
    MixerEffect effect;
    effect.kind = effect_kind_named(word[2]);
    if (bus == nullptr || effect.kind == audio::EffectKind::Count ||
        !parse_float(word[3], effect.parameter_a) || !parse_float(word[4], effect.parameter_b) ||
        !parse_flag(word[5], effect.bypass)) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: an effect names a declared bus, a known kind and two numbers");
    }
    if (bus->effects.size() >= audio::BusGraph::kMaxEffects) {
        return fail(ErrorCode::InvalidArgument, "audio mixer: a bus has at most eight effects");
    }
    bus->effects.push_back(effect);
    return ok();
}

Status parse_mixer_line(std::string_view line, Mixer& mixer) noexcept {
    const std::vector<std::string_view> word = words(line);
    if (word[0] == "bus") {
        return parse_bus_line(word, mixer);
    }
    if (word[0] == "send") {
        return parse_send_line(word, mixer);
    }
    if (word[0] == "effect") {
        return parse_effect_line(word, mixer);
    }
    return fail(ErrorCode::InvalidArgument, "audio mixer: a line is a bus, a send or an effect");
}

/// Everything a route refers to exists, and nothing routes into itself.
Status check_routes(const Mixer& mixer) noexcept {
    if (mixer.buses.empty() || mixer.buses.front().name != "Master" ||
        !mixer.buses.front().output.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "audio mixer: the first bus is Master, and Master has no output");
    }
    for (usize index = 1; index < mixer.buses.size(); ++index) {
        const MixerBus& bus = mixer.buses[index];
        if (bus.output.empty() || bus.output == bus.name ||
            bus_named(mixer, bus.output) == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "audio mixer: every bus but Master outputs into another declared bus");
        }
        for (const MixerSend& send : bus.sends) {
            if (send.target == bus.name || bus_named(mixer, send.target) == nullptr) {
                return fail(ErrorCode::InvalidArgument,
                            "audio mixer: a send targets another declared bus");
            }
        }
    }
    if (!mixer.buses.front().sends.empty()) {
        return fail(ErrorCode::InvalidArgument, "audio mixer: Master sends nowhere");
    }
    return ok();
}

/// Depth-first over outputs and sends together. 0 unvisited, 1 on the stack, 2 done.
bool reaches_cycle(const Mixer& mixer, usize index, std::vector<u8>& state) noexcept {
    if (state[index] == 1) {
        return true;
    }
    if (state[index] == 2) {
        return false;
    }
    state[index] = 1;
    const MixerBus& bus = mixer.buses[index];
    std::vector<std::string_view> next;
    if (!bus.output.empty()) {
        next.push_back(bus.output);
    }
    for (const MixerSend& send : bus.sends) {
        next.push_back(send.target);
    }
    for (std::string_view target : next) {
        const MixerBus* found = bus_named(mixer, target);
        const auto position = static_cast<usize>(found - mixer.buses.data());
        if (reaches_cycle(mixer, position, state)) {
            return true;
        }
    }
    state[index] = 2;
    return false;
}

Status check_acyclic(const Mixer& mixer) noexcept {
    std::vector<u8> state(mixer.buses.size(), 0);
    for (usize index = 0; index < mixer.buses.size(); ++index) {
        if (reaches_cycle(mixer, index, state)) {
            return fail(ErrorCode::InvalidArgument,
                        "audio mixer: the routing has a cycle, which would never finish mixing");
        }
    }
    return ok();
}

// --- The cue asset
// --------------------------------------------------------------------------------

Status parse_cue_number(std::string_view key, std::string_view value, Cue& cue) noexcept {
    f32 number = 0.0F;
    if (!parse_float(value, number)) {
        return fail(ErrorCode::InvalidArgument, "audio cue: a number is malformed");
    }
    if (key == "volume" && number >= 0.0F && number <= 4.0F) {
        cue.volume = number;
    } else if (key == "pitch" && number >= 0.125F && number <= 8.0F) {
        cue.pitch = number;
    } else if (key == "volume_variation" && number >= 0.0F && number <= 1.0F) {
        cue.volume_variation = number;
    } else if (key == "pitch_variation" && number >= 0.0F && number <= 1.0F) {
        cue.pitch_variation = number;
    } else {
        return fail(ErrorCode::InvalidArgument, "audio cue: a value is outside its range");
    }
    return ok();
}

Status parse_cue_line(std::string_view line, Cue& cue) noexcept {
    const usize space = line.find(' ');
    if (space == std::string_view::npos || space + 1 >= line.size()) {
        return fail(ErrorCode::InvalidArgument, "audio cue: a line is `<key> <value>`");
    }
    const std::string_view key = line.substr(0, space);
    const std::string_view value = line.substr(space + 1);
    if (key == "clip") {
        cue.clip = std::string(value);
        return ok();
    }
    if (key == "bus") {
        if (!valid_name(value)) {
            return fail(ErrorCode::InvalidArgument, "audio cue: the bus name is invalid");
        }
        cue.bus = std::string(value);
        return ok();
    }
    if (key == "looping") {
        return parse_flag(value, cue.looping)
                   ? ok()
                   : fail(ErrorCode::InvalidArgument, "audio cue: looping is 0 or 1");
    }
    return parse_cue_number(key, value, cue);
}

/// A tone clip: `tone:<hertz>:<seconds>`.
bool parse_tone(std::string_view clip, f32& hertz, f32& seconds) noexcept {
    if (!clip.starts_with("tone:")) {
        return false;
    }
    const std::string_view rest = clip.substr(5);
    const usize colon = rest.find(':');
    return colon != std::string_view::npos && parse_float(rest.substr(0, colon), hertz) &&
           parse_float(rest.substr(colon + 1), seconds) && hertz >= 20.0F && hertz <= 20000.0F &&
           seconds >= 0.01F && seconds <= 10.0F;
}

bool inside_project(std::string_view reference) noexcept {
    const std::filesystem::path relative(reference);
    return !reference.empty() && !relative.is_absolute() &&
           reference.find_first_of("\\:") == std::string_view::npos &&
           std::ranges::all_of(relative, [](const std::filesystem::path& part) {
               return part != "." && part != "..";
           });
}

Expected<std::string, Error> read_project_file(const std::string& project,
                                               std::string_view reference) noexcept {
    if (!inside_project(reference)) {
        return fail(ErrorCode::InvalidArgument, "audio asset path must stay inside the project");
    }
    std::ifstream input(std::filesystem::path(project) / std::filesystem::path(reference),
                        std::ios::binary);
    if (!input.is_open()) {
        return fail(ErrorCode::NotFound, "audio asset could not be opened");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return fail(ErrorCode::Io, "audio asset could not be read");
    }
    return buffer.str();
}

// --- WAV
// --------------------------------------------------------------------------------------------

u32 le32(std::string_view bytes, usize at) noexcept {
    u32 value = 0;
    for (usize byte = 0; byte < 4; ++byte) {
        value |= static_cast<u32>(static_cast<u8>(bytes[at + byte])) << (byte * 8);
    }
    return value;
}

u16 le16(std::string_view bytes, usize at) noexcept {
    return static_cast<u16>(static_cast<u8>(bytes[at]) |
                            (static_cast<u16>(static_cast<u8>(bytes[at + 1])) << 8U));
}

struct WavFormat {
    u16 encoding = 0;
    u16 channels = 0;
    u32 rate = 0;
    u16 bits = 0;
};

/// PCM 16-bit or IEEE float 32-bit, mono or stereo, at the server's rate. What the editor preview
/// needs, and no more: decoding is the asset system's job (src/servers/audio/README.md).
Status decode_wav(std::string_view bytes, std::vector<f32>& samples, u32& channels) noexcept {
    if (bytes.size() < 12 || !bytes.starts_with("RIFF") || bytes.substr(8, 4) != "WAVE") {
        return fail(ErrorCode::InvalidArgument, "audio cue: the clip is not a RIFF WAVE file");
    }
    WavFormat format;
    std::string_view data;
    for (usize at = 12; at + 8 <= bytes.size();) {
        const std::string_view id = bytes.substr(at, 4);
        const usize size = le32(bytes, at + 4);
        if (at + 8 + size > bytes.size()) {
            break;
        }
        if (id == "fmt " && size >= 16) {
            format = {le16(bytes, at + 8), le16(bytes, at + 10), le32(bytes, at + 12),
                      le16(bytes, at + 22)};
        } else if (id == "data") {
            data = bytes.substr(at + 8, size);
        }
        at += 8 + size + (size & 1U);
    }
    const bool pcm16 = format.encoding == 1 && format.bits == 16;
    const bool float32 = format.encoding == 3 && format.bits == 32;
    if ((!pcm16 && !float32) || format.channels < 1 || format.channels > 2 ||
        format.rate != AudioAuthoring::kSampleRate || data.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "audio cue: a WAV clip is 16-bit PCM or 32-bit float, mono or stereo, 48 kHz");
    }
    const usize width = pcm16 ? 2 : 4;
    samples.resize(data.size() / width);
    for (usize index = 0; index < samples.size(); ++index) {
        if (pcm16) {
            samples[index] = static_cast<f32>(static_cast<i16>(le16(data, index * 2))) / 32768.0F;
        } else {
            const u32 bits = le32(data, index * 4);
            std::memcpy(&samples[index], &bits, sizeof(f32));
        }
    }
    channels = format.channels;
    return ok();
}

// --- Scene sources
// --------------------------------------------------------------------------------

const ser::WorldTypeDecl* source_type(const ser::World& world) noexcept {
    for (const ser::WorldTypeDecl& type : world.types()) {
        if (world.text(type.name) == kAudioSourceComponent) {
            return &type;
        }
    }
    return nullptr;
}

std::string_view text_value(const ser::World& world, const ser::WorldValue& value) noexcept {
    const Span<const u8> bytes = world.blob(value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Status read_source_field(const ser::World& world, std::string_view name,
                         const ser::WorldValue& value, EmitterMarker& marker) noexcept {
    using Kind = ser::WorldValueKind;
    if (name == "cue" && value.kind == Kind::Text) {
        marker.cue = std::string(text_value(world, value));
    } else if (name == "min_distance" && value.kind == Kind::Float) {
        marker.min_distance = value.lanes[0];
    } else if (name == "max_distance" && value.kind == Kind::Float) {
        marker.max_distance = value.lanes[0];
    } else if (name == "attenuation" && value.kind == Kind::Text) {
        marker.model = attenuation_model_named(text_value(world, value));
    } else if (name == "autoplay" && value.kind == Kind::Bool) {
        marker.autoplay = value.integer != 0;
    } else if (name == "enabled" && value.kind == Kind::Bool) {
        marker.enabled = value.integer != 0;
    } else if (name == "cue" || name == "min_distance" || name == "max_distance" ||
               name == "attenuation" || name == "autoplay" || name == "enabled") {
        return fail(ErrorCode::InvalidArgument, "audio source: a field has the wrong value kind");
    }
    return ok();
}

Status validate_marker(const EmitterMarker& marker) noexcept {
    if (marker.cue.empty() || !marker.cue.ends_with(".cycue")) {
        return fail(ErrorCode::InvalidArgument, "audio source: the cue is a project .cycue asset");
    }
    if (!(marker.min_distance > 0.0F) || !(marker.max_distance > marker.min_distance) ||
        !std::isfinite(marker.max_distance)) {
        return fail(ErrorCode::InvalidArgument,
                    "audio source: distances need 0 < min_distance < max_distance");
    }
    if (marker.model == audio::AttenuationModel::Count ||
        marker.model == audio::AttenuationModel::Custom) {
        return fail(ErrorCode::InvalidArgument,
                    "audio source: attenuation is inverse, inverse-square, linear or logarithmic");
    }
    return ok();
}

// --- Encoding
// ---------------------------------------------------------------------------------------

Status put_u8(Array<u8>& out, u8 value) noexcept {
    return out.push_back(value);
}

Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (usize byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

Status put_text(Array<u8>& out, std::string_view value) noexcept {
    if (Status length = put_u32(out, static_cast<u32>(value.size())); !length) {
        return length;
    }
    return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
}

/// Collect several writes and report the first failure, so an encoder reads as a list of fields.
class Encoder {
public:
    explicit Encoder(Array<u8>& out) noexcept : out_(out) {}
    Encoder& u8v(u8 value) noexcept { return keep(put_u8(out_, value)); }
    Encoder& flag(bool value) noexcept { return keep(put_u8(out_, value ? 1U : 0U)); }
    Encoder& u32v(u32 value) noexcept { return keep(put_u32(out_, value)); }
    Encoder& count(usize value) noexcept { return keep(put_u32(out_, static_cast<u32>(value))); }
    Encoder& f32v(f32 value) noexcept { return keep(put_f32(out_, value)); }
    Encoder& text(std::string_view value) noexcept { return keep(put_text(out_, value)); }
    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Encoder& keep(Status status) noexcept {
        if (status_ && !status) {
            status_ = std::move(status);
        }
        return *this;
    }
    Array<u8>& out_;
    Status status_ = ok();
};

struct EffectVocabulary {
    audio::EffectKind kind;
    const char* label_a;
    const char* label_b;
    f32 default_a;
    f32 default_b;
};

constexpr std::array<EffectVocabulary, 4> kEffects = {{
    {audio::EffectKind::Gain, "gain", "", 1.0F, 0.0F},
    {audio::EffectKind::LowPass, "", "coefficient", 0.0F, 0.5F},
    {audio::EffectKind::HighPass, "", "coefficient", 0.0F, 0.5F},
    {audio::EffectKind::Limiter, "ceiling", "", 1.0F, 0.0F},
}};

constexpr std::array<audio::AttenuationModel, 4> kModels = {
    audio::AttenuationModel::Inverse, audio::AttenuationModel::InverseSquare,
    audio::AttenuationModel::Linear, audio::AttenuationModel::Logarithmic};

std::string_view stem_of(std::string_view reference) noexcept {
    const usize slash = reference.rfind('/');
    std::string_view file =
        slash == std::string_view::npos ? reference : reference.substr(slash + 1);
    const usize dot = file.rfind('.');
    return dot == std::string_view::npos ? file : file.substr(0, dot);
}

bool skipped_directory(const std::filesystem::path& path) noexcept {
    const std::string name = path.filename().string();
    return name.empty() || name.front() == '.' || name == "build";
}

}  // namespace

// --- Vocabulary
// -------------------------------------------------------------------------------------

audio::EffectKind effect_kind_named(std::string_view name) noexcept {
    for (const EffectVocabulary& effect : kEffects) {
        if (name == audio::effect_kind_name(effect.kind)) {
            return effect.kind;
        }
    }
    return audio::EffectKind::Count;
}

audio::AttenuationModel attenuation_model_named(std::string_view name) noexcept {
    for (const audio::AttenuationModel model : kModels) {
        if (name == audio::attenuation_model_name(model)) {
            return model;
        }
    }
    return audio::AttenuationModel::Count;
}

Expected<Mixer, Error> parse_mixer(std::string_view text) noexcept {
    const std::vector<std::string_view> lines = lines_of(text);
    if (lines.empty() || lines.front() != "cymixer 1") {
        return make_unexpected(refuse("audio mixer: expected `cymixer 1`"));
    }
    Mixer mixer;
    for (usize index = 1; index < lines.size(); ++index) {
        if (Status parsed = parse_mixer_line(lines[index], mixer); !parsed) {
            return make_unexpected(parsed.error());
        }
    }
    if (mixer.buses.size() > kMaxBuses) {
        return make_unexpected(refuse("audio mixer: at most 32 buses"));
    }
    if (Status routed = check_routes(mixer); !routed) {
        return make_unexpected(routed.error());
    }
    if (Status acyclic = check_acyclic(mixer); !acyclic) {
        return make_unexpected(acyclic.error());
    }
    return mixer;
}

Expected<Cue, Error> parse_cue(std::string_view text) noexcept {
    const std::vector<std::string_view> lines = lines_of(text);
    if (lines.empty() || lines.front() != "cycue 1") {
        return make_unexpected(refuse("audio cue: expected `cycue 1`"));
    }
    Cue cue;
    for (usize index = 1; index < lines.size(); ++index) {
        if (Status parsed = parse_cue_line(lines[index], cue); !parsed) {
            return make_unexpected(parsed.error());
        }
    }
    f32 hertz = 0.0F;
    f32 seconds = 0.0F;
    const bool tone = parse_tone(cue.clip, hertz, seconds);
    if (!tone && !(cue.clip.ends_with(".wav") && inside_project(cue.clip))) {
        return make_unexpected(
            refuse("audio cue: the clip is `tone:<hertz>:<seconds>` or a project .wav file"));
    }
    return cue;
}

Expected<std::vector<EmitterMarker>, Error> read_emitters(const ser::World& world) noexcept {
    std::vector<EmitterMarker> markers;
    const ser::WorldTypeDecl* type = source_type(world);
    if (type == nullptr) {
        return markers;
    }
    for (const ser::WorldNode& node : world.nodes()) {
        const ser::WorldComponent* component = node.live ? node.find(type->file_type) : nullptr;
        if (component == nullptr) {
            continue;
        }
        EmitterMarker marker;
        marker.identity = node.identity;
        for (const ser::WorldField& field : component->fields()) {
            const ser::WorldFieldDecl* declared = type->find(field.file_field);
            if (declared == nullptr) {
                return make_unexpected(refuse("audio source: an undeclared field"));
            }
            if (Status read =
                    read_source_field(world, world.text(declared->name), field.value, marker);
                !read) {
                return make_unexpected(read.error());
            }
        }
        if (Status valid = validate_marker(marker); !valid) {
            return make_unexpected(valid.error());
        }
        Transform placement;
        if (!ser::transform_of(world, node, placement)) {
            return make_unexpected(refuse("audio source: the entity has no Transform"));
        }
        marker.position = placement.translation;
        markers.push_back(std::move(marker));
    }
    return markers;
}

// --- AudioAuthoring
// ---------------------------------------------------------------------------------

AudioAuthoring::AudioAuthoring(Allocator& allocator, audio::AudioBackend* device) noexcept
    : device_(device), server_(allocator), adapter_(server_, allocator) {}

AudioAuthoring::~AudioAuthoring() {
    server_.shutdown();
}

Status AudioAuthoring::initialize() noexcept {
    if (initialized_) {
        return ok();
    }
    audio::AudioBackendConfig backend;
    backend.requested.sample_rate = kSampleRate;
    backend.requested.layout = audio::ChannelLayout::Stereo;
    backend.requested.buffer_frames = kBlockFrames;
    if (device_ == nullptr) {
        null_device_ = std::make_unique<audio::NullAudioBackend>();
        device_ = null_device_.get();
    }
    if (Status ready = device_->initialize(backend); !ready) {
        return ready;
    }
    audio::AudioServerConfig config;
    config.requested = backend.requested;
    config.block_frames = kBlockFrames;
    config.voice_capacity = 64;
    config.clip_capacity = kMaxCues;
    config.bus_capacity = kMaxBuses;
    if (Status configured = server_.configure(config); !configured) {
        return configured;
    }
    if (Status started = server_.initialize_with(*device_); !started) {
        return started;
    }
    const Expected<audio::ListenerHandle, Error> listener =
        server_.create_listener(audio::Listener{});
    if (!listener) {
        return make_unexpected(listener.error());
    }
    listener_ = *listener;
    // Appended rather than brace-assigned: GCC 13 reports a one-element initializer list copied
    // into a vector as an out-of-bounds memmove (-Warray-bounds) under optimisation plus TSan.
    bus_names_.clear();
    bus_names_.emplace_back("Master");
    bus_handles_.clear();
    bus_handles_.push_back(server_.buses().master());
    initialized_ = true;
    return ok();
}

audio::BusHandle AudioAuthoring::bus(std::string_view name) const noexcept {
    for (usize index = 0; index < bus_names_.size(); ++index) {
        if (bus_names_[index] == name) {
            return bus_handles_[index];
        }
    }
    return audio::BusHandle{};
}

Status AudioAuthoring::apply_mixer(std::string_view text) noexcept {
    const Expected<Mixer, Error> mixer = parse_mixer(text);
    if (!mixer) {
        return make_unexpected(mixer.error());
    }
    return apply_mixer(*mixer);
}

Status AudioAuthoring::apply_project_mixer() noexcept {
    std::error_code error;
    if (project_.empty() ||
        !std::filesystem::is_regular_file(std::filesystem::path(project_) / kProjectMixer, error)) {
        return ok();
    }
    const Expected<std::string, Error> text = read_project_file(project_, kProjectMixer);
    if (!text) {
        return make_unexpected(text.error());
    }
    return apply_mixer(std::string_view(*text));
}

Status AudioAuthoring::apply_mixer(const Mixer& mixer) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the editor audio server is not initialized");
    }
    return reconcile(mixer);
}

/// Every existing bus is first routed straight to Master with no sends, which is acyclic, so the
/// final routes can be set one at a time without an intermediate cycle the graph would refuse: any
/// cycle among a subset of the final edges would be a cycle in the final graph, which the parser
/// already refused.
Status AudioAuthoring::reconcile(const Mixer& mixer) noexcept {
    audio::BusGraph& graph = server_.buses();
    for (usize index = 1; index < bus_handles_.size(); ++index) {
        const audio::BusHandle handle = bus_handles_[index];
        (void)graph.set_output(handle, graph.master());
        while (!graph.sends(handle).empty()) {
            (void)graph.remove_send(handle, graph.sends(handle)[0].target);
        }
    }
    std::vector<std::string> names;
    names.emplace_back("Master");
    std::vector<audio::BusHandle> handles;
    handles.push_back(graph.master());
    for (usize index = 1; index < bus_names_.size(); ++index) {
        if (bus_named(mixer, bus_names_[index]) == nullptr) {
            (void)graph.destroy(bus_handles_[index]);
            continue;
        }
        names.push_back(bus_names_[index]);
        handles.push_back(bus_handles_[index]);
    }
    for (const MixerBus& bus : mixer.buses) {
        if (std::ranges::find(names, bus.name) != names.end()) {
            continue;
        }
        audio::BusDescription description;
        description.name = Name::intern(bus.name);
        description.output = graph.master();
        const Expected<audio::BusHandle, Error> created = graph.create(description);
        if (!created) {
            return make_unexpected(created.error());
        }
        names.push_back(bus.name);
        handles.push_back(*created);
    }
    bus_names_ = std::move(names);
    bus_handles_ = std::move(handles);
    for (const MixerBus& bus : mixer.buses) {
        if (Status configured = configure_bus(bus); !configured) {
            return configured;
        }
    }
    stop_voices_on_dead_buses();
    return graph.compile();
}

Status AudioAuthoring::configure_bus(const MixerBus& bus) noexcept {
    audio::BusGraph& graph = server_.buses();
    const audio::BusHandle handle = this->bus(bus.name);
    if (Status set = graph.set_volume(handle, bus.volume); !set) {
        return set;
    }
    if (Status set = graph.set_mute(handle, bus.mute); !set) {
        return set;
    }
    if (Status set = graph.set_solo(handle, bus.solo); !set) {
        return set;
    }
    if (Status set = graph.set_bypass(handle, bus.bypass); !set) {
        return set;
    }
    if (!bus.output.empty()) {
        if (Status routed = graph.set_output(handle, this->bus(bus.output)); !routed) {
            return routed;
        }
    }
    for (const MixerSend& send : bus.sends) {
        if (Status sent = graph.add_send(handle, this->bus(send.target), send.level); !sent) {
            return sent;
        }
    }
    if (Status cleared = graph.clear_effects(handle); !cleared) {
        return cleared;
    }
    for (const MixerEffect& effect : bus.effects) {
        audio::BusEffect engine;
        engine.kind = effect.kind;
        engine.parameter_a = effect.parameter_a;
        engine.parameter_b = effect.parameter_b;
        engine.bypass = effect.bypass;
        if (Status added = graph.add_effect(handle, engine); !added) {
            return added;
        }
    }
    return ok();
}

void AudioAuthoring::stop_voices_on_dead_buses() noexcept {
    // A voice's bus is fixed when it starts, so a mixer edit that removes the bus is the one place
    // a voice can outlive it. Stopped rather than rerouted: which bus it belongs on is the
    // author's.
    const auto orphaned = [this](const ActiveVoice& active) {
        if (server_.buses().alive(active.bus)) {
            return false;
        }
        (void)server_.stop(active.voice);
        return true;
    };
    std::erase_if(preview_voices_, orphaned);
    std::erase_if(play_voices_, orphaned);
}

void AudioAuthoring::forget_finished_voices() noexcept {
    const auto finished = [this](const ActiveVoice& active) {
        return !server_.playing(active.voice);
    };
    std::erase_if(preview_voices_, finished);
    std::erase_if(play_voices_, finished);
    if (preview_voices_.empty()) {
        preview_report_.playing = false;
    }
}

Status AudioAuthoring::render_clip(const Cue& cue, std::vector<f32>& samples,
                                   u32& channels) const noexcept {
    f32 hertz = 0.0F;
    f32 seconds = 0.0F;
    if (parse_tone(cue.clip, hertz, seconds)) {
        const auto frames = static_cast<usize>(seconds * static_cast<f32>(kSampleRate));
        const usize ramp = std::min<usize>(frames / 2, kSampleRate / 200);
        samples.resize(frames);
        for (usize frame = 0; frame < frames; ++frame) {
            const f32 edge = static_cast<f32>(std::min({frame, frames - 1 - frame, ramp})) /
                             static_cast<f32>(std::max<usize>(ramp, 1));
            const f32 phase = 2.0F * std::numbers::pi_v<f32> * hertz * static_cast<f32>(frame) /
                              static_cast<f32>(kSampleRate);
            samples[frame] = 0.5F * std::sin(phase) * std::min(edge, 1.0F);
        }
        channels = kToneChannels;
        return ok();
    }
    const Expected<std::string, Error> bytes = read_project_file(project_, cue.clip);
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    return decode_wav(*bytes, samples, channels);
}

Status AudioAuthoring::load_cue(std::string_view name, std::string_view text) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the editor audio server is not initialized");
    }
    if (name.empty() || !inside_project(name)) {
        return fail(ErrorCode::InvalidArgument, "audio cue: the name is a project path");
    }
    const Expected<Cue, Error> cue = parse_cue(text);
    if (!cue) {
        return make_unexpected(cue.error());
    }
    auto loaded = std::make_unique<LoadedCue>();
    loaded->name = std::string(name);
    loaded->cue = *cue;
    u32 channels = 1;
    if (Status rendered = render_clip(loaded->cue, loaded->samples, channels); !rendered) {
        return rendered;
    }
    audio::ClipDescription clip;
    clip.name = Name::intern(name);
    clip.samples = loaded->samples.data();
    clip.channels = channels;
    clip.frame_count = static_cast<u32>(loaded->samples.size() / channels);
    clip.sample_rate = kSampleRate;
    const Expected<audio::ClipHandle, Error> created = server_.create_clip(clip);
    if (!created) {
        return make_unexpected(created.error());
    }
    loaded->clip = *created;
    if (Status named = adapter_.add_cue(Name::intern(stem_of(name)), *created); !named) {
        server_.destroy_clip(*created);
        return named;
    }
    const auto existing = std::ranges::find_if(
        cues_,
        [name](const std::unique_ptr<LoadedCue>& cue_slot) { return cue_slot->name == name; });
    if (existing != cues_.end()) {
        stop_preview();
        server_.destroy_clip((*existing)->clip);
        *existing = std::move(loaded);
        return ok();
    }
    if (cues_.size() >= kMaxCues) {
        server_.destroy_clip(*created);
        return fail(ErrorCode::OutOfRange, "audio cue: at most 256 cues are loaded");
    }
    cues_.push_back(std::move(loaded));
    return ok();
}

const AudioAuthoring::LoadedCue* AudioAuthoring::find_cue(std::string_view name) const noexcept {
    for (const std::unique_ptr<LoadedCue>& cue : cues_) {
        if (cue->name == name) {
            return cue.get();
        }
    }
    return nullptr;
}

Expected<const AudioAuthoring::LoadedCue*, Error> AudioAuthoring::cue_for_source(
    std::string_view reference) noexcept {
    if (const LoadedCue* loaded = find_cue(reference); loaded != nullptr) {
        return loaded;
    }
    const Expected<std::string, Error> text = read_project_file(project_, reference);
    if (!text) {
        return make_unexpected(text.error());
    }
    if (Status loaded = load_cue(reference, *text); !loaded) {
        return make_unexpected(loaded.error());
    }
    return find_cue(reference);
}

Expected<AudioAuthoring::ActiveVoice, Error> AudioAuthoring::start_voice(
    const LoadedCue& cue, const PreviewPlacement& placement, bool looping) noexcept {
    audio::VoiceDescription voice;
    voice.clip = cue.clip;
    voice.bus = bus(cue.cue.bus);
    if (voice.bus.is_null()) {
        return make_unexpected(refuse("audio cue: its bus is not in the mixer"));
    }
    voice.volume = cue.cue.volume;
    voice.pitch = cue.cue.pitch;
    voice.volume_variation = cue.cue.volume_variation;
    voice.pitch_variation = cue.cue.pitch_variation;
    voice.looping = looping;
    voice.spatialised = placement.spatial;
    voice.position = placement.position;
    voice.attenuation.model = placement.model;
    voice.attenuation.reference_distance = placement.min_distance;
    voice.attenuation.max_distance = placement.max_distance;
    voice.minimum_tier = audio::SimulationTier::Spatialised;
    const Expected<audio::VoiceHandle, Error> started = server_.play(voice);
    if (!started) {
        return make_unexpected(started.error());
    }
    return ActiveVoice{*started, voice.bus};
}

void AudioAuthoring::set_listener(Vec3 position, Vec3 forward) noexcept {
    if (!initialized_) {
        return;
    }
    audio::Listener listener;
    listener.transform.translation = position;
    if (length(forward) > 1e-6F) {
        listener.transform.rotation = Quat::look_rotation(normalize(forward));
    }
    (void)server_.set_listener(listener_, listener);
}

Status AudioAuthoring::preview(std::string_view name, const PreviewPlacement& placement) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the editor audio server is not initialized");
    }
    const LoadedCue* cue = find_cue(name);
    if (cue == nullptr) {
        return fail(ErrorCode::NotFound, "audio preview: that cue is not loaded");
    }
    stop_preview();
    if (placement.spatial) {
        set_listener(placement.listener, placement.listener_forward);
    }
    const Expected<ActiveVoice, Error> voice = start_voice(*cue, placement, false);
    if (!voice) {
        return make_unexpected(voice.error());
    }
    preview_voices_.push_back(*voice);

    PreviewReport report;
    report.present = true;
    report.cue = cue->name;
    report.playing = server_.playing(voice->voice);
    if (placement.spatial) {
        audio::Attenuation attenuation;
        attenuation.model = placement.model;
        attenuation.reference_distance = placement.min_distance;
        attenuation.max_distance = placement.max_distance;
        const Vec3 to_source = placement.position - placement.listener;
        report.distance = length(to_source);
        report.gain = audio::attenuation_gain(attenuation, report.distance);
        const audio::Listener* listener = server_.primary_listener();
        const Quat rotation = listener != nullptr ? listener->transform.rotation : Quat::identity();
        const audio::PanGains pan = audio::pan_stereo(inverse(rotation) * to_source);
        report.left = pan.left;
        report.right = pan.right;
    }
    preview_report_ = std::move(report);
    return ok();
}

void AudioAuthoring::stop_preview() noexcept {
    for (const ActiveVoice& active : preview_voices_) {
        (void)server_.stop(active.voice);
    }
    preview_voices_.clear();
    preview_report_.playing = false;
}

Status AudioAuthoring::start_play(const ser::World& world) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the editor audio server is not initialized");
    }
    stop_play();
    stop_preview();
    if (Status mixed = apply_project_mixer(); !mixed) {
        return mixed;
    }
    // Every cue in the project is named for Swift, so `Audio.play("boom")` finds a cue no scene
    // source references.
    std::error_code error;
    const std::filesystem::path root(project_);
    for (auto entry = std::filesystem::recursive_directory_iterator(root, error);
         !error && entry != std::filesystem::recursive_directory_iterator();
         entry.increment(error)) {
        if (entry->is_directory() && skipped_directory(entry->path())) {
            entry.disable_recursion_pending();
            continue;
        }
        if (entry->path().extension() == ".cycue") {
            const std::string reference =
                std::filesystem::relative(entry->path(), root, error).generic_string();
            (void)cue_for_source(reference);
        }
    }
    const Expected<std::vector<EmitterMarker>, Error> sources = read_emitters(world);
    if (!sources) {
        return make_unexpected(sources.error());
    }
    for (const EmitterMarker& source : *sources) {
        if (!source.enabled || !source.autoplay) {
            continue;
        }
        const Expected<const LoadedCue*, Error> cue = cue_for_source(source.cue);
        if (!cue) {
            stop_play();
            return make_unexpected(cue.error());
        }
        PreviewPlacement placement;
        placement.spatial = true;
        placement.position = source.position;
        placement.min_distance = source.min_distance;
        placement.max_distance = source.max_distance;
        placement.model = source.model;
        const Expected<ActiveVoice, Error> voice =
            start_voice(**cue, placement, (*cue)->cue.looping);
        if (!voice) {
            stop_play();
            return make_unexpected(voice.error());
        }
        play_voices_.push_back(*voice);
    }
    play_active_ = true;
    return ok();
}

void AudioAuthoring::stop_play() noexcept {
    for (const ActiveVoice& active : play_voices_) {
        (void)server_.stop(active.voice);
    }
    play_voices_.clear();
    play_active_ = false;
}

void AudioAuthoring::pause_play(bool paused) noexcept {
    for (const ActiveVoice& active : play_voices_) {
        (void)(paused ? server_.pause(active.voice) : server_.resume(active.voice));
    }
}

void AudioAuthoring::pump(f32 delta_seconds) noexcept {
    if (!initialized_ || !(delta_seconds >= 0.0F)) {
        return;
    }
    adapter_.update(delta_seconds);
    server_.update(delta_seconds);
    forget_finished_voices();
    if (null_device_ == nullptr) {
        return;
    }
    // Driven: pull the frame's worth of samples, carrying the remainder so a 60 Hz frame does not
    // round 800 frames down to three blocks forever.
    pending_seconds_ += delta_seconds;
    auto frames = static_cast<u32>(pending_seconds_ * static_cast<f32>(kSampleRate));
    frames -= frames % kBlockFrames;
    pending_seconds_ -= static_cast<f32>(frames) / static_cast<f32>(kSampleRate);
    if (frames == 0) {
        return;
    }
    mix_scratch_.resize(static_cast<usize>(frames) * 2U);
    (void)null_device_->advance(mix_scratch_.data(), frames);
    server_.update(0.0F);
    forget_finished_voices();
}

Status AudioAuthoring::encode_state(Array<u8>& out) const noexcept {
    const audio::BusGraph& graph = server_.buses();
    const audio::AudioStatistics& statistics = server_.statistics();
    Encoder encode(out);
    encode.u32v(1)
        .text(server_.backend_name())
        .u32v(kSampleRate)
        .u32v(statistics.active_voices)
        .u32v(statistics.virtual_voices)
        .u32v(statistics.blocks_mixed)
        .flag(play_active_)
        .count(play_voices_.size())
        .count(bus_names_.size());
    for (usize index = 0; index < bus_names_.size(); ++index) {
        const audio::BusHandle handle = bus_handles_[index];
        const audio::BusDescription* description = graph.description(handle);
        if (description == nullptr) {
            return fail(ErrorCode::Internal, "audio state: a mixer bus is not in the graph");
        }
        std::string_view output;
        for (usize other = 0; other < bus_handles_.size(); ++other) {
            if (bus_handles_[other] == description->output) {
                output = bus_names_[other];
            }
        }
        const u32 level_index = graph.index_of(handle);
        const audio::BusLevel level = level_index < server_.bus_levels().size()
                                          ? server_.bus_levels()[level_index]
                                          : audio::BusLevel{};
        encode.text(bus_names_[index])
            .text(output)
            .f32v(description->volume)
            .flag(description->mute)
            .flag(description->solo)
            .flag(description->bypass)
            .flag(graph.audible(handle))
            .count(graph.sends(handle).size());
        for (const audio::BusSend& send : graph.sends(handle)) {
            std::string_view target;
            for (usize other = 0; other < bus_handles_.size(); ++other) {
                if (bus_handles_[other] == send.target) {
                    target = bus_names_[other];
                }
            }
            encode.text(target).f32v(send.level);
        }
        encode.count(graph.effects(handle).size());
        for (const audio::BusEffect& effect : graph.effects(handle)) {
            encode.text(audio::effect_kind_name(effect.kind))
                .f32v(effect.parameter_a)
                .f32v(effect.parameter_b)
                .flag(effect.bypass);
        }
        encode.f32v(level.peak).f32v(level.rms);
    }
    encode.count(cues_.size());
    for (const std::unique_ptr<LoadedCue>& cue : cues_) {
        encode.text(cue->name);
    }
    encode.flag(preview_report_.present);
    if (preview_report_.present) {
        encode.text(preview_report_.cue)
            .flag(preview_report_.playing)
            .f32v(preview_report_.distance)
            .f32v(preview_report_.gain)
            .f32v(preview_report_.left)
            .f32v(preview_report_.right);
    }
    return encode.status();
}

Status AudioAuthoring::encode_capabilities(Array<u8>& out) const noexcept {
    Encoder encode(out);
    encode.u32v(1)
        .text(server_.backend_name())
        .u32v(kSampleRate)
        .u32v(static_cast<u32>(kMaxBuses))
        .u32v(audio::BusGraph::kMaxSends)
        .u32v(audio::BusGraph::kMaxEffects)
        .count(kEffects.size());
    for (const EffectVocabulary& effect : kEffects) {
        encode.text(audio::effect_kind_name(effect.kind))
            .text(effect.label_a)
            .text(effect.label_b)
            .f32v(effect.default_a)
            .f32v(effect.default_b);
    }
    encode.count(kModels.size());
    for (const audio::AttenuationModel model : kModels) {
        encode.text(audio::attenuation_model_name(model));
    }
    return encode.status();
}

}  // namespace cy::editor
