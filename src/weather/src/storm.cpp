// Storms as spatial phenomena, and lightning as an event nobody plays. M10 task 3.2.

#include <cy/weather/storm.h>

#include <cy/core/determinism/random.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::weather {

namespace {

/// The stream storms draw from. Named once, here, so that a replay and a peer derive the same
/// strikes from the same name — `simulation-and-determinism`'s counter-based streams and nothing
/// else, as design.md §3 requires.
constexpr const char* kLightningStream = "weather.storm.lightning";

/// How a storm's influence falls off from its centre. Smoothstep rather than linear: a storm with a
/// linear edge has a visible ring where its rain stops, and a cell-resolution model shows it.
[[nodiscard]] f32 storm_falloff(f32 normalised) noexcept {
    if (normalised >= 1.0F) {
        return 0.0F;
    }
    const f32 t = 1.0F - normalised;
    return t * t * (3.0F - (2.0F * t));
}

/// The per-type character of a storm, as a table rather than as a switch inside the contribution.
struct StormCharacter {
    f32 pressure_drop_hpa;
    f32 wind_gain;
    f32 rain_gain;
    f32 cloud_gain;
    f32 lightning_gain;
    f32 visibility_loss;
    /// How much of the wind is tangential (rotating) rather than inflowing. A cyclone is nearly all
    /// rotation; a squall line is nearly all inflow.
    f32 rotation;
};

[[nodiscard]] StormCharacter character_of(StormType type) noexcept {
    switch (type) {
        case StormType::RainBand:
            return {6.0F, 4.0F, 14.0F, 0.55F, 0.0F, 6'000.0F, 0.15F};
        case StormType::Thunderstorm:
            return {12.0F, 11.0F, 42.0F, 0.85F, 0.9F, 14'000.0F, 0.35F};
        case StormType::Squall:
            return {9.0F, 18.0F, 22.0F, 0.7F, 0.25F, 10'000.0F, 0.1F};
        case StormType::Blizzard:
            return {14.0F, 16.0F, 12.0F, 0.95F, 0.0F, 22'000.0F, 0.3F};
        case StormType::SandStorm:
            return {5.0F, 20.0F, 0.0F, 0.2F, 0.0F, 30'000.0F, 0.1F};
        case StormType::Cyclone:
            return {38.0F, 34.0F, 55.0F, 1.0F, 0.6F, 24'000.0F, 0.85F};
        default:
            return {6.0F, 5.0F, 10.0F, 0.5F, 0.1F, 5'000.0F, 0.2F};
    }
}

/// Bytes on the wire, per storm. Fixed and documented, because the specification's "a storm costs
/// little bandwidth" scenario is a claim about this number.
///   8 identity + 1 type + 24 position + 8 velocity + 4 radius + 4 intensity + 8 seed
///   + 4 age + 4 lifetime = 65, padded to nothing: the encoder writes fields, not structs.
constexpr u32 kStormWireBytes = 65;

void write_u64(Array<u8>& out, u64 value, bool& ok_flag) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
            !pushed) {
            ok_flag = false;
            return;
        }
    }
}

[[nodiscard]] u64 read_u64(Span<const u8> bytes, usize offset) noexcept {
    u64 value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(bytes[offset + byte]) << (byte * 8U);
    }
    return value;
}

void write_f32(Array<u8>& out, f32 value, bool& ok_flag) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (u32 byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((bits >> (byte * 8U)) & 0xFFU));
            !pushed) {
            ok_flag = false;
            return;
        }
    }
}

[[nodiscard]] f32 read_f32(Span<const u8> bytes, usize offset) noexcept {
    u32 bits = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        bits |= static_cast<u32>(bytes[offset + byte]) << (byte * 8U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f64(Array<u8>& out, f64 value, bool& ok_flag) noexcept {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(out, bits, ok_flag);
}

[[nodiscard]] f64 read_f64(Span<const u8> bytes, usize offset) noexcept {
    const u64 bits = read_u64(bytes, offset);
    f64 value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

const char* storm_type_name(StormType type) noexcept {
    switch (type) {
        case StormType::RainBand:
            return "rain-band";
        case StormType::Thunderstorm:
            return "thunderstorm";
        case StormType::Squall:
            return "squall";
        case StormType::Blizzard:
            return "blizzard";
        case StormType::SandStorm:
            return "sandstorm";
        case StormType::Cyclone:
            return "cyclone";
        default:
            return "project";
    }
}

StormContribution storm_contribution_at(const Storm& storm, f64 x, f64 z) noexcept {
    StormContribution out;
    if (storm.radius_metres <= 0.0F || storm.intensity <= 0.0F) {
        return out;
    }
    const f64 dx = x - storm.position.x;
    const f64 dz = z - storm.position.z;
    const f64 distance = std::sqrt((dx * dx) + (dz * dz));
    const auto normalised = static_cast<f32>(distance / static_cast<f64>(storm.radius_metres));
    const f32 influence = storm_falloff(normalised);
    if (influence <= 0.0F) {
        return out;
    }
    const StormCharacter character = character_of(storm.type);
    const f32 scale = influence * storm.intensity;

    out.influence = influence;
    out.pressure_delta = -character.pressure_drop_hpa * scale;
    out.precipitation_mm_per_hour = character.rain_gain * scale;
    out.cloud_coverage = character.cloud_gain * scale;
    out.lightning_potential = character.lightning_gain * scale;
    out.visibility_loss_metres = character.visibility_loss * scale;

    // The wind: a rotation about the centre plus an inflow toward it, in the proportion the type's
    // character declares. A storm whose wind was purely inflowing would suck a boat to the middle
    // and leave it there; one that was purely rotational would never be felt approaching.
    if (distance > 1.0) {
        const auto ux = static_cast<f32>(dx / distance);
        const auto uz = static_cast<f32>(dz / distance);
        const f32 speed = character.wind_gain * scale;
        const f32 rotation = character.rotation;
        // Counter-clockwise in the world's x/z plane, which is the northern-hemisphere sense. A
        // project wanting the other sense negates its storms' rotation by declaring a project type.
        out.wind.x = ((-uz * rotation) - (ux * (1.0F - rotation))) * speed;
        out.wind.y = ((ux * rotation) - (uz * (1.0F - rotation))) * speed;
    }
    // The storm's own motion is carried with it, so the leading edge is windier than the trailing
    // one — which is what makes a storm feel like it is arriving rather than merely being present.
    out.wind.x += storm.velocity.x * influence * 0.5F;
    out.wind.y += storm.velocity.y * influence * 0.5F;
    return out;
}

StormRegistry::StormRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), storms_(allocator), lightning_(allocator) {}

Status StormRegistry::spawn(const Storm& storm) noexcept {
    if (!storm.id.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: a storm needs a non-zero identity");
    }
    if (storm.radius_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "weather: a storm needs a positive radius");
    }
    for (const Storm& existing : storms_.span()) {
        if (existing.id == storm.id) {
            return fail(ErrorCode::AlreadyExists,
                        "weather: a storm with that identity already exists");
        }
    }
    return storms_.push_back(storm);
}

Status StormRegistry::despawn(StormId id) noexcept {
    for (usize index = 0; index < storms_.size(); ++index) {
        if (storms_[index].id == id) {
            // Ordered removal, not `remove_unordered`: the encode order is the array order, and a
            // removal that permuted it would make two peers that removed storms in different orders
            // produce different wire bytes for the same state.
            for (usize shift = index + 1; shift < storms_.size(); ++shift) {
                storms_[shift - 1] = storms_[shift];
            }
            return storms_.resize(storms_.size() - 1);
        }
    }
    return fail(ErrorCode::NotFound, "weather: no storm with that identity");
}

const Storm* StormRegistry::find(StormId id) const noexcept {
    for (const Storm& storm : storms_.span()) {
        if (storm.id == id) {
            return &storm;
        }
    }
    return nullptr;
}

Status StormRegistry::emit_strikes(const Storm& storm, determinism::SimulationPoint at, f32 seconds,
                                   u64 session_seed) noexcept {
    const StormCharacter character = character_of(storm.type);
    const f32 rate = character.lightning_gain * storm.intensity;
    if (rate <= 0.0F || seconds <= 0.0F) {
        return ok();
    }
    // ONE SUBSTREAM PER STORM, indexed by the tick. The sequence of strikes is a pure function of
    // (session seed, storm seed, tick), so two peers agree without exchanging a bolt and a replay
    // reconstructs them from the record it already has.
    const determinism::StreamId stream = determinism::substream(
        determinism::stream_id(kLightningStream), storm.seed ^ storm.id.value);
    const determinism::RandomStream random(session_seed, stream,
                                           determinism::StreamPurpose::Authoritative);

    // Strikes per second, turned into a count by a threshold draw per unit of expectation. A
    // Poisson process sampled this way is exact for the rates a storm has (well below one per
    // second) and needs no state between ticks, which is the property that matters here.
    const f32 expected = rate * seconds;
    const u32 attempts = (expected > 8.0F) ? 8U : static_cast<u32>(expected) + 1U;
    for (u32 attempt = 0; attempt < attempts; ++attempt) {
        const f32 probability = expected / static_cast<f32>(attempts);
        const auto draw_index = static_cast<u64>(attempt) * 4U;
        if (random.unit_float(at, storm.id.value, draw_index) >= probability) {
            continue;
        }
        const f32 angle = random.unit_float(at, storm.id.value, draw_index + 1U) * 6.2831853F;
        const f32 radial =
            std::sqrt(random.unit_float(at, storm.id.value, draw_index + 2U)) * storm.radius_metres;
        LightningEvent event;
        event.position.x = storm.position.x + static_cast<f64>(std::cos(angle) * radial);
        event.position.z = storm.position.z + static_cast<f64>(std::sin(angle) * radial);
        event.position.y = storm.position.y;
        event.intensity = 0.4F + (0.6F * random.unit_float(at, storm.id.value, draw_index + 3U) *
                                  storm.intensity);
        // The event's own seed, from which a consumer derives the bolt's geometry. Derived from the
        // storm and the draw index rather than counted, for the reason design.md §1.6 gives about
        // generated identity: a counter is not reproducible under a partial re-evaluation.
        event.seed = random.draw(at, storm.id.value, 0x5eedULL + attempt);
        event.tick = at.tick;
        event.storm = storm.id;
        if (Status pushed = lightning_.push_back(event); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status StormRegistry::advance(determinism::SimulationPoint at, f32 seconds,
                              u64 session_seed) noexcept {
    if (seconds <= 0.0F) {
        return ok();
    }
    usize live = 0;
    for (auto& storm : storms_) {
        storm.position.x += static_cast<f64>(storm.velocity.x) * static_cast<f64>(seconds);
        storm.position.z += static_cast<f64>(storm.velocity.y) * static_cast<f64>(seconds);
        storm.age_seconds += seconds;
        if (Status emitted = emit_strikes(storm, at, seconds, session_seed); !emitted) {
            return emitted;
        }
        if (storm.expired()) {
            continue;
        }
        storms_[live] = storm;
        ++live;
    }
    return storms_.resize(live);
}

StormContribution StormRegistry::contribution_at(f64 x, f64 z) const noexcept {
    StormContribution total;
    for (const Storm& storm : storms_.span()) {
        const StormContribution one = storm_contribution_at(storm, x, z);
        if (one.influence <= 0.0F) {
            continue;
        }
        total.pressure_delta += one.pressure_delta;
        total.wind.x += one.wind.x;
        total.wind.y += one.wind.y;
        total.precipitation_mm_per_hour += one.precipitation_mm_per_hour;
        total.cloud_coverage += one.cloud_coverage;
        total.lightning_potential += one.lightning_potential;
        total.visibility_loss_metres += one.visibility_loss_metres;
        total.influence = std::max(total.influence, one.influence);
    }
    return total;
}

usize StormRegistry::contributions_at(f64 x, f64 z, Span<StormId> ids,
                                      Span<StormContribution> out) const noexcept {
    usize written = 0;
    const usize room = std::min(ids.size(), out.size());
    for (const Storm& storm : storms_.span()) {
        if (written >= room) {
            break;
        }
        const StormContribution one = storm_contribution_at(storm, x, z);
        if (one.influence <= 0.0F) {
            continue;
        }
        ids[written] = storm.id;
        out[written] = one;
        ++written;
    }
    return written;
}

Status StormRegistry::drain_lightning(Array<LightningEvent>& out) noexcept {
    for (const LightningEvent& event : lightning_.span()) {
        if (Status pushed = out.push_back(event); !pushed) {
            return pushed;
        }
    }
    lightning_.clear();
    return ok();
}

u32 StormRegistry::encoded_storm_bytes() noexcept {
    return kStormWireBytes;
}

Status StormRegistry::encode(Array<u8>& out) const noexcept {
    bool ok_flag = true;
    write_u64(out, static_cast<u64>(storms_.size()), ok_flag);
    for (const Storm& storm : storms_.span()) {
        write_u64(out, storm.id.value, ok_flag);
        if (Status pushed = out.push_back(static_cast<u8>(storm.type)); !pushed) {
            return pushed;
        }
        write_f64(out, storm.position.x, ok_flag);
        write_f64(out, storm.position.y, ok_flag);
        write_f64(out, storm.position.z, ok_flag);
        write_f32(out, storm.velocity.x, ok_flag);
        write_f32(out, storm.velocity.y, ok_flag);
        write_f32(out, storm.radius_metres, ok_flag);
        write_f32(out, storm.intensity, ok_flag);
        write_u64(out, storm.seed, ok_flag);
        write_f32(out, storm.age_seconds, ok_flag);
        write_f32(out, storm.lifetime_seconds, ok_flag);
    }
    if (!ok_flag) {
        return fail(ErrorCode::OutOfMemory, "weather: the storm encoding ran out of room");
    }
    return ok();
}

Status StormRegistry::decode(Span<const u8> bytes) noexcept {
    if (bytes.size() < 8) {
        return fail(ErrorCode::InvalidArgument, "weather: a storm message needs a count");
    }
    const u64 count = read_u64(bytes, 0);
    if (bytes.size() < 8 + (count * kStormWireBytes)) {
        return fail(ErrorCode::InvalidArgument, "weather: the storm message is short");
    }
    storms_.clear();
    usize offset = 8;
    for (u64 index = 0; index < count; ++index) {
        Storm storm;
        storm.id.value = read_u64(bytes, offset);
        offset += 8;
        storm.type = static_cast<StormType>(bytes[offset]);
        offset += 1;
        storm.position.x = read_f64(bytes, offset);
        offset += 8;
        storm.position.y = read_f64(bytes, offset);
        offset += 8;
        storm.position.z = read_f64(bytes, offset);
        offset += 8;
        storm.velocity.x = read_f32(bytes, offset);
        offset += 4;
        storm.velocity.y = read_f32(bytes, offset);
        offset += 4;
        storm.radius_metres = read_f32(bytes, offset);
        offset += 4;
        storm.intensity = read_f32(bytes, offset);
        offset += 4;
        storm.seed = read_u64(bytes, offset);
        offset += 8;
        storm.age_seconds = read_f32(bytes, offset);
        offset += 4;
        storm.lifetime_seconds = read_f32(bytes, offset);
        offset += 4;
        if (Status pushed = storms_.push_back(storm); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::weather
