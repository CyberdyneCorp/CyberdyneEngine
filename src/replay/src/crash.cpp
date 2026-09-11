// The crash artefact: the bounded ring, flushed into something loadable. M9 task 3.4.

#include <cy/replay/crash.h>

#include <cstdio>
#include <cstring>

namespace cy::replay {
namespace {

// A sequential writer and reader rather than a table of fixed offsets. The artefact is variable
// length — a ring of N records — so an offset table would have to be computed anyway, and a cursor
// that cannot run past its end is the shape that makes a truncated file a refusal rather than a
// read of whatever follows it in memory.

class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(&out) {}

    void u8_(u8 value) noexcept {
        if (failed_) {
            return;
        }
        record(out_->push_back(value));
    }

    void u16_(u16 value) noexcept {
        for (u32 byte = 0; byte < 2; ++byte) {
            u8_(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
        }
    }

    void u32_(u32 value) noexcept {
        for (u32 byte = 0; byte < 4; ++byte) {
            u8_(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
        }
    }

    void u64_(u64 value) noexcept {
        for (u32 byte = 0; byte < 8; ++byte) {
            u8_(static_cast<u8>((value >> (byte * 8U)) & 0xFFU));
        }
    }

    /// A fixed-width field, NUL-padded to the end. Padded rather than stopped: a tail left as
    /// whatever happened to be in the buffer would make two artefacts of the same crash differ.
    void text(const char* value, u32 width) noexcept {
        bool ended = value == nullptr;
        for (u32 index = 0; index < width; ++index) {
            const char character = ended ? '\0' : value[index];
            ended = ended || character == '\0';
            u8_(static_cast<u8>(character));
        }
    }

    /// The first failure, or `ok()`. **Once a write has failed every later one is a no-op**, so a
    /// serialiser reads as a list of fields rather than as twenty-seven identical guards, and the
    /// error that comes back is the one that actually happened rather than the last one attempted.
    [[nodiscard]] Status status() const noexcept { return failed_ ? first_ : ok(); }

private:
    void record(const Status& result) noexcept {
        if (!failed_ && !result) {
            failed_ = true;
            first_ = result;
        }
    }

    Array<u8>* out_;
    Status first_ = ok();
    bool failed_ = false;
};

class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !overrun_; }
    [[nodiscard]] usize remaining() const noexcept {
        return overrun_ ? 0 : bytes_.size() - cursor_;
    }

    [[nodiscard]] u8 u8_() noexcept {
        if (remaining() < 1) {
            overrun_ = true;
            return 0;
        }
        return bytes_[cursor_++];
    }

    [[nodiscard]] u16 u16_() noexcept {
        u16 value = 0;
        for (u32 byte = 0; byte < 2; ++byte) {
            value = static_cast<u16>(value | (static_cast<u16>(u8_()) << (byte * 8U)));
        }
        return value;
    }

    [[nodiscard]] u32 u32_() noexcept {
        u32 value = 0;
        for (u32 byte = 0; byte < 4; ++byte) {
            value |= static_cast<u32>(u8_()) << (byte * 8U);
        }
        return value;
    }

    [[nodiscard]] u64 u64_() noexcept {
        u64 value = 0;
        for (u32 byte = 0; byte < 8; ++byte) {
            value |= static_cast<u64>(u8_()) << (byte * 8U);
        }
        return value;
    }

    void text(char* out, u32 width) noexcept {
        for (u32 index = 0; index < width; ++index) {
            out[index] = static_cast<char>(u8_());
        }
        out[width - 1] = '\0';
    }

    [[nodiscard]] Span<const u8> take(usize count) noexcept {
        if (remaining() < count) {
            overrun_ = true;
            return Span<const u8>{};
        }
        const Span<const u8> slice{bytes_.data() + cursor_, count};
        cursor_ += count;
        return slice;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    bool overrun_ = false;
};

void write_manifest(Writer& writer, const CompatibilityManifest& manifest) noexcept {
    writer.u64_(manifest.engine_build);
    writer.u64_(manifest.project_build);
    writer.u64_(manifest.plugin_lockfile_hash);
    writer.u64_(manifest.content_manifest_hash);
    writer.u64_(manifest.session_seed);
    writer.u32_(manifest.tick_rate_numerator);
    writer.u32_(manifest.tick_rate_denominator);
    writer.u16_(manifest.command_schema_version);
    writer.u16_(manifest.state_schema_version);
    writer.u8_(static_cast<u8>(manifest.profile));
}

void read_manifest(Reader& reader, CompatibilityManifest& manifest) noexcept {
    manifest.engine_build = reader.u64_();
    manifest.project_build = reader.u64_();
    manifest.plugin_lockfile_hash = reader.u64_();
    manifest.content_manifest_hash = reader.u64_();
    manifest.session_seed = reader.u64_();
    manifest.tick_rate_numerator = reader.u32_();
    manifest.tick_rate_denominator = reader.u32_();
    manifest.command_schema_version = reader.u16_();
    manifest.state_schema_version = reader.u16_();
    manifest.profile = static_cast<determinism::DeterminismProfile>(reader.u8_());
}

}  // namespace

const char* crash_trigger_name(CrashTrigger trigger) noexcept {
    switch (trigger) {
        case CrashTrigger::Crash:
            return "Crash";
        case CrashTrigger::Assertion:
            return "Assertion";
        case CrashTrigger::ReportedDefect:
            return "ReportedDefect";
    }
    return "Unknown";
}

u32 crash_ring_records(u64 seconds, u32 tick_rate_numerator, u32 tick_rate_denominator,
                       u32 records_per_tick) noexcept {
    if (seconds == 0 || tick_rate_numerator == 0 || tick_rate_denominator == 0 ||
        records_per_tick == 0) {
        return 0;
    }
    const u64 ticks = (seconds * tick_rate_numerator) / tick_rate_denominator;
    const u64 records = ticks * records_per_tick;
    if (records == 0) {
        return 0;
    }
    // Capped rather than wrapped. A ring of four billion records is not a bounded buffer, and a
    // silent truncation to 32 bits would give a session a window it did not ask for.
    return records > 0x0100'0000ULL ? 0x0100'0000U : static_cast<u32>(records);
}

Status CrashArtefact::assemble(const CompatibilityManifest& manifest, const CrashReplayBuffer& ring,
                               CrashTrigger trigger, determinism::SimulationPoint at) noexcept {
    clear();
    if (ring.size() == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a crash artefact over an empty ring reproduces nothing");
    }
    if (Status flushed = ring.flush(records_); !flushed) {
        return flushed;
    }
    manifest_ = manifest;
    trigger_ = trigger;
    at_ = at;
    lost_ = ring.overwritten();
    first_tick_ = records_[0].tick;
    last_tick_ = records_[records_.size() - 1].tick;

    // The recent hashes, DERIVED from the records rather than passed in beside them — see the
    // header. The newest `kRecentHashes` of them, kept in tick order.
    for (const LogRecord& record : records_) {
        if (record.kind != RecordKind::StateHash) {
            continue;
        }
        if (recent_hash_count_ == kRecentHashes) {
            for (u32 slot = 1; slot < kRecentHashes; ++slot) {
                recent_hashes_[slot - 1] = recent_hashes_[slot];
                recent_hash_ticks_[slot - 1] = recent_hash_ticks_[slot];
            }
            --recent_hash_count_;
        }
        recent_hashes_[recent_hash_count_] = record.value;
        recent_hash_ticks_[recent_hash_count_] = record.tick;
        ++recent_hash_count_;
    }
    assembled_ = true;
    return ok();
}

Status CrashArtefact::attach_divergence(const DivergenceReport& report) noexcept {
    if (!assembled_) {
        return fail(ErrorCode::InvalidArgument, "replay: assemble the artefact before attaching");
    }
    if (!report.valid) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: that divergence report was never narrowed");
    }
    divergence_ = report;
    // The names are copied because `FieldDivergence` holds pointers into a schema that will not
    // survive the process, let alone the file.
    const char* component = report.field.component_name;
    const char* field = report.field.field_name;
    std::snprintf(component_name_, sizeof(component_name_), "%s",
                  component == nullptr ? "" : component);
    std::snprintf(field_name_, sizeof(field_name_), "%s", field == nullptr ? "" : field);
    divergence_.field.component_name = component_name_;
    divergence_.field.field_name = field_name_;
    has_divergence_ = true;
    return ok();
}

Status CrashArtefact::write(Array<u8>& out) const noexcept {
    if (!assembled_) {
        return fail(ErrorCode::InvalidArgument, "replay: nothing to write");
    }
    // A LIST OF FIELDS, NOT A LIST OF GUARDS. `Writer` short-circuits on its first failure and
    // `status()` reports it, so the shape of the file is readable here rather than buried under
    // twenty-seven identical `if (!written) return written;` blocks.
    Writer writer(out);
    writer.u32_(kCrashMagic);
    writer.u32_(kCrashFormatVersion);
    write_manifest(writer, manifest_);
    writer.u8_(static_cast<u8>(trigger_));
    writer.u32_(at_.epoch.value);
    writer.u64_(at_.tick);
    writer.u64_(lost_);

    writer.u32_(recent_hash_count_);
    for (u32 index = 0; index < recent_hash_count_; ++index) {
        writer.u64_(recent_hash_ticks_[index]);
        writer.u64_(recent_hashes_[index]);
    }

    writer.u8_(has_divergence_ ? 1U : 0U);
    if (has_divergence_) {
        const determinism::FieldDivergence& field = divergence_.field;
        writer.u8_(field.shape_mismatch ? 1U : 0U);
        writer.u64_(field.entity);
        writer.u64_(field.component);
        writer.u64_(field.field);
        writer.u64_(field.left);
        writer.u64_(field.right);
        writer.u32_(field.depth);
        writer.text(field.component_name, kArtefactNameLength);
        writer.text(field.field_name, kArtefactNameLength);
        writer.u64_(divergence_.window.last_agreeing_tick);
        writer.u64_(divergence_.window.first_diverging_tick);
        writer.u64_(divergence_.window.session_seed);
        writer.u32_(divergence_.window.command_count);
        writer.u32_(divergence_.window.external_count);
    }

    writer.u32_(static_cast<u32>(records_.size()));
    if (Status written = writer.status(); !written) {
        return written;
    }
    for (const LogRecord& record : records_) {
        // THE SAME ENCODER THE LOG USES. A crash artefact with an encoding of its own would be a
        // sixth representation of the record, and the first thing to drift.
        if (Status encoded = encode(record, out); !encoded) {
            return encoded;
        }
    }
    return ok();
}

Status CrashArtefact::read(Span<const u8> bytes, RejectReason& why) noexcept {
    clear();
    why = RejectReason::None;
    Reader reader(bytes);
    if (reader.u32_() != kCrashMagic || !reader.ok()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: not a crash artefact");
    }
    if (reader.u32_() != kCrashFormatVersion) {
        why = RejectReason::BuildMismatch;
        return fail(ErrorCode::InvalidArgument, "replay: crash artefact from another format");
    }
    read_manifest(reader, manifest_);
    trigger_ = static_cast<CrashTrigger>(reader.u8_());
    at_.epoch.value = reader.u32_();
    at_.tick = reader.u64_();
    lost_ = reader.u64_();

    const u32 hashes = reader.u32_();
    if (hashes > kRecentHashes || !reader.ok()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: crash artefact hash table is damaged");
    }
    for (u32 index = 0; index < hashes; ++index) {
        recent_hash_ticks_[index] = reader.u64_();
        recent_hashes_[index] = reader.u64_();
    }
    recent_hash_count_ = hashes;

    has_divergence_ = reader.u8_() != 0;
    if (has_divergence_) {
        determinism::FieldDivergence& field = divergence_.field;
        field.diverged = true;
        field.shape_mismatch = reader.u8_() != 0;
        field.entity = reader.u64_();
        field.component = reader.u64_();
        field.field = reader.u64_();
        field.left = reader.u64_();
        field.right = reader.u64_();
        field.depth = reader.u32_();
        reader.text(component_name_, kArtefactNameLength);
        reader.text(field_name_, kArtefactNameLength);
        field.component_name = component_name_;
        field.field_name = field_name_;
        divergence_.window.last_agreeing_tick = reader.u64_();
        divergence_.window.first_diverging_tick = reader.u64_();
        divergence_.window.session_seed = reader.u64_();
        divergence_.window.command_count = reader.u32_();
        divergence_.window.external_count = reader.u32_();
        divergence_.window.valid = true;
        divergence_.valid = true;
    }

    const u32 count = reader.u32_();
    if (!reader.ok()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: crash artefact header is truncated");
    }
    for (u32 index = 0; index < count; ++index) {
        const Span<const u8> slice = reader.take(kEncodedRecordSize);
        if (!reader.ok()) {
            why = RejectReason::Corrupt;
            return fail(ErrorCode::InvalidArgument, "replay: crash artefact records are truncated");
        }
        LogRecord record;
        if (Status decoded = decode(slice, record); !decoded) {
            why = RejectReason::Corrupt;
            return decoded;
        }
        if (Status pushed = records_.push_back(record); !pushed) {
            return pushed;
        }
    }
    if (records_.empty()) {
        why = RejectReason::Corrupt;
        return fail(ErrorCode::InvalidArgument, "replay: a crash artefact with no records");
    }
    first_tick_ = records_[0].tick;
    last_tick_ = records_[records_.size() - 1].tick;
    assembled_ = true;
    return ok();
}

Status CrashArtefact::to_log(RecordLog& out) const noexcept {
    if (!assembled_) {
        return fail(ErrorCode::InvalidArgument, "replay: nothing to load");
    }
    for (const LogRecord& record : records_) {
        if (Status appended = out.append(record); !appended) {
            return appended;
        }
    }
    return ok();
}

void CrashArtefact::clear() noexcept {
    records_.clear();
    manifest_ = CompatibilityManifest{};
    divergence_ = DivergenceReport{};
    at_ = determinism::SimulationPoint{};
    for (u32 index = 0; index < kRecentHashes; ++index) {
        recent_hashes_[index] = 0;
        recent_hash_ticks_[index] = 0;
    }
    component_name_[0] = '\0';
    field_name_[0] = '\0';
    first_tick_ = 0;
    last_tick_ = 0;
    lost_ = 0;
    recent_hash_count_ = 0;
    trigger_ = CrashTrigger::Crash;
    has_divergence_ = false;
    assembled_ = false;
}

}  // namespace cy::replay
