#ifndef CY_CORE_ASSETS_DIAGNOSTICS_H
#define CY_CORE_ASSETS_DIAGNOSTICS_H
// The asset layer's counters, on the M0 trace. Tasks 3.3.1 through 3.3.6.
//
// The report is a snapshot of everything the layer counts, plus one trace instant carrying the same
// figures as classified fields. Emission is a no-op when no trace is open, so a shipping build that
// never opens one pays a load and a branch.
//
// PRIVACY. Every field is declared with CY_TRACE_FIELD, which takes its classification as a
// REQUIRED third argument. Counts are Public — a number about the engine identifies nobody. A
// FILESYSTEM PATH IS NOT: it carries a user's account name, their project's name, and sometimes
// their employer's. It is classified PotentiallyPersonal and it is carried as a FIELD, never as an
// event name, which is the rule design.md section 2 states — a path baked into a name is not
// reachable by the writer's redaction.

#include <cy/core/assets/identity.h>
#include <cy/core/base/types.h>
#include <cy/core/values/asset_id.h>

namespace cy::assets {

/// Everything the asset layer counts, in one snapshot.
struct AssetDiagnostics {
    // Identity
    u64 ids_minted = 0;
    u64 meta_collisions = 0;
    u64 placeholders_served = 0;

    // The virtual filesystem
    u64 path_rejections = 0;
    u64 mount_resolutions = 0;
    u64 mount_misses = 0;

    // Packages
    u64 packages_opened = 0;
    u64 packages_refused = 0;
    u64 package_bytes_read = 0;
    u64 entries_mapped = 0;
    u64 integrity_failures = 0;

    // Compression
    u64 bytes_compressed_in = 0;
    u64 bytes_compressed_out = 0;
    u64 bytes_decompressed = 0;
    u64 frames_touched = 0;
};

/// A snapshot of the counters. Cheap: relaxed atomic loads, no locking.
[[nodiscard]] AssetDiagnostics assets_diagnostics() noexcept;

/// Emit the counters onto the shared trace as Counter records plus one instant carrying the whole
/// snapshot. A no-op when no trace is open.
void assets_trace_report() noexcept;

/// Write the counters to the log at Info.
void assets_log_report() noexcept;

/// The diagnostic the "source deleted" scenario requires: a reference could not be resolved, so a
/// typed placeholder was served. Names the missing asset, the REFERRER, and the kind that was
/// expected — the referrer is the half that makes the message actionable, because the missing id
/// alone says nothing about which material to fix.
void assets_log_missing_asset(cy::AssetId missing, cy::AssetId referrer, AssetKind kind) noexcept;

/// A package was refused at mount. `reason` is a literal or storage that outlives the call.
void assets_log_package_refused(const char* path, const char* reason) noexcept;

/// The counters the layer increments. Declared here so that every module of the layer reaches the
/// same ones, and so a test can assert on a delta.
namespace counters {
void record_id_minted() noexcept;
void record_meta_collision() noexcept;
void record_placeholder_served() noexcept;
void record_path_rejection() noexcept;
void record_mount_resolution(bool hit) noexcept;
void record_package_opened(bool refused) noexcept;
void record_package_bytes(u64 bytes) noexcept;
void record_entry_mapped() noexcept;
void record_integrity_failure() noexcept;
void record_compression(u64 input, u64 output) noexcept;
void record_decompression(u64 bytes, u64 frames) noexcept;
void reset() noexcept;
}  // namespace counters

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_DIAGNOSTICS_H
