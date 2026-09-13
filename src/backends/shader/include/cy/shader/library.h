#ifndef CY_SHADER_LIBRARY_H
#define CY_SHADER_LIBRARY_H
// The shader library: the cooked artefact a compilation is packaged into. Task 3.4.
//
// `shader-system` — "Shader library and caching": compiled shaders are stored in **shader
// libraries**, content-addressed artefacts containing SPIR-V or backend-native code, reflection
// data, and the permutation key.
//
// --- WHY REFLECTION IS IN THE ARTEFACT -------------------------------------------------------------
//
// It could be recomputed from the SPIR-V at load. It is stored instead, for a reason that is easy to
// miss: **a shipping build has no compiler, and a shipping build for a backend-native target has no
// SPIR-V either.** `shader-system`'s pipeline step 4 retains SPIR-V for Vulkan but produces MSL for
// Metal and DXIL for D3D12, and neither of those can be reflected by the parser in `spirv.h`.
// Reflection therefore has to survive the cook, in a form that does not depend on which target the
// code half of the entry is. That is also what lets a library be *inspected* — `just` recipes and
// the editor's shader view read the reflection without a compiler and without a device.
//
// --- THE FORMAT ------------------------------------------------------------------------------------
//
//   header      magic, format version, entry count, the library's own content hash
//   entries     fixed-size records: permutation key, stage, target, entry-point name, and the
//               offsets and lengths of this entry's code and reflection blobs
//   blobs       code and reflection payloads, each aligned to four bytes
//
// Fixed-size records and explicit offsets, not a serialization framework: the file is read by
// `memcpy` out of a mapped region, an entry is found by binary search over a sorted table, and there
// is no per-entry allocation on the load path. That matters because a project's library holds tens
// of thousands of entries and a game loads it during a loading screen it is being measured on.
//
// Everything is little-endian and the format version is checked. A library from a different version
// is rejected rather than migrated: it is derived data, and recompiling it is what the cache is for.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/compiler.h>
#include <cy/shader/permutation.h>
#include <cy/shader/reflection.h>

namespace cy::shader {

/// `CYSL`, little-endian.
inline constexpr u32 kLibraryMagic = 0x4C53'5943U;

/// Bumped whenever a record's layout changes. Part of every cache key, because a library written by
/// an older engine is derived data an older engine derived.
inline constexpr u32 kLibraryVersion = 1;

/// One compiled variant, as the library stores it.
struct LibraryEntry {
    /// The full permutation key, specialization axes included. The *compilation* key is what the
    /// cache is addressed by; this is what a pipeline lookup matches.
    PermutationKey permutation;
    /// The entry-point name in the code blob — the SPIR-V one, not the Slang one. See
    /// `spirv_entry_points()` for why the two differ.
    ShortName entry_point;
    Stage stage = Stage::Vertex;
    Target target = Target::SpirV;
    /// The content hash of the code blob. Two entries with the same hash share one blob, which is
    /// how `shader-system`'s "two materials share a pipeline" is realised in the file rather than
    /// only in the cache.
    ContentHash code_hash;
    u32 code_offset = 0;
    u32 code_size = 0;
    u32 reflection_offset = 0;
    u32 reflection_size = 0;
};

/// A library being assembled. Write side: the cook step and the hot-reload path both use it.
class ShaderLibraryBuilder {
public:
    explicit ShaderLibraryBuilder(Allocator& allocator) noexcept;

    ShaderLibraryBuilder(const ShaderLibraryBuilder&) = delete;
    ShaderLibraryBuilder& operator=(const ShaderLibraryBuilder&) = delete;

    /// Add one compiled variant. Deduplicates the code blob by content hash.
    ///
    /// Rejects a variant whose (permutation, entry point, stage, target) is already present: a
    /// second compilation of the same variant means two different sources produced it, and picking
    /// one silently is how a library ends up disagreeing with the manifest that indexes it.
    [[nodiscard]] Status add(PermutationKey permutation, Stage stage, Target target,
                             std::string_view entry_point, Span<const u32> code,
                             const Reflection& reflection) noexcept;

    [[nodiscard]] u32 entry_count() const noexcept { return static_cast<u32>(entries_.size()); }

    /// Serialise into `out`, replacing it. The bytes are deterministic: entries are sorted into a
    /// canonical order first, so two builds that compiled the same variants produce byte-identical
    /// libraries and therefore the same content hash.
    [[nodiscard]] Status finish(Array<u8>& out) noexcept;

private:
    struct Blob {
        u32 offset = 0;
        u32 size = 0;
        ContentHash hash;
    };

    /// Append bytes to the payload arena, reusing an identical blob. Returns its range.
    [[nodiscard]] Expected<Blob, Error> intern_blob(const void* data, u32 size) noexcept;

    Allocator* allocator_;
    Array<LibraryEntry> entries_;
    Array<u8> payload_;
    Array<Blob> blobs_;
};

/// A library being read. Non-owning over the bytes: the caller keeps the mapping alive, which is
/// what makes loading a library a mapping and a header check rather than a copy.
class ShaderLibrary {
public:
    ShaderLibrary() noexcept = default;

    /// Validate the header and the entry table against `bytes`. Rejects a wrong magic, a wrong
    /// version, a truncated file, and any entry whose blob ranges fall outside the payload — the
    /// last one because a library is content addressed and a corrupt one should be reported here
    /// rather than as an unexplained SPIR-V parse failure much later.
    [[nodiscard]] static Expected<ShaderLibrary, Error> open(Span<const u8> bytes) noexcept;

    [[nodiscard]] u32 entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] LibraryEntry entry(u32 index) const noexcept;
    /// The library's own content hash, as written. The artefact's address.
    [[nodiscard]] ContentHash hash() const noexcept { return hash_; }

    /// Find a variant. Entries are sorted, so this is a binary search and not a scan — a forward
    /// pass looks up one per draw call in the worst case.
    [[nodiscard]] Expected<u32, Error> find(PermutationKey permutation, Stage stage, Target target,
                                            std::string_view entry_point) const noexcept;

    /// The code words of an entry. Empty when the entry holds a non-SPIR-V target, whose bytes are
    /// reached through `code_bytes()` instead.
    [[nodiscard]] Span<const u32> code(u32 index) const noexcept;
    [[nodiscard]] Span<const u8> code_bytes(u32 index) const noexcept;

    /// Decode an entry's stored reflection. Allocates; called once per pipeline, not per draw.
    [[nodiscard]] Expected<Reflection, Error> reflection(u32 index,
                                                         Allocator& allocator) const noexcept;

private:
    Span<const u8> bytes_;
    u32 entry_count_ = 0;
    u32 table_offset_ = 0;
    u32 payload_offset_ = 0;
    ContentHash hash_;
};

/// Serialise reflection into a self-describing blob, and read it back.
///
/// Public because the library is not the only consumer: a diagnostic dump and the editor's shader
/// view both want the blob without the container around it, and a second encoder for them would be a
/// second thing to keep in step with `Reflection`.
[[nodiscard]] Status encode_reflection(const Reflection& reflection, Array<u8>& out) noexcept;
[[nodiscard]] Expected<Reflection, Error> decode_reflection(Span<const u8> bytes,
                                                            Allocator& allocator) noexcept;

}  // namespace cy::shader

#endif  // CY_SHADER_LIBRARY_H
