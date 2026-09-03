#include "writer.h"

#include "internal.h"
#include "platform_bits.h"

#include <cy/core/diagnostics/log.h>

#include <chrono>
#include <cstring>

namespace cy::diag {
namespace {

using format::ChunkHeader;
using format::FieldRecord;
using format::FileHeader;
using format::RecordBody;
using format::RecordHeader;

constexpr u32 align_up(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void append_bytes(std::vector<u8>& out, const void* data, usize bytes) {
    const u8* source = static_cast<const u8*>(data);
    out.insert(out.end(), source, source + bytes);
}

void append_u32(std::vector<u8>& out, u32 value) {
    append_bytes(out, &value, sizeof(value));
}

void append_string(std::vector<u8>& out, const char* text) {
    const usize length = (text == nullptr) ? 0 : std::strlen(text);
    const u16 stored = static_cast<u16>((length > 0xFFFEu) ? 0xFFFEu : length);
    append_bytes(out, &stored, sizeof(stored));
    append_bytes(out, text, stored);
}

const FieldRecord* fields_of(const u8* record) noexcept {
    const u8* base = record + format::kRecordFixedBytes;
    // The ring aligns every record to eight bytes and the fixed part is a multiple of eight, so the
    // field array is aligned; the copy keeps that fact from being an assumption in the reader.
    return reinterpret_cast<const FieldRecord*>(base);
}

const u8* text_of(const u8* record, u16 field_count) noexcept {
    return record + format::kRecordFixedBytes + (usize{field_count} * sizeof(FieldRecord));
}

u64 wall_clock_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace

TraceWriter::~TraceWriter() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

Expected<TraceId, cy::Error> TraceWriter::open(const TraceConfig& config,
                                               TraceId trace_id) noexcept {
    file_ = std::fopen(config.path, "wb");
    if (file_ == nullptr) {
        return fail(ErrorCode::Io, "the trace file could not be created");
    }
    config_ = config;
    policy_ = config.policy;
    stats_ = TraceStats{};
    stats_.trace_id = trace_id;

    FileHeader header{};
    std::memcpy(header.magic, format::kMagic, sizeof(header.magic));
    header.format_version = format::kFormatVersion;
    header.header_bytes = static_cast<u32>(sizeof(FileHeader));
    header.trace_id = trace_id;
    header.wall_ns = wall_clock_ns();
    header.monotonic_ns = monotonic_now_ns();
    header.process_id = platform_process_id();
    header.max_classification = static_cast<u8>(policy_.ceiling());
    header.compression = static_cast<u8>(format::Compression::None);
    if (!write_bytes(&header, sizeof(header))) {
        std::fclose(file_);
        file_ = nullptr;
        return fail(ErrorCode::Io, "the trace header could not be written");
    }
    // A metadata chunk now as well as at close, so a capture truncated by a crash still resolves
    // the identifiers registered before it started.
    write_metadata_chunk();
    return trace_id;
}

bool TraceWriter::write_bytes(const void* data, usize bytes) noexcept {
    if (file_ == nullptr || write_failed_) {
        return false;
    }
    if (bytes != 0 && std::fwrite(data, 1, bytes, file_) != bytes) {
        write_failed_ = true;
        return false;
    }
    stats_.bytes_written += bytes;
    return true;
}

void TraceWriter::write_chunk(u32 tag, u32 thread_index, const u8* payload, usize bytes,
                              u64 first_timestamp, u64 last_timestamp) noexcept {
    if (file_ == nullptr) {
        return;
    }
    IndexEntry entry{};
    entry.tag = tag;
    entry.thread_index = thread_index;
    entry.offset = static_cast<u64>(std::ftell(file_));
    entry.payload_bytes = bytes;
    entry.first_timestamp = first_timestamp;
    entry.last_timestamp = last_timestamp;

    ChunkHeader header{};
    header.tag = tag;
    header.flags = thread_index;
    header.payload_bytes = bytes;
    header.uncompressed_bytes = bytes;
    if (write_bytes(&header, sizeof(header)) && write_bytes(payload, bytes)) {
        index_.push_back(entry);
    }
}

void TraceWriter::begin_thread_chunk(u32 thread_index) noexcept {
    current_thread_ = thread_index;
    staging_.clear();
    chunk_first_timestamp_ = 0;
    chunk_last_timestamp_ = 0;
}

void TraceWriter::end_thread_chunk() noexcept {
    if (staging_.empty()) {
        return;
    }
    write_chunk(format::kChunkEvents, current_thread_, staging_.data(), staging_.size(),
                chunk_first_timestamp_, chunk_last_timestamp_);
    staging_.clear();
}

void TraceWriter::append_record(const u8* record, u32 size) noexcept {
    if (file_ == nullptr || size < format::kRecordFixedBytes) {
        return;
    }
    redact_into_staging(record, size);
    ++stats_.events_written;
}

/// Decide one field. Returns the field record to write, having copied any admitted text into
/// `scratch_`. This is the whole of the privacy rule, and it reads as one paragraph on purpose.
FieldRecord TraceWriter::redact_field(const FieldRecord& source, const u8* text,
                                      u16 text_bytes) noexcept {
    FieldRecord out = source;
    FieldInfo info{};
    const bool classified = lookup_field(source.field, info);
    if (!classified) {
        ++stats_.unclassified_fields;
    } else if (!policy_.allows(info.privacy)) {
        ++stats_.redacted_fields;
    } else if (info.type != FieldType::Text) {
        return out;
    } else {
        const u32 length = static_cast<u32>(source.bits);
        if (u32{source.text_offset} + length <= u32{text_bytes}) {
            out.text_offset = static_cast<u16>(scratch_.size());
            out.bits = length;
            scratch_.insert(scratch_.end(), text + source.text_offset,
                            text + source.text_offset + length);
            return out;
        }
    }
    out.flags |= format::kFieldRedacted;
    out.bits = 0;
    out.text_offset = 0;
    return out;
}

void TraceWriter::redact_into_staging(const u8* record, u32 size) noexcept {
    RecordHeader header{};
    RecordBody body{};
    std::memcpy(&header, record, sizeof(header));
    std::memcpy(&body, record + sizeof(header), sizeof(body));

    const u32 field_count = body.field_count;
    if (format::kRecordFixedBytes + (field_count * sizeof(FieldRecord)) > size) {
        return;  // malformed: not written rather than trusted
    }
    const FieldRecord* fields = fields_of(record);
    const u8* text = text_of(record, body.field_count);

    scratch_.clear();
    kept_.clear();
    for (u32 index = 0; index < field_count; ++index) {
        kept_.push_back(redact_field(fields[index], text, body.text_bytes));
    }

    const u32 payload = format::kRecordFixedBytes +
                        static_cast<u32>(field_count * sizeof(FieldRecord)) +
                        static_cast<u32>(scratch_.size());
    RecordHeader out_header = header;
    out_header.size = static_cast<u16>(align_up(payload, format::kRecordAlignment));
    RecordBody out_body = body;
    out_body.text_bytes = static_cast<u16>(scratch_.size());

    if (chunk_first_timestamp_ == 0) {
        chunk_first_timestamp_ = body.timestamp_ns;
    }
    chunk_last_timestamp_ = body.timestamp_ns;

    append_bytes(staging_, &out_header, sizeof(out_header));
    append_bytes(staging_, &out_body, sizeof(out_body));
    if (!kept_.empty()) {
        append_bytes(staging_, kept_.data(), kept_.size() * sizeof(FieldRecord));
    }
    if (!scratch_.empty()) {
        append_bytes(staging_, scratch_.data(), scratch_.size());
    }
    staging_.resize(staging_.size() + (out_header.size - payload), 0);

    report_to_console(out_header, out_body, kept_.data(), static_cast<u32>(kept_.size()),
                      scratch_.data());
}

void TraceWriter::report_to_console(const RecordHeader& header, const RecordBody& body,
                                    const FieldRecord* fields, u32 field_count,
                                    const u8* text) const noexcept {
    // Presentation, not emission: the record was written by a producer that formatted nothing, and
    // it is formatted here, once, by the consumer, only if someone asked to see it.
    if (config_.console_level == LogLevel::Off || header.kind != static_cast<u8>(EventKind::Log)) {
        return;
    }
    const auto level = static_cast<LogLevel>(body.a);
    if (static_cast<u8>(level) < static_cast<u8>(config_.console_level)) {
        return;
    }
    const char* message = lookup_name(header.name);
    const char* site = lookup_name(static_cast<NameId>(body.b));
    std::fprintf(stderr, "%-7s %s %s", log_level_name(level), (message != nullptr) ? message : "?",
                 (site != nullptr) ? site : "");
    for (u32 index = 0; index < field_count; ++index) {
        FieldInfo info{};
        if (!lookup_field(fields[index].field, info)) {
            continue;
        }
        if ((fields[index].flags & format::kFieldRedacted) != 0) {
            std::fprintf(stderr, " %s=<redacted:%s>", info.name, privacy_name(info.privacy));
        } else if (info.type == FieldType::Text) {
            std::fprintf(stderr, " %s=%.*s", info.name, static_cast<int>(fields[index].bits),
                         reinterpret_cast<const char*>(text) + fields[index].text_offset);
        } else {
            std::fprintf(stderr, " %s=%llu", info.name,
                         static_cast<unsigned long long>(fields[index].bits));
        }
    }
    std::fputc('\n', stderr);
}

void TraceWriter::record_loss(u32 thread_index, Channel channel, format::LossReason reason,
                              u64 count) noexcept {
    if (count == 0) {
        return;
    }
    losses_.push_back(
        LossEntry{thread_index, static_cast<u8>(channel), static_cast<u8>(reason), count});
    // Only a refused record is a dropped event. A redacted field and a registration the metadata
    // table had no room for are losses of a different kind, reported in the same chunk and counted
    // separately, so "how many events did this capture lose" stays answerable.
    if (reason == format::LossReason::BufferPressure ||
        reason == format::LossReason::RecordTooLarge) {
        stats_.dropped[static_cast<u32>(channel)] += count;
    }
}

void TraceWriter::write_metadata_chunk() noexcept {
    const RegistryStats counts = registry_stats();
    std::vector<u8> payload;

    append_u32(payload, counts.names);
    for (u32 id = 1; id <= counts.names; ++id) {
        append_u32(payload, id);
        append_string(payload, lookup_name(id));
    }
    append_u32(payload, counts.categories);
    for (u32 id = 1; id <= counts.categories; ++id) {
        append_u32(payload, id);
        append_string(payload, lookup_category(id));
    }
    append_u32(payload, counts.fields);
    for (u32 id = 1; id <= counts.fields; ++id) {
        FieldInfo info{};
        lookup_field(id, info);
        append_u32(payload, id);
        const u8 type = static_cast<u8>(info.type);
        const u8 privacy = static_cast<u8>(info.privacy);
        append_bytes(payload, &type, 1);
        append_bytes(payload, &privacy, 1);
        append_string(payload, info.name);
    }

    const char* identity[][2] = {
        {"engine_version", CY_DIAG_ENGINE_VERSION},
        {"build_configuration", CY_DIAG_BUILD_CONFIGURATION},
        {"build_identity", (config_.build_identity != nullptr) ? config_.build_identity : ""},
        {"export_policy", privacy_name(policy_.ceiling())},
    };
    append_u32(payload, static_cast<u32>(sizeof(identity) / sizeof(identity[0])));
    for (const auto& pair : identity) {
        append_string(payload, pair[0]);
        append_string(payload, pair[1]);
    }

    write_chunk(format::kChunkMeta, 0, payload.data(), payload.size(), 0, 0);
}

void TraceWriter::write_loss_chunk() noexcept {
    std::vector<u8> payload;
    append_u32(payload, static_cast<u32>(losses_.size()));
    for (const LossEntry& entry : losses_) {
        append_u32(payload, entry.thread_index);
        append_bytes(payload, &entry.channel, 1);
        append_bytes(payload, &entry.reason, 1);
        const u16 pad = 0;
        append_bytes(payload, &pad, sizeof(pad));
        append_bytes(payload, &entry.count, sizeof(entry.count));
    }
    write_chunk(format::kChunkLoss, 0, payload.data(), payload.size(), 0, 0);
}

void TraceWriter::write_index_chunk() noexcept {
    const u64 offset = static_cast<u64>(std::ftell(file_));
    std::vector<u8> payload;
    append_u32(payload, static_cast<u32>(index_.size()));
    for (const IndexEntry& entry : index_) {
        append_u32(payload, entry.tag);
        append_u32(payload, entry.thread_index);
        append_bytes(payload, &entry.offset, sizeof(entry.offset));
        append_bytes(payload, &entry.payload_bytes, sizeof(entry.payload_bytes));
        append_bytes(payload, &entry.first_timestamp, sizeof(entry.first_timestamp));
        append_bytes(payload, &entry.last_timestamp, sizeof(entry.last_timestamp));
    }
    append_bytes(payload, &stats_.events_written, sizeof(stats_.events_written));
    u64 dropped_total = 0;
    for (u64 count : stats_.dropped) {
        dropped_total += count;
    }
    append_bytes(payload, &dropped_total, sizeof(dropped_total));

    write_chunk(format::kChunkIndex, 0, payload.data(), payload.size(), 0, 0);
    // The file's last eight bytes are the index chunk's own offset: a reader seeks to the end,
    // reads the index, and loads only the regions it wants.
    write_bytes(&offset, sizeof(offset));
}

void TraceWriter::flush() noexcept {
    if (file_ != nullptr) {
        std::fflush(file_);
    }
}

Expected<TraceStats, cy::Error> TraceWriter::close() noexcept {
    if (file_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the trace is not open");
    }
    write_metadata_chunk();
    // What the policy removed, and what the fixed-capacity tables refused, recorded in the artefact
    // rather than only in the process that wrote it.
    record_loss(0, Channel::Critical, format::LossReason::PolicyRedaction, stats_.redacted_fields);
    record_loss(0, Channel::Critical, format::LossReason::UnclassifiedField,
                stats_.unclassified_fields);
    record_loss(0, Channel::Critical, format::LossReason::RegistryFull, registry_stats().rejected);
    write_loss_chunk();
    write_index_chunk();
    const bool failed = write_failed_;
    std::fclose(file_);
    file_ = nullptr;
    if (failed) {
        return fail(ErrorCode::Io, "the trace file could not be written");
    }
    return stats_;
}

}  // namespace cy::diag
