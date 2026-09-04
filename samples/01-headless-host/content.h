// The content: cooked into a package, mounted, loaded, and decoded back into components.
//
// THERE IS NO COOKER YET — it is M2 — so this sample cooks its own single package before it mounts
// it. That is the one place where the artefact stands in for something that does not exist, and it
// is deliberately a separate phase: `cook()` produces a `.cypak` from nothing but an entity count,
// and everything after it goes through the virtual filesystem and the asset system exactly as a
// shipped build would. Delete `cook()` when the cooker lands and the rest is unchanged.
//
// WHAT IS ACTUALLY IN THE PACKAGE. Each entry's payload is a run of reflected records, written by
// `cy::reflect::write_record`: a record addresses its fields by FieldId and carries no names and no
// offsets, so the package survives a field being renamed, reordered, or inserted upstream. The
// entity count divides into entries so the load has something to parallelise and so the package has
// more than one chunk to deduplicate against.

#ifndef CY_SAMPLE_HEADLESS_HOST_CONTENT_H
#define CY_SAMPLE_HEADLESS_HOST_CONTENT_H

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/reflect/registry.h>

#include "simulation.h"

namespace sample {

/// How the entities are divided into package entries. One entry is one block of consecutive
/// entities; the last block absorbs the remainder.
struct Layout {
    cy::u32 entities = 0;
    cy::u32 blocks = 0;

    [[nodiscard]] cy::u32 first_of(cy::u32 block) const noexcept;
    [[nodiscard]] cy::u32 count_of(cy::u32 block) const noexcept;
};

/// The asset id block `index` is stored under.
///
/// Derived from the index rather than minted, so a run is reproducible. A cooker mints an id ONCE
/// and records it in the sidecar (`cy::assets::mint_asset_id`); deriving one is correct only
/// because this content is regenerated from scratch every run and nothing persists a reference to
/// it. It stays outside the reserved placeholder namespace.
[[nodiscard]] cy::AssetId block_id(cy::u32 index) noexcept;

/// What the cook produced.
struct PackageStats {
    cy::u32 entries = 0;
    cy::u32 chunks = 0;
    cy::u64 payload_bytes = 0;       ///< uncompressed records written
    cy::u64 deduplicated_bytes = 0;  ///< payloads an identical chunk already covered
    cy::u64 file_bytes = 0;          ///< the `.cypak` on disk
};

/// Write the package. `native_path` is a real filesystem path; the directory must exist.
[[nodiscard]] cy::Expected<PackageStats, cy::Error> cook(const char* native_path,
                                                         const Layout& layout) noexcept;

/// Open the package and mount it into `files` at the base-package priority.
[[nodiscard]] cy::Status mount(cy::assets::VirtualFileSystem& files,
                               const char* native_path) noexcept;

/// What the load and decode phases produced.
struct LoadStats {
    cy::u32 assets = 0;
    cy::u64 bytes = 0;
    cy::u64 records = 0;
    cy::u64 partitions = 0;
};

/// Load every block through the asset system and decode the records into `world`.
///
/// The read stage runs on the async service — the one thread where blocking is legal — and the
/// decode runs as an indexed parallel loop over the blocks, whose partitioning is a function of the
/// block count and the grain alone. Blocks write disjoint slices of the component arrays, which is
/// what makes the loop safe without a lock.
[[nodiscard]] cy::Expected<LoadStats, cy::Error> load_and_decode(
    cy::assets::AssetSystem& assets, cy::jobs::JobSystem& jobs,
    const cy::reflect::TypeRegistry& registry, const Layout& layout, World& world) noexcept;

}  // namespace sample

#endif  // CY_SAMPLE_HEADLESS_HOST_CONTENT_H
