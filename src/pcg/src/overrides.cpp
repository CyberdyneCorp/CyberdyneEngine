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

/// "No point in the set." Spelled as an unsigned constant rather than as `kNoPoint`,
/// because the cast form makes every comparison against it a signed/unsigned one.
inline constexpr usize kNoPoint = ~usize{0};

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
    return kNoPoint;
}

/// The nearest point to an override's spatial anchor, within `tolerance`. THE FALLBACK, and it is
/// reported as a re-anchor rather than as a bind, because it is a guess.
[[nodiscard]] usize find_anchor(const PointSet& points, f64 origin_x, f64 origin_z,
                                const Override& record, f32 tolerance, Span<const u8> taken,
                                f32& distance) noexcept {
    if (tolerance <= 0.0F) {
        return kNoPoint;
    }
    usize best = kNoPoint;
    f64 best_distance = static_cast<f64>(tolerance) * static_cast<f64>(tolerance);
    for (usize index = 0; index < points.size(); ++index) {
        // An instance another override BOUND BY IDENTITY is that override's, and a guess must not
        // reach for it: the identity is the strong claim and the anchor is the weak one.
        if (index < taken.size() && taken[index] != 0) {
            continue;
        }
        const f64 dx = origin_x + static_cast<f64>(points.x(index)) - record.anchor_x;
        const f64 dz = origin_z + static_cast<f64>(points.z(index)) - record.anchor_z;
        const f64 squared = (dx * dx) + (dz * dz);
        // Strictly nearer, ties broken by the candidate's SLOT, so the answer is a function of the
        // point set rather than of the order it happened to be built in.
        const bool nearer =
            squared < best_distance || (best != kNoPoint && squared == best_distance &&
                                        points.slot(index) < points.slot(best));
        if (nearer) {
            best_distance = squared;
            best = index;
        }
    }
    if (best != kNoPoint) {
        distance = static_cast<f32>(std::sqrt(best_distance));
    }
    return best;
}

/// The slot a point that did not come from a generator's sequence carries.
///
/// A slot is "the candidate's index in the generating node's OWN sequence, before any rejection",
/// and an instance the generator never produced has no such index. A sentinel says so, rather than
/// borrowing a number that would read as a position in a sequence this point was never in.
inline constexpr u32 kOverrideSlot = 0xFFFF'FFFFU;

/// Put an instance the base does not contain INTO the merged set: a hand-placed `Add`, or a `Lock`
/// whose target the generator has stopped producing.
///
/// The `Override` record is the whole of what such an instance is — its identity, where it sits,
/// and the variant column — which is why `anchor_*` is recorded for every override and not only for
/// a move. An instance materialised here therefore carries what the override knows about it and the
/// column defaults for everything else; a generator-derived column an editor wants preserved
/// through a lock has to be recorded in the override, and there is nowhere else it could come from.
[[nodiscard]] bool materialise(const Override& record, PointSet& points, f64 origin_x, f64 origin_z,
                               f64 x, f64 y, f64 z, usize& index) noexcept {
    const Expected<u32, Error> added =
        points.add(static_cast<f32>(x - origin_x), static_cast<f32>(y),
                   static_cast<f32>(z - origin_z), kOverrideSlot);
    if (!added) {
        return false;
    }
    index = *added;
    points.set_identity(index, record.target.value);
    if (record.attribute.is_valid()) {
        points.set_i32(record.attribute, index, static_cast<i32>(record.variant));
    }
    return true;
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

/// What one override resolved to, decided BEFORE anything is applied.
///
/// Two phases and not one, and that is the M10 gate's finding rather than a tidier shape: `merge()`
/// used to bind and apply one override at a time, so a `Move` relocated the very point a later
/// spatial re-anchor measured its distance to, and the same two overrides placed in the other order
/// produced a different world. Resolving every override against the point set AS THE REGENERATION
/// LEFT IT makes the result a function of the overrides rather than of the order a designer wrote
/// them in — which is the write-order failure `environment-fields` refuses one layer down.
struct Resolution {
    usize point = kNoPoint;
    BindResult result = BindResult::Orphaned;
    GeneratedId reanchored_to;
    f32 distance_metres = 0.0F;
    /// The candidate a spatial search picked, before contention is settled.
    usize guess = kNoPoint;
    bool materialise = false;
};

/// Phase one: the identity binds, and the claim each one puts on its instance.
[[nodiscard]] Status plan_identity_binds(Span<const Override> overrides, const PointSet& points,
                                         Array<Resolution>& plan, Array<u8>& taken) noexcept {
    for (const Override& record : overrides) {
        Resolution resolution;
        if (record.op == OverrideOp::kCount) {
            // Removed. A tombstone keeps the index stable; see `OverrideLayer::remove()`.
        } else if (record.op == OverrideOp::Add) {
            resolution.materialise = true;
            resolution.result = BindResult::Bound;
        } else {
            const usize found = find_identity(points, record.target);
            if (found != kNoPoint) {
                resolution.point = found;
                resolution.result = BindResult::Bound;
                taken[found] = 1;
            }
        }
        if (Status pushed = plan.push_back(resolution); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Phase two: the spatial fallback, over the point set as it stood, and CONTENTION SETTLED BY
/// GIVING THE INSTANCE TO NOBODY.
///
/// A guess two overrides both want is not a guess worth making: one of them would be moving another
/// designer's tree, and which one depends on nothing anybody wrote down. Both become orphans, which
/// is visible, rather than one becoming a silent mis-bind. It is also what makes the outcome
/// independent of the order the overrides were placed in, with no ordering rule to invent.
void plan_spatial_fallbacks(Span<const Override> overrides, const PointSet& points, f64 origin_x,
                            f64 origin_z, f32 tolerance, u32 current_version,
                            Array<Resolution>& plan, Span<const u8> taken) noexcept {
    for (usize index = 0; index < plan.size(); ++index) {
        const Override& record = overrides[index];
        if (record.op == OverrideOp::kCount || plan[index].materialise ||
            plan[index].result == BindResult::Bound) {
            continue;
        }
        // Identity first, spatial anchor second. The fallback is only offered when the generator
        // version moved: within one version a missing identity means the instance is genuinely gone
        // — the spike's 235 LOST overrides of 7 877 — and re-anchoring those would convert a
        // visible loss into an invisible mis-bind, which is the one outcome §1.4 designs against.
        if (record.written_against_version == current_version) {
            continue;
        }
        f32 distance = 0.0F;
        const usize anchored =
            find_anchor(points, origin_x, origin_z, record, tolerance, taken, distance);
        plan[index].guess = anchored;
        plan[index].distance_metres = distance;
    }

    for (usize index = 0; index < plan.size(); ++index) {
        if (plan[index].guess == kNoPoint) {
            continue;
        }
        bool contested = false;
        for (usize other = 0; other < plan.size() && !contested; ++other) {
            contested = other != index && plan[other].guess == plan[index].guess;
        }
        if (contested) {
            continue;  // orphaned, and reported as such: nobody takes a contested instance
        }
        plan[index].point = plan[index].guess;
        plan[index].result = BindResult::Reanchored;
        plan[index].reanchored_to = GeneratedId{points.identity(plan[index].guess)};
    }
}

/// How many instances the merged set must hold that the regenerated one does not: a hand-placed
/// `Add`, and a `Lock` whose target the generator has stopped producing.
[[nodiscard]] usize count_materialisations(Span<const Override> overrides,
                                           const PointSet& base) noexcept {
    usize extras = 0;
    for (const Override& record : overrides) {
        if (record.op == OverrideOp::Add ||
            (record.op == OverrideOp::Lock && find_identity(base, record.target) == kNoPoint)) {
            ++extras;
        }
    }
    return extras;
}

/// Phase three: one resolved override, put into effect. Separate from the loop that drives it so
/// that `merge()` reads as its three phases rather than as all of them at once.
[[nodiscard]] Status apply_resolved(const Override& record, Resolution& resolution, f64 origin_x,
                                    f64 origin_z, MergeResult& result, Array<u8>& keep) noexcept {
    if (record.op == OverrideOp::Lock && resolution.result == BindResult::Orphaned) {
        // "Locked instances SHALL be preserved through regeneration." A lock is the designer saying
        // KEEP THIS whatever the rules now say, so a regeneration that stopped producing the
        // instance is the case the lock exists for rather than the case it gives up on. Reporting
        // it as an orphan — which is what this did before the M10 gate's adversarial pass — is a
        // correct report of an incorrect outcome: the tree is gone.
        resolution.materialise = true;
        resolution.result = BindResult::Bound;
    }
    if (resolution.materialise) {
        // An added or preserved instance is not looked for in the base: it is not there by
        // definition. It is PUT IN and not merely counted — `generated base + author overrides =
        // authored result` makes the designer's tree part of the result, and a merge that counted
        // it and dropped it would be deleting a designer's work while reporting that it bound.
        const bool is_add = record.op == OverrideOp::Add;
        const f64 x = is_add ? record.x : record.anchor_x;
        const f64 y = is_add ? record.y : record.anchor_y;
        const f64 z = is_add ? record.z : record.anchor_z;
        if (!materialise(record, result.points, origin_x, origin_z, x, y, z, resolution.point)) {
            return Status{make_unexpected(
                Error{ErrorCode::OutOfRange,
                      "pcg: no room to materialise an instance the generator did not produce"})};
        }
        result.added += is_add ? 1U : 0U;
    }

    BindReport report;
    report.target = record.target;
    report.op = record.op;
    report.origin = record.origin;
    report.result = resolution.result;
    report.reanchored_to = resolution.reanchored_to;
    report.distance_metres =
        resolution.result == BindResult::Reanchored ? resolution.distance_metres : 0.0F;
    if (Status pushed = result.reports.push_back(report); !pushed) {
        return pushed;
    }
    if (resolution.result == BindResult::Orphaned) {
        // RETAINED AND REPORTED, never discarded: "Regenerating a region must not quietly delete a
        // designer's work."
        ++result.orphaned;
        return ok();
    }
    result.bound += resolution.result == BindResult::Bound ? 1U : 0U;
    result.reanchored += resolution.result == BindResult::Reanchored ? 1U : 0U;
    if (!apply_override(record, result.points, resolution.point, origin_x, origin_z, result)) {
        keep[resolution.point] = 0;
    }
    return ok();
}

}  // namespace

Expected<MergeResult, Error> OverrideLayer::merge(const PointSet& base, f64 origin_x, f64 origin_z,
                                                  f32 tolerance_metres,
                                                  u32 current_version) const noexcept {
    MergeResult result(*allocator_);

    // ROOM FOR THE INSTANCES THE BASE DOES NOT HOLD, counted before anything is copied. A point set
    // refuses to grow past its reserved capacity on purpose — that refusal is what makes "no
    // per-point heap allocation" checkable — so the one caller that legitimately adds to a finished
    // set asks for the room up front.
    const usize extras = count_materialisations(overrides_.span(), base);
    Expected<PointSet, Error> merged = base.clone(extras);
    if (!merged) {
        return make_unexpected(merged.error());
    }
    result.points = std::move(*merged);

    Array<u8> keep(*allocator_);
    if (Status sized = keep.resize(result.points.size() + extras); !sized) {
        return make_unexpected(sized.error());
    }
    for (u8& value : keep) {
        value = 1;
    }
    Array<u8> taken(*allocator_);
    if (Status sized = taken.resize(result.points.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u8& value : taken) {
        value = 0;
    }

    // --- RESOLVE EVERY OVERRIDE FIRST, APPLY AFTERWARDS. See `Resolution` above for why.
    Array<Resolution> plan(*allocator_);
    if (Status reserved = plan.reserve(overrides_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status planned = plan_identity_binds(overrides_.span(), result.points, plan, taken);
        !planned) {
        return make_unexpected(planned.error());
    }
    plan_spatial_fallbacks(overrides_.span(), result.points, origin_x, origin_z, tolerance_metres,
                           current_version, plan, taken.span());

    for (usize index = 0; index < plan.size(); ++index) {
        if (overrides_[index].op == OverrideOp::kCount) {
            continue;  // removed
        }
        if (Status applied =
                apply_resolved(overrides_[index], plan[index], origin_x, origin_z, result, keep);
            !applied) {
            return make_unexpected(applied.error());
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
                                 (sizeof(u8) * 2) +          // op, origin
                                 (sizeof(f64) * 6) +         // anchor and position
                                 (sizeof(u32) * 2) +         // variant, version
                                 sizeof(u16) + sizeof(f32);  // attribute, value
    return kHeaderBytes + (kRecordBytes * exceptions.size());
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
