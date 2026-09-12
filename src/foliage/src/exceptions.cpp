// Exceptions, their two anchors, re-resolution and the overlay. See exceptions.h for why neither
// anchor alone is enough and why an ambiguous spatial match is an orphan rather than a coin toss.

#include <cy/foliage/exceptions.h>

#include <cmath>
#include <cstring>

namespace cy::foliage {

const char* exception_kind_name(ExceptionKind kind) noexcept {
    switch (kind) {
        case ExceptionKind::Removed:
            return "removed";
        case ExceptionKind::Moved:
            return "moved";
        case ExceptionKind::Added:
            return "added";
        case ExceptionKind::Modified:
            return "modified";
    }
    return "unknown";
}

bool authored_by_default(ExceptionKind kind) noexcept {
    // An author adds and moves; gameplay removes and modifies. It is only a DEFAULT — a level
    // designer deleting a tree and a player felling one produce the same kind — which is why
    // `FoliageException::authored` is what actually decides.
    return kind == ExceptionKind::Added || kind == ExceptionKind::Moved;
}

const char* resolution_outcome_name(ResolutionOutcome outcome) noexcept {
    switch (outcome) {
        case ResolutionOutcome::BoundByIdentity:
            return "bound-by-identity";
        case ResolutionOutcome::BoundBySpace:
            return "bound-by-space";
        case ResolutionOutcome::Standalone:
            return "standalone";
        case ResolutionOutcome::Orphaned:
            return "orphaned";
        case ResolutionOutcome::Ambiguous:
            return "ambiguous";
    }
    return "unknown";
}

ExceptionStore::ExceptionStore(Allocator& allocator) noexcept
    : allocator_(&allocator), exceptions_(allocator), index_(allocator) {}

Expected<ExceptionStore::Bucket*, Error> ExceptionStore::bucket_for(ClusterId cluster) noexcept {
    if (usize* slot = index_.find(cluster.value); slot != nullptr) {
        return &exceptions_[*slot];
    }
    Bucket bucket(*allocator_);
    bucket.cluster = cluster;
    if (Status pushed = exceptions_.push_back(static_cast<Bucket&&>(bucket)); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Expected<usize*, Error> placed = index_.insert(cluster.value, exceptions_.size() - 1);
        !placed) {
        exceptions_.pop_back();
        return fail(placed.error().code, placed.error().message);
    }
    return &exceptions_.back();
}

const ExceptionStore::Bucket* ExceptionStore::find_bucket(ClusterId cluster) const noexcept {
    const usize* slot = index_.find(cluster.value);
    return slot == nullptr ? nullptr : &exceptions_[*slot];
}

Status ExceptionStore::record(const FoliageException& exception) noexcept {
    Expected<Bucket*, Error> bucket = bucket_for(exception.cluster);
    if (!bucket) {
        return make_unexpected(bucket.error());
    }
    for (FoliageException& existing : bucket.value()->items) {
        if (existing.kind == exception.kind && existing.identity == exception.identity) {
            // Replacing rather than appending is what makes "fell the same tree twice" one
            // exception, and what stops a replay that re-applies an event from growing the save.
            if (exception.sequence >= existing.sequence) {
                existing = exception;
            }
            return ok();
        }
    }
    return bucket.value()->items.push_back(exception);
}

Span<const FoliageException> ExceptionStore::of_cluster(ClusterId cluster) const noexcept {
    const Bucket* bucket = find_bucket(cluster);
    return bucket == nullptr ? Span<const FoliageException>() : bucket->items.span();
}

usize ExceptionStore::size() const noexcept {
    usize total = 0;
    for (const Bucket& bucket : exceptions_) {
        total += bucket.items.size();
    }
    return total;
}

u64 ExceptionStore::bytes() const noexcept {
    u64 total = 0;
    for (const Bucket& bucket : exceptions_) {
        total += static_cast<u64>(bucket.items.size()) * sizeof(FoliageException);
    }
    return total;
}

namespace {

[[nodiscard]] f64 planar_distance(const world::WorldVec3d& a, const world::WorldVec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

/// The spatial fallback. Constrained three ways at once — species, radius and UNIQUENESS — because
/// any two of them alone is what mis-binds; see exceptions.h.
[[nodiscard]] Resolution resolve_spatially(const FoliageException& exception,
                                           const FoliageCluster& cluster,
                                           const ResolutionPolicy& policy) noexcept {
    Resolution resolution;
    if (!policy.allow_spatial) {
        resolution.outcome = ResolutionOutcome::Orphaned;
        return resolution;
    }
    f64 best = static_cast<f64>(policy.radius_metres);
    f64 runner_up = best;
    u32 best_slot = FoliageCluster::kNoSlot;
    for (u32 slot = 0; slot < static_cast<u32>(cluster.size()); ++slot) {
        const FoliageInstance* instance = cluster.at(slot);
        if (instance == nullptr) {
            continue;
        }
        if (!(cluster.species_at(instance->species_slot) == exception.species)) {
            continue;
        }
        const f64 distance =
            planar_distance(exception.position, cluster.bounds().decode(*instance));
        if (distance < best) {
            runner_up = best;
            best = distance;
            best_slot = slot;
        } else if (distance < runner_up) {
            runner_up = distance;
        }
    }
    if (best_slot == FoliageCluster::kNoSlot) {
        resolution.outcome = ResolutionOutcome::Orphaned;
        return resolution;
    }
    if (runner_up - best < static_cast<f64>(policy.ambiguity_metres)) {
        // Two equally good candidates. An orphan an author can fix, not a coin toss nobody finds.
        resolution.outcome = ResolutionOutcome::Ambiguous;
        return resolution;
    }
    resolution.outcome = ResolutionOutcome::BoundBySpace;
    resolution.slot = best_slot;
    resolution.drift_metres = static_cast<f32>(best);
    return resolution;
}

}  // namespace

Expected<ResolutionReport, Error> ExceptionStore::resolve(u64 seed, ClusterId old_cluster,
                                                          const FoliageCluster& regenerated,
                                                          const ResolutionPolicy& policy,
                                                          Array<Resolution>& out) const noexcept {
    ResolutionReport report;
    const Span<const FoliageException> items = of_cluster(old_cluster);
    for (const FoliageException& exception : items) {
        ++report.examined;
        Resolution resolution;
        if (exception.kind == ExceptionKind::Added) {
            // An added instance names no generated one. It is applied by the builder before the
            // cluster is finished, and there is nothing here for it to bind to.
            resolution.outcome = ResolutionOutcome::Standalone;
            ++report.standalone;
        } else if (const u32 slot = regenerated.slot_of(seed, exception.identity);
                   slot != FoliageCluster::kNoSlot) {
            resolution.outcome = ResolutionOutcome::BoundByIdentity;
            resolution.slot = slot;
            ++report.bound_by_identity;
        } else {
            resolution = resolve_spatially(exception, regenerated, policy);
            switch (resolution.outcome) {
                case ResolutionOutcome::BoundBySpace:
                    ++report.bound_by_space;
                    break;
                case ResolutionOutcome::Ambiguous:
                    ++report.ambiguous;
                    break;
                default:
                    ++report.orphaned;
                    break;
            }
        }
        if (Status pushed = out.push_back(resolution); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return report;
}

Expected<u32, Error> ExceptionStore::apply(ClusterId old_cluster, Span<const Resolution> resolved,
                                           FoliageCluster& cluster) const noexcept {
    const Span<const FoliageException> items = of_cluster(old_cluster);
    if (resolved.size() != items.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "one resolution per exception is required, in of_cluster() order");
    }
    u32 applied = 0;
    for (usize index = 0; index < items.size(); ++index) {
        const Resolution& resolution = resolved[index];
        if (resolution.slot == FoliageCluster::kNoSlot) {
            continue;
        }
        const FoliageException& exception = items[index];
        const FoliageInstance* instance = cluster.at(resolution.slot);
        if (instance == nullptr) {
            continue;
        }
        InstanceFlags flags = instance->flags;
        switch (exception.kind) {
            case ExceptionKind::Removed:
                flags.set(InstanceFlags::kRemoved);
                flags.set(InstanceFlags::kException);
                break;
            case ExceptionKind::Modified:
                flags.bits = static_cast<u8>(flags.bits | exception.state.bits);
                flags.set(InstanceFlags::kException);
                break;
            case ExceptionKind::Moved: {
                // The stored record replaces the generated one wholesale, keeping the species slot
                // the regenerated cluster assigned — the exception's own slot index is meaningless
                // in a cluster it was not built against, and `replace_instance()` refuses a change
                // of species for the reason it gives.
                FoliageInstance moved = exception.instance;
                moved.species_slot = instance->species_slot;
                moved.flags.set(InstanceFlags::kException);
                if (Status replaced = cluster.replace_instance(resolution.slot, moved); !replaced) {
                    return make_unexpected(replaced.error());
                }
                ++applied;
                continue;
            }
            case ExceptionKind::Added:
                continue;
        }
        if (Status set = cluster.set_flags(resolution.slot, flags); !set) {
            return make_unexpected(set.error());
        }
        ++applied;
    }
    return applied;
}

Status ExceptionStore::orphans(ClusterId cluster, Span<const Resolution> resolved,
                               Array<FoliageException>& out) const noexcept {
    const Span<const FoliageException> items = of_cluster(cluster);
    if (resolved.size() != items.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "one resolution per exception is required, in of_cluster() order");
    }
    for (usize index = 0; index < items.size(); ++index) {
        const ResolutionOutcome outcome = resolved[index].outcome;
        if (outcome == ResolutionOutcome::Orphaned || outcome == ResolutionOutcome::Ambiguous) {
            if (Status pushed = out.push_back(items[index]); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

// --- Persistence
// ----------------------------------------------------------------------------------

namespace {

/// The blob's own header. A version, because a save written by an older engine must be REPORTED
/// rather than reinterpreted — `world::PersistenceOverlay` does not interpret the bytes, so the
/// check has to be here.
struct BlobHeader {
    u32 magic = 0x464C4732U;  // "FLG2"
    u32 version = 1;
    u32 count = 0;
    u32 record_bytes = static_cast<u32>(sizeof(FoliageException));
};

}  // namespace

Expected<u32, Error> ExceptionStore::write_overlay(world::PersistenceOverlay& overlay,
                                                   world::CellId cell,
                                                   ClusterId cluster) const noexcept {
    const Bucket* bucket = find_bucket(cluster);
    if (bucket == nullptr) {
        return 0U;
    }
    Array<u8> bytes(*allocator_);
    BlobHeader header;
    u32 written = 0;
    for (const FoliageException& exception : bucket->items) {
        if (exception.authored) {
            continue;  // Cooked. Writing it would grow every save by the whole authored set.
        }
        ++written;
    }
    if (written == 0) {
        return 0U;
    }
    header.count = written;
    if (Status sized = bytes.resize(sizeof(BlobHeader) +
                                    (static_cast<usize>(written) * sizeof(FoliageException)));
        !sized) {
        return make_unexpected(sized.error());
    }
    std::memcpy(bytes.data(), &header, sizeof(BlobHeader));
    usize offset = sizeof(BlobHeader);
    for (const FoliageException& exception : bucket->items) {
        if (exception.authored) {
            continue;
        }
        std::memcpy(bytes.data() + offset, &exception, sizeof(FoliageException));
        offset += sizeof(FoliageException);
    }
    if (Status recorded =
            overlay.record_blob(cell, kOverlayChannelFoliage, cluster.value, bytes.span());
        !recorded) {
        return make_unexpected(recorded.error());
    }
    return written;
}

Expected<u32, Error> ExceptionStore::read_overlay(const world::PersistenceOverlay& overlay,
                                                  world::CellId cell, ClusterId cluster) noexcept {
    const Span<const u8> bytes = overlay.blob(cell, kOverlayChannelFoliage, cluster.value);
    if (bytes.size() < sizeof(BlobHeader)) {
        return 0U;
    }
    BlobHeader header;
    std::memcpy(&header, bytes.data(), sizeof(BlobHeader));
    if (header.magic != BlobHeader{}.magic || header.version != BlobHeader{}.version ||
        header.record_bytes != sizeof(FoliageException)) {
        return fail(ErrorCode::InvalidArgument,
                    "foliage overlay blob was written by a different engine version");
    }
    const usize needed =
        sizeof(BlobHeader) + (static_cast<usize>(header.count) * sizeof(FoliageException));
    if (bytes.size() < needed) {
        return fail(ErrorCode::InvalidArgument, "foliage overlay blob is truncated");
    }
    u32 read = 0;
    for (u32 index = 0; index < header.count; ++index) {
        FoliageException exception;
        std::memcpy(&exception,
                    bytes.data() + sizeof(BlobHeader) +
                        (static_cast<usize>(index) * sizeof(FoliageException)),
                    sizeof(FoliageException));
        if (Status recorded = record(exception); !recorded) {
            return make_unexpected(recorded.error());
        }
        ++read;
    }
    return read;
}

Status ExceptionStore::clusters(Array<ClusterId>& out) const noexcept {
    for (const Bucket& bucket : exceptions_) {
        if (Status pushed = out.push_back(bucket.cluster); !pushed) {
            return pushed;
        }
    }
    // Sorted, because a SAVE must be the same bytes for the same state however that state was
    // reached — `world::PersistenceOverlay::cells()` sorts for exactly this reason.
    for (usize index = 1; index < out.size(); ++index) {
        const ClusterId key = out[index];
        usize hole = index;
        while (hole > 0 && key < out[hole - 1]) {
            out[hole] = out[hole - 1];
            --hole;
        }
        out[hole] = key;
    }
    return ok();
}

Expected<u32, Error> apply_added(const ExceptionStore& store, ClusterId cluster,
                                 const ClusterBounds& bounds, ClusterBuilder& builder) noexcept {
    u32 added = 0;
    for (const FoliageException& exception : store.of_cluster(cluster)) {
        if (exception.kind != ExceptionKind::Added) {
            continue;
        }
        InstanceFlags flags = exception.instance.flags;
        flags.set(InstanceFlags::kException);
        if (exception.authored) {
            flags.set(InstanceFlags::kPainted);
        }
        const world::WorldVec3d position = bounds.decode(exception.instance);
        // The stored record's own quantised fields are re-encoded against THIS cluster's bounds,
        // because a cluster regenerated over different terrain has a different vertical extent and
        // a raw copy would move the instance vertically.
        const f32 yaw = static_cast<f32>(exception.instance.yaw) * (6.283185307F / 65536.0F);
        const f32 scale = static_cast<f32>(exception.instance.scale) * (1.0F / 65535.0F);
        if (Status pushed =
                builder.add(exception.species, position, yaw, scale, exception.instance.variation,
                            exception.instance.age, 0.0F, 0.0F, flags);
            !pushed) {
            return make_unexpected(pushed.error());
        }
        ++added;
    }
    return added;
}

}  // namespace cy::foliage
