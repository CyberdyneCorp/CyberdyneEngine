// Generations, the journal, retention, and the five-phase commit. Tasks 6.2 and 6.4.

#include <cy/save/archive.h>

#include <cstring>

namespace cy::save {
namespace {

constexpr std::string_view kPointerKey = "current";
constexpr std::string_view kChunkPrefix = "chunks/";
constexpr std::string_view kManifestPrefix = "generations/";
constexpr std::string_view kJournalPrefix = "journal/";
constexpr std::string_view kChunkSuffix = ".cychunk";
constexpr std::string_view kManifestSuffix = ".cymanifest";
constexpr std::string_view kJournalSuffix = ".cyjournal";

/// The pointer's first line. A save directory should be readable by a person looking for why a save
/// did not load, so `current` is text rather than a fourth binary format.
constexpr std::string_view kPointerHeader = "cysave-current 1\n";

constexpr char kHexDigits[] = "0123456789abcdef";

/// A key, built on the stack. Long enough for every key this file forms and checked against the
/// backend's limit, so a key is never silently truncated into a different object's name.
struct KeyBuffer {
    char text[kMaxSaveKeyLength + 1] = {};
    usize length = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {text, length}; }

    void append(std::string_view part) noexcept {
        const usize room = kMaxSaveKeyLength - length;
        const usize count = part.size() < room ? part.size() : room;
        std::memcpy(text + length, part.data(), count);
        length += count;
        text[length] = '\0';
    }

    void append_hex(u64 value, u32 digits) noexcept {
        for (u32 index = 0; index < digits; ++index) {
            const u32 shift = (digits - 1U - index) * 4U;
            const char digit = kHexDigits[(value >> shift) & 0xfU];
            append(std::string_view(&digit, 1));
        }
    }
};

KeyBuffer chunk_key(const assets::ContentHash& hash) noexcept {
    char text[assets::ContentHash::kTextLength + 1] = {};
    hash.format(text);
    KeyBuffer key;
    key.append(kChunkPrefix);
    key.append(std::string_view(text, assets::ContentHash::kTextLength));
    key.append(kChunkSuffix);
    return key;
}

KeyBuffer manifest_key(u32 generation) noexcept {
    KeyBuffer key;
    key.append(kManifestPrefix);
    key.append_hex(generation, 8);
    key.append(kManifestSuffix);
    return key;
}

KeyBuffer journal_prefix_for(u32 generation) noexcept {
    KeyBuffer key;
    key.append(kJournalPrefix);
    key.append_hex(generation, 8);
    key.append("-");
    return key;
}

KeyBuffer journal_key(u32 generation, u32 sequence) noexcept {
    KeyBuffer key = journal_prefix_for(generation);
    key.append_hex(sequence, 8);
    key.append(kJournalSuffix);
    return key;
}

/// Parse `digits` hex characters. Anything else is a key this build did not write.
Expected<u64, Error> read_hex(std::string_view text, usize offset, u32 digits) noexcept {
    if (text.size() < offset + digits) {
        return fail(ErrorCode::InvalidArgument, "a save key is shorter than its format");
    }
    u64 value = 0;
    for (u32 index = 0; index < digits; ++index) {
        const char character = text[offset + index];
        u32 digit = 16U;
        if (character >= '0' && character <= '9') {
            digit = static_cast<u32>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<u32>(character - 'a') + 10U;
        }
        if (digit > 15U) {
            return fail(ErrorCode::InvalidArgument,
                        "a save key is not hexadecimal where it must be");
        }
        value = (value << 4U) | digit;
    }
    return value;
}

/// Every generation the store holds, ascending. Collected during the walk and never mutated during
/// it: some backends enumerate their own storage directly.
struct GenerationCollector {
    Array<u32>* out;
    Status status = ok();
};

bool collect_generation(void* user, std::string_view key) noexcept {
    GenerationCollector& state = *static_cast<GenerationCollector*>(user);
    if (!key.ends_with(kManifestSuffix)) {
        return true;
    }
    const Expected<u64, Error> number = read_hex(key, kManifestPrefix.size(), 8);
    if (!number) {
        return true;  // not a key this build wrote; leave it alone rather than fail the listing
    }
    state.status = state.out->push_back(static_cast<u32>(*number));
    return state.status.has_value();
}

struct KeyCollector {
    Array<KeyBuffer>* out;
    Status status = ok();
};

bool collect_key(void* user, std::string_view key) noexcept {
    KeyCollector& state = *static_cast<KeyCollector*>(user);
    KeyBuffer buffer;
    buffer.append(key);
    state.status = state.out->push_back(buffer);
    return state.status.has_value();
}

}  // namespace

const char* write_phase_name(WritePhase phase) noexcept {
    switch (phase) {
        case WritePhase::ChunksWritten:
            return "chunks-written";
        case WritePhase::ChunksVerified:
            return "chunks-verified";
        case WritePhase::ManifestWritten:
            return "manifest-written";
        case WritePhase::PointerSwitched:
            return "pointer-switched";
        case WritePhase::Pruned:
            return "pruned";
    }
    return "unknown";
}

void SaveArchive::notify(WritePhase phase) noexcept {
    if (observer_ != nullptr) {
        observer_(observer_user_, phase);
    }
}

Status SaveArchive::open(SaveBackend& backend, const ArchiveConfig& config) noexcept {
    if (config.retained_generations == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a save archive retains at least one generation; zero would delete the save "
                    "it just wrote");
    }
    backend_ = &backend;
    config_ = config;
    return ok();
}

// --- Writing ------------------------------------------------------------------------------------

Status SaveArchive::write_chunk(Span<const u8> payload, ChunkRef& out) noexcept {
    out.hash = assets::content_hash(payload.data(), payload.size());
    out.size = static_cast<u32>(payload.size());
    const KeyBuffer key = chunk_key(out.hash);
    if (backend_->exists(key.view())) {
        // Content-addressed: the bytes under that name are these bytes. This is the whole of
        // "incremental writes touch only changed chunks", and it is why generations share storage.
        return ok();
    }
    return backend_->write(key.view(), payload);
}

Status SaveArchive::verify_chunk(const ChunkRef& chunk, LoadReport& report) noexcept {
    Array<u8> bytes(*allocator_);
    const KeyBuffer key = chunk_key(chunk.hash);
    if (Status read = backend_->read(key.view(), bytes); !read) {
        report.failure = LoadFailure::MissingChunk;
        report.detail = "a chunk the manifest names is not in the store";
        report.chunk = chunk.hash;
        return read;
    }
    if (assets::content_hash(bytes.data(), bytes.size()) != chunk.hash) {
        report.failure = LoadFailure::CorruptChunk;
        report.detail = "a chunk's bytes do not hash to the name the manifest gave them";
        report.chunk = chunk.hash;
        return fail(ErrorCode::Io, report.detail);
    }
    return ok();
}

Status SaveArchive::write_manifest(const Manifest& manifest, assets::ContentHash& hash) noexcept {
    Array<u8> bytes(*allocator_);
    if (Status encoded = encode_manifest(manifest, bytes); !encoded) {
        return encoded;
    }
    hash = assets::content_hash(bytes.data(), bytes.size());
    const KeyBuffer key = manifest_key(manifest.generation);
    return backend_->write(key.view(), bytes.span());
}

Status SaveArchive::write_pointer(u32 generation, const assets::ContentHash& hash) noexcept {
    char text[assets::ContentHash::kTextLength + 1] = {};
    hash.format(text);

    KeyBuffer body;
    body.append(kPointerHeader);
    body.append_hex(generation, 8);
    body.append("\n");
    body.append(std::string_view(text, assets::ContentHash::kTextLength));
    body.append("\n");
    return backend_->write(kPointerKey,
                           Span<const u8>(reinterpret_cast<const u8*>(body.text), body.length));
}

namespace {

/// What a commit assembles before anything is written. Kept together so the phases below read as
/// the sequence the header documents rather than as bookkeeping.
Status build_manifest(const Overlay& overlay, const SaveIdentity& identity, u32 generation,
                      Manifest& out) noexcept {
    out.format_version = kSaveFormatVersion;
    out.generation = generation;
    const usize build_length = std::strlen(identity.build_id);
    const usize copied =
        build_length < Manifest::kBuildIdLength ? build_length : Manifest::kBuildIdLength;
    std::memcpy(out.build_id, identity.build_id, copied);
    out.build_id[copied] = '\0';
    out.project = identity.project;
    out.save = identity.save;
    out.campaign = identity.campaign;
    out.session_seed = identity.session_seed;
    out.simulation_point = overlay.simulation_point();
    out.content_version =
        identity.content_version.is_zero() ? overlay.content_version() : identity.content_version;
    out.plugin_version = identity.plugin_version;
    return ok();
}

}  // namespace

Expected<u32, Error> SaveArchive::commit(const Overlay& overlay,
                                         const SaveIdentity& identity) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    const Expected<u32, Error> active = active_generation();
    if (!active) {
        return make_unexpected(active.error());
    }
    const u32 generation = *active + 1U;

    Manifest manifest(*allocator_);
    if (Status built = build_manifest(overlay, identity, generation, manifest); !built) {
        return make_unexpected(built.error());
    }

    // Phase 1: every chunk, under the hash of its own bytes. Nothing existing is touched.
    for (const Region& region : overlay.regions()) {
        if (Status encoded = encode_region(overlay, region.key, scratch_); !encoded) {
            return make_unexpected(encoded.error());
        }
        ChunkRef chunk;
        chunk.scope = Scope::World;
        chunk.region = region.key;
        chunk.entry_count = static_cast<u32>(region.entries.size());
        if (Status written = write_chunk(scratch_.span(), chunk); !written) {
            return make_unexpected(written.error());
        }
        if (Status added = manifest.add_chunk(chunk); !added) {
            return make_unexpected(added.error());
        }
    }
    for (u8 scope = 0; scope < static_cast<u8>(Scope::Count); ++scope) {
        u32 count = 0;
        for (const Fragment& fragment : overlay.fragments()) {
            count += static_cast<u8>(fragment.scope) == scope ? 1U : 0U;
        }
        if (count == 0) {
            continue;
        }
        if (Status encoded = encode_fragments(overlay, static_cast<Scope>(scope), scratch_);
            !encoded) {
            return make_unexpected(encoded.error());
        }
        ChunkRef chunk;
        chunk.scope = static_cast<Scope>(scope);
        chunk.region = kGlobalRegion;
        chunk.entry_count = count;
        if (Status written = write_chunk(scratch_.span(), chunk); !written) {
            return make_unexpected(written.error());
        }
        if (Status added = manifest.add_chunk(chunk); !added) {
            return make_unexpected(added.error());
        }
    }
    notify(WritePhase::ChunksWritten);

    // Phase 2: read them back. A store that accepted a write and cannot return it is a store this
    // save must not switch to.
    if (config_.verify_on_write) {
        LoadReport verification;
        for (const ChunkRef& chunk : manifest.chunks) {
            if (Status verified = verify_chunk(chunk, verification); !verified) {
                return make_unexpected(verified.error());
            }
        }
    }
    notify(WritePhase::ChunksVerified);

    // Phase 3: the manifest. Still unreferenced — `current` names the previous generation.
    assets::ContentHash manifest_hash;
    if (Status written = write_manifest(manifest, manifest_hash); !written) {
        return make_unexpected(written.error());
    }
    notify(WritePhase::ManifestWritten);

    // Phase 4: the switch. One atomic write of one small object, and the only instant at which
    // what loads changes.
    if (Status switched = write_pointer(generation, manifest_hash); !switched) {
        return make_unexpected(switched.error());
    }
    notify(WritePhase::PointerSwitched);

    // Phase 5: housekeeping, after the save is already safe. An interrupted prune costs disk, not
    // data, and the next one finishes the job.
    if (Status pruned = prune(); !pruned) {
        return make_unexpected(pruned.error());
    }
    notify(WritePhase::Pruned);
    return generation;
}

Status SaveArchive::append_journal(const Overlay& overlay) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    const Expected<u32, Error> active = active_generation();
    if (!active) {
        return make_unexpected(active.error());
    }
    if (*active == 0) {
        return fail(ErrorCode::Unavailable,
                    "a journal extends a checkpoint; commit one before appending to it");
    }

    Array<RegionKey> dirty(*allocator_);
    if (Status listed = overlay.dirty_regions(dirty); !listed) {
        return listed;
    }
    if (dirty.empty()) {
        return ok();
    }

    // The next sequence number is one past the highest already there, so two appends never name
    // one object and replay order is key order.
    Array<KeyBuffer> existing(*allocator_);
    KeyCollector collector{&existing};
    const KeyBuffer prefix = journal_prefix_for(*active);
    if (Status listed = backend_->list(prefix.view(), collect_key, &collector); !listed) {
        return listed;
    }
    if (!collector.status) {
        return collector.status;
    }
    u32 sequence = static_cast<u32>(existing.size());

    for (const RegionKey& region : dirty) {
        if (Status encoded = encode_region(overlay, region, scratch_); !encoded) {
            return encoded;
        }
        const KeyBuffer key = journal_key(*active, sequence);
        if (Status written = backend_->write(key.view(), scratch_.span()); !written) {
            return written;
        }
        ++sequence;
    }
    return ok();
}

Expected<u32, Error> SaveArchive::compact(const SaveIdentity& identity) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    Overlay folded(*allocator_);
    LoadReport report;
    LoadPolicy policy;
    policy.compatibility = Compatibility::BestEffort;
    if (Status loaded = load(policy, folded, report); !loaded) {
        return make_unexpected(loaded.error());
    }
    const Expected<u32, Error> previous = active_generation();
    if (!previous) {
        return make_unexpected(previous.error());
    }

    // The new base goes through the same five phases, so the old base and its journal stay valid
    // until `current` names the new one.
    Expected<u32, Error> generation = commit(folded, identity);
    if (!generation) {
        return generation;
    }

    Array<KeyBuffer> stale(*allocator_);
    KeyCollector collector{&stale};
    const KeyBuffer prefix = journal_prefix_for(*previous);
    if (Status listed = backend_->list(prefix.view(), collect_key, &collector); !listed) {
        return make_unexpected(listed.error());
    }
    for (const KeyBuffer& key : stale) {
        if (Status removed = backend_->remove(key.view()); !removed) {
            return make_unexpected(removed.error());
        }
    }
    return generation;
}

// --- Reading ------------------------------------------------------------------------------------

Expected<u32, Error> SaveArchive::active_generation() const noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    if (!backend_->exists(kPointerKey)) {
        return 0U;  // nothing has been committed; not an error, and not a generation
    }
    Array<u8> bytes(*allocator_);
    if (Status read = backend_->read(kPointerKey, bytes); !read) {
        return make_unexpected(read.error());
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!text.starts_with(kPointerHeader)) {
        return fail(ErrorCode::InvalidArgument, "the save pointer is not one this build wrote");
    }
    const Expected<u64, Error> generation = read_hex(text, kPointerHeader.size(), 8);
    if (!generation) {
        return make_unexpected(generation.error());
    }
    return static_cast<u32>(*generation);
}

Status SaveArchive::generations(Array<u32>& out) const noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    out.clear();
    GenerationCollector collector{&out};
    if (Status listed = backend_->list(kManifestPrefix, collect_generation, &collector); !listed) {
        return listed;
    }
    return collector.status;
}

Status SaveArchive::read_manifest(u32 generation, Manifest& out, LoadReport& report) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    Array<u8> bytes(*allocator_);
    const KeyBuffer key = manifest_key(generation);
    if (Status read = backend_->read(key.view(), bytes); !read) {
        report.failure = LoadFailure::MissingChunk;
        report.detail = "that generation's manifest is not in the store";
        return read;
    }
    return decode_manifest(bytes.span(), out, report);
}

Status SaveArchive::load_generation(u32 generation, const LoadPolicy& policy, Overlay& out,
                                    LoadReport& report) noexcept {
    Manifest manifest(*allocator_);
    if (Status read = read_manifest(generation, manifest, report); !read) {
        return read;
    }
    if (Status checked = check_compatibility(manifest, policy, report); !checked) {
        return checked;
    }
    out.set_content_version(manifest.content_version);
    out.set_simulation_point(manifest.simulation_point);

    Array<u8> bytes(*allocator_);
    for (const ChunkRef& chunk : manifest.chunks) {
        const KeyBuffer key = chunk_key(chunk.hash);
        if (Status read = backend_->read(key.view(), bytes); !read) {
            report.failure = LoadFailure::MissingChunk;
            report.detail = "a chunk the manifest names is not in the store";
            report.chunk = chunk.hash;
            return read;
        }
        if (config_.verify_on_load &&
            assets::content_hash(bytes.data(), bytes.size()) != chunk.hash) {
            report.failure = LoadFailure::CorruptChunk;
            report.detail = "a chunk's bytes do not hash to the name the manifest gave them";
            report.chunk = chunk.hash;
            return fail(ErrorCode::Io, report.detail);
        }
        if (Status decoded = decode_chunk(bytes.span(), policy, out, report); !decoded) {
            return decoded;
        }
    }
    return replay_journal(generation, policy, out, report);
}

Status SaveArchive::replay_journal(u32 generation, const LoadPolicy& policy, Overlay& out,
                                   LoadReport& report) noexcept {
    Array<KeyBuffer> entries(*allocator_);
    KeyCollector collector{&entries};
    const KeyBuffer prefix = journal_prefix_for(generation);
    if (Status listed = backend_->list(prefix.view(), collect_key, &collector); !listed) {
        return listed;
    }
    if (!collector.status) {
        return collector.status;
    }

    // Keys are zero-padded hex, and every backend lists in ascending key order, so this is the
    // order the entries were appended in without a sort and without a sequence field to trust.
    Array<u8> bytes(*allocator_);
    for (const KeyBuffer& key : entries) {
        if (Status read = backend_->read(key.view(), bytes); !read) {
            return read;
        }
        if (Status decoded = decode_chunk(bytes.span(), policy, out, report); !decoded) {
            return decoded;
        }
    }
    return ok();
}

namespace {

/// Whether a failure is one an older generation might not have. Corruption and a missing chunk are;
/// an incompatible build and a missing plugin are not — every generation was written by the same
/// build, so falling back would only lose the diagnostic.
bool is_recoverable(LoadFailure failure) noexcept {
    return failure == LoadFailure::CorruptChunk || failure == LoadFailure::MissingChunk ||
           failure == LoadFailure::IncompatibleFormat;
}

}  // namespace

Status SaveArchive::load(const LoadPolicy& policy, Overlay& out, LoadReport& report) noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    const Expected<u32, Error> active = active_generation();
    if (!active) {
        return make_unexpected(active.error());
    }
    if (*active == 0) {
        return fail(ErrorCode::NotFound, "this save store holds no committed generation");
    }
    Array<u32> available(*allocator_);
    if (Status listed = generations(available); !listed) {
        return listed;
    }

    u32 candidate = *active;
    Status last = ok();
    for (;;) {
        LoadReport attempt;
        attempt.generations_skipped = report.generations_skipped;
        out.clear();
        last = load_generation(candidate, policy, out, attempt);
        report = attempt;
        if (last) {
            return ok();
        }
        if (!is_recoverable(attempt.failure)) {
            return last;
        }
        // The next older generation, which shares every chunk that did not change — so a fallback
        // usually loses only the newest few seconds rather than the campaign.
        u32 older = 0;
        for (const u32 generation : available) {
            if (generation < candidate && generation > older) {
                older = generation;
            }
        }
        if (older == 0) {
            return last;
        }
        candidate = older;
        ++report.generations_skipped;
    }
}

// --- Retention
// ------------------------------------------------------------------------------------

namespace {

/// Referenced chunk hashes, sorted so membership is a binary search.
Status collect_referenced(SaveArchive& archive, Span<const u32> generations, Allocator& allocator,
                          Array<assets::ContentHash>& out) noexcept {
    for (const u32 generation : generations) {
        Manifest manifest(allocator);
        LoadReport report;
        if (Status read = archive.read_manifest(generation, manifest, report); !read) {
            continue;  // a manifest that cannot be read references nothing this collector can save
        }
        for (const ChunkRef& chunk : manifest.chunks) {
            if (Status pushed = out.push_back(chunk.hash); !pushed) {
                return pushed;
            }
        }
    }
    for (usize index = 1; index < out.size(); ++index) {
        const assets::ContentHash key = out[index];
        usize position = index;
        while (position > 0 && key < out[position - 1]) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = key;
    }
    return ok();
}

bool references(Span<const assets::ContentHash> sorted, const assets::ContentHash& hash) noexcept {
    usize low = 0;
    usize high = sorted.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (sorted[middle] < hash) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < sorted.size() && sorted[low] == hash;
}

}  // namespace

Status SaveArchive::prune() noexcept {
    if (!is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    Array<u32> available(*allocator_);
    if (Status listed = generations(available); !listed) {
        return listed;
    }
    const Expected<u32, Error> active = active_generation();
    if (!active) {
        return make_unexpected(active.error());
    }

    // Retained: the newest `retained_generations`, never above the active one. A generation newer
    // than `current` is an interrupted commit's manifest, and dropping it is how phase 3's debris
    // is collected.
    u32 floor = *active;
    for (u32 kept = 1; kept < config_.retained_generations; ++kept) {
        u32 older = 0;
        for (const u32 generation : available) {
            if (generation < floor && generation > older) {
                older = generation;
            }
        }
        if (older == 0) {
            break;
        }
        floor = older;
    }

    Array<u32> retained(*allocator_);
    for (const u32 generation : available) {
        if (generation >= floor && generation <= *active) {
            if (Status pushed = retained.push_back(generation); !pushed) {
                return pushed;
            }
        }
    }

    Array<assets::ContentHash> referenced(*allocator_);
    if (Status collected = collect_referenced(*this, retained.span(), *allocator_, referenced);
        !collected) {
        return collected;
    }

    // Collected first, deleted after: a backend may enumerate its own storage, and removing an
    // object during its own walk is how a listing loses its place.
    Array<KeyBuffer> doomed(*allocator_);
    KeyCollector collector{&doomed};
    if (Status listed = backend_->list(kChunkPrefix, collect_key, &collector); !listed) {
        return listed;
    }
    if (!collector.status) {
        return collector.status;
    }
    for (const KeyBuffer& key : doomed) {
        const std::string_view text = key.view();
        if (!text.ends_with(kChunkSuffix)) {
            continue;
        }
        const Expected<assets::ContentHash, Error> hash = assets::ContentHash::parse(
            text.substr(kChunkPrefix.size(), assets::ContentHash::kTextLength));
        if (!hash || references(referenced.span(), *hash)) {
            continue;
        }
        if (Status removed = backend_->remove(text); !removed) {
            return removed;
        }
    }

    for (const u32 generation : available) {
        if (generation >= floor && generation <= *active) {
            continue;
        }
        const KeyBuffer key = manifest_key(generation);
        if (Status removed = backend_->remove(key.view()); !removed) {
            return removed;
        }
        Array<KeyBuffer> journal(*allocator_);
        KeyCollector journal_collector{&journal};
        const KeyBuffer prefix = journal_prefix_for(generation);
        if (Status listed = backend_->list(prefix.view(), collect_key, &journal_collector);
            !listed) {
            return listed;
        }
        for (const KeyBuffer& entry : journal) {
            if (Status removed = backend_->remove(entry.view()); !removed) {
                return removed;
            }
        }
    }
    return ok();
}

}  // namespace cy::save
