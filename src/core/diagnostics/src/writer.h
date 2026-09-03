#pragma once
// The artefact writer, and the only place redaction happens.
//
// `diagnostics-profiling-and-crash` — "Privacy classification": a field's classification determines
// whether it is included in an artefact that leaves the machine. Enforcing that here, rather than
// at the producer, is what makes it enforceable: a producer states what a value *is* by declaring
// its field, and the writer decides what may be written by comparing that declaration against the
// artefact's declared ceiling. A field cannot be exported at a level it was never classified for,
// and a field whose id carries no classification at all is redacted and counted.
//
// Redaction keeps the field entry and clears its value, so a reader sees that something was removed
// and what it was called. A gap that is invisible is a gap that misleads.

#include <cy/core/diagnostics/format.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/trace.h>

#include <cstdio>
#include <vector>

namespace cy::diag {

class TraceWriter {
public:
    TraceWriter() = default;
    ~TraceWriter();

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;
    TraceWriter(TraceWriter&&) = delete;
    TraceWriter& operator=(TraceWriter&&) = delete;

    Expected<TraceId, cy::Error> open(const TraceConfig& config, TraceId trace_id) noexcept;

    /// One drain pass, one thread: begin, append every record the ring produced, end. An empty pass
    /// writes no chunk.
    void begin_thread_chunk(u32 thread_index) noexcept;
    void append_record(const u8* record, u32 size) noexcept;
    void end_thread_chunk() noexcept;

    /// A refusal the loss policy made. Recorded twice on purpose: as a Loss record on the timeline,
    /// where the gap is, and as a LOSS chunk entry, where a reader looks for the total.
    void record_loss(u32 thread_index, Channel channel, format::LossReason reason,
                     u64 count) noexcept;

    void flush() noexcept;
    Expected<TraceStats, cy::Error> close() noexcept;

    [[nodiscard]] TraceStats stats() const noexcept { return stats_; }
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

private:
    struct IndexEntry {
        u32 tag = 0;
        u32 thread_index = 0;
        u64 offset = 0;
        u64 payload_bytes = 0;
        u64 first_timestamp = 0;
        u64 last_timestamp = 0;
    };

    struct LossEntry {
        u32 thread_index = 0;
        u8 channel = 0;
        u8 reason = 0;
        u64 count = 0;
    };

    bool write_bytes(const void* data, usize bytes) noexcept;
    void write_chunk(u32 tag, u32 thread_index, const u8* payload, usize bytes, u64 first_timestamp,
                     u64 last_timestamp) noexcept;
    void write_metadata_chunk() noexcept;
    void write_loss_chunk() noexcept;
    void write_index_chunk() noexcept;

    /// Copy one record into `staging_`, dropping every field the policy does not admit.
    void redact_into_staging(const u8* record, u32 size) noexcept;
    format::FieldRecord redact_field(const format::FieldRecord& source, const u8* text,
                                     u16 text_bytes) noexcept;
    void report_to_console(const format::RecordHeader& header, const format::RecordBody& body,
                           const format::FieldRecord* fields, u32 field_count,
                           const u8* text) const noexcept;

    std::FILE* file_ = nullptr;
    TraceConfig config_{};
    ExportPolicy policy_ = ExportPolicy::local();
    TraceStats stats_{};
    std::vector<u8> staging_;
    std::vector<IndexEntry> index_;
    std::vector<LossEntry> losses_;
    std::vector<u8> scratch_;
    std::vector<format::FieldRecord> kept_;
    u32 current_thread_ = 0;
    u64 chunk_first_timestamp_ = 0;
    u64 chunk_last_timestamp_ = 0;
    bool write_failed_ = false;
};

}  // namespace cy::diag
