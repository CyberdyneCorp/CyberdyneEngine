#pragma once
// The cook path: authoring documents on disk in, a package of cooked assets out. M2 task 3.2.13.
//
// `core-assets-and-io` — "Assets are cooked, not parsed at runtime": "the runtime SHALL load only
// cooked assets", and "a shipping build's package SHALL contain cooked assets only". M1 built the
// package format's write path and left `PackageWriter::add` taking bytes that were already cooked;
// M2's `src/scene/serialization/` produces those bytes for scenes and prefabs. This is the driver
// that joins them, and it is the first thing in the engine that turns a directory of authored files
// into something the runtime can load.
//
//   read      every `*.cyscene` and `*.cyprefab` under a source root, through the virtual
//   filesystem register  each one into a `Library`, keyed by the asset id its own header declares
//   validate  the dependency graph — cycles rejected as a chain, before anything is cooked
//   cook      resolve, bind parameters, validate references, flatten, assign identity, emit blocks
//   wrap      each cooked stream in the cooked-asset header (`<cy/core/assets/cooked.h>`)
//   write     into a `.cypak`, recording each asset's dependencies so the loader can preload them
//
// --- WHAT THIS TOOL DELIBERATELY DOES NOT DO -----------------------------------------------------
//
// **It does not import source formats.** A `.png` or a `.gltf` needs an importer per format, and
// the specification's list of them is a milestone of its own. What it cooks is the engine's own
// authoring form, which is the one M2 defines.
//
// **It does not decide the component set.** A cook needs to know each component's size, alignment
// and entity-reference offsets, and the only authority on that is a world that has registered them
// — so the caller supplies one. A tool that guessed would produce a package the runtime rejects at
// the build-schema check, which is the check working as intended and a poor way to find out.
//
// **It is not incremental.** Every document is cooked every run. The content hash the cooked header
// records is what an incremental build would compare, and it is written; the comparison is not.
// Said plainly here rather than discovered from a slow build.

#include <cy/core/assets/identity.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/scene/serialization/cook.h>
#include <cy/scene/serialization/document.h>
#include <cy/scene/serialization/library.h>

namespace cy::cook {

/// What one run of the cook was asked to do.
struct CookRequest {
    /// The virtual directory holding the authoring documents. Enumerated recursively.
    assets::VirtualPath source;
    /// The world whose registered components define the layout every cooked block is emitted
    /// against. Not modified: the cook reads its component registry and nothing else.
    const ecs::World* world = nullptr;
    /// The platform-and-feature key every cooked asset in this run is stamped with.
    assets::VariantKey variant;
    /// How the cooker finds an entity's local transform, for flattening and for scene placement.
    scene::serialization::TransformBinding transform;
    /// Strip prefab provenance. True for a shipping package, where "the resulting entities SHALL
    /// carry no prefab link and no override data".
    bool shipping = false;
    /// Fail the run when unresolved override conflicts remain, rather than reporting them.
    bool fail_on_conflicts = false;
};

/// One document the cook read and what it produced from it.
struct CookedDocumentReport {
    AssetId id;
    scene::serialization::AssetKind kind = scene::serialization::AssetKind::Scene;
    u32 entities = 0;
    u32 blocks = 0;
    u32 relationships_flattened = 0;
    u32 relationships_retained = 0;
    u32 reference_sites = 0;
    u32 conflicts = 0;
    u32 payload_bytes = 0;
    /// Total bytes written into the package, header included.
    u32 cooked_bytes = 0;
};

/// The whole run, for the report a build prints and a gate reads.
struct CookRunReport {
    explicit CookRunReport(Allocator& allocator) noexcept
        : documents(allocator), cycle(allocator) {}

    Array<CookedDocumentReport> documents;
    u32 documents_read = 0;
    u32 documents_cooked = 0;
    u32 total_entities = 0;
    u32 total_relationships_flattened = 0;
    u32 total_conflicts = 0;
    /// Filled in when the dependency graph contains a cycle, so the failure names the chain rather
    /// than one asset in it.
    Array<AssetId> cycle;
};

/// Read every authoring document under `source` into `out`, in a deterministic order.
///
/// The documents are the caller's to own because a `Library` borrows them and a cook resolves
/// through it, so they must outlive both. Ordered by path, so that two runs over one tree read them
/// in the same order and produce the same package byte for byte.
[[nodiscard]] Status read_documents(assets::VirtualFileSystem& vfs,
                                    const assets::VirtualPath& source,
                                    Array<scene::serialization::Document>& out) noexcept;

/// Cook every document into `package`, which the caller opens and writes.
///
/// Fails, without writing anything, when the dependency graph contains a cycle: a package half of
/// whose assets were cooked from a graph the other half could not be is worse than no package.
[[nodiscard]] Status cook_documents(const CookRequest& request,
                                    Array<scene::serialization::Document>& documents,
                                    assets::PackageWriter& package, CookRunReport& report) noexcept;

/// The whole path: read, cook, and write a package at `destination`.
[[nodiscard]] Status run(assets::VirtualFileSystem& vfs, const CookRequest& request,
                         const char* destination, CookRunReport& report) noexcept;

/// The file extensions the cook reads. A document's kind comes from its own header, not from its
/// name — the extension only decides what is offered to the reader.
inline constexpr const char* kSceneExtension = ".cyscene";
inline constexpr const char* kPrefabExtension = ".cyprefab";

}  // namespace cy::cook
