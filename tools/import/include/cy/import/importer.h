#ifndef CY_IMPORT_IMPORTER_H
#define CY_IMPORT_IMPORTER_H
// The importer framework: what an importer is, what it is given, and what it produces. M5 task 5.1.
//
// `asset-import-pipeline` — "Importer framework": "An **importer** SHALL declare: the source
// extensions it handles, its version, its options schema (a reflected settings struct), and the
// asset kinds it produces. Import SHALL be a pure function of (source bytes, options, importer
// version, platform variant), so its output is cacheable and reproducible."
//
// --- PURITY IS THE INTERFACE'S SHAPE, NOT A RULE IN A COMMENT ------------------------------------
//
// `Importer::import` is handed the source BYTES and cannot open a file. That is the whole of the
// enforcement, and it is enough: an importer that wants a second file — a glTF's external `.bin`, a
// shader's include, a material's texture — must ask for it through `ImportResolver`, which records
// what it read as a dependency at the moment it reads it. So the dependency list cannot be
// forgotten or fall out of step with what was actually read, because reading IS recording.
//
// The alternative — letting an importer open files and asking it to please declare what it opened —
// is what every pipeline that has a "why did this not re-cook" problem does.
//
// --- WHY A VIRTUAL INTERFACE HERE, IN A TREE THAT MOSTLY AVOIDS THEM
// ------------------------------
//
// "**Custom importers** SHALL be registrable from modules and from Swift." A registry of function
// pointers would do it, and a class with three virtual functions says what the three are. The
// engine compiles with -fno-rtti, so this is a vtable and nothing more: no `dynamic_cast`, no
// `typeid`, and the registry holds raw pointers to importers the caller owns for the caller's own
// lifetime.
//
// --- WHAT AN IMPORT PRODUCES ---------------------------------------------------------------------
//
// Sub-assets, each with a STABLE NAME. Not an index: "WHEN a scene file produces meshes, materials
// and animations THEN each SHALL receive a stable sub-asset id ... so references survive
// re-import", and an index is the one identifier that does not survive somebody adding a node at
// the top of a file. The importer names what it produced; `sidecar.h` is what turns a name into an
// id that never moves.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/hash.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/path.h>
#include <cy/core/assets/toolchain.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/options.h>

#include <string_view>

namespace cy::import {

/// What a build needs, as a declared policy rather than an accumulation of per-asset flags.
///
/// `asset-import-pipeline` — "Cook profiles". The profile is part of the derivation key, so a
/// `DedicatedServer` cook and a `Client` cook of the same source are two entries rather than one
/// that overwrites the other.
enum class CookProfile : u8 {
    /// Everything a playable client needs.
    Client = 0,
    /// Collision, navigation, gameplay and the animation data gameplay depends on. No textures, no
    /// shaders, no audio, no VFX, no UI.
    DedicatedServer = 1,
    /// Everything, including source references and authoring metadata.
    Editor = 2,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* cook_profile_name(CookProfile profile) noexcept;
[[nodiscard]] Expected<CookProfile, Error> cook_profile_from_name(std::string_view name) noexcept;

/// The prefix a collision sub-asset's name carries. M6 task 8.3.
///
/// It is a naming convention rather than an `AssetKind` because a collider IS a mesh — the physics
/// server loads one through the same path — and adding a kind for it would make every consumer that
/// switches on kind grow a case it does not want. What the prefix buys is the one thing a kind
/// would have bought: `profile_retains` can keep a collider in a cook that drops render meshes,
/// which is the "collision survives mesh exclusion" scenario.
inline constexpr std::string_view kCollisionSubAssetPrefix = "collision/";

/// Whether a cook profile keeps a sub-asset. M6 task 8.3.
///
/// `asset-import-pipeline` — "Cook profiles": a profile declares what a build needs, "so that
/// content selection is a declared policy rather than an accumulation of per-asset flags", and
/// "WHERE a server needs a **subset** of an otherwise client-only asset — collision geometry
/// derived from a render mesh — the profile SHALL retain that subset rather than either the whole
/// asset or nothing."
///
/// The table, in one function so that a second opinion about what a dedicated server needs cannot
/// exist:
///
/// | Profile           | Keeps                                                    |
/// |-------------------|----------------------------------------------------------|
/// | `Client`          | Everything an importer produces                           |
/// | `DedicatedServer` | Prefabs, collision meshes, and nothing else this pipeline produces |
/// | `Editor`          | Everything                                                |
///
/// A `DedicatedServer` cook drops textures, materials and render meshes — "Textures, shaders,
/// high-resolution meshes, audio, VFX assets, UI assets" — and keeps the prefab, because the
/// hierarchy IS the gameplay data, and the colliders, because that is the subset the requirement
/// names. Levels of detail go with the render mesh they reduce.
[[nodiscard]] bool profile_retains(CookProfile profile, assets::AssetKind kind,
                                   std::string_view sub_asset_name) noexcept;

/// Whether an identity may be minted during this cook. M6 task 8.3.
///
/// design.md §1.7: `assets::mint_asset_id()` draws 128 random bits, and the sidecar that records
/// what it drew is authoritative metadata that belongs in source control. Nothing enforced that,
/// and M6 is the milestone at which it stops being harmless — a prefab references its meshes by
/// `AssetId`, so from the commit that puts an id inside a payload, two cold builds of one project
/// stop producing the same bytes.
///
/// So a cook that has to be reproducible refuses to mint: it fails naming the asset, and the remedy
/// is to commit the sidecar, which should have happened anyway. An interactive import mints freely,
/// because minting is exactly what a first import is for.
enum class MintPolicy : u8 {
    /// Draw an id for a sub-asset the record does not know. What an editor and a first import do.
    Mint = 0,
    /// Refuse, naming the sub-asset. What a shipping cook and continuous integration do.
    Refuse = 1,
};

/// How serious a diagnostic is.
enum class ImportSeverity : u8 { Info = 0, Warning = 1, Error = 2 };

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* import_severity_name(ImportSeverity severity) noexcept;

/// One thing the importer has to say about an asset.
///
/// `code` is a stable identifier a report can group by and a project can suppress —
/// "srgb-normal-map" rather than a sentence that changes when somebody improves the wording.
/// `detail` is the sentence, and `subject` names the part of the source it is about: a node, a
/// material slot, an image.
struct ImportDiagnostic {
    static constexpr usize kDetailCapacity = 192;
    static constexpr usize kSubjectCapacity = 96;

    ImportSeverity severity = ImportSeverity::Info;
    /// A string literal owned by the importer. Never null.
    const char* code = "";
    char detail[kDetailCapacity] = {};
    char subject[kSubjectCapacity] = {};
};

/// One thing an import produced.
///
/// `name` is the STABLE path within the source — "mesh/Chair", "material/Oak", "animation/Idle". It
/// is what a sub-asset id is bound to across re-imports, so an importer must derive it from
/// something an artist controls (the node's name) rather than from something the file's ordering
/// controls (its index). Where a source names two things identically, the importer disambiguates
/// and says so in a diagnostic — silently producing two sub-assets with one name would make both of
/// them move on the next import.
struct SubAsset {
    assets::AssetKind kind = assets::AssetKind::Unknown;
    /// Owned. The importer builds it, the pipeline reads it and it dies with the result.
    Array<char> name;
    /// The cooked payload, ready to have a `CookedAssetHeader` put in front of it.
    Array<u8> payload;
    /// True for the one sub-asset that IS the source asset — the prefab of a scene, the image of a
    /// texture. Exactly one sub-asset carries it, and the pipeline refuses a result without one.
    bool primary = false;

    [[nodiscard]] std::string_view view() const noexcept;
};

/// Read an input the importer discovered it needed, recording it as a dependency in the same act.
///
/// The engine side of the purity argument at the top of this file. `read` returns the bytes AND
/// files the dependency; there is no way to get one without the other.
class ImportResolver {
public:
    virtual ~ImportResolver() = default;

    /// Read a path relative to the source's own directory, or a project-absolute virtual path.
    ///
    /// Fails with `NotFound` when it does not exist, which the importer reports as a diagnostic
    /// naming the referrer rather than failing the whole import — a glTF with one missing texture
    /// still produces its meshes.
    [[nodiscard]] virtual Expected<Span<const u8>, Error> read(std::string_view path) noexcept = 0;

    /// Record a dependency the importer consulted but did not read through `read` — an engine
    /// setting, a project convention, a referenced asset resolved by id.
    [[nodiscard]] virtual Status observe(std::string_view name,
                                         const assets::ContentHash& digest) noexcept = 0;

protected:
    ImportResolver() = default;
    ImportResolver(const ImportResolver&) = default;
    ImportResolver& operator=(const ImportResolver&) = default;
};

/// What an importer is asked to do.
struct ImportRequest {
    /// Where the source lives, for diagnostics and for resolving relative references.
    assets::VirtualPath source;
    /// The source's bytes. An importer reads these and nothing else without going through
    /// `resolver`.
    Span<const u8> bytes;
    /// The values in force for this import.
    const ImportOptions* options = nullptr;
    /// The platform-and-feature variant being cooked for.
    assets::VariantKey variant;
    CookProfile profile = CookProfile::Client;
    /// Where a discovered input is read from, and where reading it is recorded. Never null.
    ImportResolver* resolver = nullptr;

    /// One option's value, or its default. A convenience over `options->get(schema, name)` that an
    /// importer uses on every line of its own configuration.
    [[nodiscard]] Expected<OptionValue, Error> option(const OptionsSchema& schema,
                                                      std::string_view name) const noexcept;
};

/// What an import produced.
///
/// Diagnostics are accumulated rather than returned: an import that found four problems should
/// report four, and a `Status` can carry one. `has_errors()` is what the pipeline checks, and an
/// import that reports an error produces no cache entry — a cached failure is a failure you cannot
/// clear by fixing the source.
class ImportResult {
public:
    ImportResult() noexcept = default;

    ImportResult(const ImportResult&) = delete;
    ImportResult& operator=(const ImportResult&) = delete;
    ImportResult(ImportResult&&) noexcept = default;
    ImportResult& operator=(ImportResult&&) noexcept = default;

    /// Add a produced sub-asset. Takes ownership of both arrays.
    [[nodiscard]] Status add(assets::AssetKind kind, std::string_view name, Array<u8>&& payload,
                             bool primary) noexcept;

    /// Say something about the import. `detail` and `subject` are copied and truncated at the
    /// capacities in `ImportDiagnostic`; `code` must be a literal.
    [[nodiscard]] Status report(ImportSeverity severity, const char* code, std::string_view detail,
                                std::string_view subject) noexcept;

    /// Record an input this import read. Called by the resolver; an importer that reads through the
    /// resolver never calls it directly.
    [[nodiscard]] Status add_dependency(std::string_view name,
                                        const assets::ContentHash& digest) noexcept;

    [[nodiscard]] Span<const SubAsset> assets() const noexcept;
    [[nodiscard]] Span<const ImportDiagnostic> diagnostics() const noexcept;

    [[nodiscard]] usize dependency_count() const noexcept { return dependencies_.size(); }
    /// One recorded dependency, as the cache wants it. The name points into this result.
    [[nodiscard]] assets::DerivedDependency dependency(usize index) const noexcept;

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] usize warning_count() const noexcept;

    /// The sub-asset marked primary, or null when the importer produced none.
    [[nodiscard]] const SubAsset* primary() const noexcept;

    void clear() noexcept;

private:
    struct DependencySlot {
        u32 offset = 0;
        u32 length = 0;
        assets::ContentHash digest;
    };

    Array<SubAsset> assets_;
    Array<ImportDiagnostic> diagnostics_;
    Array<DependencySlot> dependencies_;
    /// One blob for every dependency name, so a name is stable while the result lives and costs one
    /// allocation for all of them rather than one each.
    Array<char> dependency_names_;
};

/// What an importer says about itself.
struct ImporterInfo {
    /// A stable lower-case identifier: "gltf", "texture", "font". It is part of every derivation
    /// key this importer produces, so renaming it re-cooks everything it handles — which is why it
    /// is the importer's identity and not its label.
    std::string_view name;
    /// Moved whenever the output changes for the same input. Part of the derivation key.
    u32 version = 1;
    /// Lower-case, with the dot: ".gltf", ".glb".
    Span<const std::string_view> extensions;
    /// What it can produce. A report groups by it, and a cook profile excludes by it.
    Span<const assets::AssetKind> produces;
    /// What it is, in a sentence, for a listing a person or a machine caller reads.
    std::string_view description;
};

/// Something that turns source bytes into cooked sub-assets.
class Importer {
public:
    virtual ~Importer() = default;

    [[nodiscard]] virtual ImporterInfo info() const noexcept = 0;
    [[nodiscard]] virtual OptionsSchema schema() const noexcept = 0;

    /// Do the work. Every failure that is about the SOURCE is a diagnostic on `out`; a returned
    /// error is about the importer itself — out of memory, a bug's guard, an unimplemented path.
    [[nodiscard]] virtual Status import(const ImportRequest& request,
                                        ImportResult& out) noexcept = 0;

protected:
    Importer() = default;
    Importer(const Importer&) = default;
    Importer& operator=(const Importer&) = default;
};

/// Every importer this build knows.
///
/// Holds borrowed pointers: importers are long-lived objects the caller owns, whether they are
/// static built-ins or plugin-supplied. The registry does not own them and does not outlive them,
/// which is checked by nothing and is the reason `register_importer` takes a pointer rather than a
/// value — a signature that lies about ownership is worse than one that makes the caller think.
class ImporterRegistry {
public:
    ImporterRegistry() noexcept = default;

    ImporterRegistry(const ImporterRegistry&) = delete;
    ImporterRegistry& operator=(const ImporterRegistry&) = delete;

    /// Register an importer, refusing:
    ///   * a null pointer, an empty name, or a name already registered;
    ///   * an extension another importer already claims — two importers for one extension is an
    ///     ambiguity nothing downstream can resolve, and the first one to register would win
    ///     silently;
    ///   * a schema `OptionsSchema::validate` rejects.
    [[nodiscard]] Status register_importer(Importer* importer) noexcept;

    /// The importer for an extension, or null. The extension includes its dot and is matched
    /// case-insensitively, because `.GLTF` off a Windows filesystem is the same file.
    [[nodiscard]] Importer* find_for_extension(std::string_view extension) const noexcept;

    /// The importer for a source path, by its extension. Null when nothing handles it.
    [[nodiscard]] Importer* find_for_source(const assets::VirtualPath& source) const noexcept;

    /// The importer of a given name, or null. What a sidecar resolves through, so that a re-import
    /// uses the importer that produced the asset rather than whichever now claims the extension.
    [[nodiscard]] Importer* find_by_name(std::string_view name) const noexcept;

    [[nodiscard]] usize size() const noexcept { return importers_.size(); }
    [[nodiscard]] Importer* at(usize index) const noexcept;

    void clear() noexcept { importers_.clear(); }

private:
    Array<Importer*> importers_;
};

/// Contribute an import's fixed inputs to a derivation key, in one place.
///
/// Every producer must build its key in ONE function or it will eventually miss its own cache by
/// adding a field in two orders. This is that function for imports, and an importer that wants
/// something else in the key adds it through its options schema rather than by building a key of
/// its own.
///
/// It contributes `assets::current_toolchain()` and FAILS on an incomplete one — M7 task 1.1. Until
/// that landed this function named no compiler, no flags and no library versions, so two builds of
/// one importer at `-O2` and at `-O0` computed the same key and a shared cache served either
/// binary's artefact to the other; M6's closing gate measured that as 1 hit, 0 miss. The five
/// toolchain fields are added by `ToolchainFingerprint::contribute`, which is the same function
/// `cy::build::derivation_key` and `cy::shader::derive_cache_key` call, so no two producers in this
/// tree can disagree about what "the toolchain" means.
[[nodiscard]] Expected<assets::DerivationKey, Error> import_derivation_key(
    const ImporterInfo& info, const OptionsSchema& schema, const ImportRequest& request,
    const assets::ContentHash& source_hash) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_IMPORTER_H
