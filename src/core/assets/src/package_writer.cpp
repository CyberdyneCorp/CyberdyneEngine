// Producing a `.cypak`. Task 3.3.3.
//
// The container's writer. It takes bytes that are already cooked and never looks inside them —
// cooking is M2, and an importer that ended up here would be the beginning of a runtime that parses
// source assets, which is exactly what the specification's cook-on-import model forbids.
//
// EVERYTHING IS BUFFERED AND WRITTEN ONCE, atomically, at `write()`. A package is produced by a
// tool, not by the runtime, and the sizes involved at M1 are small; the trade is that the directory
// and the chunk table can be sorted before anything is on disk, which is what makes the output
// byte-reproducible for a given set of inputs regardless of the order they were added in.

#include <cy/core/assets/package.h>

#include <cy/core/base/assert.h>

#include "package_format.h"
#include "package_manifest.h"

#include <algorithm>
#include <cstring>

namespace cy::assets {
namespace {

/// The order the directory is written in, and the order `find` binary-searches.
bool entry_precedes(const PackageEntry& a, const PackageEntry& b) noexcept {
    if (a.id != b.id) {
        return a.id < b.id;
    }
    return a.variant < b.variant;
}

}  // namespace

Status PackageWriter::set_manifest(const PackageManifest& manifest) noexcept {
    manifest_ = manifest;
    return ok();
}

Expected<u32, Error> PackageWriter::intern_chunk(Span<const u8> payload,
                                                 const EntryOptions& options,
                                                 const ContentHash& hash) noexcept {
    for (usize index = 0; index < chunks_.size(); ++index) {
        if (chunks_[index].hash == hash) {
            // Identical content, stored once. This is the deduplication requirement, and it costs
            // one comparison over a table that is small because it holds one row per distinct
            // payload rather than one per entry.
            bytes_deduplicated_ += payload.size();
            return static_cast<u32>(index);
        }
    }

    const CompressionMethod method =
        options.method_is_explicit ? options.method : default_method_for(options.form);
    if (!compression_method_available(method)) {
        return fail(ErrorCode::Unsupported,
                    "that compression method is not available in this "
                    "build");
    }

    Expected<PendingChunk*, Error> added = chunks_.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    PendingChunk& chunk = *added.value();
    chunk.hash = hash;
    chunk.uncompressed_size = payload.size();
    chunk.frame_bytes = options.frame_bytes;
    chunk.method = method;
    chunk.aligned = options.align_for_mapping && method == CompressionMethod::None;

    if (Status framed = compress_framed(method, options.level, payload.data(), payload.size(),
                                        options.frame_bytes, chunk.stored, chunk.frames);
        !framed) {
        chunks_.pop_back();
        return make_unexpected(framed.error());
    }
    return static_cast<u32>(chunks_.size() - 1);
}

Expected<u32, Error> PackageWriter::intern_dependencies(
    Span<const cy::AssetId> dependencies) noexcept {
    const auto first = static_cast<u32>(dependencies_.size());
    for (const cy::AssetId dependency : dependencies) {
        if (Status added = dependencies_.push_back(dependency); !added) {
            return make_unexpected(added.error());
        }
    }
    return first;
}

Status PackageWriter::insert_entry(const PackageEntry& entry) noexcept {
    for (PackageEntry& existing : entries_) {
        if (existing.id == entry.id && existing.variant == entry.variant) {
            return fail(ErrorCode::AlreadyExists,
                        "that asset id and variant is already in this package");
        }
    }
    return entries_.push_back(entry);
}

Status PackageWriter::add(cy::AssetId id, VariantKey variant, Span<const u8> payload,
                          const EntryOptions& options,
                          Span<const cy::AssetId> dependencies) noexcept {
    if (id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "a package entry needs an asset id");
    }
    if (options.frame_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a frame of zero bytes would never terminate");
    }

    // The hash is of the UNCOMPRESSED payload, so it is the same whatever method is chosen and two
    // entries that differ only in how they were compressed still deduplicate.
    const ContentHash hash = content_hash(payload.data(), payload.size());

    Expected<u32, Error> chunk = intern_chunk(payload, options, hash);
    if (!chunk) {
        return make_unexpected(chunk.error());
    }
    Expected<u32, Error> first_dependency = intern_dependencies(dependencies);
    if (!first_dependency) {
        return make_unexpected(first_dependency.error());
    }

    PackageEntry entry;
    entry.id = id;
    entry.variant = variant;
    entry.content = hash;
    entry.uncompressed_size = payload.size();
    entry.chunk = chunk.value();
    entry.dependency_first = first_dependency.value();
    entry.dependency_count = static_cast<u32>(dependencies.size());
    entry.kind = options.kind;
    entry.flags = EntryFlags::None;
    return insert_entry(entry);
}

Status PackageWriter::add_external(cy::AssetId id, VariantKey variant, const ContentHash& content,
                                   u64 uncompressed_size, AssetKind kind,
                                   Span<const cy::AssetId> dependencies) noexcept {
    if (id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "a package entry needs an asset id");
    }
    Expected<u32, Error> first_dependency = intern_dependencies(dependencies);
    if (!first_dependency) {
        return make_unexpected(first_dependency.error());
    }

    PackageEntry entry;
    entry.id = id;
    entry.variant = variant;
    entry.content = content;
    entry.uncompressed_size = uncompressed_size;
    entry.chunk = PackageEntry::kNoChunk;
    entry.dependency_first = first_dependency.value();
    entry.dependency_count = static_cast<u32>(dependencies.size());
    entry.kind = kind;
    entry.flags = EntryFlags::ExternalChunk;
    return insert_entry(entry);
}

Status PackageWriter::mark_deleted(cy::AssetId id, VariantKey variant) noexcept {
    PackageEntry entry;
    entry.id = id;
    entry.variant = variant;
    entry.chunk = PackageEntry::kNoChunk;
    entry.flags = EntryFlags::Deleted;
    return insert_entry(entry);
}

Status PackageWriter::write(const char* native_path) noexcept {
    CY_ASSERT(native_path != nullptr);

    // The directory is sorted here rather than kept sorted, so that the file does not depend on the
    // order entries were added in. Same for the chunk table, whose order the directory then refers
    // to by index — which is why the entries' chunk indices are remapped below.
    Array<u32> chunk_order;
    if (Status grown = chunk_order.resize(chunks_.size()); !grown) {
        return grown;
    }
    for (usize index = 0; index < chunks_.size(); ++index) {
        chunk_order[index] = static_cast<u32>(index);
    }
    std::ranges::sort(chunk_order,
                      [this](u32 a, u32 b) noexcept { return chunks_[a].hash < chunks_[b].hash; });
    Array<u32> chunk_rank;
    if (Status grown = chunk_rank.resize(chunks_.size()); !grown) {
        return grown;
    }
    for (usize rank = 0; rank < chunk_order.size(); ++rank) {
        chunk_rank[chunk_order[rank]] = static_cast<u32>(rank);
    }

    std::ranges::sort(entries_, entry_precedes);

    // --- lay the file out ---------------------------------------------------------------------
    u64 cursor = format::kHeaderBytes;
    const u64 payload_offset = cursor;

    struct Placement {
        u64 offset;
        u32 frame_first;
    };
    Array<Placement> placement;
    if (Status grown = placement.resize(chunks_.size()); !grown) {
        return grown;
    }
    u32 frame_total = 0;
    for (const u32 index : chunk_order) {
        PendingChunk& chunk = chunks_[index];
        if (chunk.aligned) {
            const u64 slack = cursor % format::kChunkAlignment;
            if (slack != 0) {
                cursor += format::kChunkAlignment - slack;
            }
        }
        placement[index] = Placement{cursor, frame_total};
        cursor += chunk.stored.size();
        frame_total += static_cast<u32>(chunk.frames.size());
    }

    const u64 chunk_offset = cursor;
    cursor += static_cast<u64>(chunks_.size()) * format::kChunkBytes;
    const u64 directory_offset = cursor;
    cursor += static_cast<u64>(entries_.size()) * format::kEntryBytes;
    const u64 frame_offset = cursor;
    cursor += static_cast<u64>(frame_total) * format::kFrameBytes;
    const u64 dependency_offset = cursor;
    cursor += static_cast<u64>(dependencies_.size()) * format::kDependencyBytes;
    const u64 manifest_offset = cursor;

    char manifest_text[4096] = {};
    Expected<usize, Error> manifest_size =
        write_package_manifest(manifest_, manifest_text, sizeof(manifest_text));
    if (!manifest_size) {
        return make_unexpected(manifest_size.error());
    }
    cursor += manifest_size.value();
    const u64 file_size = cursor;

    // --- fill the buffer ----------------------------------------------------------------------
    Array<u8> file;
    if (Status grown = file.resize(static_cast<usize>(file_size)); !grown) {
        return grown;
    }
    std::memset(file.data(), 0, file.size());
    u8* base = file.data();

    PackageFlags flags = PackageFlags::None;
    for (const PackageEntry& entry : entries_) {
        if (entry.is_deleted()) {
            flags = flags | PackageFlags::Patch;
        }
    }
    for (const PendingChunk& chunk : chunks_) {
        if (chunk.aligned) {
            flags = flags | PackageFlags::ChunkAligned;
        }
    }

    std::memcpy(base + format::header::kMagic, kPackageMagic, sizeof(kPackageMagic));
    format::write_u32(base + format::header::kFormatVersion, kPackageFormatVersion);
    format::write_u32(base + format::header::kFlags, static_cast<u32>(flags));
    format::write_u64(base + format::header::kPayloadOffset, payload_offset);
    format::write_u64(base + format::header::kDirectoryOffset, directory_offset);
    format::write_u32(base + format::header::kDirectoryCount, static_cast<u32>(entries_.size()));
    format::write_u64(base + format::header::kChunkOffset, chunk_offset);
    format::write_u32(base + format::header::kChunkCount, static_cast<u32>(chunks_.size()));
    format::write_u64(base + format::header::kFrameOffset, frame_offset);
    format::write_u32(base + format::header::kFrameCount, frame_total);
    format::write_u64(base + format::header::kDependencyOffset, dependency_offset);
    format::write_u32(base + format::header::kDependencyCount,
                      static_cast<u32>(dependencies_.size()));
    format::write_u64(base + format::header::kManifestOffset, manifest_offset);
    format::write_u32(base + format::header::kManifestSize,
                      static_cast<u32>(manifest_size.value()));
    format::write_u64(base + format::header::kFileSize, file_size);

    u8* frame_cursor = base + frame_offset;
    for (usize rank = 0; rank < chunk_order.size(); ++rank) {
        const u32 index = chunk_order[rank];
        const PendingChunk& chunk = chunks_[index];
        const Placement& where = placement[index];

        if (!chunk.stored.empty()) {
            std::memcpy(base + where.offset, chunk.stored.data(), chunk.stored.size());
        }

        u8* record = base + chunk_offset + (rank * format::kChunkBytes);
        std::memcpy(record, chunk.hash.bytes, ContentHash::kByteLength);
        format::write_u64(record + 32, where.offset);
        format::write_u64(record + 40, chunk.stored.size());
        format::write_u64(record + 48, chunk.uncompressed_size);
        format::write_u32(record + 56, chunk.frame_bytes);
        format::write_u32(record + 60, where.frame_first);
        format::write_u32(record + 64, static_cast<u32>(chunk.frames.size()));
        format::write_u8(record + 68, static_cast<u8>(chunk.method));
        format::write_u8(record + 69, chunk.aligned ? 1u : 0u);

        for (const FrameIndexEntry& frame : chunk.frames) {
            format::write_u64(frame_cursor, frame.compressed_offset);
            format::write_u32(frame_cursor + 8, frame.compressed_size);
            format::write_u32(frame_cursor + 12, frame.uncompressed_size);
            frame_cursor += format::kFrameBytes;
        }
    }

    for (usize index = 0; index < entries_.size(); ++index) {
        const PackageEntry& entry = entries_[index];
        u8* record = base + directory_offset + (index * format::kEntryBytes);
        format::write_u64(record, entry.id.high());
        format::write_u64(record + 8, entry.id.low());
        std::memcpy(record + 16, entry.variant.c_str(), VariantKey::kCapacity + 1);
        std::memcpy(record + 40, entry.content.bytes, ContentHash::kByteLength);
        const u32 chunk_index = entry.chunk == PackageEntry::kNoChunk ? PackageEntry::kNoChunk
                                                                      : chunk_rank[entry.chunk];
        format::write_u32(record + 72, chunk_index);
        format::write_u64(record + 76, entry.uncompressed_size);
        format::write_u32(record + 84, entry.dependency_first);
        format::write_u32(record + 88, entry.dependency_count);
        format::write_u16(record + 92, static_cast<u16>(entry.kind));
        format::write_u8(record + 94, static_cast<u8>(entry.flags));
    }

    u8* dependency_cursor = base + dependency_offset;
    for (const cy::AssetId dependency : dependencies_) {
        format::write_u64(dependency_cursor, dependency.high());
        format::write_u64(dependency_cursor + 8, dependency.low());
        dependency_cursor += format::kDependencyBytes;
    }

    if (manifest_size.value() != 0) {
        std::memcpy(base + manifest_offset, manifest_text, manifest_size.value());
    }

    // Atomic: a package half-written by an interrupted cook must not be mountable.
    return fs::write_atomic(native_path, file.data(), file.size());
}

}  // namespace cy::assets
