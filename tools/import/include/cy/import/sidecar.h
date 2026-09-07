#ifndef CY_IMPORT_SIDECAR_H
#define CY_IMPORT_SIDECAR_H
// The import record: which importer, which options, and which id every sub-asset was given. M5 task
// 5.1.
//
// `asset-import-pipeline` — "Importer framework": "Each source asset SHALL have a `.meta` sidecar
// recording its `AssetId`, the importer used, its option values, and the ids of produced
// sub-assets." And: "WHEN a scene file produces meshes, materials and animations THEN each SHALL
// receive a stable sub-asset id recorded in the `.meta`, **so references survive re-import**."
//
// --- TWO FILES, AND WHY, STATED PLAINLY ----------------------------------------------------------
//
// The specification says one sidecar. This delivers two, beside every imported source:
//
//   `<source>.meta`    the ENGINE's record: `cy::assets::AssetMeta` — the asset's id, its kind, its
//                      source path and the digests. Written by `cy::assets::write_meta`, at layer
//                      0, read by the asset database and the editor.
//   `<source>.import`  THIS record: which importer ran, at which version, with which options, and
//                      which id each produced sub-asset holds.
//
// The reason is structural rather than stylistic. `AssetMeta` is a fixed-size, trivially copyable
// struct that `AssetDatabase` stores by value in a sorted array and copies on every registration; a
// sub-asset table is variable-length, so putting one inside it makes the type move-only and turns
// `register_asset(const AssetMeta&)` into a redesign of a layer-0 file. `parse_meta` also rejects
// unknown keys by design — "a sidecar written by a newer engine carries meaning this one would drop
// on the next write" — so the import keys cannot simply be appended to the same file.
//
// The split earns something too, which is why it is not merely a workaround: the identity record
// must NEVER change once written, and the import record changes whenever an option is edited.
// Keeping them apart means an option change is a one-line diff in a file that is not the identity,
// and a review can see at a glance that no id moved.
//
// Whoever unifies them should widen `AssetMeta` first; `pipeline.cpp` writes both and is the one
// place that would change.
//
// --- WHY AN ID IS REUSED AND NEVER DERIVED -------------------------------------------------------
//
// The obvious way to make a sub-asset id survive re-import is to derive it from the parent id and
// the sub-asset's name. `identity.h` forbids it for the reason the whole model exists: an id is
// minted once and survives every edit, and a derived id changes the moment somebody renames a node
// — which is precisely the case a stable id is for. So the record REMEMBERS: a name it has seen
// keeps its id, and a name it has not seen mints one. Renaming a node therefore mints a new id and
// orphans the old one, which is visible in the record's diff and is the honest outcome; nothing
// silently retargets a reference at a different mesh.

#include <cy/core/assets/hash.h>
#include <cy/core/assets/identity.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>

#include <string_view>

namespace cy::import {

/// The `.import` file's own format version. Bumped when this layout changes; a reader refuses a
/// newer one rather than dropping fields it does not know on the next write.
inline constexpr u32 kImportRecordVersion = 1;

/// The conventional name of the import record for a source path: `<source>.import`.
[[nodiscard]] Expected<assets::VirtualPath, Error> import_record_path_for(
    const assets::VirtualPath& source) noexcept;

/// What an earlier import of one source recorded.
///
/// Owns the text it was parsed from, so every `string_view` it hands out — an option's text value,
/// a sub-asset's name — is stable for the record's lifetime. A record built from scratch owns the
/// names it was given instead. Either way, nothing here points at a caller's buffer.
class ImportRecord {
public:
    ImportRecord() noexcept = default;

    ImportRecord(const ImportRecord&) = delete;
    ImportRecord& operator=(const ImportRecord&) = delete;
    ImportRecord(ImportRecord&&) noexcept = default;
    ImportRecord& operator=(ImportRecord&&) noexcept = default;

    /// Parse a record. `schema` is the schema of the importer named in the record, so that an
    /// option value's type is known while reading it; pass null to read the record's identity and
    /// sub-assets without its options, which is what a listing tool wants.
    ///
    /// Fails with `Unsupported` for a newer format version, and with `InvalidArgument` naming the
    /// line for anything malformed. An unknown key is an error rather than a silent skip, for the
    /// same reason `parse_meta` makes it one.
    [[nodiscard]] static Expected<ImportRecord, Error> parse(std::string_view text,
                                                             const OptionsSchema* schema) noexcept;

    /// Start a record for a source that has none, naming the importer that will run.
    [[nodiscard]] static Expected<ImportRecord, Error> create(std::string_view importer,
                                                              u32 importer_version) noexcept;

    [[nodiscard]] std::string_view importer() const noexcept;
    [[nodiscard]] u32 importer_version() const noexcept { return importer_version_; }
    [[nodiscard]] const assets::ContentHash& source_hash() const noexcept { return source_hash_; }
    void set_source_hash(const assets::ContentHash& hash) noexcept { source_hash_ = hash; }
    void set_importer_version(u32 version) noexcept { importer_version_ = version; }

    /// The option values this source was last imported with.
    [[nodiscard]] const ImportOptions& options() const noexcept { return options_; }
    [[nodiscard]] ImportOptions& options() noexcept { return options_; }

    /// The id bound to a sub-asset name, or the nil id when this record has never seen it.
    [[nodiscard]] cy::AssetId sub_asset(std::string_view name) const noexcept;

    /// Bind a name to an id, replacing any binding it had.
    [[nodiscard]] Status bind(std::string_view name, cy::AssetId id) noexcept;

    /// Forget every binding whose name is not in `keep`.
    ///
    /// Called after an import, so that a mesh an artist deleted stops occupying the record. Its id
    /// is NOT reused: nothing here mints, and `identity.h`'s `unregister` says the same thing.
    /// Returns how many bindings were dropped, which the report shows — an import that silently
    /// orphaned forty ids is a thing somebody wants to be told about.
    [[nodiscard]] usize retain_only(Span<const SubAsset> keep) noexcept;

    [[nodiscard]] usize binding_count() const noexcept { return bindings_.size(); }
    [[nodiscard]] std::string_view binding_name(usize index) const noexcept;
    [[nodiscard]] cy::AssetId binding_id(usize index) const noexcept;

    /// Render the record. Writes at most `capacity` bytes including the terminator and reports how
    /// many it wrote; fails with `BufferTooSmall` rather than truncating, because a truncated
    /// record reads as a valid one with sub-assets missing — which would mint new ids for them.
    [[nodiscard]] Expected<usize, Error> write(const OptionsSchema& schema, char* out,
                                               usize capacity) const noexcept;

    /// The buffer size `write` needs for this record, including the terminator. A caller sizes its
    /// buffer from this rather than guessing and retrying.
    [[nodiscard]] usize written_size(const OptionsSchema& schema) const noexcept;

private:
    struct Binding {
        u32 offset = 0;
        u32 length = 0;
        cy::AssetId id;
    };

    /// Copy a name into `names_` and return its slot, so the record owns every name it holds.
    [[nodiscard]] Expected<Binding, Error> intern(std::string_view name, cy::AssetId id) noexcept;
    [[nodiscard]] std::string_view name_of(const Binding& binding) const noexcept;

    /// The text this record was parsed from, or the importer name when it was created. Every view
    /// the record hands out points here or into `names_`.
    Array<char> storage_;
    u32 importer_offset_ = 0;
    u32 importer_length_ = 0;
    u32 importer_version_ = 0;
    assets::ContentHash source_hash_{};
    ImportOptions options_;
    Array<Binding> bindings_;
    Array<char> names_;
};

/// Give every sub-asset an id: the one it had, or a freshly minted one.
///
/// This is "references survive re-import", in one function so there is one place it can be got
/// wrong. `out_ids` is filled in `result.assets()` order and must have room for all of them;
/// `out_minted` reports how many were new, which is what a report shows and what a test asserts is
/// zero on a second import of an unchanged source.
///
/// `policy` is `MintPolicy::Refuse` for a cook that must be reproducible. It is a REQUIRED argument
/// rather than one with a default because the default is the dangerous answer: minting is what
/// makes two cold builds of one project differ, and a caller that has not thought about it should
/// be made to. See `MintPolicy` in importer.h for why M6 is the milestone that cares.
///
/// Refusing fails with `PermissionDenied` and names the first sub-asset that would have been
/// minted, so the diagnostic says which sidecar is missing rather than that something is.
[[nodiscard]] Status bind_sub_assets(ImportRecord& record, const ImportResult& result,
                                     Span<cy::AssetId> out_ids, usize& out_minted,
                                     MintPolicy policy) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_SIDECAR_H
