#pragma once
// The save container: the manifest, the chunks, and what a failed load says. Tasks 6.2 and 6.3.
//
// `save-and-persistence` — "Save container and manifest" — fixes what a manifest declares and
// requires content to be **chunked**, "so that loading does not require reading the whole save into
// memory and so that incremental writes touch only changed chunks". "Compatibility and migration"
// fixes what a load may refuse and requires the refusal to be structured: "a boolean failure SHALL
// NOT be the interface".
//
// ONE CHUNK PER REGION, ADDRESSED BY ITS CONTENT. A chunk holds one region's entries or one scope's
// fragments, and it is stored under the hash of its own bytes. Three properties fall out of that
// and none of them needed code:
//
//   * an incremental write touches only what changed — a region whose state did not change encodes
//     to the same bytes (the overlay is sorted by key, see overlay.h) and therefore to a chunk that
//     is already in the store;
//   * generations share storage, so retaining three of them costs three manifests plus whatever
//     actually differs;
//   * corruption is detectable, because the name of a chunk is a claim about its content, and
//     "chunks SHALL carry content hashes, and the manifest SHALL carry the hashes of its chunks, so
//     corruption is detected rather than loaded".
//
// EVERYTHING IN A CHUNK IS A VALUE RECORD, INCLUDING THE STRUCTURE. An entry's identity, kind,
// template and owner are written as a record of a reserved type, followed by its components'
// records. This is not a trick to save writing a parser: it is what makes migration, unknown-field
// preservation and skip-unknown apply to the save's own structure and not only to its payload —
// `serialize::read_record` reads a record of a type this build has never heard of into a
// `ValueRecord` and `write_record` writes it back byte for byte, so a chunk written by a build with
// a plugin loads in a build without one and saves again with that plugin's state intact.
//
// THE RESERVED TYPE IDENTIFIERS ARE NOT MANIFEST IDENTIFIERS. `identity/manifest.toml` issues
// `TypeId`s from a counter that starts low; the three below are at the top of the 32-bit range so
// that a collision would require the manifest to have issued four billion identifiers. They are
// written into files, so they are fixed forever.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/serialize/migration.h>
#include <cy/core/serialize/tagged.h>
#include <cy/core/values/asset_id.h>
#include <cy/save/identity.h>
#include <cy/save/overlay.h>

namespace cy::save {

/// The container's FORMAT version: what a parser must understand to read the bytes at all.
///
/// "Save format version and type schema versions SHALL be independent: a container change and a
/// gameplay type change are different events and SHALL NOT share one number." A record's schema
/// version is the other one, and it lives on the record.
inline constexpr u16 kSaveFormatVersion = 1;

/// Chunk tags, in the spelling `serialize::chunk_tag` reads back out of a hex dump.
inline constexpr u32 kManifestChunkTag = serialize::chunk_tag('M', 'F', 'S', 'T');
inline constexpr u32 kRegionChunkTag = serialize::chunk_tag('R', 'G', 'N', ' ');
inline constexpr u32 kFragmentChunkTag = serialize::chunk_tag('F', 'R', 'A', 'G');

/// Reserved record types. Fixed forever; see the note above about why they are up here.
inline constexpr reflect::TypeId kChunkHeaderType{0xFFFF'FF01U};
inline constexpr reflect::TypeId kEntryHeaderType{0xFFFF'FF02U};
inline constexpr reflect::TypeId kManifestHeaderType{0xFFFF'FF03U};
inline constexpr reflect::TypeId kChunkRefType{0xFFFF'FF04U};
inline constexpr reflect::TypeId kPluginRefType{0xFFFF'FF05U};

/// What a load refused to do, and why. Structured, because "incompatible build, migration failed,
/// missing plugin, corrupt chunk, missing content, unresolvable reference" are six different
/// problems with six different fixes and one boolean tells a player none of them.
enum class LoadFailure : u8 {
    None = 0,
    /// The bytes are not a save this build can parse: wrong magic, or a newer format version.
    IncompatibleFormat,
    /// The save was written by a build this project's policy will not load.
    IncompatibleBuild,
    /// A migration chain could not carry a record to the current schema version.
    MigrationFailed,
    /// A type in the save belongs to a plugin that is not present.
    MissingPlugin,
    /// A chunk's bytes do not hash to the name the manifest gave them.
    CorruptChunk,
    /// A chunk the manifest names is not in the store.
    MissingChunk,
    /// The save refers to cooked content that is not installed, or is a different version.
    MissingContent,
    /// A persistent reference names an entity nothing can resolve.
    UnresolvableReference,
};

const char* load_failure_name(LoadFailure failure) noexcept;

/// A project's declared tolerance for loading a save another build wrote.
enum class Compatibility : u8 {
    /// Only the build that wrote it. What a shipped competitive title uses.
    ExactBuild = 0,
    /// Any build declaring the same major version.
    SameMajorVersion = 1,
    /// Any build whose migration chains reach the save's schema versions. The default.
    Migratable = 2,
    /// Load what can be loaded and report the rest. Development only.
    BestEffort = 3,
};

/// What a load did, and what stopped it. Filled in whether or not the load succeeded, because the
/// counts are the save inspector's numbers and a successful load has them too.
struct LoadReport {
    LoadFailure failure = LoadFailure::None;
    /// What was incompatible, missing or damaged. A literal or a pointer into caller storage that
    /// outlives the report; nothing here owns text.
    const char* detail = "";
    /// The chunk the failure names, when it names one.
    assets::ContentHash chunk;

    u32 chunks_read = 0;
    u32 records_read = 0;
    u32 records_migrated = 0;
    /// Records of a type this build's schema registry does not declare — a disabled plugin's
    /// state, or a type that was removed — carried through untouched, and the fields in them.
    /// "Preservation SHALL be bounded and reportable" is this pair of counters.
    u32 records_unknown = 0;
    u32 fields_preserved = 0;
    u32 entries_applied = 0;
    /// Generations tried before one loaded. Non-zero means the newest failed verification and an
    /// earlier one was used, which is a fallback the player should be told about.
    u32 generations_skipped = 0;

    [[nodiscard]] bool failed() const noexcept { return failure != LoadFailure::None; }
};

/// A plugin whose types the save contains, and the version it was written against.
struct PluginRequirement {
    static constexpr usize kNameLength = 47;
    /// NUL-terminated, so the diagnostic can name it without allocating.
    char name[kNameLength + 1] = {};
    u32 version = 0;
};

/// One chunk, as the manifest records it.
struct ChunkRef {
    Scope scope = Scope::World;
    RegionKey region;
    assets::ContentHash hash;
    u32 size = 0;
    u32 entry_count = 0;
};

/// Everything a save declares about itself. `save-and-persistence` lists the members; the two
/// identity hashes and the plugin inventory are what a load checks before it reads a chunk.
struct Manifest {
    explicit Manifest(Allocator& allocator = current_allocator()) noexcept
        : chunks(allocator), plugins(allocator) {}

    u16 format_version = kSaveFormatVersion;
    u32 generation = 0;

    /// The build that wrote it, as a NUL-terminated string. Compared per the project's policy.
    static constexpr usize kBuildIdLength = 63;
    char build_id[kBuildIdLength + 1] = {};

    AssetId project;
    AssetId save;
    AssetId campaign;

    /// The tick at whose commit boundary the state was captured.
    u64 simulation_point = 0;
    /// The seed the session's random streams were built from.
    u64 session_seed = 0;

    /// The cooked content this save is a delta against, and the plugin set that was installed.
    assets::ContentHash content_version;
    assets::ContentHash plugin_version;

    /// Sorted by (scope, region), so a manifest is byte-identical for equal content.
    Array<ChunkRef> chunks;
    Array<PluginRequirement> plugins;

    [[nodiscard]] Status add_chunk(const ChunkRef& chunk) noexcept;
    [[nodiscard]] Status require_plugin(const char* name, u32 version) noexcept;
    [[nodiscard]] const ChunkRef* find_chunk(Scope scope, RegionKey region) const noexcept;
    [[nodiscard]] u64 total_chunk_bytes() const noexcept;
    /// The bytes this scope's chunks occupy. The inspector's "size by scope".
    [[nodiscard]] u64 chunk_bytes_in(Scope scope) const noexcept;
};

/// What a plugin inventory answers when a load asks whether a required plugin is present.
using PluginPresence = bool (*)(void* user, const PluginRequirement& plugin) noexcept;

/// What a load is allowed to accept, and what it checks against.
struct LoadPolicy {
    Compatibility compatibility = Compatibility::Migratable;
    /// The build doing the loading. Empty means "do not check", which is what a tool inspecting a
    /// save wants and what a game never does.
    const char* build_id = "";
    /// The installed content's version. Zero means "do not check".
    assets::ContentHash content_version;
    /// Migration chains for the types in the save. Null means no type is declared, and every record
    /// passes through untouched — which is what keeps unknown data preserved rather than rejected.
    const serialize::SchemaRegistry* schemas = nullptr;
    /// Asked once per plugin the manifest requires. Null means every plugin is considered present.
    PluginPresence plugin_present = nullptr;
    void* plugin_user = nullptr;
};

// --- Encoding -----------------------------------------------------------------------------------

/// Encode one region's entries as a chunk payload. Empty regions produce a chunk with no entries
/// rather than nothing, so that "this region has no delta" is a statement the save makes.
[[nodiscard]] Status encode_region(const Overlay& overlay, RegionKey region,
                                   Array<u8>& out) noexcept;

/// Encode one scope's fragments as a chunk payload.
[[nodiscard]] Status encode_fragments(const Overlay& overlay, Scope scope, Array<u8>& out) noexcept;

/// Decode a chunk of either kind into `out`, layering it over whatever `out` already holds.
///
/// Migration happens HERE, one record at a time, before the record reaches the overlay: the load
/// path is "serialized record → value record → migration chain → current schema", and a save is
/// "every form of tagged data addressed by these identifiers" that the chain applies to.
[[nodiscard]] Status decode_chunk(Span<const u8> payload, const LoadPolicy& policy, Overlay& out,
                                  LoadReport& report) noexcept;

[[nodiscard]] Status encode_manifest(const Manifest& manifest, Array<u8>& out) noexcept;
[[nodiscard]] Status decode_manifest(Span<const u8> bytes, Manifest& out,
                                     LoadReport& report) noexcept;

/// Check a manifest against what this build is: format version, build identity under the policy,
/// content version, and the plugin inventory. The whole of "validate compatibility", in one call
/// made before a single chunk is read.
[[nodiscard]] Status check_compatibility(const Manifest& manifest, const LoadPolicy& policy,
                                         LoadReport& report) noexcept;

}  // namespace cy::save
