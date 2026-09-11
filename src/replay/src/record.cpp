// THE ONE RECORD. M9 tasks 1.1 and 1.5.

#include <cy/replay/record.h>

#include <cy/replay/divergence.h>
#include <cy/replay/readers.h>

#include <cstring>

namespace cy::replay {
namespace {

// --- The layout -----------------------------------------------------------------------------
//
// Fixed offsets, little-endian, and every byte of the encoding is written. A `memcpy` of the struct
// would write its padding, and padding is whatever last occupied those bytes — two runs agreeing
// about every value would produce different files, which is the defect class this milestone exists
// to detect.

constexpr u32 kOffKind = 0;
constexpr u32 kOffRecordPayloadSize = 2;
constexpr u32 kOffEpoch = 4;
constexpr u32 kOffTick = 8;
constexpr u32 kOffSequence = 16;
constexpr u32 kOffCommandType = 20;
constexpr u32 kOffCommandTick = 24;
constexpr u32 kOffParticipant = 32;
constexpr u32 kOffSource = 40;
constexpr u32 kOffTarget = 48;
constexpr u32 kOffGroup = 56;
constexpr u32 kOffProvenanceKind = 64;
constexpr u32 kOffCommandPayloadSize = 66;
constexpr u32 kOffProvenanceSource = 68;
constexpr u32 kOffCommandSequence = 72;
constexpr u32 kOffSubject = 80;
constexpr u32 kOffValue = 88;
constexpr u32 kOffCommandPayload = 96;
constexpr u32 kOffRecordPayload = 144;

static_assert(kOffRecordPayload + kMaxRecordPayload == kEncodedRecordSize,
              "the encoding must fill exactly kEncodedRecordSize bytes");
static_assert(gameplay::kMaxCommandPayload == kMaxRecordPayload,
              "the two payload areas are the same width by construction; if they diverge the "
              "layout above has to say which is which");

void put_u16(u8* at, u16 value) noexcept {
    at[0] = static_cast<u8>(value & 0xFFU);
    at[1] = static_cast<u8>((value >> 8U) & 0xFFU);
}

void put_u32(u8* at, u32 value) noexcept {
    for (u32 byte = 0; byte < 4; ++byte) {
        at[byte] = static_cast<u8>((value >> (byte * 8U)) & 0xFFU);
    }
}

void put_u64(u8* at, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        at[byte] = static_cast<u8>((value >> (byte * 8U)) & 0xFFU);
    }
}

[[nodiscard]] u16 get_u16(const u8* at) noexcept {
    return static_cast<u16>(static_cast<u16>(at[0]) | (static_cast<u16>(at[1]) << 8U));
}

[[nodiscard]] u32 get_u32(const u8* at) noexcept {
    u32 value = 0;
    for (u32 byte = 0; byte < 4; ++byte) {
        value |= static_cast<u32>(at[byte]) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] u64 get_u64(const u8* at) noexcept {
    u64 value = 0;
    for (u32 byte = 0; byte < 8; ++byte) {
        value |= static_cast<u64>(at[byte]) << (byte * 8U);
    }
    return value;
}

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 fold(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

const char* record_kind_name(RecordKind kind) noexcept {
    switch (kind) {
        case RecordKind::Command:
            return "Command";
        case RecordKind::ExternalResult:
            return "ExternalResult";
        case RecordKind::Checkpoint:
            return "Checkpoint";
        case RecordKind::StateHash:
            return "StateHash";
        case RecordKind::Effect:
            return "Effect";
    }
    return "Command";
}

const char* log_reader_name(LogReader reader) noexcept {
    switch (reader) {
        case LogReader::Playback:
            return "Playback";
        case LogReader::Rollback:
            return "Rollback";
        case LogReader::ReplicationInput:
            return "ReplicationInput";
        case LogReader::CrashBuffer:
            return "CrashBuffer";
        case LogReader::Validator:
            return "Validator";
        case LogReader::Count:
            break;
    }
    return "Count";
}

Status encode(const LogRecord& record, Array<u8>& out) noexcept {
    const usize mark = out.size();
    if (Status resized = out.resize(mark + kEncodedRecordSize); !resized) {
        return resized;
    }
    u8* at = out.data() + mark;
    std::memset(at, 0, kEncodedRecordSize);

    at[kOffKind] = static_cast<u8>(record.kind);
    put_u16(at + kOffRecordPayloadSize, record.payload_size);
    put_u32(at + kOffEpoch, record.epoch.value);
    put_u64(at + kOffTick, record.tick);
    put_u32(at + kOffSequence, record.sequence);

    put_u32(at + kOffCommandType, record.command.type);
    put_u64(at + kOffCommandTick, record.command.tick);
    put_u64(at + kOffParticipant, record.command.participant.bits());
    put_u64(at + kOffSource, record.command.source.bits());
    put_u64(at + kOffTarget, record.command.target.bits());
    put_u64(at + kOffGroup, record.command.group.bits());
    at[kOffProvenanceKind] = static_cast<u8>(record.command.provenance.kind);
    put_u16(at + kOffCommandPayloadSize, record.command.payload_size);
    put_u32(at + kOffProvenanceSource, record.command.provenance.source);
    put_u32(at + kOffCommandSequence, record.command.sequence);

    put_u64(at + kOffSubject, record.subject);
    put_u64(at + kOffValue, record.value);

    // The payloads are copied whole, including the bytes past `payload_size`. They are zeroed above
    // and `set_payload()` writes only the prefix, so what is written is the same for two runs that
    // produced the same command — which is the property the file digest depends on.
    std::memcpy(at + kOffCommandPayload, record.command.payload, gameplay::kMaxCommandPayload);
    std::memcpy(at + kOffRecordPayload, record.payload, kMaxRecordPayload);
    return ok();
}

Status decode(Span<const u8> bytes, LogRecord& out) noexcept {
    if (bytes.size() < kEncodedRecordSize) {
        return fail(ErrorCode::BufferTooSmall,
                    "replay: fewer bytes than one record; a short buffer is refused rather than "
                    "read past");
    }
    const u8* at = bytes.data();
    const u8 kind = at[kOffKind];
    if (kind > static_cast<u8>(RecordKind::Effect)) {
        return fail(ErrorCode::InvalidArgument, "replay: record kind out of range");
    }
    out = LogRecord{};
    out.kind = static_cast<RecordKind>(kind);
    out.payload_size = get_u16(at + kOffRecordPayloadSize);
    if (out.payload_size > kMaxRecordPayload) {
        return fail(ErrorCode::InvalidArgument, "replay: record payload size out of range");
    }
    out.epoch.value = get_u32(at + kOffEpoch);
    out.tick = get_u64(at + kOffTick);
    out.sequence = get_u32(at + kOffSequence);

    out.command.type = get_u32(at + kOffCommandType);
    out.command.tick = get_u64(at + kOffCommandTick);
    out.command.participant = gameplay::ParticipantId::from_bits(get_u64(at + kOffParticipant));
    out.command.source = gameplay::ControlSourceId::from_bits(get_u64(at + kOffSource));
    out.command.target = ecs::Entity::from_bits(get_u64(at + kOffTarget));
    out.command.group = gameplay::GroupId::from_bits(get_u64(at + kOffGroup));
    if (at[kOffProvenanceKind] >= static_cast<u8>(gameplay::ControlSourceKind::Count)) {
        return fail(ErrorCode::InvalidArgument, "replay: provenance kind out of range");
    }
    out.command.provenance.kind = static_cast<gameplay::ControlSourceKind>(at[kOffProvenanceKind]);
    out.command.payload_size = get_u16(at + kOffCommandPayloadSize);
    if (out.command.payload_size > gameplay::kMaxCommandPayload) {
        return fail(ErrorCode::InvalidArgument, "replay: command payload size out of range");
    }
    out.command.provenance.source = get_u32(at + kOffProvenanceSource);
    out.command.sequence = get_u32(at + kOffCommandSequence);

    out.subject = get_u64(at + kOffSubject);
    out.value = get_u64(at + kOffValue);

    std::memcpy(out.command.payload, at + kOffCommandPayload, gameplay::kMaxCommandPayload);
    std::memcpy(out.payload, at + kOffRecordPayload, kMaxRecordPayload);
    return ok();
}

u64 record_hash(u64 accumulator, const LogRecord& record) noexcept {
    u64 hash = accumulator == 0 ? kFnvOffset : accumulator;
    hash = fold(hash, static_cast<u64>(record.kind));
    hash = fold(hash, record.epoch.value);
    hash = fold(hash, record.tick);
    hash = fold(hash, record.sequence);
    hash = fold(hash, record.subject);
    hash = fold(hash, record.value);
    hash = fold(hash, record.command.type);
    hash = fold(hash, record.command.tick);
    hash = fold(hash, record.command.participant.bits());
    hash = fold(hash, record.command.target.bits());
    hash = fold(hash, record.command.group.bits());
    hash = fold(hash, record.command.sequence);
    // NOT `command.source` and NOT `command.provenance`. Both say where a command came from, and a
    // replay's records carry `Replay` provenance where the originals carried `Human`: a digest that
    // included either would make every replay differ from the session it reproduces, and the first
    // repair anyone reached for would be to make the replay lie about its own origin.
    hash = fold(hash, record.command.payload_size);
    for (u16 index = 0; index < record.command.payload_size; ++index) {
        hash = fold(hash, record.command.payload[index]);
    }
    hash = fold(hash, record.payload_size);
    for (u16 index = 0; index < record.payload_size; ++index) {
        hash = fold(hash, record.payload[index]);
    }
    return hash;
}

// --- The five bindings ---------------------------------------------------------------------------
//
// Built from the reader types themselves, never restated. `bind_reader<R>()` reads
// `R::record_type::kRecordTypeId`, so a reader that substituted a record of its own would either
// fail to satisfy `ReadsTheOneRecord` (a compile error) or, if it named a type with a different
// identity, carry that identity here — which `tests/test_one_record.cpp` fails on by name.

static_assert(ReadsTheOneRecord<PlaybackCursor>);
static_assert(ReadsTheOneRecord<RollbackCursor>);
static_assert(ReadsTheOneRecord<ReplicationInputCursor>);
static_assert(ReadsTheOneRecord<CrashReplayBuffer>);
static_assert(ReadsTheOneRecord<DivergenceCursor>);

Span<const LogReaderBinding> log_readers() noexcept {
    static const LogReaderBinding kBindings[] = {
        bind_reader<PlaybackCursor>(LogReader::Playback, "cy::replay::PlaybackCursor"),
        bind_reader<RollbackCursor>(LogReader::Rollback, "cy::replay::RollbackCursor"),
        bind_reader<ReplicationInputCursor>(LogReader::ReplicationInput,
                                            "cy::replay::ReplicationInputCursor"),
        bind_reader<CrashReplayBuffer>(LogReader::CrashBuffer, "cy::replay::CrashReplayBuffer"),
        bind_reader<DivergenceCursor>(LogReader::Validator, "cy::replay::DivergenceCursor"),
    };
    static_assert(sizeof(kBindings) / sizeof(kBindings[0]) == kLogReaderCount,
                  "every reader named by LogReader is bound here, and a sixth reader added to the "
                  "enumeration without a binding stops the build rather than the test");
    return {kBindings, kLogReaderCount};
}

}  // namespace cy::replay
