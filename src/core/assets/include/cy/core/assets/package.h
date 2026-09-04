#ifndef CY_CORE_ASSETS_PACKAGE_H
#define CY_CORE_ASSETS_PACKAGE_H
// The runtime package format, `.cypak`. Task 3.3.3 — THE READ PATH.
//
// `core-assets-and-io` — "Package format": a header (magic, version, flags), a directory of entries
// (asset id, variant key, offset, size, uncompressed size, compression method, content hash,
// flags), and a payload region; a manifest declaring build identity, content compatibility
// versions, install-bundle membership and package dependencies; bulk payload addressable as
// content-addressed chunks that may be shared between packages and between builds; flags for
// encrypted directory, encrypted payload, deleted-entry markers for patches, and chunk alignment
// for memory mapping; per-entry compression with seekable framing.
//
// THE LAYOUT, so that the reader below is readable:
//
//   header            fixed, at offset 0
//   payload region    chunk payloads, in the order they were added, aligned where flagged
//   chunk table       one record per content-addressed chunk, sorted by hash
//   directory         one record per entry, sorted by (asset id, variant key)
//   frame index       every chunk's frames, concatenated; a chunk names its first and its count
//   dependency table  every entry's dependencies, concatenated; an entry names its slice
//   manifest          text, at the end, so it can be read without the rest
//
// EVERY MULTI-BYTE FIELD IS LITTLE-ENDIAN AND WRITTEN BYTE BY BYTE. No structure is memcpy'd to
// disk: a packed struct's layout is a compiler's decision, and the format is the engine's.
//
// AN ENTRY IS A REFERENCE TO A CHUNK, and this is the whole of content addressing. Two entries with
// identical payloads name the same chunk, so the bytes are stored once. An entry may also name a
// chunk this package does not hold — `PackageSet` resolves it from whichever mounted package does,
// which is how a patch ships only what changed and how identical content is stored once across
// builds.
//
// WHAT IS NOT HERE. Cooking (M2): nothing imports, converts or parses a source asset. Encryption:
// the flags are defined and `open` REFUSES a package that sets them, because a decryption path with
// no key management is a hole rather than a feature. Streaming and residency (M6): the reader
// serves a range when asked and holds no policy about what should be resident.
//
// `PackageWriter` IS PART OF THE FORMAT, NOT PART OF COOKING. A read path that nothing can produce
// input for cannot be tested, and the container's writer is the format's other half — it takes
// already-cooked bytes and never looks inside them. M2's cooker produces those bytes and calls
// this; it does not replace it.

#include <cy/core/assets/compression.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>

namespace cy::assets {

/// "CYPAK" and three bytes that make an accidental text file fail the check.
inline constexpr u8 kPackageMagic[8] = {'C', 'Y', 'P', 'A', 'K', 0x00, 0x0D, 0x0A};

/// The container format's own version, bumped when the layout changes. Distinct from a package's
/// CONTENT compatibility version, which is the project's and lives in the manifest.
inline constexpr u32 kPackageFormatVersion = 1;

/// Package-wide flags.
enum class PackageFlags : u32 {
    None = 0,
    /// The directory is encrypted. Declared, refused on open; see the note at the top of this file.
    EncryptedDirectory = 1u << 0,
    /// The payload region is encrypted, after compression. Declared, refused on open.
    EncryptedPayload = 1u << 1,
    /// The package carries deleted-entry markers, i.e. it is a patch.
    Patch = 1u << 2,
    /// Chunk payloads are aligned so that an uncompressed one can be memory-mapped.
    ChunkAligned = 1u << 3,
};

[[nodiscard]] constexpr PackageFlags operator|(PackageFlags a, PackageFlags b) noexcept {
    return static_cast<PackageFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_flag(PackageFlags value, PackageFlags flag) noexcept {
    return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
}

/// Per-entry flags.
enum class EntryFlags : u8 {
    None = 0,
    /// A patch's tombstone: this entry MASKS the same asset in every lower-priority package rather
    /// than providing it. It carries no payload.
    Deleted = 1u << 0,
    /// The chunk this entry names is stored in another package. `PackageSet` resolves it.
    ExternalChunk = 1u << 1,
};

[[nodiscard]] constexpr EntryFlags operator|(EntryFlags a, EntryFlags b) noexcept {
    return static_cast<EntryFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has_flag(EntryFlags value, EntryFlags flag) noexcept {
    return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
}

/// The compatibility a package declares and a runtime offers.
///
/// `build-and-packaging` owns what these numbers mean; this file owns that they are checked BEFORE
/// anything is read, so an incompatible package is refused rather than partially loaded.
struct ContentCompatibility {
    /// The content version this package was produced at, or the runtime implements.
    u32 content_version = 1;
    /// The oldest counterpart it will work with.
    u32 minimum_content_version = 1;
};

/// The manifest, declared in text at the end of the file.
struct PackageManifest {
    static constexpr usize kBuildIdCapacity = 63;
    static constexpr usize kBundleCapacity = 63;
    static constexpr usize kMaxDependencies = 32;
    static constexpr usize kNameCapacity = 63;

    /// The build identity that produced this package.
    char build_id[kBuildIdCapacity + 1] = {};
    /// The install bundle this package belongs to.
    char bundle[kBundleCapacity + 1] = {};
    ContentCompatibility compatibility;
    /// Other packages this one needs mounted. Names, resolved by whoever mounts.
    char dependencies[kMaxDependencies][kNameCapacity + 1] = {};
    u32 dependency_count = 0;

    [[nodiscard]] Status set_build_id(std::string_view text) noexcept;
    [[nodiscard]] Status set_bundle(std::string_view text) noexcept;
    [[nodiscard]] Status add_dependency(std::string_view name) noexcept;
};

/// One content-addressed chunk, as the reader sees it.
struct PackageChunk {
    ContentHash hash;
    u64 offset = 0;             ///< absolute, within the package file
    u64 stored_size = 0;        ///< bytes on disk
    u64 uncompressed_size = 0;  ///< bytes after decompression
    u32 frame_bytes = 0;        ///< the uncompressed span of one frame
    u32 frame_first = 0;        ///< index into the package's frame index
    u32 frame_count = 0;
    CompressionMethod method = CompressionMethod::None;
    /// True when `offset` is aligned for memory mapping.
    bool aligned = false;
};

/// One directory entry, as the reader sees it.
struct PackageEntry {
    cy::AssetId id;
    VariantKey variant;
    /// The hash of the UNCOMPRESSED payload. Also the chunk key.
    ContentHash content;
    u64 uncompressed_size = 0;
    /// Index into this package's chunk table, or `kNoChunk` for a deleted or external entry.
    u32 chunk = 0xFFFF'FFFFu;
    u32 dependency_first = 0;
    u32 dependency_count = 0;
    AssetKind kind = AssetKind::Unknown;
    EntryFlags flags = EntryFlags::None;

    static constexpr u32 kNoChunk = 0xFFFF'FFFFu;

    [[nodiscard]] bool is_deleted() const noexcept { return has_flag(flags, EntryFlags::Deleted); }
    [[nodiscard]] bool is_external() const noexcept {
        return has_flag(flags, EntryFlags::ExternalChunk);
    }
};

/// How a package is opened.
struct PackageOpenOptions {
    /// What the runtime implements. A package outside it is refused at open.
    ContentCompatibility runtime;
    /// Memory-map the whole file, so an uncompressed aligned entry is served without a copy.
    bool map_file = true;
    /// Check every entry's payload against its recorded hash as it is read. Off by default: it
    /// doubles the cost of a load, and it is what a shipping build turns on for untrusted content.
    bool verify_on_read = false;
};

class PackageSet;

/// A `.cypak`, opened for reading.
class PackageReader {
public:
    PackageReader() noexcept = default;
    ~PackageReader();

    PackageReader(const PackageReader&) = delete;
    PackageReader& operator=(const PackageReader&) = delete;

    /// Open a package from a NATIVE path.
    ///
    /// Fails, before reading any payload, on: a bad magic or a newer format version; a declared
    /// encryption flag; a manifest whose compatibility does not match the runtime's
    /// (`ErrorCode::Unsupported`, naming both versions); and a directory or chunk table that does
    /// not fit inside the file.
    [[nodiscard]] static Expected<UniquePtr<PackageReader>, Error> open(
        const char* native_path, const PackageOpenOptions& options) noexcept;

    [[nodiscard]] const PackageManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] PackageFlags flags() const noexcept { return flags_; }
    [[nodiscard]] const char* path() const noexcept { return path_; }

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }
    [[nodiscard]] const PackageEntry& entry_at(usize index) const noexcept {
        return entries_[index];
    }

    /// The entry for an id and a variant.
    ///
    /// An exact variant match wins; failing that, an entry cooked for every platform (`any`) is
    /// served, which is what an asset with no variants is. Returns null when the package has
    /// neither — including when it has only a *different* variant, because serving a mobile texture
    /// to a desktop build silently is worse than reporting it missing.
    [[nodiscard]] const PackageEntry* find(cy::AssetId id, VariantKey variant) const noexcept;

    [[nodiscard]] usize chunk_count() const noexcept { return chunks_.size(); }
    /// The chunk with this content hash, or null. How a package answers "do you hold this?" for
    /// another package's external reference.
    [[nodiscard]] const PackageChunk* find_chunk(const ContentHash& hash) const noexcept;

    /// The dependency ids an entry declares.
    [[nodiscard]] Span<const cy::AssetId> dependencies(const PackageEntry& entry) const noexcept;

    /// Read a whole entry's payload into `out`. `bytes_from_disk`, when given, receives the number
    /// of compressed bytes actually read, which is what the partial-read test asserts on.
    [[nodiscard]] Status read_entry(const PackageEntry& entry, Array<u8>& out,
                                    u64* bytes_from_disk = nullptr) const noexcept;

    /// Read the half-open uncompressed range `[offset, offset + size)` of an entry.
    ///
    /// Only the frames covering the range are read and decompressed — the specification's "partial
    /// read of a large asset". `frames_touched` reports how many, so a test can show that the rest
    /// of the entry was never touched.
    [[nodiscard]] Status read_entry_range(const PackageEntry& entry, u64 offset, void* destination,
                                          usize size, u64* bytes_from_disk = nullptr,
                                          u32* frames_touched = nullptr) const noexcept;

    /// A view of an entry's payload without copying it.
    ///
    /// Succeeds only when the package is mapped, the chunk is stored uncompressed, and its offset
    /// is aligned — the specification's "memory-mapped read" conditions, each reported separately
    /// so a caller learns which one it failed. Reports Unsupported otherwise; every caller then
    /// copies, which is why mapping is an optimisation and never a requirement.
    [[nodiscard]] Expected<Span<const u8>, Error> map_entry(
        const PackageEntry& entry) const noexcept;

    /// Read the entry and check its payload against the recorded hash.
    ///
    /// `core-assets-and-io` — "Tampered package": a payload that does not match fails with an
    /// integrity error. `read_entry` does this on every read when `verify_on_read` is set.
    [[nodiscard]] Status verify_entry(const PackageEntry& entry) const noexcept;

    /// Read a chunk this package holds, by hash. What `PackageSet` calls to satisfy another
    /// package's external reference.
    [[nodiscard]] Status read_chunk(const ContentHash& hash, Array<u8>& out) const noexcept;

    // --- The staged path ----------------------------------------------------------------------
    //
    // `core-assets-and-io` requires that read, decompress, deserialize and upload be SEPARATE
    // stages that can overlap across requests. `read_entry` does the first two together, which is
    // what a tool wants; the asset system wants them apart, so that the blocking half runs on the
    // I/O thread and the CPU half runs on a worker. These three are that seam.

    /// The chunk an entry names, or null when it has none (deleted, or external).
    [[nodiscard]] const PackageChunk* chunk_of(const PackageEntry& entry) const noexcept;

    /// The frame index of a chunk, as `decompress_range` wants it.
    [[nodiscard]] Span<const FrameIndexEntry> frames_of(const PackageChunk& chunk) const noexcept;

    /// Read a chunk's payload EXACTLY AS STORED — still compressed, still framed. The blocking half
    /// of a staged load; `decompress_range` over `frames_of` is the other half.
    [[nodiscard]] Status read_chunk_stored(const PackageChunk& chunk,
                                           Array<u8>& out) const noexcept;

    [[nodiscard]] u64 bytes_read() const noexcept { return bytes_read_; }
    [[nodiscard]] u64 integrity_failures() const noexcept { return integrity_failures_; }

private:
    friend class PackageSet;

    [[nodiscard]] Status read_chunk_payload(const PackageChunk& chunk, Array<u8>& out,
                                            u64* bytes_from_disk) const noexcept;
    [[nodiscard]] Status read_chunk_range(const PackageChunk& chunk, u64 offset, void* destination,
                                          usize size, u64* bytes_from_disk,
                                          u32* frames_touched) const noexcept;

    char path_[kMaxPathLength + 1] = {};
    mutable File file_;
    MappedFile mapping_;
    PackageFlags flags_ = PackageFlags::None;
    PackageManifest manifest_;
    bool verify_on_read_ = false;

    Array<PackageEntry> entries_;  ///< sorted by (id, variant)
    Array<PackageChunk> chunks_;   ///< sorted by hash
    Array<FrameIndexEntry> frames_;
    Array<cy::AssetId> dependencies_;

    mutable u64 bytes_read_ = 0;
    mutable u64 integrity_failures_ = 0;
};

/// Several packages in priority order, resolving external chunks between them.
///
/// This is what makes "identical content is stored once as a shared chunk" true ACROSS packages: a
/// patch declares an entry whose chunk lives in the base package and ships no copy of the bytes.
class PackageSet {
public:
    PackageSet() noexcept = default;

    PackageSet(const PackageSet&) = delete;
    PackageSet& operator=(const PackageSet&) = delete;

    /// Add a reader the set does not own. Higher priority is consulted first.
    [[nodiscard]] Status add(PackageReader& reader, i32 priority) noexcept;
    void clear() noexcept;

    /// The highest-priority package holding a chunk with this hash.
    [[nodiscard]] PackageReader* holder_of(const ContentHash& hash) const noexcept;

    /// Read a chunk from wherever it lives.
    [[nodiscard]] Status read_chunk(const ContentHash& hash, Array<u8>& out) const noexcept;

    /// Read an entry, following an external chunk reference into the package that holds it.
    [[nodiscard]] Status read_entry(const PackageReader& owner, const PackageEntry& entry,
                                    Array<u8>& out) const noexcept;

    [[nodiscard]] usize size() const noexcept { return packages_.size(); }

private:
    struct Slot {
        PackageReader* reader = nullptr;
        i32 priority = 0;
        u32 sequence = 0;
    };
    Array<Slot> packages_;
    u32 next_sequence_ = 0;
};

// --- The mount
// ------------------------------------------------------------------------------------

/// The synthetic directory a package's entries appear under in the virtual filesystem.
///
/// A package is addressed by asset id, and the virtual filesystem is addressed by path, so the two
/// meet at one spelling: `packaged/<32 hex id>` for an asset with no variant and
/// `packaged/<32 hex id>.<variant>` for one with. That is not a cosmetic choice — it is what puts
/// package mounts, patch masking and mount priority on the SAME machinery as every other mount,
/// rather than giving packages a second, parallel resolution order that could disagree with it.
inline constexpr const char* kPackageMountRoot = "packaged";

/// The path an id and a variant appear at under a package mount.
[[nodiscard]] Expected<VirtualPath, Error> package_entry_path(cy::AssetId id,
                                                              VariantKey variant) noexcept;

/// A mounted package.
class PackageMount final : public Mount {
public:
    /// Takes the reader. The mount owns it, so unmounting closes the file.
    explicit PackageMount(UniquePtr<PackageReader> reader) noexcept;

    [[nodiscard]] MountKind kind() const noexcept override { return MountKind::Package; }
    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] bool contains(const VirtualPath& path) const noexcept override;
    [[nodiscard]] bool is_deleted(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Expected<u64, Error> size_of(const VirtualPath& path) const noexcept override;
    [[nodiscard]] Status read(const VirtualPath& path, u64 offset, void* destination,
                              usize size) const noexcept override;
    [[nodiscard]] Status enumerate(const VirtualPath& directory, bool recursive,
                                   VirtualVisitor visitor, void* user) const noexcept override;
    [[nodiscard]] PackageMount* as_package() noexcept override { return this; }

    [[nodiscard]] PackageReader& reader() noexcept { return *reader_; }
    [[nodiscard]] const PackageReader& reader() const noexcept { return *reader_; }

    /// The entry a path names, or null.
    [[nodiscard]] const PackageEntry* entry_for(const VirtualPath& path) const noexcept;

private:
    UniquePtr<PackageReader> reader_;
};

// --- The writer
// -----------------------------------------------------------------------------------

/// Produces a `.cypak`. See the note at the top of this file: this is the container's writer, not
/// a cooker — it takes bytes that are already cooked and never looks inside them.
class PackageWriter {
public:
    PackageWriter() noexcept = default;

    PackageWriter(const PackageWriter&) = delete;
    PackageWriter& operator=(const PackageWriter&) = delete;

    struct EntryOptions {
        AssetKind kind = AssetKind::Binary;
        /// What kind of bytes these are, which chooses the default method. An explicit `method`
        /// overrides it.
        PayloadForm form = PayloadForm::Compressible;
        /// Left unset to take `default_method_for(form)`.
        CompressionMethod method = CompressionMethod::None;
        bool method_is_explicit = false;
        CompressionLevel level = CompressionLevel::Balanced;
        u32 frame_bytes = kDefaultFrameBytes;
        /// Align the chunk so it can be memory-mapped. Only meaningful with method `None`.
        bool align_for_mapping = false;
    };

    [[nodiscard]] Status set_manifest(const PackageManifest& manifest) noexcept;

    /// Add an entry, storing its payload as a content-addressed chunk. A payload identical to one
    /// already added reuses that chunk and stores nothing further — the deduplication requirement,
    /// which `chunks_written()` and `bytes_deduplicated()` report on.
    [[nodiscard]] Status add(cy::AssetId id, VariantKey variant, Span<const u8> payload,
                             const EntryOptions& options,
                             Span<const cy::AssetId> dependencies = {}) noexcept;

    /// Add an entry whose chunk another package stores. Ships no bytes; `PackageSet` resolves it.
    [[nodiscard]] Status add_external(cy::AssetId id, VariantKey variant,
                                      const ContentHash& content, u64 uncompressed_size,
                                      AssetKind kind,
                                      Span<const cy::AssetId> dependencies = {}) noexcept;

    /// Add a patch's tombstone, masking this asset in every lower-priority package.
    [[nodiscard]] Status mark_deleted(cy::AssetId id, VariantKey variant) noexcept;

    /// Write the package to a NATIVE path, atomically.
    [[nodiscard]] Status write(const char* native_path) noexcept;

    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }
    [[nodiscard]] usize chunks_written() const noexcept { return chunks_.size(); }
    /// Bytes a deduplicated payload did not cost, because an identical chunk was already present.
    [[nodiscard]] u64 bytes_deduplicated() const noexcept { return bytes_deduplicated_; }

private:
    struct PendingChunk {
        ContentHash hash;
        Array<u8> stored;
        Array<FrameIndexEntry> frames;
        u64 uncompressed_size = 0;
        u32 frame_bytes = 0;
        CompressionMethod method = CompressionMethod::None;
        bool aligned = false;
    };

    [[nodiscard]] Expected<u32, Error> intern_chunk(Span<const u8> payload,
                                                    const EntryOptions& options,
                                                    const ContentHash& hash) noexcept;
    [[nodiscard]] Expected<u32, Error> intern_dependencies(
        Span<const cy::AssetId> dependencies) noexcept;
    [[nodiscard]] Status insert_entry(const PackageEntry& entry) noexcept;

    PackageManifest manifest_;
    Array<PackageEntry> entries_;
    Array<PendingChunk> chunks_;
    Array<cy::AssetId> dependencies_;
    u64 bytes_deduplicated_ = 0;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_PACKAGE_H
