// Reading a `.cypak`. Task 3.3.3 — the read path, which is all of the package format M1 owns.

#include <cy/core/assets/package.h>

#include <cy/core/assets/diagnostics.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/scope.h>

#include "package_format.h"
#include "package_manifest.h"

#include <algorithm>
#include <cstring>

namespace cy::assets {
namespace {

/// What `decompress_range` calls to reach the compressed bytes of one frame. The offsets in a frame
/// index are relative to the chunk, so the base is added here rather than in the codec.
struct ChunkFrameSource {
    File* file;
    const MappedFile* mapping;
    u64 base;
    u64* bytes_from_disk;
};

Status read_frame(void* user, u64 offset, void* destination, usize size) noexcept {
    auto* source = static_cast<ChunkFrameSource*>(user);
    if (source->bytes_from_disk != nullptr) {
        *source->bytes_from_disk += size;
    }
    if (source->mapping != nullptr && source->mapping->is_mapped()) {
        const u64 absolute = source->base + offset;
        if (absolute + size > source->mapping->size()) {
            return fail(ErrorCode::OutOfRange, "a frame lies past the end of the package");
        }
        std::memcpy(destination, source->mapping->data() + absolute, size);
        return ok();
    }
    Expected<usize, Error> read = source->file->read_at(source->base + offset, destination, size);
    if (!read) {
        return make_unexpected(read.error());
    }
    if (read.value() != size) {
        return fail(ErrorCode::Io, "the package is shorter than its directory claims");
    }
    return ok();
}

}  // namespace

PackageReader::~PackageReader() = default;

Expected<UniquePtr<PackageReader>, Error> PackageReader::open(
    const char* native_path, const PackageOpenOptions& options) noexcept {
    CY_ASSERT(native_path != nullptr);

    Expected<UniquePtr<PackageReader>, Error> created =
        make_unique<PackageReader>(current_allocator());
    if (!created) {
        return make_unexpected(created.error());
    }
    PackageReader& reader = *created.value();
    reader.verify_on_read_ = options.verify_on_read;

    const usize path_length = std::strlen(native_path);
    if (path_length > kMaxPathLength) {
        return fail(ErrorCode::InvalidArgument, "the package path is longer than a path may be");
    }
    std::memcpy(reader.path_, native_path, path_length);

    Expected<File, Error> file = File::open(native_path, FileMode::Read);
    if (!file) {
        return make_unexpected(file.error());
    }
    reader.file_ = std::move(file.value());

    Expected<u64, Error> file_size = reader.file_.size();
    if (!file_size) {
        return make_unexpected(file_size.error());
    }
    if (file_size.value() < format::kHeaderBytes) {
        return fail(ErrorCode::Io, "the file is too short to be a package");
    }

    u8 header[format::kHeaderBytes] = {};
    Expected<usize, Error> read = reader.file_.read_at(0, header, sizeof(header));
    if (!read) {
        return make_unexpected(read.error());
    }
    if (read.value() != sizeof(header) ||
        std::memcmp(header, kPackageMagic, sizeof(kPackageMagic)) != 0) {
        return fail(ErrorCode::InvalidArgument, "the file does not begin with the package magic");
    }

    const u32 version = format::read_u32(header + format::header::kFormatVersion);
    if (version != kPackageFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "the package's container format version is not the one this engine reads");
    }
    reader.flags_ = static_cast<PackageFlags>(format::read_u32(header + format::header::kFlags));
    if (has_flag(reader.flags_, PackageFlags::EncryptedDirectory) ||
        has_flag(reader.flags_, PackageFlags::EncryptedPayload)) {
        // Declared in the format, refused here. See the note at the top of package.h: a decryption
        // path with no key management would be a hole, and silently reading an encrypted package as
        // plaintext would be worse than either.
        assets_log_package_refused(native_path,
                                   "the package is encrypted and this build has no key management");
        return fail(ErrorCode::Unsupported,
                    "the package is encrypted and this build has no key management");
    }

    const u64 directory_offset = format::read_u64(header + format::header::kDirectoryOffset);
    const u32 directory_count = format::read_u32(header + format::header::kDirectoryCount);
    const u64 chunk_offset = format::read_u64(header + format::header::kChunkOffset);
    const u32 chunk_count = format::read_u32(header + format::header::kChunkCount);
    const u64 frame_offset = format::read_u64(header + format::header::kFrameOffset);
    const u32 frame_count = format::read_u32(header + format::header::kFrameCount);
    const u64 dependency_offset = format::read_u64(header + format::header::kDependencyOffset);
    const u32 dependency_count = format::read_u32(header + format::header::kDependencyCount);
    const u64 manifest_offset = format::read_u64(header + format::header::kManifestOffset);
    const u32 manifest_size = format::read_u32(header + format::header::kManifestSize);
    const u64 declared_size = format::read_u64(header + format::header::kFileSize);

    if (declared_size != file_size.value()) {
        return fail(ErrorCode::Io,
                    "the package's recorded size does not match the file; it is truncated or "
                    "appended to");
    }
    // Every table must lie inside the file. Checked before anything is read, because a table offset
    // taken from a corrupt header is exactly how a reader is steered out of its own buffer.
    const auto within = [total = file_size.value()](u64 offset, u64 bytes) noexcept {
        return offset <= total && bytes <= total - offset;
    };
    if (!within(directory_offset, static_cast<u64>(directory_count) * format::kEntryBytes) ||
        !within(chunk_offset, static_cast<u64>(chunk_count) * format::kChunkBytes) ||
        !within(frame_offset, static_cast<u64>(frame_count) * format::kFrameBytes) ||
        !within(dependency_offset, static_cast<u64>(dependency_count) * format::kDependencyBytes) ||
        !within(manifest_offset, manifest_size)) {
        return fail(ErrorCode::Io, "a package table extends past the end of the file");
    }

    // --- the manifest, and the compatibility gate ------------------------------------------------
    if (manifest_size > 0) {
        Array<char> manifest_text;
        if (Status grown = manifest_text.resize(manifest_size); !grown) {
            return make_unexpected(grown.error());
        }
        Expected<usize, Error> got =
            reader.file_.read_at(manifest_offset, manifest_text.data(), manifest_size);
        if (!got) {
            return make_unexpected(got.error());
        }
        Expected<PackageManifest, Error> manifest =
            parse_package_manifest(std::string_view(manifest_text.data(), manifest_size));
        if (!manifest) {
            return make_unexpected(manifest.error());
        }
        reader.manifest_ = manifest.value();
    }
    if (Status compatible = check_compatibility(reader.manifest_.compatibility, options.runtime);
        !compatible) {
        // `core-assets-and-io` — "Incompatible package is refused": mounting fails with a
        // diagnostic rather than partially loading, and the diagnostic carries the PATH as a
        // classified field rather than as part of an event name.
        assets_log_package_refused(native_path, compatible.error().message);
        return make_unexpected(compatible.error());
    }

    // --- the tables ------------------------------------------------------------------------------
    Array<u8> table;

    if (chunk_count > 0) {
        if (Status grown = table.resize(static_cast<usize>(chunk_count) * format::kChunkBytes);
            !grown) {
            return make_unexpected(grown.error());
        }
        if (Expected<usize, Error> got =
                reader.file_.read_at(chunk_offset, table.data(), table.size());
            !got) {
            return make_unexpected(got.error());
        }
        if (Status grown = reader.chunks_.resize(chunk_count); !grown) {
            return make_unexpected(grown.error());
        }
        for (u32 index = 0; index < chunk_count; ++index) {
            const u8* record = table.data() + (static_cast<usize>(index) * format::kChunkBytes);
            PackageChunk& chunk = reader.chunks_[index];
            std::memcpy(chunk.hash.bytes, record, ContentHash::kByteLength);
            chunk.offset = format::read_u64(record + 32);
            chunk.stored_size = format::read_u64(record + 40);
            chunk.uncompressed_size = format::read_u64(record + 48);
            chunk.frame_bytes = format::read_u32(record + 56);
            chunk.frame_first = format::read_u32(record + 60);
            chunk.frame_count = format::read_u32(record + 64);
            chunk.method = static_cast<CompressionMethod>(format::read_u8(record + 68));
            chunk.aligned = format::read_u8(record + 69) != 0;
            if (!within(chunk.offset, chunk.stored_size) || chunk.frame_first > frame_count ||
                chunk.frame_count > frame_count - chunk.frame_first) {
                return fail(ErrorCode::Io, "a chunk record points outside the package");
            }
        }
    }

    if (directory_count > 0) {
        if (Status grown = table.resize(static_cast<usize>(directory_count) * format::kEntryBytes);
            !grown) {
            return make_unexpected(grown.error());
        }
        if (Expected<usize, Error> got =
                reader.file_.read_at(directory_offset, table.data(), table.size());
            !got) {
            return make_unexpected(got.error());
        }
        if (Status grown = reader.entries_.resize(directory_count); !grown) {
            return make_unexpected(grown.error());
        }
        for (u32 index = 0; index < directory_count; ++index) {
            const u8* record = table.data() + (static_cast<usize>(index) * format::kEntryBytes);
            PackageEntry& entry = reader.entries_[index];
            entry.id = cy::AssetId(format::read_u64(record), format::read_u64(record + 8));
            char variant[VariantKey::kCapacity + 1] = {};
            std::memcpy(variant, record + 16, VariantKey::kCapacity);
            Expected<VariantKey, Error> key = VariantKey::parse(variant);
            if (!key) {
                return make_unexpected(key.error());
            }
            entry.variant = key.value();
            std::memcpy(entry.content.bytes, record + 40, ContentHash::kByteLength);
            entry.chunk = format::read_u32(record + 72);
            entry.uncompressed_size = format::read_u64(record + 76);
            entry.dependency_first = format::read_u32(record + 84);
            entry.dependency_count = format::read_u32(record + 88);
            entry.kind = static_cast<AssetKind>(format::read_u16(record + 92));
            entry.flags = static_cast<EntryFlags>(format::read_u8(record + 94));
            if (entry.chunk != PackageEntry::kNoChunk && entry.chunk >= chunk_count) {
                return fail(ErrorCode::Io,
                            "a directory entry names a chunk the package has not "
                            "got");
            }
            if (entry.dependency_first > dependency_count ||
                entry.dependency_count > dependency_count - entry.dependency_first) {
                return fail(ErrorCode::Io, "a directory entry's dependency slice is out of range");
            }
        }
    }

    if (frame_count > 0) {
        if (Status grown = table.resize(static_cast<usize>(frame_count) * format::kFrameBytes);
            !grown) {
            return make_unexpected(grown.error());
        }
        if (Expected<usize, Error> got =
                reader.file_.read_at(frame_offset, table.data(), table.size());
            !got) {
            return make_unexpected(got.error());
        }
        if (Status grown = reader.frames_.resize(frame_count); !grown) {
            return make_unexpected(grown.error());
        }
        for (u32 index = 0; index < frame_count; ++index) {
            const u8* record = table.data() + (static_cast<usize>(index) * format::kFrameBytes);
            reader.frames_[index] =
                FrameIndexEntry{format::read_u64(record), format::read_u32(record + 8),
                                format::read_u32(record + 12)};
        }
    }

    if (dependency_count > 0) {
        if (Status grown =
                table.resize(static_cast<usize>(dependency_count) * format::kDependencyBytes);
            !grown) {
            return make_unexpected(grown.error());
        }
        if (Expected<usize, Error> got =
                reader.file_.read_at(dependency_offset, table.data(), table.size());
            !got) {
            return make_unexpected(got.error());
        }
        if (Status grown = reader.dependencies_.resize(dependency_count); !grown) {
            return make_unexpected(grown.error());
        }
        for (u32 index = 0; index < dependency_count; ++index) {
            const u8* record =
                table.data() + (static_cast<usize>(index) * format::kDependencyBytes);
            reader.dependencies_[index] =
                cy::AssetId(format::read_u64(record), format::read_u64(record + 8));
        }
    }

    // The whole file, mapped once. `map_entry` then returns a view into it for an uncompressed,
    // aligned chunk, which is the specification's memory-mapped read. A failure to map is not a
    // failure to open: every read has a copying path.
    counters::record_package_opened(false);

    if (options.map_file && memory_mapping_available()) {
        Expected<MappedFile, Error> mapping = MappedFile::map(native_path);
        if (mapping) {
            reader.mapping_ = std::move(mapping.value());
        }
    }

    return created;
}

const PackageEntry* PackageReader::find(cy::AssetId id, VariantKey variant) const noexcept {
    // The directory is sorted by (id, variant), so the entries for one id are contiguous.
    PackageEntry probe;
    probe.id = id;
    const auto* first = std::ranges::lower_bound(
        entries_, probe,
        [](const PackageEntry& a, const PackageEntry& b) noexcept { return a.id < b.id; });
    const PackageEntry* fallback = nullptr;
    for (const PackageEntry* entry = first; entry != entries_.end() && entry->id == id; ++entry) {
        if (entry->variant == variant) {
            return entry;
        }
        if (entry->variant.is_any()) {
            fallback = entry;
        }
    }
    return fallback;
}

const PackageChunk* PackageReader::find_chunk(const ContentHash& hash) const noexcept {
    const auto* found = std::ranges::lower_bound(
        chunks_, hash, [](const ContentHash& a, const ContentHash& b) noexcept { return a < b; },
        [](const PackageChunk& chunk) noexcept { return chunk.hash; });
    if (found != chunks_.end() && found->hash == hash) {
        return found;
    }
    return nullptr;
}

Span<const cy::AssetId> PackageReader::dependencies(const PackageEntry& entry) const noexcept {
    if (entry.dependency_count == 0) {
        return {};
    }
    return {dependencies_.data() + entry.dependency_first, entry.dependency_count};
}

Status PackageReader::read_chunk_payload(const PackageChunk& chunk, Array<u8>& out,
                                         u64* bytes_from_disk) const noexcept {
    out.clear();
    if (Status grown = out.resize(static_cast<usize>(chunk.uncompressed_size)); !grown) {
        return grown;
    }
    if (chunk.uncompressed_size == 0) {
        return ok();
    }
    return read_chunk_range(chunk, 0, out.data(), out.size(), bytes_from_disk, nullptr);
}

Status PackageReader::read_chunk_range(const PackageChunk& chunk, u64 offset, void* destination,
                                       usize size, u64* bytes_from_disk,
                                       u32* frames_touched) const noexcept {
    if (chunk.frame_count == 0) {
        return size == 0 ? ok()
                         : fail(ErrorCode::OutOfRange, "the chunk holds no frames to read from");
    }
    const Span<const FrameIndexEntry> index(frames_.data() + chunk.frame_first, chunk.frame_count);

    u64 read_bytes = 0;
    ChunkFrameSource source{&file_, mapping_.is_mapped() ? &mapping_ : nullptr, chunk.offset,
                            &read_bytes};
    Status done = decompress_range(chunk.method, index, chunk.frame_bytes, read_frame, &source,
                                   offset, destination, size, frames_touched);
    bytes_read_ += read_bytes;
    counters::record_package_bytes(read_bytes);
    if (bytes_from_disk != nullptr) {
        *bytes_from_disk = read_bytes;
    }
    return done;
}

Status PackageReader::read_entry(const PackageEntry& entry, Array<u8>& out,
                                 u64* bytes_from_disk) const noexcept {
    if (entry.is_deleted()) {
        return fail(ErrorCode::NotFound,
                    "the entry is a patch's deleted-entry marker and carries no payload");
    }
    if (entry.is_external() || entry.chunk == PackageEntry::kNoChunk) {
        return fail(ErrorCode::NotFound,
                    "the entry's chunk lives in another package; read it through a PackageSet");
    }
    if (Status read = read_chunk_payload(chunks_[entry.chunk], out, bytes_from_disk); !read) {
        return read;
    }
    if (verify_on_read_) {
        if (!(content_hash(out.data(), out.size()) == entry.content)) {
            ++integrity_failures_;
            counters::record_integrity_failure();
            return fail(ErrorCode::Io,
                        "the entry's payload does not match its recorded content hash");
        }
    }
    return ok();
}

Status PackageReader::read_entry_range(const PackageEntry& entry, u64 offset, void* destination,
                                       usize size, u64* bytes_from_disk,
                                       u32* frames_touched) const noexcept {
    if (entry.is_deleted() || entry.chunk == PackageEntry::kNoChunk) {
        return fail(ErrorCode::NotFound, "the entry carries no payload in this package");
    }
    return read_chunk_range(chunks_[entry.chunk], offset, destination, size, bytes_from_disk,
                            frames_touched);
}

Expected<Span<const u8>, Error> PackageReader::map_entry(const PackageEntry& entry) const noexcept {
    if (!mapping_.is_mapped()) {
        return fail(ErrorCode::Unsupported, "this package is not memory-mapped");
    }
    if (entry.chunk == PackageEntry::kNoChunk) {
        return fail(ErrorCode::NotFound, "the entry carries no payload in this package");
    }
    const PackageChunk& chunk = chunks_[entry.chunk];
    if (chunk.method != CompressionMethod::None) {
        return fail(ErrorCode::Unsupported, "a compressed entry cannot be mapped; decompress it");
    }
    if (!chunk.aligned) {
        return fail(ErrorCode::Unsupported,
                    "the entry was not written with chunk alignment, so it cannot be mapped");
    }
    if (chunk.offset + chunk.stored_size > mapping_.size()) {
        return fail(ErrorCode::OutOfRange, "the chunk lies past the end of the mapping");
    }
    counters::record_entry_mapped();
    return Span<const u8>(mapping_.data() + chunk.offset,
                          static_cast<usize>(chunk.uncompressed_size));
}

Status PackageReader::verify_entry(const PackageEntry& entry) const noexcept {
    Array<u8> payload;
    if (Status read = read_entry(entry, payload, nullptr); !read) {
        return read;
    }
    if (!(content_hash(payload.data(), payload.size()) == entry.content)) {
        ++integrity_failures_;
        counters::record_integrity_failure();
        return fail(ErrorCode::Io, "the entry's payload does not match its recorded content hash");
    }
    return ok();
}

const PackageChunk* PackageReader::chunk_of(const PackageEntry& entry) const noexcept {
    if (entry.chunk == PackageEntry::kNoChunk || entry.chunk >= chunks_.size()) {
        return nullptr;
    }
    return &chunks_[entry.chunk];
}

Span<const FrameIndexEntry> PackageReader::frames_of(const PackageChunk& chunk) const noexcept {
    if (chunk.frame_count == 0) {
        return {};
    }
    return {frames_.data() + chunk.frame_first, chunk.frame_count};
}

Status PackageReader::read_chunk_stored(const PackageChunk& chunk, Array<u8>& out) const noexcept {
    out.clear();
    if (Status grown = out.resize(static_cast<usize>(chunk.stored_size)); !grown) {
        return grown;
    }
    if (chunk.stored_size == 0) {
        return ok();
    }
    if (mapping_.is_mapped()) {
        if (chunk.offset + chunk.stored_size > mapping_.size()) {
            return fail(ErrorCode::OutOfRange, "the chunk lies past the end of the mapping");
        }
        std::memcpy(out.data(), mapping_.data() + chunk.offset,
                    static_cast<usize>(chunk.stored_size));
        bytes_read_ += chunk.stored_size;
        counters::record_package_bytes(chunk.stored_size);
        return ok();
    }
    Expected<usize, Error> read = file_.read_at(chunk.offset, out.data(), out.size());
    if (!read) {
        return make_unexpected(read.error());
    }
    if (read.value() != out.size()) {
        return fail(ErrorCode::Io, "the package is shorter than its chunk table claims");
    }
    bytes_read_ += chunk.stored_size;
    counters::record_package_bytes(chunk.stored_size);
    return ok();
}

Status PackageReader::read_chunk(const ContentHash& hash, Array<u8>& out) const noexcept {
    const PackageChunk* chunk = find_chunk(hash);
    if (chunk == nullptr) {
        return fail(ErrorCode::NotFound, "this package does not hold that chunk");
    }
    return read_chunk_payload(*chunk, out, nullptr);
}

// --- PackageSet
// ------------------------------------------------------------------------------------

Status PackageSet::add(PackageReader& reader, i32 priority) noexcept {
    Expected<Slot*, Error> added = packages_.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    added.value()->reader = &reader;
    added.value()->priority = priority;
    added.value()->sequence = next_sequence_++;
    std::ranges::sort(packages_, [](const Slot& a, const Slot& b) noexcept {
        return a.priority != b.priority ? a.priority > b.priority : a.sequence > b.sequence;
    });
    return ok();
}

void PackageSet::clear() noexcept {
    packages_.clear();
}

PackageReader* PackageSet::holder_of(const ContentHash& hash) const noexcept {
    for (const Slot& slot : packages_) {
        if (slot.reader->find_chunk(hash) != nullptr) {
            return slot.reader;
        }
    }
    return nullptr;
}

Status PackageSet::read_chunk(const ContentHash& hash, Array<u8>& out) const noexcept {
    PackageReader* holder = holder_of(hash);
    if (holder == nullptr) {
        return fail(ErrorCode::NotFound, "no mounted package holds that chunk");
    }
    return holder->read_chunk(hash, out);
}

Status PackageSet::read_entry(const PackageReader& owner, const PackageEntry& entry,
                              Array<u8>& out) const noexcept {
    if (entry.is_deleted()) {
        return fail(ErrorCode::NotFound, "the entry is a deleted-entry marker");
    }
    if (!entry.is_external() && entry.chunk != PackageEntry::kNoChunk) {
        return owner.read_entry(entry, out, nullptr);
    }
    // The bytes are in whichever package holds the chunk — the patch shipped only the reference.
    return read_chunk(entry.content, out);
}

}  // namespace cy::assets
