// The override layer, orphans and the persistent delta. See include/cy/pcg/overrides.h.

#include <cy/pcg/overrides.h>

#include <cmath>
#include <utility>

namespace cy::pcg {

const char* override_op_name(OverrideOp op) noexcept {
    switch (op) {
        case OverrideOp::Delete:
            return "delete";
        case OverrideOp::Move:
            return "move";
        case OverrideOp::Replace:
            return "replace";
        case OverrideOp::Add:
            return "add";
        case OverrideOp::Lock:
            return "lock";
        case OverrideOp::SetAttribute:
            return "set-attribute";
        case OverrideOp::kCount:
            break;
    }
    return "unknown";
}

const char* bind_result_name(BindResult result) noexcept {
    switch (result) {
        case BindResult::Bound:
            return "bound";
        case BindResult::Reanchored:
            return "reanchored";
        case BindResult::Orphaned:
            return "orphaned";
    }
    return "unknown";
}

namespace {

[[nodiscard]] u64 override_key(GeneratedId target, OverrideOp op) noexcept {
    // The op participates: a designer who both moved and locked a tree has written two overrides of
    // one instance, and a key on the identity alone would make the second replace the first.
    return target.value ^ (static_cast<u64>(op) * 0x9e3779b97f4a7c15ULL);
}

/// Find `target` in a point set. Linear, because a region holds hundreds of points and the map a
/// binary search would need is a second index to keep in step with the point set's own order.
[[nodiscard]] usize find_identity(const PointSet& points, GeneratedId target) noexcept {
    for (usize index = 0; index < points.size(); ++index) {
        if (points.identity(index) == target.value) {
            return index;
        }
    }
    return static_cast<usize>(-1);
}

/// The nearest point to an override's spatial anchor, within `tolerance`. THE FALLBACK, and it is
/// reported as a re-anchor rather than as a bind, because it is a guess.
[[nodiscard]] usize find_anchor(const PointSet& points, f64 origin_x, f64 origin_z,
                                const Override& record, f32 tolerance, f32& distance) noexcept {
    if (tolerance <= 0.0F) {
        return static_cast<usize>(-1);
    }
    usize best = static_cast<usize>(-1);
    f64 best_distance = static_cast<f64>(tolerance) * static_cast<f64>(tolerance);
    for (usize index = 0; index < points.size(); ++index) {
        const f64 dx = origin_x + static_cast<f64>(points.x(index)) - record.anchor_x;
        const f64 dz = origin_z + static_cast<f64>(points.z(index)) - record.anchor_z;
        const f64 squared = dx * dx + dz * dz;
        // Strictly nearer, ties broken by the candidate's SLOT, so the answer is a function of the
        // point set rather than of the order it happened to be built in.
        const bool nearer = squared < best_distance ||
                            (best != static_cast<usize>(-1) && squared == best_distance &&
                             points.slot(index) < points.slot(best));
        if (nearer) {
            best_distance = squared;
            best = index;
        }
    }
    if (best != static_cast<usize>(-1)) {
        distance = static_cast<f32>(std::sqrt(best_distance));
    }
    return best;
}

/// Apply one bound override to the merged set. Returns whether the point survives.
[[nodiscard]] bool apply_override(const Override& record, PointSet& points, usize index,
                                  f64 origin_x, f64 origin_z, MergeResult& result) noexcept {
    switch (record.op) {
        case OverrideOp::Delete:
            ++result.deleted;
            return false;
        case OverrideOp::Move:
            // A moved point keeps its identity AND its slot: the instance did not become a
            // different instance by being dragged, which is the whole content of "the override
            // SHALL reattach by identity and the tree SHALL remain where it was placed". The
            // override's position is absolute world metres and the point set's is region-local, so
            // the origin is subtracted here rather than left for every consumer to remember.
            points.set_position(index, static_cast<f32>(record.x - origin_x),
                                static_cast<f32>(record.y), static_cast<f32>(record.z - origin_z));
            return true;
        case OverrideOp::Replace:
            points.set_i32(record.attribute, index, static_cast<i32>(record.variant));
            return true;
        case OverrideOp::SetAttribute:
            points.set_f32(record.attribute, index, record.value);
            return true;
        case OverrideOp::Lock:
            ++result.locked;
            return true;
        case OverrideOp::Add:
        case OverrideOp::kCount:
            return true;
    }
    return true;
}

}  // namespace

Status OverrideLayer::place(const Override& record) noexcept {
    const u64 key = override_key(record.target, record.op);
    if (const usize* existing = index_.find(key); existing != nullptr) {
        overrides_[*existing] = record;
        return ok();
    }
    if (Status pushed = overrides_.push_back(record); !pushed) {
        return pushed;
    }
    const usize index = overrides_.size() - 1;
    if (Expected<usize*, Error> inserted = index_.insert(key, index); !inserted) {
        return Status{make_unexpected(inserted.error())};
    }
    return ok();
}

bool OverrideLayer::remove(GeneratedId target, OverrideOp op) noexcept {
    const u64 key = override_key(target, op);
    const usize* existing = index_.find(key);
    if (existing == nullptr) {
        return false;
    }
    // The record is blanked rather than erased: erasing would move another override into its slot
    // and invalidate the index, and an override layer is small enough that a tombstone costs less
    // than the reindex.
    overrides_[*existing].op = OverrideOp::kCount;
    return true;
}

const Override* OverrideLayer::find(GeneratedId target, OverrideOp op) const noexcept {
    const usize* existing = index_.find(override_key(target, op));
    return existing != nullptr ? &overrides_[*existing] : nullptr;
}

namespace {

/// Merge one override into the base, reporting how it bound. Split out of `merge()` so that the
/// bind-then-fall-back-then-orphan decision reads as three cases rather than as nesting.
[[nodiscard]] BindReport bind_one(const Override& record, PointSet& points, f64 origin_x,
                                  f64 origin_z, f32 tolerance, u32 current_version,
                                  usize& index) noexcept {
    BindReport report;
    report.target = record.target;
    report.op = record.op;
    report.origin = record.origin;

    index = find_identity(points, record.target);
    if (index != static_cast<usize>(-1)) {
        report.result = BindResult::Bound;
        return report;
    }
    // Identity first, spatial anchor second. The fallback is only offered when the generator
    // version moved: within one version a missing identity means the instance is genuinely gone —
    // the spike's 235 LOST overrides of 7 877 — and re-anchoring those would convert a visible loss
    // into an invisible mis-bind, which is the one outcome §1.4 says to design against.
    if (record.written_against_version != current_version) {
        f32 distance = 0.0F;
        const usize anchored = find_anchor(points, origin_x, origin_z, record, tolerance, distance);
        if (anchored != static_cast<usize>(-1)) {
            index = anchored;
            report.result = BindResult::Reanchored;
            report.reanchored_to = GeneratedId{points.identity(anchored)};
            report.distance_metres = distance;
            return report;
        }
    }
    report.result = BindResult::Orphaned;
    return report;
}

}  // namespace

Expected<MergeResult, Error> OverrideLayer::merge(const PointSet& base, f64 origin_x, f64 origin_z,
                                                  f32 tolerance_metres,
                                                  u32 current_version) const noexcept {
    MergeResult result(*allocator_);
    Expected<PointSet, Error> merged = base.clone();
    if (!merged) {
        return make_unexpected(merged.error());
    }
    result.points = std::move(*merged);

    Array<u8> keep(*allocator_);
    if (Status sized = keep.resize(result.points.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u8& value : keep) {
        value = 1;
    }

    for (const Override& record : overrides_) {
        if (record.op == OverrideOp::kCount) {
            continue;  // removed
        }
        if (record.op == OverrideOp::Add) {
            // An added instance is not looked for in the base: it is not there by definition. Its
            // identity was minted by the override layer and can never collide with a generated one.
            ++result.added;
            BindReport report;
            report.target = record.target;
            report.op = record.op;
            report.origin = record.origin;
            report.result = BindResult::Bound;
            if (Status pushed = result.reports.push_back(report); !pushed) {
                return make_unexpected(pushed.error());
            }
            ++result.bound;
            continue;
        }
        usize index = 0;
        const BindReport report = bind_one(record, result.points, origin_x, origin_z,
                                           tolerance_metres, current_version, index);
        if (Status pushed = result.reports.push_back(report); !pushed) {
            return make_unexpected(pushed.error());
        }
        switch (report.result) {
            case BindResult::Bound:
                ++result.bound;
                break;
            case BindResult::Reanchored:
                ++result.reanchored;
                break;
            case BindResult::Orphaned:
                // RETAINED AND REPORTED, never discarded: "Regenerating a region must not quietly
                // delete a designer's work."
                ++result.orphaned;
                continue;
        }
        if (!apply_override(record, result.points, index, origin_x, origin_z, result)) {
            keep[index] = 0;
        }
    }
    if (Status retained = result.points.retain(Span<const u8>(keep.data(), result.points.size()));
        !retained) {
        return make_unexpected(retained.error());
    }
    return result;
}

Status OverrideLayer::refresh_orphans(const MergeResult& result) noexcept {
    orphans_.clear();
    for (const BindReport& report : result.reports) {
        if (report.result != BindResult::Orphaned) {
            continue;
        }
        if (Status pushed = orphans_.push_back(report); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Persistence ------------------------------------------------------------------------------

u64 PersistentDelta::encoded_bytes() const noexcept {
    // The header, then one fixed record per exception. A function of the EXCEPTION COUNT and of
    // nothing about the world the exceptions are in — which is the mechanical content of "WHEN a
    // player fells two hundred trees in a generated forest of a million THEN the save SHALL record
    // two hundred exceptions".
    constexpr u64 kHeaderBytes = sizeof(u64) + sizeof(u32) + sizeof(u64);
    constexpr u64 kRecordBytes = sizeof(u64) +               // identity
                                 sizeof(u8) * 2 +            // op, origin
                                 sizeof(f64) * 6 +           // anchor and position
                                 sizeof(u32) * 2 +           // variant, version
                                 sizeof(u16) + sizeof(f32);  // attribute, value
    return kHeaderBytes + kRecordBytes * exceptions.size();
}

Status collect_persistent(const OverrideLayer& layer, u64 seed, u32 version, u64 program_digest,
                          PersistentDelta& out) noexcept {
    out.seed = seed;
    out.generator_version = version;
    out.program_digest = program_digest;
    out.exceptions.clear();
    for (const Override& record : layer.records()) {
        // GAMEPLAY exceptions only. An authored override is project content and travels with the
        // project; the generated base is in neither. "Generated base content SHALL NOT be saved."
        if (record.origin != OverrideOrigin::Gameplay || record.op == OverrideOp::kCount) {
            continue;
        }
        if (Status pushed = out.exceptions.push_back(record); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Expected<MergeResult, Error> re_resolve(Allocator& allocator, const PersistentDelta& delta,
                                        const PointSet& regenerated, f64 origin_x, f64 origin_z,
                                        u32 current_version, f32 tolerance_metres) noexcept {
    OverrideLayer layer(allocator);
    for (const Override& record : delta.exceptions) {
        if (Status placed = layer.place(record); !placed) {
            return make_unexpected(placed.error());
        }
    }
    // A DIFFERENT current version is what opens the spatial fallback. Under the same version the
    // merge takes the identity path alone and reports a missing instance as an orphan, which is
    // correct: within one version a missing identity is a removal, not a rename.
    return layer.merge(regenerated, origin_x, origin_z, tolerance_metres, current_version);
}

}  // namespace cy::pcg
