// The save container: manifest and chunks, encoded as value records. Tasks 6.2 and 6.3.

#include <cy/save/container.h>

#include <cstring>

namespace cy::save {
namespace {

// --- Field identifiers of the reserved records --------------------------------------------------
//
// Written into files, so fixed forever. Grouped per record rather than in one enumeration, because
// each set is addressed only within its own record and one shared enumeration would suggest
// otherwise.

namespace chunk_field {
constexpr reflect::FieldId kFormatVersion{1};
constexpr reflect::FieldId kRegion{2};
constexpr reflect::FieldId kScope{3};
constexpr reflect::FieldId kEntryCount{4};
}  // namespace chunk_field

namespace entry_field {
constexpr reflect::FieldId kIdHigh{1};
constexpr reflect::FieldId kIdLow{2};
constexpr reflect::FieldId kKind{3};
constexpr reflect::FieldId kTemplateHigh{4};
constexpr reflect::FieldId kTemplateLow{5};
constexpr reflect::FieldId kOwnerHigh{6};
constexpr reflect::FieldId kOwnerLow{7};
}  // namespace entry_field

namespace manifest_field {
constexpr reflect::FieldId kFormatVersion{1};
constexpr reflect::FieldId kGeneration{2};
constexpr reflect::FieldId kBuildId{3};
constexpr reflect::FieldId kProjectHigh{4};
constexpr reflect::FieldId kProjectLow{5};
constexpr reflect::FieldId kSaveHigh{6};
constexpr reflect::FieldId kSaveLow{7};
constexpr reflect::FieldId kCampaignHigh{8};
constexpr reflect::FieldId kCampaignLow{9};
constexpr reflect::FieldId kSimulationPoint{10};
constexpr reflect::FieldId kSessionSeed{11};
constexpr reflect::FieldId kContentVersion{12};
constexpr reflect::FieldId kPluginVersion{13};
}  // namespace manifest_field

namespace chunk_ref_field {
constexpr reflect::FieldId kScope{1};
constexpr reflect::FieldId kRegion{2};
constexpr reflect::FieldId kHash{3};
constexpr reflect::FieldId kSize{4};
constexpr reflect::FieldId kEntryCount{5};
}  // namespace chunk_ref_field

namespace plugin_field {
constexpr reflect::FieldId kName{1};
constexpr reflect::FieldId kVersion{2};
}  // namespace plugin_field

// --- Scalars in and out of a record --------------------------------------------------------------

Status set_u64(serialize::ValueRecord& record, reflect::FieldId field, u64 value) noexcept {
    return record.set_scalar(field, serialize::WireType::U64, &value, sizeof(value));
}

Status set_u32(serialize::ValueRecord& record, reflect::FieldId field, u32 value) noexcept {
    return record.set_scalar(field, serialize::WireType::U32, &value, sizeof(value));
}

Status set_u16(serialize::ValueRecord& record, reflect::FieldId field, u16 value) noexcept {
    return record.set_scalar(field, serialize::WireType::U16, &value, sizeof(value));
}

Status set_u8(serialize::ValueRecord& record, reflect::FieldId field, u8 value) noexcept {
    return record.set_scalar(field, serialize::WireType::U8, &value, sizeof(value));
}

/// Read a scalar back. A field that is absent or of the wrong width is an error rather than a
/// default, because every caller here is reading structure the writer always wrote.
template <class T>
Expected<T, Error> get_scalar(const serialize::ValueRecord& record, reflect::FieldId field,
                              serialize::WireType wire) noexcept {
    const serialize::FieldValue* value = record.find(field);
    if (value == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a save record is missing a required field");
    }
    const Span<const u8> bytes = record.bytes(*value);
    T result{};
    if (Status decoded =
            serialize::decode_scalar(wire, bytes.data(), static_cast<u32>(bytes.size()), &result);
        !decoded) {
        return make_unexpected(decoded.error());
    }
    return result;
}

Expected<u64, Error> get_u64(const serialize::ValueRecord& record,
                             reflect::FieldId field) noexcept {
    return get_scalar<u64>(record, field, serialize::WireType::U64);
}

Expected<u32, Error> get_u32(const serialize::ValueRecord& record,
                             reflect::FieldId field) noexcept {
    return get_scalar<u32>(record, field, serialize::WireType::U32);
}

Expected<u16, Error> get_u16(const serialize::ValueRecord& record,
                             reflect::FieldId field) noexcept {
    return get_scalar<u16>(record, field, serialize::WireType::U16);
}

Expected<u8, Error> get_u8(const serialize::ValueRecord& record, reflect::FieldId field) noexcept {
    return get_scalar<u8>(record, field, serialize::WireType::U8);
}

Status set_hash(serialize::ValueRecord& record, reflect::FieldId field,
                const assets::ContentHash& hash) noexcept {
    return record.set(field, serialize::WireType::Bytes, hash.bytes,
                      static_cast<u32>(assets::ContentHash::kByteLength));
}

Expected<assets::ContentHash, Error> get_hash(const serialize::ValueRecord& record,
                                              reflect::FieldId field) noexcept {
    const Span<const u8> bytes = record.bytes(field);
    if (bytes.size() != assets::ContentHash::kByteLength) {
        return fail(ErrorCode::InvalidArgument, "a content hash in a save is not 32 bytes");
    }
    assets::ContentHash hash;
    std::memcpy(hash.bytes, bytes.data(), assets::ContentHash::kByteLength);
    return hash;
}

/// Copy a NUL-terminated field into a fixed buffer, truncating rather than overrunning. Returns the
/// number of characters, so a caller can tell a truncated name from a complete one.
usize copy_text(char* out, usize capacity, Span<const u8> bytes) noexcept {
    const usize length = bytes.size() < capacity ? bytes.size() : capacity;
    if (length != 0) {
        std::memcpy(out, bytes.data(), length);
    }
    out[length] = '\0';
    return length;
}

// --- Streams ------------------------------------------------------------------------------------

/// Open a one-chunk tagged stream. Every artefact this file writes is exactly one chunk, so the
/// two calls are always made together and are worth one name.
Status begin_one_chunk(serialize::TaggedWriter& writer, u32 tag) noexcept {
    if (Status began = writer.begin_stream(); !began) {
        return began;
    }
    return writer.begin_chunk(tag);
}

Status end_one_chunk(serialize::TaggedWriter& writer) noexcept {
    if (Status ended = writer.end_chunk(); !ended) {
        return ended;
    }
    return writer.end_stream();
}

/// The single chunk of a stream, checked against the tag the caller expected.
Expected<serialize::TaggedChunk, Error> read_one_chunk(Span<const u8> bytes,
                                                       LoadReport& report) noexcept {
    serialize::TaggedReader reader(bytes.data(), bytes.size());
    if (Status header = reader.read_header(); !header) {
        report.failure = LoadFailure::IncompatibleFormat;
        report.detail = "the bytes are not a tagged stream this build can parse";
        return make_unexpected(header.error());
    }
    Expected<serialize::TaggedChunk, Error> chunk = reader.next_chunk();
    if (!chunk) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a save artefact carries no chunk";
    }
    return chunk;
}

/// Make sure a failure NAMES something. Every path below reports its own diagnostic; this is the
/// backstop that keeps "the load failed and the report says nothing was wrong" from ever being a
/// state this decoder can produce — which is the whole point of a structured failure.
Status note_failure(Status status, LoadReport& report, const char* detail) noexcept {
    if (!status && report.failure == LoadFailure::None) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = detail;
    }
    return status;
}

/// True for the record types this file writes itself. Those are structure, not payload: they carry
/// no schema version a migration chain would advance.
bool is_reserved(reflect::TypeId type) noexcept {
    return type == kChunkHeaderType || type == kEntryHeaderType || type == kManifestHeaderType ||
           type == kChunkRefType || type == kPluginRefType;
}

}  // namespace

const char* load_failure_name(LoadFailure failure) noexcept {
    switch (failure) {
        case LoadFailure::None:
            return "none";
        case LoadFailure::IncompatibleFormat:
            return "incompatible-format";
        case LoadFailure::IncompatibleBuild:
            return "incompatible-build";
        case LoadFailure::MigrationFailed:
            return "migration-failed";
        case LoadFailure::MissingPlugin:
            return "missing-plugin";
        case LoadFailure::CorruptChunk:
            return "corrupt-chunk";
        case LoadFailure::MissingChunk:
            return "missing-chunk";
        case LoadFailure::MissingContent:
            return "missing-content";
        case LoadFailure::UnresolvableReference:
            return "unresolvable-reference";
    }
    return "unknown";
}

// --- Manifest -----------------------------------------------------------------------------------

Status Manifest::add_chunk(const ChunkRef& chunk) noexcept {
    // Sorted on insertion by (scope, region), so a manifest is byte-identical for equal content
    // however the caller enumerated its chunks.
    usize index = 0;
    while (index < chunks.size()) {
        const ChunkRef& existing = chunks[index];
        if (existing.scope > chunk.scope ||
            (existing.scope == chunk.scope && chunk.region < existing.region)) {
            break;
        }
        if (existing.scope == chunk.scope && existing.region == chunk.region) {
            chunks[index] = chunk;
            return ok();
        }
        ++index;
    }
    if (Status pushed = chunks.push_back(chunk); !pushed) {
        return pushed;
    }
    for (usize position = chunks.size() - 1; position > index; --position) {
        const ChunkRef moved = chunks[position - 1];
        chunks[position - 1] = chunks[position];
        chunks[position] = moved;
    }
    return ok();
}

Status Manifest::require_plugin(const char* name, u32 version) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a plugin requirement needs a name");
    }
    PluginRequirement plugin;
    plugin.version = version;
    const usize length = std::strlen(name);
    copy_text(plugin.name, PluginRequirement::kNameLength,
              Span<const u8>(reinterpret_cast<const u8*>(name), length));
    return plugins.push_back(plugin);
}

const ChunkRef* Manifest::find_chunk(Scope scope, RegionKey region) const noexcept {
    for (const ChunkRef& chunk : chunks) {
        if (chunk.scope == scope && chunk.region == region) {
            return &chunk;
        }
    }
    return nullptr;
}

u64 Manifest::total_chunk_bytes() const noexcept {
    u64 total = 0;
    for (const ChunkRef& chunk : chunks) {
        total += chunk.size;
    }
    return total;
}

u64 Manifest::chunk_bytes_in(Scope scope) const noexcept {
    u64 total = 0;
    for (const ChunkRef& chunk : chunks) {
        if (chunk.scope == scope) {
            total += chunk.size;
        }
    }
    return total;
}

// --- Encoding chunks
// ------------------------------------------------------------------------------

namespace {

Status write_chunk_header(serialize::TaggedWriter& writer, serialize::ValueRecord& scratch,
                          RegionKey region, Scope scope, u32 entry_count) noexcept {
    scratch.clear();
    scratch.set_type(kChunkHeaderType);
    if (Status set = set_u16(scratch, chunk_field::kFormatVersion, kSaveFormatVersion); !set) {
        return set;
    }
    if (Status set = set_u64(scratch, chunk_field::kRegion, region.value()); !set) {
        return set;
    }
    if (Status set = set_u8(scratch, chunk_field::kScope, static_cast<u8>(scope)); !set) {
        return set;
    }
    if (Status set = set_u32(scratch, chunk_field::kEntryCount, entry_count); !set) {
        return set;
    }
    return writer.write_record(scratch);
}

Status write_entry_header(serialize::TaggedWriter& writer, serialize::ValueRecord& scratch,
                          const Entry& entry) noexcept {
    scratch.clear();
    scratch.set_type(kEntryHeaderType);
    if (Status set = set_u64(scratch, entry_field::kIdHigh, entry.id.high()); !set) {
        return set;
    }
    if (Status set = set_u64(scratch, entry_field::kIdLow, entry.id.low()); !set) {
        return set;
    }
    if (Status set = set_u8(scratch, entry_field::kKind, static_cast<u8>(entry.kind)); !set) {
        return set;
    }
    if (Status set = set_u64(scratch, entry_field::kTemplateHigh, entry.template_asset.high());
        !set) {
        return set;
    }
    if (Status set = set_u64(scratch, entry_field::kTemplateLow, entry.template_asset.low());
        !set) {
        return set;
    }
    if (Status set = set_u64(scratch, entry_field::kOwnerHigh, entry.owner.high()); !set) {
        return set;
    }
    if (Status set = set_u64(scratch, entry_field::kOwnerLow, entry.owner.low()); !set) {
        return set;
    }
    return writer.write_record(scratch);
}

}  // namespace

Status encode_region(const Overlay& overlay, RegionKey region, Array<u8>& out) noexcept {
    out.clear();
    const Region* found = overlay.find_region(region);
    const u32 entry_count = found == nullptr ? 0U : static_cast<u32>(found->entries.size());

    serialize::TaggedWriter writer(out);
    if (Status began = begin_one_chunk(writer, kRegionChunkTag); !began) {
        return began;
    }
    serialize::ValueRecord scratch(overlay.allocator());
    if (Status header = write_chunk_header(writer, scratch, region, Scope::World, entry_count);
        !header) {
        return header;
    }
    if (found != nullptr) {
        for (const Entry& entry : found->entries) {
            if (Status written = write_entry_header(writer, scratch, entry); !written) {
                return written;
            }
            for (const ComponentDelta& component : entry.components) {
                if (Status written = writer.write_record(component.record); !written) {
                    return written;
                }
            }
        }
    }
    return end_one_chunk(writer);
}

Status encode_fragments(const Overlay& overlay, Scope scope, Array<u8>& out) noexcept {
    out.clear();
    u32 count = 0;
    for (const Fragment& fragment : overlay.fragments()) {
        count += fragment.scope == scope ? 1U : 0U;
    }

    serialize::TaggedWriter writer(out);
    if (Status began = begin_one_chunk(writer, kFragmentChunkTag); !began) {
        return began;
    }
    serialize::ValueRecord scratch(overlay.allocator());
    if (Status header = write_chunk_header(writer, scratch, kGlobalRegion, scope, count); !header) {
        return header;
    }
    for (const Fragment& fragment : overlay.fragments()) {
        if (fragment.scope != scope) {
            continue;
        }
        if (Status written = writer.write_record(fragment.record); !written) {
            return written;
        }
    }
    return end_one_chunk(writer);
}

// --- Decoding chunks
// ------------------------------------------------------------------------------

namespace {

/// Where a chunk's records are being applied: which region, which scope, and which entry the
/// component records that follow belong to.
struct ChunkCursor {
    RegionKey region;
    Scope scope = Scope::World;
    PersistentId entry;
    bool have_entry = false;
    bool fragments = false;
};

Status apply_chunk_header(const serialize::ValueRecord& record, ChunkCursor& cursor,
                          LoadReport& report) noexcept {
    const Expected<u16, Error> version = get_u16(record, chunk_field::kFormatVersion);
    if (!version) {
        return make_unexpected(version.error());
    }
    if (*version > kSaveFormatVersion) {
        report.failure = LoadFailure::IncompatibleFormat;
        report.detail = "the chunk was written by a newer save format";
        return fail(ErrorCode::Unsupported, report.detail);
    }
    const Expected<u64, Error> region = get_u64(record, chunk_field::kRegion);
    if (!region) {
        return make_unexpected(region.error());
    }
    const Expected<u8, Error> scope = get_u8(record, chunk_field::kScope);
    if (!scope) {
        return make_unexpected(scope.error());
    }
    if (!is_known_scope(*scope)) {
        report.failure = LoadFailure::IncompatibleFormat;
        report.detail = "the chunk declares a scope this build does not know";
        return fail(ErrorCode::Unsupported, report.detail);
    }
    cursor.region = RegionKey(*region);
    cursor.scope = static_cast<Scope>(*scope);
    return ok();
}

Status apply_entry_header(const serialize::ValueRecord& record, ChunkCursor& cursor, Overlay& out,
                          LoadReport& report) noexcept {
    const Expected<u64, Error> high = get_u64(record, entry_field::kIdHigh);
    const Expected<u64, Error> low = get_u64(record, entry_field::kIdLow);
    const Expected<u8, Error> kind = get_u8(record, entry_field::kKind);
    if (!high || !low || !kind) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "an entry header is missing its identity";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    cursor.entry = PersistentId(*high, *low);
    cursor.have_entry = true;
    ++report.entries_applied;

    if (*kind == static_cast<u8>(EntryKind::Tombstone)) {
        cursor.have_entry = false;  // a tombstone owns no components
        return out.destroy_entity(cursor.region, cursor.entry);
    }
    if (*kind == static_cast<u8>(EntryKind::Created)) {
        const Expected<u64, Error> template_high = get_u64(record, entry_field::kTemplateHigh);
        const Expected<u64, Error> template_low = get_u64(record, entry_field::kTemplateLow);
        const Expected<u64, Error> owner_high = get_u64(record, entry_field::kOwnerHigh);
        const Expected<u64, Error> owner_low = get_u64(record, entry_field::kOwnerLow);
        if (!template_high || !template_low || !owner_high || !owner_low) {
            report.failure = LoadFailure::CorruptChunk;
            report.detail = "a created entity is missing its template or owner";
            return fail(ErrorCode::InvalidArgument, report.detail);
        }
        return out.create_entity(cursor.region, cursor.entry,
                                 AssetId(*template_high, *template_low),
                                 PersistentId(*owner_high, *owner_low));
    }
    return ok();
}

/// Carry one payload record up to its type's current schema version, then hand it to the overlay.
Status apply_payload(serialize::ValueRecord& record, const LoadPolicy& policy,
                     const ChunkCursor& cursor, Overlay& out, LoadReport& report) noexcept {
    const bool declared = policy.schemas != nullptr && policy.schemas->declares(record.type());
    if (!declared) {
        ++report.records_unknown;
        report.fields_preserved += static_cast<u32>(record.size());
    } else {
        const u16 written_at = record.schema_version();
        if (Status migrated = policy.schemas->migrate(record); !migrated) {
            report.failure = LoadFailure::MigrationFailed;
            report.detail = "a record could not be migrated to the current schema version";
            return migrated;
        }
        report.records_migrated += record.schema_version() != written_at ? 1U : 0U;
    }

    if (cursor.fragments) {
        return out.record_fragment(cursor.scope, record.type(), record.schema_version(), record);
    }
    if (!cursor.have_entry) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a component record appears before any entry header";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    return out.record_component(cursor.region, cursor.entry, record.type(), record.schema_version(),
                                record);
}

/// Apply one record of a chunk. Split out of the loop below so that `decode_chunk` reads as "read a
/// record, apply it, count it" rather than as a four-way dispatch nested inside a loop inside a
/// stream — the shape a decoder acquires by accretion and then keeps.
Status apply_record(serialize::ValueRecord& record, const LoadPolicy& policy, ChunkCursor& cursor,
                    Overlay& out, LoadReport& report, bool& header_read) noexcept {
    if (record.type() == kChunkHeaderType) {
        header_read = true;
        return note_failure(apply_chunk_header(record, cursor, report), report,
                            "a chunk header is malformed");
    }
    if (!header_read) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a chunk does not open with its header";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    if (record.type() == kEntryHeaderType) {
        return note_failure(apply_entry_header(record, cursor, out, report), report,
                            "an entry header could not be applied");
    }
    if (is_reserved(record.type())) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a reserved record appears where a payload record belongs";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    return note_failure(apply_payload(record, policy, cursor, out, report), report,
                        "a record could not be applied to the overlay");
}

/// The chunk's opening: its tag checked, and the cursor told what kind of chunk it is.
Expected<serialize::TaggedChunk, Error> open_chunk(Span<const u8> payload, ChunkCursor& cursor,
                                                   LoadReport& report) noexcept {
    Expected<serialize::TaggedChunk, Error> chunk = read_one_chunk(payload, report);
    if (!chunk) {
        return chunk;
    }
    if (chunk->tag != kRegionChunkTag && chunk->tag != kFragmentChunkTag) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a save chunk carries a tag this build does not write";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    cursor.fragments = chunk->tag == kFragmentChunkTag;
    return chunk;
}

}  // namespace

Status decode_chunk(Span<const u8> payload, const LoadPolicy& policy, Overlay& out,
                    LoadReport& report) noexcept {
    ChunkCursor cursor;
    Expected<serialize::TaggedChunk, Error> chunk = open_chunk(payload, cursor, report);
    if (!chunk) {
        return make_unexpected(chunk.error());
    }

    serialize::ByteReader reader(chunk->payload.data(), chunk->payload.size());
    serialize::ValueRecord record(out.allocator());
    bool header_read = false;
    while (!reader.empty()) {
        if (Status read = serialize::read_record(reader, record); !read) {
            report.failure = LoadFailure::CorruptChunk;
            report.detail = "a chunk ended in the middle of a record";
            return read;
        }
        ++report.records_read;
        if (Status applied = apply_record(record, policy, cursor, out, report, header_read);
            !applied) {
            return applied;
        }
    }
    if (!header_read) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a chunk is empty";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    ++report.chunks_read;
    return ok();
}

// --- The manifest, encoded
// ------------------------------------------------------------------------

Status encode_manifest(const Manifest& manifest, Array<u8>& out) noexcept {
    out.clear();
    serialize::TaggedWriter writer(out);
    if (Status began = begin_one_chunk(writer, kManifestChunkTag); !began) {
        return began;
    }

    serialize::ValueRecord record(out.allocator());
    record.set_type(kManifestHeaderType);
    const Status header[] = {
        set_u16(record, manifest_field::kFormatVersion, manifest.format_version),
        set_u32(record, manifest_field::kGeneration, manifest.generation),
        record.set(manifest_field::kBuildId, serialize::WireType::Bytes, manifest.build_id,
                   static_cast<u32>(std::strlen(manifest.build_id))),
        set_u64(record, manifest_field::kProjectHigh, manifest.project.high()),
        set_u64(record, manifest_field::kProjectLow, manifest.project.low()),
        set_u64(record, manifest_field::kSaveHigh, manifest.save.high()),
        set_u64(record, manifest_field::kSaveLow, manifest.save.low()),
        set_u64(record, manifest_field::kCampaignHigh, manifest.campaign.high()),
        set_u64(record, manifest_field::kCampaignLow, manifest.campaign.low()),
        set_u64(record, manifest_field::kSimulationPoint, manifest.simulation_point),
        set_u64(record, manifest_field::kSessionSeed, manifest.session_seed),
        set_hash(record, manifest_field::kContentVersion, manifest.content_version),
        set_hash(record, manifest_field::kPluginVersion, manifest.plugin_version),
    };
    for (const Status& step : header) {
        if (!step) {
            return step;
        }
    }
    if (Status written = writer.write_record(record); !written) {
        return written;
    }

    for (const ChunkRef& chunk : manifest.chunks) {
        record.clear();
        record.set_type(kChunkRefType);
        const Status fields[] = {
            set_u8(record, chunk_ref_field::kScope, static_cast<u8>(chunk.scope)),
            set_u64(record, chunk_ref_field::kRegion, chunk.region.value()),
            set_hash(record, chunk_ref_field::kHash, chunk.hash),
            set_u32(record, chunk_ref_field::kSize, chunk.size),
            set_u32(record, chunk_ref_field::kEntryCount, chunk.entry_count),
        };
        for (const Status& step : fields) {
            if (!step) {
                return step;
            }
        }
        if (Status written = writer.write_record(record); !written) {
            return written;
        }
    }

    for (const PluginRequirement& plugin : manifest.plugins) {
        record.clear();
        record.set_type(kPluginRefType);
        if (Status set = record.set(plugin_field::kName, serialize::WireType::Bytes, plugin.name,
                                    static_cast<u32>(std::strlen(plugin.name)));
            !set) {
            return set;
        }
        if (Status set = set_u32(record, plugin_field::kVersion, plugin.version); !set) {
            return set;
        }
        if (Status written = writer.write_record(record); !written) {
            return written;
        }
    }
    return end_one_chunk(writer);
}

namespace {

Status read_manifest_header(const serialize::ValueRecord& record, Manifest& out) noexcept {
    const Expected<u16, Error> format = get_u16(record, manifest_field::kFormatVersion);
    const Expected<u32, Error> generation = get_u32(record, manifest_field::kGeneration);
    const Expected<u64, Error> project_high = get_u64(record, manifest_field::kProjectHigh);
    const Expected<u64, Error> project_low = get_u64(record, manifest_field::kProjectLow);
    const Expected<u64, Error> save_high = get_u64(record, manifest_field::kSaveHigh);
    const Expected<u64, Error> save_low = get_u64(record, manifest_field::kSaveLow);
    const Expected<u64, Error> campaign_high = get_u64(record, manifest_field::kCampaignHigh);
    const Expected<u64, Error> campaign_low = get_u64(record, manifest_field::kCampaignLow);
    const Expected<u64, Error> point = get_u64(record, manifest_field::kSimulationPoint);
    const Expected<u64, Error> seed = get_u64(record, manifest_field::kSessionSeed);
    const Expected<assets::ContentHash, Error> content =
        get_hash(record, manifest_field::kContentVersion);
    const Expected<assets::ContentHash, Error> plugins =
        get_hash(record, manifest_field::kPluginVersion);
    if (!format || !generation || !project_high || !project_low || !save_high || !save_low ||
        !campaign_high || !campaign_low || !point || !seed || !content || !plugins) {
        return fail(ErrorCode::InvalidArgument, "a save manifest is missing a required field");
    }

    out.format_version = *format;
    out.generation = *generation;
    copy_text(out.build_id, Manifest::kBuildIdLength, record.bytes(manifest_field::kBuildId));
    out.project = AssetId(*project_high, *project_low);
    out.save = AssetId(*save_high, *save_low);
    out.campaign = AssetId(*campaign_high, *campaign_low);
    out.simulation_point = *point;
    out.session_seed = *seed;
    out.content_version = *content;
    out.plugin_version = *plugins;
    return ok();
}

Status read_chunk_ref(const serialize::ValueRecord& record, Manifest& out) noexcept {
    const Expected<u8, Error> scope = get_u8(record, chunk_ref_field::kScope);
    const Expected<u64, Error> region = get_u64(record, chunk_ref_field::kRegion);
    const Expected<assets::ContentHash, Error> hash = get_hash(record, chunk_ref_field::kHash);
    const Expected<u32, Error> size = get_u32(record, chunk_ref_field::kSize);
    const Expected<u32, Error> entries = get_u32(record, chunk_ref_field::kEntryCount);
    if (!scope || !region || !hash || !size || !entries || !is_known_scope(*scope)) {
        return fail(ErrorCode::InvalidArgument, "a save manifest holds a malformed chunk entry");
    }
    ChunkRef chunk;
    chunk.scope = static_cast<Scope>(*scope);
    chunk.region = RegionKey(*region);
    chunk.hash = *hash;
    chunk.size = *size;
    chunk.entry_count = *entries;
    return out.add_chunk(chunk);
}

Status read_plugin_ref(const serialize::ValueRecord& record, Manifest& out) noexcept {
    const Expected<u32, Error> version = get_u32(record, plugin_field::kVersion);
    if (!version) {
        return fail(ErrorCode::InvalidArgument, "a plugin requirement is missing its version");
    }
    PluginRequirement plugin;
    plugin.version = *version;
    copy_text(plugin.name, PluginRequirement::kNameLength, record.bytes(plugin_field::kName));
    return out.plugins.push_back(plugin);
}

}  // namespace

Status decode_manifest(Span<const u8> bytes, Manifest& out, LoadReport& report) noexcept {
    Expected<serialize::TaggedChunk, Error> chunk = read_one_chunk(bytes, report);
    if (!chunk) {
        return make_unexpected(chunk.error());
    }
    if (chunk->tag != kManifestChunkTag) {
        report.failure = LoadFailure::IncompatibleFormat;
        report.detail = "that artefact is not a save manifest";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }

    out.chunks.clear();
    out.plugins.clear();
    serialize::ByteReader reader(chunk->payload.data(), chunk->payload.size());
    serialize::ValueRecord record(out.chunks.allocator());
    bool header_read = false;
    while (!reader.empty()) {
        if (Status read = serialize::read_record(reader, record); !read) {
            report.failure = LoadFailure::CorruptChunk;
            report.detail = "the manifest ended in the middle of a record";
            return read;
        }
        Status applied = ok();
        if (record.type() == kManifestHeaderType) {
            applied = read_manifest_header(record, out);
            header_read = true;
        } else if (record.type() == kChunkRefType) {
            applied = read_chunk_ref(record, out);
        } else if (record.type() == kPluginRefType) {
            applied = read_plugin_ref(record, out);
        }
        // Anything else is a record a later format added, and stepping over it is the point of the
        // tagged form. `read_record` has already consumed it exactly.
        if (!applied) {
            report.failure = LoadFailure::CorruptChunk;
            report.detail = "the manifest is malformed";
            return applied;
        }
    }
    if (!header_read) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "the manifest carries no header record";
        return fail(ErrorCode::InvalidArgument, report.detail);
    }
    return ok();
}

// --- Compatibility
// --------------------------------------------------------------------------------

namespace {

/// Compare up to the first '.', which is the major version of a dotted build identity.
bool same_major_version(const char* a, const char* b) noexcept {
    usize index = 0;
    while (a[index] != '\0' && a[index] != '.' && b[index] != '\0' && b[index] != '.') {
        if (a[index] != b[index]) {
            return false;
        }
        ++index;
    }
    const bool a_ended = a[index] == '\0' || a[index] == '.';
    const bool b_ended = b[index] == '\0' || b[index] == '.';
    return a_ended && b_ended;
}

Status check_build(const Manifest& manifest, const LoadPolicy& policy,
                   LoadReport& report) noexcept {
    if (policy.build_id == nullptr || policy.build_id[0] == '\0') {
        return ok();
    }
    const bool matches = policy.compatibility == Compatibility::ExactBuild
                             ? std::strcmp(manifest.build_id, policy.build_id) == 0
                             : same_major_version(manifest.build_id, policy.build_id);
    if (matches || policy.compatibility == Compatibility::Migratable ||
        policy.compatibility == Compatibility::BestEffort) {
        return ok();
    }
    report.failure = LoadFailure::IncompatibleBuild;
    report.detail = manifest.build_id;
    return fail(ErrorCode::Unsupported, "the save was written by an incompatible build");
}

Status check_content(const Manifest& manifest, const LoadPolicy& policy,
                     LoadReport& report) noexcept {
    if (policy.compatibility == Compatibility::BestEffort || policy.content_version.is_zero() ||
        manifest.content_version.is_zero() || manifest.content_version == policy.content_version) {
        return ok();
    }
    report.failure = LoadFailure::MissingContent;
    report.detail = "the save was produced against different cooked content";
    return fail(ErrorCode::Unsupported, report.detail);
}

Status check_plugins(const Manifest& manifest, const LoadPolicy& policy,
                     LoadReport& report) noexcept {
    if (policy.plugin_present == nullptr) {
        return ok();
    }
    for (const PluginRequirement& plugin : manifest.plugins) {
        if (policy.plugin_present(policy.plugin_user, plugin)) {
            continue;
        }
        report.failure = LoadFailure::MissingPlugin;
        // Points into the caller's manifest, which outlives the report by construction: the report
        // is read beside the manifest that produced it.
        report.detail = plugin.name;
        return fail(ErrorCode::NotFound, "a plugin the save requires is not present");
    }
    return ok();
}

}  // namespace

Status check_compatibility(const Manifest& manifest, const LoadPolicy& policy,
                           LoadReport& report) noexcept {
    if (manifest.format_version > kSaveFormatVersion) {
        report.failure = LoadFailure::IncompatibleFormat;
        report.detail = "the save was written by a newer save format";
        return fail(ErrorCode::Unsupported, report.detail);
    }
    if (Status build = check_build(manifest, policy, report); !build) {
        return build;
    }
    if (Status content = check_content(manifest, policy, report); !content) {
        return content;
    }
    return check_plugins(manifest, policy, report);
}

}  // namespace cy::save
