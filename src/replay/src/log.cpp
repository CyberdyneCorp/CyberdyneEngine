// The log: chunked over tick ranges, indexed, and reconstructible from its own bytes. M9 task 1.1.

#include <cy/replay/log.h>

#include <cstring>

namespace cy::replay {
namespace {

constexpr u32 kHeaderSize = 96;

/// One index entry: two u64 ticks, two u32 record positions, a u64 checkpoint tick and a u32
/// checkpoint position. ONE constant for the writer and the reader — the first version of this file
/// had the reader's copy four bytes short, which reported every file it wrote as `Corrupt`.
constexpr usize kIndexEntrySize = 8 + 8 + 4 + 4 + 8 + 4;

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

[[nodiscard]] Status append_u32(Array<u8>& out, u32 value) noexcept {
    u8 bytes[4] = {};
    put_u32(bytes, value);
    return out.append(Span<const u8>(bytes, 4));
}

[[nodiscard]] Status append_u64(Array<u8>& out, u64 value) noexcept {
    u8 bytes[8] = {};
    put_u64(bytes, value);
    return out.append(Span<const u8>(bytes, 8));
}

}  // namespace

const char* compatibility_name(Compatibility value) noexcept {
    switch (value) {
        case Compatibility::Reproducible:
            return "Reproducible";
        case Compatibility::Compatible:
            return "Compatible";
        case Compatibility::Incompatible:
            return "Incompatible";
    }
    return "Incompatible";
}

const char* reject_reason_name(RejectReason value) noexcept {
    switch (value) {
        case RejectReason::None:
            return "None";
        case RejectReason::BuildMismatch:
            return "BuildMismatch";
        case RejectReason::ContentMismatch:
            return "ContentMismatch";
        case RejectReason::SchemaOutsideWindow:
            return "SchemaOutsideWindow";
        case RejectReason::ProfileMismatch:
            return "ProfileMismatch";
        case RejectReason::TickRateMismatch:
            return "TickRateMismatch";
        case RejectReason::Corrupt:
            return "Corrupt";
    }
    return "Corrupt";
}

CompatibilityVerdict classify(const CompatibilityManifest& recorded,
                              const CompatibilityManifest& current,
                              const MigrationWindow& window) noexcept {
    // The tick rate first, because it is the one mismatch no migration window can cover: a replay
    // recorded at 30 Hz replayed at 60 does not produce the same state at any tick.
    if (recorded.tick_rate_numerator != current.tick_rate_numerator ||
        recorded.tick_rate_denominator != current.tick_rate_denominator) {
        return {Compatibility::Incompatible, RejectReason::TickRateMismatch,
                "the replay's tick rate is not this build's"};
    }
    if (recorded.profile != current.profile) {
        // A replay recorded under a weaker profile does not carry what a stronger one would verify,
        // and one recorded under a stronger profile makes a claim this session is not making.
        return {Compatibility::Incompatible, RejectReason::ProfileMismatch,
                "the replay's determinism profile is not this session's"};
    }
    if (recorded.command_schema_version < window.oldest_command_schema ||
        recorded.state_schema_version < window.oldest_state_schema ||
        recorded.command_schema_version > current.command_schema_version ||
        recorded.state_schema_version > current.state_schema_version) {
        return {Compatibility::Incompatible, RejectReason::SchemaOutsideWindow,
                "the replay's schema versions are outside the declared migration window"};
    }
    if (recorded.engine_build == current.engine_build &&
        recorded.project_build == current.project_build &&
        recorded.plugin_lockfile_hash == current.plugin_lockfile_hash &&
        recorded.content_manifest_hash == current.content_manifest_hash &&
        recorded.command_schema_version == current.command_schema_version &&
        recorded.state_schema_version == current.state_schema_version) {
        // The only class under which a bit-exact claim is made. Everything else is "we will play
        // it, and we are not promising the same numbers".
        return {Compatibility::Reproducible, RejectReason::None, "identical build and content"};
    }
    if (recorded.content_manifest_hash != current.content_manifest_hash) {
        return {Compatibility::Compatible, RejectReason::ContentMismatch,
                "the content manifest differs; playback is within the migration window and is not "
                "bit-exact"};
    }
    return {Compatibility::Compatible, RejectReason::BuildMismatch,
            "the build differs; playback is within the migration window and is not bit-exact"};
}

Status RecordLog::append(const LogRecord& record) noexcept {
    if (any_ && record.tick < last_tick_) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: records are appended in tick order; the order they arrive in is the "
                    "order the simulation consumed them, and a log that sorted would be a log with "
                    "an opinion");
    }
    const u32 index = static_cast<u32>(records_.size());
    if (Status added = records_.push_back(record); !added) {
        return added;
    }

    const u64 chunk_first = (record.tick / kChunkTicks) * kChunkTicks;
    if (chunks_.empty() || chunks_[chunks_.size() - 1].first_tick != chunk_first) {
        ChunkIndexEntry entry;
        entry.first_tick = chunk_first;
        entry.last_tick = record.tick;
        entry.first_record = index;
        entry.record_count = 0;
        // A new chunk inherits the last checkpoint the previous one knew about, so "the nearest
        // checkpoint at or before this tick" is answerable from one entry rather than by walking
        // back through the table.
        if (!chunks_.empty()) {
            entry.checkpoint_tick = chunks_[chunks_.size() - 1].checkpoint_tick;
            entry.checkpoint_record = chunks_[chunks_.size() - 1].checkpoint_record;
        }
        if (Status added = chunks_.push_back(entry); !added) {
            (void)records_.resize(index);
            return added;
        }
    }
    ChunkIndexEntry& chunk = chunks_[chunks_.size() - 1];
    chunk.last_tick = record.tick;
    ++chunk.record_count;
    if (record.kind == RecordKind::Checkpoint) {
        chunk.checkpoint_tick = record.tick;
        chunk.checkpoint_record = index;
    }

    last_tick_ = record.tick;
    any_ = true;
    return ok();
}

u32 RecordLog::lower_bound(u64 tick) const noexcept {
    // Binary search over the chunk table, then a short scan inside one chunk. Never a scan of the
    // whole log: "The replay index SHALL allow locating the nearest checkpoint at or before a
    // target tick without scanning the file."
    if (chunks_.empty()) {
        return 0;
    }
    usize low = 0;
    usize high = chunks_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (chunks_[middle].last_tick < tick) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == chunks_.size()) {
        return static_cast<u32>(records_.size());
    }
    const ChunkIndexEntry& chunk = chunks_[low];
    for (u32 offset = 0; offset < chunk.record_count; ++offset) {
        if (records_[chunk.first_record + offset].tick >= tick) {
            return chunk.first_record + offset;
        }
    }
    return chunk.first_record + chunk.record_count;
}

bool RecordLog::nearest_checkpoint(u64 tick, LogRecord& out) const noexcept {
    if (chunks_.empty()) {
        return false;
    }
    // The chunk containing `tick`, or the last one before it.
    usize low = 0;
    usize high = chunks_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (chunks_[middle].first_tick <= tick) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        return false;
    }
    const ChunkIndexEntry& chunk = chunks_[low - 1];
    // A checkpoint inside this chunk may be *after* `tick`; the entry records the chunk's last one.
    // Walk this chunk's own records back to the last at or before `tick`, and fall back to what the
    // chunk inherited.
    for (u32 offset = chunk.record_count; offset > 0; --offset) {
        const LogRecord& record = records_[chunk.first_record + offset - 1];
        if (record.kind == RecordKind::Checkpoint && record.tick <= tick) {
            out = record;
            return true;
        }
    }
    if (chunk.checkpoint_tick == ChunkIndexEntry::kNoCheckpoint) {
        return false;
    }
    if (chunk.checkpoint_tick > tick) {
        return false;
    }
    out = records_[chunk.checkpoint_record];
    return true;
}

u64 RecordLog::hash() const noexcept {
    u64 hash = 0;
    for (const LogRecord& record : records_) {
        hash = record_hash(hash, record);
    }
    return hash;
}

void RecordLog::clear() noexcept {
    records_.clear();
    chunks_.clear();
    last_tick_ = 0;
    any_ = false;
}

// --- The deterministic run-length encoding
// --------------------------------------------------------
//
// A run is a length byte and a value byte for a repeat, or a length byte and that many literals.
// `0x80` distinguishes them; runs are capped at 127 so the length always fits. Deterministic by
// construction — the encoder makes the same choice for the same bytes every time, which is what a
// file digest a lockstep session compares depends on.

namespace {

/// How many bytes from `at` repeat the byte at `at`, capped at 127 so the length fits the control
/// byte beside the flag bit.
[[nodiscard]] usize repeat_length(Span<const u8> input, usize at) noexcept {
    usize run = 1;
    while (at + run < input.size() && run < 127 && input[at + run] == input[at]) {
        ++run;
    }
    return run;
}

/// How many bytes from `at` to emit literally: up to the next run of three, capped at 127. Never
/// zero, so the encoder always makes progress.
[[nodiscard]] usize literal_length(Span<const u8> input, usize at) noexcept {
    usize literals = 0;
    while (at + literals < input.size() && literals < 127) {
        const usize remaining = input.size() - at - literals;
        const bool run_starts_here = remaining >= 3 &&
                                     input[at + literals] == input[at + literals + 1] &&
                                     input[at + literals] == input[at + literals + 2];
        if (run_starts_here) {
            break;
        }
        ++literals;
    }
    return literals == 0 ? 1 : literals;
}

}  // namespace

Status compress(Span<const u8> input, Array<u8>& out) noexcept {
    usize index = 0;
    while (index < input.size()) {
        const usize run = repeat_length(input, index);
        if (run >= 3) {
            if (Status pushed = out.push_back(static_cast<u8>(0x80U | run)); !pushed) {
                return pushed;
            }
            if (Status pushed = out.push_back(input[index]); !pushed) {
                return pushed;
            }
            index += run;
            continue;
        }
        const usize literals = literal_length(input, index);
        if (Status pushed = out.push_back(static_cast<u8>(literals)); !pushed) {
            return pushed;
        }
        if (Status appended = out.append(Span<const u8>(input.data() + index, literals));
            !appended) {
            return appended;
        }
        index += literals;
    }
    return ok();
}

Status decompress(Span<const u8> input, usize expected_size, Array<u8>& out) noexcept {
    const usize mark = out.size();
    usize index = 0;
    while (index < input.size()) {
        const u8 control = input[index++];
        const usize length = control & 0x7FU;
        if (length == 0) {
            return fail(ErrorCode::InvalidArgument,
                        "replay: zero-length run in a compressed chunk");
        }
        if ((control & 0x80U) != 0) {
            if (index >= input.size()) {
                return fail(ErrorCode::InvalidArgument, "replay: truncated run in a chunk");
            }
            const u8 value = input[index++];
            for (usize repeat = 0; repeat < length; ++repeat) {
                if (Status pushed = out.push_back(value); !pushed) {
                    return pushed;
                }
            }
            continue;
        }
        if (index + length > input.size()) {
            return fail(ErrorCode::InvalidArgument, "replay: truncated literal block in a chunk");
        }
        if (Status appended = out.append(Span<const u8>(input.data() + index, length)); !appended) {
            return appended;
        }
        index += length;
    }
    if (out.size() - mark != expected_size) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a chunk expanded to a different size than its header declared");
    }
    return ok();
}

namespace {

/// The header: magic, format, the record type's identity, the manifest, the counts and the digest.
[[nodiscard]] Status append_header(const RecordLog& log, Array<u8>& out) noexcept {
    const CompatibilityManifest& manifest = log.manifest();
    u8 header[kHeaderSize] = {};
    put_u32(header + 0, kLogMagic);
    put_u32(header + 4, kLogFormatVersion);
    put_u32(header + 8, LogRecord::kRecordTypeId);
    put_u32(header + 12, kEncodedRecordSize);
    put_u64(header + 16, manifest.engine_build);
    put_u64(header + 24, manifest.project_build);
    put_u64(header + 32, manifest.plugin_lockfile_hash);
    put_u64(header + 40, manifest.content_manifest_hash);
    put_u64(header + 48, manifest.session_seed);
    put_u32(header + 56, manifest.tick_rate_numerator);
    put_u32(header + 60, manifest.tick_rate_denominator);
    put_u32(header + 64, manifest.command_schema_version);
    put_u32(header + 68, manifest.state_schema_version);
    put_u32(header + 72, static_cast<u32>(manifest.profile));
    put_u32(header + 76, log.size());
    put_u32(header + 80, log.chunk_count());
    // The log's own value identity, so a damaged file is `Corrupt` rather than a replay that
    // silently plays something else.
    put_u64(header + 84, log.hash());
    return out.append(Span<const u8>(header, kHeaderSize));
}

/// The index: one fixed-width entry per chunk, `kIndexEntrySize` bytes each.
[[nodiscard]] Status append_index(const RecordLog& log, Array<u8>& out) noexcept {
    for (u32 index = 0; index < log.chunk_count(); ++index) {
        const ChunkIndexEntry& chunk = log.chunk(index);
        const u64 words[] = {chunk.first_tick, chunk.last_tick, chunk.checkpoint_tick};
        const u32 halves[] = {chunk.first_record, chunk.record_count, chunk.checkpoint_record};
        if (Status added = append_u64(out, words[0]); !added) {
            return added;
        }
        if (Status added = append_u64(out, words[1]); !added) {
            return added;
        }
        if (Status added = append_u32(out, halves[0]); !added) {
            return added;
        }
        if (Status added = append_u32(out, halves[1]); !added) {
            return added;
        }
        if (Status added = append_u64(out, words[2]); !added) {
            return added;
        }
        if (Status added = append_u32(out, halves[2]); !added) {
            return added;
        }
    }
    return ok();
}

/// One chunk's records, encoded then compressed, behind its plain and packed sizes.
[[nodiscard]] Status append_chunk(const RecordLog& log, const ChunkIndexEntry& chunk,
                                  Array<u8>& plain, Array<u8>& packed, Array<u8>& out) noexcept {
    plain.clear();
    packed.clear();
    for (u32 offset = 0; offset < chunk.record_count; ++offset) {
        if (Status encoded = encode(log.at(chunk.first_record + offset), plain); !encoded) {
            return encoded;
        }
    }
    if (Status compressed = compress(plain.span(), packed); !compressed) {
        return compressed;
    }
    if (Status added = append_u32(out, static_cast<u32>(plain.size())); !added) {
        return added;
    }
    if (Status added = append_u32(out, static_cast<u32>(packed.size())); !added) {
        return added;
    }
    return out.append(packed.span());
}

}  // namespace

Status write_log(const RecordLog& log, Array<u8>& out) noexcept {
    if (Status written = append_header(log, out); !written) {
        return written;
    }
    if (Status written = append_index(log, out); !written) {
        return written;
    }
    // One compressed block per chunk. Chunked rather than written per record, so reading a chunk is
    // one seek and one expansion.
    Array<u8> plain(out.allocator());
    Array<u8> packed(out.allocator());
    for (u32 index = 0; index < log.chunk_count(); ++index) {
        if (Status written = append_chunk(log, log.chunk(index), plain, packed, out); !written) {
            return written;
        }
    }
    return ok();
}

namespace {

/// The header's fixed prefix, checked before anything is read past it.
[[nodiscard]] Status check_header(Span<const u8> bytes, RejectReason& why) noexcept {
    if (bytes.size() < kHeaderSize) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: fewer bytes than a header");
    }
    const u8* header = bytes.data();
    if (get_u32(header + 0) != kLogMagic || get_u32(header + 8) != LogRecord::kRecordTypeId ||
        get_u32(header + 12) != kEncodedRecordSize) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument,
                    "replay: these bytes are not a replay written by this record type");
    }
    if (get_u32(header + 4) != kLogFormatVersion) {
        why = RejectReason::SchemaOutsideWindow;
        return fail(ErrorCode::Unsupported, "replay: a replay format this build does not read");
    }
    return ok();
}

/// One chunk: its two sizes, its compressed body, and the records it expands to, appended to `out`.
/// Advances `cursor` past the chunk.
[[nodiscard]] Status read_chunk(Span<const u8> bytes, usize& cursor, Array<u8>& plain,
                                RecordLog& out, RejectReason& why) noexcept {
    if (cursor + 8 > bytes.size()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: truncated chunk header");
    }
    const u32 plain_size = get_u32(bytes.data() + cursor);
    const u32 packed_size = get_u32(bytes.data() + cursor + 4);
    cursor += 8;
    if (cursor + packed_size > bytes.size()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: truncated chunk");
    }
    plain.clear();
    if (Status expanded =
            decompress(Span<const u8>(bytes.data() + cursor, packed_size), plain_size, plain);
        !expanded) {
        why = RejectReason::Corrupt;
        return expanded;
    }
    cursor += packed_size;

    for (usize offset = 0; offset + kEncodedRecordSize <= plain.size();
         offset += kEncodedRecordSize) {
        LogRecord record;
        if (Status decoded =
                decode(Span<const u8>(plain.data() + offset, kEncodedRecordSize), record);
            !decoded) {
            why = RejectReason::Corrupt;
            return decoded;
        }
        if (Status added = out.append(record); !added) {
            // A REFUSAL WITH NO REASON IS A REFUSAL A SUPPORT QUEUE CANNOT ANSWER, and this branch
            // produced seventy of them. `RecordLog::append()` refuses a record whose tick goes
            // backwards, which is exactly what a corrupted tick field in a chunk produces — and the
            // reason was left at `None`, so `read_log()` reported a damaged file as a refusal with
            // no verdict at all. `replay-and-rollback`: "the reason SHALL distinguish a build or
            // content mismatch from a damaged file". The defect was invisible because nothing ever
            // fed `read_log()` a file it had not just written itself; `tests/determinism/
            // test_replay_fuzz.cpp` is the sweep that found it.
            //
            // Only `InvalidArgument` is a verdict about the FILE. An allocator that could not grow
            // the log says nothing about the bytes, so it is left without a reason rather than
            // reported as damage.
            if (added.error().code == ErrorCode::InvalidArgument) {
                why = RejectReason::Corrupt;
            }
            return added;
        }
    }
    return ok();
}

}  // namespace

Status read_log(Span<const u8> bytes, RecordLog& out, RejectReason& why) noexcept {
    why = RejectReason::None;
    if (Status checked = check_header(bytes, why); !checked) {
        return checked;
    }

    const u8* header = bytes.data();
    const u32 record_count = get_u32(header + 76);
    const u32 chunk_count = get_u32(header + 80);
    const u64 recorded_hash = get_u64(header + 84);

    out.clear();
    usize cursor = kHeaderSize;
    if (cursor + (static_cast<usize>(chunk_count) * kIndexEntrySize) > bytes.size()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: the index does not fit the file");
    }
    // The index is re-derived from the records by `append()`, so it is read past rather than
    // trusted: an index that disagreed with the records would be a second opinion about the same
    // thing, which is the failure this milestone is named after.
    cursor += static_cast<usize>(chunk_count) * kIndexEntrySize;

    Array<u8> plain(out.allocator());
    for (u32 chunk = 0; chunk < chunk_count; ++chunk) {
        if (Status read = read_chunk(bytes, cursor, plain, out, why); !read) {
            return read;
        }
    }

    if (out.size() != record_count) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument,
                    "replay: the file holds a different number of records than its header claims");
    }
    if (out.hash() != recorded_hash) {
        // A damaged file, and said so: `replay-and-rollback` requires the reason to distinguish a
        // build or content mismatch from a damaged one, and this is the damaged branch.
        why = RejectReason::Corrupt;
        return fail(ErrorCode::Io, "replay: the records do not hash to what the header recorded");
    }
    return ok();
}

}  // namespace cy::replay
