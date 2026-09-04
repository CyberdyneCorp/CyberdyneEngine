// The asset layer's diagnostics, emitted onto the M0 trace. Tasks 3.3.1 through 3.3.6.
//
// See the header for the privacy argument. The short version: counts are Public, and a filesystem
// path is a PotentiallyPersonal FIELD rather than part of an event name.

#include <cy/core/assets/diagnostics.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include <atomic>
#include <cstring>

namespace cy::assets {
namespace {

CY_TRACE_CATEGORY(category, "assets")

CY_TRACE_FIELD(ids_minted, u64, cy::Privacy::Public)
CY_TRACE_FIELD(meta_collisions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(placeholders_served, u64, cy::Privacy::Public)
CY_TRACE_FIELD(path_rejections, u64, cy::Privacy::Public)
CY_TRACE_FIELD(mount_resolutions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(mount_misses, u64, cy::Privacy::Public)
CY_TRACE_FIELD(packages_opened, u64, cy::Privacy::Public)
CY_TRACE_FIELD(packages_refused, u64, cy::Privacy::Public)
CY_TRACE_FIELD(package_bytes_read, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(entries_mapped, u64, cy::Privacy::Public)
CY_TRACE_FIELD(integrity_failures, u64, cy::Privacy::Public)
CY_TRACE_FIELD(bytes_compressed_in, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(bytes_compressed_out, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(bytes_decompressed, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(frames_touched, u64, cy::Privacy::Public)

/// An asset id is a content identifier the engine minted; it names no person and no machine.
CY_TRACE_FIELD(asset_id, string, cy::Privacy::Public)
CY_TRACE_FIELD(referrer_id, string, cy::Privacy::Public)
CY_TRACE_FIELD(asset_kind, string, cy::Privacy::Public)

/// A FILESYSTEM PATH. Classified, and a field rather than an event name — see the header.
CY_TRACE_FIELD(package_path, string, cy::Privacy::PotentiallyPersonal)
CY_TRACE_FIELD(refusal_reason, string, cy::Privacy::Public)

CY_TRACE_NAME(missing_asset_event, "assets.missing")
CY_TRACE_NAME(package_refused_event, "assets.package_refused")

/// The counters themselves. Relaxed atomics: a counter read one increment late is a counter, and an
/// acquire on every allocation would be a cost paid by the work rather than by the reporting.
struct Counters {
    std::atomic<u64> ids_minted{0};
    std::atomic<u64> meta_collisions{0};
    std::atomic<u64> placeholders_served{0};
    std::atomic<u64> path_rejections{0};
    std::atomic<u64> mount_resolutions{0};
    std::atomic<u64> mount_misses{0};
    std::atomic<u64> packages_opened{0};
    std::atomic<u64> packages_refused{0};
    std::atomic<u64> package_bytes_read{0};
    std::atomic<u64> entries_mapped{0};
    std::atomic<u64> integrity_failures{0};
    std::atomic<u64> bytes_compressed_in{0};
    std::atomic<u64> bytes_compressed_out{0};
    std::atomic<u64> bytes_decompressed{0};
    std::atomic<u64> frames_touched{0};
};

Counters& counters_storage() noexcept {
    static Counters storage;
    return storage;
}

void bump(std::atomic<u64>& counter, u64 amount = 1) noexcept {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

void emit_counter(const char* name, u64 value) noexcept {
    const diag::NameId id = diag::register_name(name);
    diag::trace_counter(id, category(), diag::Channel::Verbose, value);
}

}  // namespace

namespace counters {

void record_id_minted() noexcept {
    bump(counters_storage().ids_minted);
}
void record_meta_collision() noexcept {
    bump(counters_storage().meta_collisions);
}
void record_placeholder_served() noexcept {
    bump(counters_storage().placeholders_served);
}
void record_path_rejection() noexcept {
    bump(counters_storage().path_rejections);
}
void record_mount_resolution(bool hit) noexcept {
    bump(hit ? counters_storage().mount_resolutions : counters_storage().mount_misses);
}
void record_package_opened(bool refused) noexcept {
    bump(refused ? counters_storage().packages_refused : counters_storage().packages_opened);
}
void record_package_bytes(u64 bytes) noexcept {
    bump(counters_storage().package_bytes_read, bytes);
}
void record_entry_mapped() noexcept {
    bump(counters_storage().entries_mapped);
}
void record_integrity_failure() noexcept {
    bump(counters_storage().integrity_failures);
}
void record_compression(u64 input, u64 output) noexcept {
    bump(counters_storage().bytes_compressed_in, input);
    bump(counters_storage().bytes_compressed_out, output);
}
void record_decompression(u64 bytes, u64 frames) noexcept {
    bump(counters_storage().bytes_decompressed, bytes);
    bump(counters_storage().frames_touched, frames);
}
void reset() noexcept {
    Counters& storage = counters_storage();
    storage.ids_minted.store(0, std::memory_order_relaxed);
    storage.meta_collisions.store(0, std::memory_order_relaxed);
    storage.placeholders_served.store(0, std::memory_order_relaxed);
    storage.path_rejections.store(0, std::memory_order_relaxed);
    storage.mount_resolutions.store(0, std::memory_order_relaxed);
    storage.mount_misses.store(0, std::memory_order_relaxed);
    storage.packages_opened.store(0, std::memory_order_relaxed);
    storage.packages_refused.store(0, std::memory_order_relaxed);
    storage.package_bytes_read.store(0, std::memory_order_relaxed);
    storage.entries_mapped.store(0, std::memory_order_relaxed);
    storage.integrity_failures.store(0, std::memory_order_relaxed);
    storage.bytes_compressed_in.store(0, std::memory_order_relaxed);
    storage.bytes_compressed_out.store(0, std::memory_order_relaxed);
    storage.bytes_decompressed.store(0, std::memory_order_relaxed);
    storage.frames_touched.store(0, std::memory_order_relaxed);
}

}  // namespace counters

AssetDiagnostics assets_diagnostics() noexcept {
    const Counters& storage = counters_storage();
    AssetDiagnostics out;
    out.ids_minted = storage.ids_minted.load(std::memory_order_relaxed);
    out.meta_collisions = storage.meta_collisions.load(std::memory_order_relaxed);
    out.placeholders_served = storage.placeholders_served.load(std::memory_order_relaxed);
    out.path_rejections = storage.path_rejections.load(std::memory_order_relaxed);
    out.mount_resolutions = storage.mount_resolutions.load(std::memory_order_relaxed);
    out.mount_misses = storage.mount_misses.load(std::memory_order_relaxed);
    out.packages_opened = storage.packages_opened.load(std::memory_order_relaxed);
    out.packages_refused = storage.packages_refused.load(std::memory_order_relaxed);
    out.package_bytes_read = storage.package_bytes_read.load(std::memory_order_relaxed);
    out.entries_mapped = storage.entries_mapped.load(std::memory_order_relaxed);
    out.integrity_failures = storage.integrity_failures.load(std::memory_order_relaxed);
    out.bytes_compressed_in = storage.bytes_compressed_in.load(std::memory_order_relaxed);
    out.bytes_compressed_out = storage.bytes_compressed_out.load(std::memory_order_relaxed);
    out.bytes_decompressed = storage.bytes_decompressed.load(std::memory_order_relaxed);
    out.frames_touched = storage.frames_touched.load(std::memory_order_relaxed);
    return out;
}

void assets_trace_report() noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    const AssetDiagnostics snapshot = assets_diagnostics();

    emit_counter("assets.package_bytes_read", snapshot.package_bytes_read);
    emit_counter("assets.bytes_decompressed", snapshot.bytes_decompressed);
    emit_counter("assets.entries_mapped", snapshot.entries_mapped);
    emit_counter("assets.placeholders_served", snapshot.placeholders_served);
    emit_counter("assets.integrity_failures", snapshot.integrity_failures);

    const diag::FieldValue fields[] = {
        diag::field_u64(ids_minted(), snapshot.ids_minted),
        diag::field_u64(meta_collisions(), snapshot.meta_collisions),
        diag::field_u64(placeholders_served(), snapshot.placeholders_served),
        diag::field_u64(path_rejections(), snapshot.path_rejections),
        diag::field_u64(mount_resolutions(), snapshot.mount_resolutions),
        diag::field_u64(mount_misses(), snapshot.mount_misses),
        diag::field_u64(packages_opened(), snapshot.packages_opened),
        diag::field_u64(packages_refused(), snapshot.packages_refused),
        diag::field_u64(package_bytes_read(), snapshot.package_bytes_read),
        diag::field_u64(entries_mapped(), snapshot.entries_mapped),
        diag::field_u64(integrity_failures(), snapshot.integrity_failures),
        diag::field_u64(bytes_compressed_in(), snapshot.bytes_compressed_in),
        diag::field_u64(bytes_compressed_out(), snapshot.bytes_compressed_out),
        diag::field_u64(bytes_decompressed(), snapshot.bytes_decompressed),
        diag::field_u64(frames_touched(), snapshot.frames_touched),
    };
    diag::trace_instant(diag::register_name("assets.report"), category(), diag::Channel::Verbose,
                        fields, static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void assets_log_report() noexcept {
    const AssetDiagnostics snapshot = assets_diagnostics();
    // The message is an IDENTIFIER and the numbers are classified fields — that is the whole of the
    // M0 log model, and it is why a report is readable by a tool rather than only by a person.
    CY_LOG(category(), diag::LogLevel::Info, "assets.report",
           diag::field_u64(packages_opened(), snapshot.packages_opened),
           diag::field_u64(package_bytes_read(), snapshot.package_bytes_read),
           diag::field_u64(bytes_decompressed(), snapshot.bytes_decompressed),
           diag::field_u64(entries_mapped(), snapshot.entries_mapped),
           diag::field_u64(placeholders_served(), snapshot.placeholders_served),
           diag::field_u64(integrity_failures(), snapshot.integrity_failures));
}

void assets_log_missing_asset(cy::AssetId missing, cy::AssetId referrer, AssetKind kind) noexcept {
    counters::record_placeholder_served();

    char missing_text[cy::AssetId::kTextLength + 1] = {};
    (void)missing.format(missing_text);
    char referrer_text[cy::AssetId::kTextLength + 1] = {};
    (void)referrer.format(referrer_text);

    const char* kind_name = asset_kind_name(kind);
    CY_LOG(category(), diag::LogLevel::Warning, "assets.missing",
           diag::field_text(asset_id(), missing_text, cy::AssetId::kTextLength),
           diag::field_text(referrer_id(), referrer_text, cy::AssetId::kTextLength),
           diag::field_text(asset_kind(), kind_name, static_cast<u32>(std::strlen(kind_name))));

    if (diag::trace_is_open()) {
        const diag::FieldValue fields[] = {
            diag::field_text(asset_id(), missing_text, cy::AssetId::kTextLength),
            diag::field_text(referrer_id(), referrer_text, cy::AssetId::kTextLength),
            diag::field_text(asset_kind(), kind_name, static_cast<u32>(std::strlen(kind_name))),
        };
        diag::trace_instant(missing_asset_event(), category(), diag::Channel::Important, fields,
                            static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
    }
}

void assets_log_package_refused(const char* path, const char* reason) noexcept {
    counters::record_package_opened(true);

    // The path is a CLASSIFIED FIELD and the event name is a constant. A path in the name would be
    // outside the writer's redaction, which is the mistake this shape exists to prevent.
    const diag::FieldValue fields[] = {
        diag::field_text(package_path(), path, static_cast<u32>(std::strlen(path))),
        diag::field_text(refusal_reason(), reason, static_cast<u32>(std::strlen(reason))),
    };
    CY_LOG(category(), diag::LogLevel::Error, "assets.package_refused", fields[0], fields[1]);

    if (diag::trace_is_open()) {
        diag::trace_instant(package_refused_event(), category(), diag::Channel::Important, fields,
                            static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
    }
}

}  // namespace cy::assets
