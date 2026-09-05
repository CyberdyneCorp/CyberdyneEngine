// The camera stack: entries, blends, and the contribution report.
// See cy/servers/camera/stack.h.

#include <cy/servers/camera/stack.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

namespace cy::camera {
namespace {

/// Sort key: priority first, then push order. See stack.h's note on determinism.
[[nodiscard]] bool orders_before(i32 priority_a, u32 sequence_a, i32 priority_b,
                                 u32 sequence_b) noexcept {
    if (priority_a != priority_b) {
        return priority_a < priority_b;
    }
    return sequence_a < sequence_b;
}

/// Blend `addition` into `accumulated` at `weight`, channel by channel.
void blend_channels(EvaluatedCamera& accumulated, const EvaluatedCamera& addition, f32 weight,
                    const BlendPolicy& policy, ContributionKind kind) noexcept {
    if (kind == ContributionKind::Additive) {
        // ADDITIVE IS THE ONE KIND THAT IS NOT A BLEND. Recoil and sway are offsets over whatever
        // is below them; blending toward them would REPLACE the base pose with a small offset from
        // the origin, which is a camera at the world origin whenever a weapon fires.
        if (policy.position) {
            accumulated.pose.translation =
                accumulated.pose.translation + (addition.pose.translation * weight);
        }
        if (policy.rotation) {
            accumulated.pose.rotation =
                accumulated.pose.rotation * slerp(Quat::identity(), addition.pose.rotation, weight);
        }
        return;
    }

    if (policy.position) {
        accumulated.pose.translation =
            lerp(accumulated.pose.translation, addition.pose.translation, weight);
    }
    if (policy.rotation) {
        accumulated.pose.rotation =
            slerp(accumulated.pose.rotation, addition.pose.rotation, weight);
    }
    if (policy.lens) {
        // Through `camera::blend`, which knows that two physical lenses interpolate in focal
        // length. A component-wise lerp here would be the defect lens.h exists to prevent.
        accumulated.lens = blend(accumulated.lens, addition.lens, weight);
    }
}

}  // namespace

const char* contribution_kind_name(ContributionKind kind) noexcept {
    switch (kind) {
        case ContributionKind::Base:
            return "base";
        case ContributionKind::Additive:
            return "additive";
        case ContributionKind::Effect:
            return "effect";
        case ContributionKind::Volume:
            return "volume";
        case ContributionKind::Cinematic:
            return "cinematic";
        case ContributionKind::Debug:
            return "debug";
        case ContributionKind::Count:
            break;
    }
    return "unknown";
}

const char* blend_curve_name(BlendCurve curve) noexcept {
    switch (curve) {
        case BlendCurve::Linear:
            return "linear";
        case BlendCurve::EaseIn:
            return "ease-in";
        case BlendCurve::EaseOut:
            return "ease-out";
        case BlendCurve::EaseInOut:
            return "ease-in-out";
        case BlendCurve::Step:
            return "step";
        case BlendCurve::Count:
            break;
    }
    return "unknown";
}

f32 apply_curve(BlendCurve curve, f32 t) noexcept {
    const f32 clamped = math::clamp(t, 0.0F, 1.0F);
    switch (curve) {
        case BlendCurve::Linear:
            return clamped;
        case BlendCurve::EaseIn:
            return clamped * clamped;
        case BlendCurve::EaseOut:
            return 1.0F - ((1.0F - clamped) * (1.0F - clamped));
        case BlendCurve::EaseInOut:
            // Smoothstep: zero derivative at both ends, which is what makes a transition in and out
            // of a cinematic have no visible corner.
            return clamped * clamped * (3.0F - (2.0F * clamped));
        case BlendCurve::Step:
        case BlendCurve::Count:
            break;
    }
    return (clamped > 0.0F) ? 1.0F : 0.0F;
}

f32 CameraStack::effective_weight(const Record& record) noexcept {
    const BlendPolicy& policy = record.releasing ? record.entry.blend_out : record.entry.blend_in;
    const f32 progress = (policy.duration_seconds <= 0.0F)
                             ? 1.0F
                             : math::clamp(record.elapsed / policy.duration_seconds, 0.0F, 1.0F);
    const f32 curved = apply_curve(policy.curve, progress);
    if (record.releasing) {
        // Fades from the weight it HAD when release was called, so a contribution released half way
        // through its blend in does not jump to full and then fade.
        return record.release_from * (1.0F - curved);
    }
    return record.entry.target_weight * curved;
}

Expected<StackEntryId, Error> CameraStack::push(const StackEntry& entry) noexcept {
    if (entry.rig.is_null()) {
        return fail(ErrorCode::InvalidArgument, "a camera stack entry names no rig");
    }

    Record record;
    record.entry = entry;
    record.entry.target_weight = math::clamp(entry.target_weight, 0.0F, 1.0F);
    record.id = next_id_++;
    record.sequence = next_sequence_++;

    // Insert in sorted position rather than sorting later: the array is small, insertion keeps the
    // order an invariant of the container, and `blend()` can then assume it without checking.
    usize position = entries_.size();
    for (usize i = 0; i < entries_.size(); ++i) {
        if (orders_before(record.entry.priority, record.sequence, entries_[i].entry.priority,
                          entries_[i].sequence)) {
            position = i;
            break;
        }
    }
    if (Status pushed = entries_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    for (usize i = entries_.size() - 1; i > position; --i) {
        const Record moved = entries_[i - 1];
        entries_[i - 1] = entries_[i];
        entries_[i] = moved;
    }
    return record.id;
}

Status CameraStack::release(StackEntryId id) noexcept {
    Record* record = find(id);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no such camera stack entry");
    }
    if (record->releasing) {
        return ok();
    }
    record->release_from = effective_weight(*record);
    record->releasing = true;
    record->elapsed = 0.0F;
    return ok();
}

Status CameraStack::remove(StackEntryId id) noexcept {
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id != id) {
            continue;
        }
        // Ordered removal: the array is kept sorted, and `remove_unordered` would break that for
        // the saving of one memmove over a handful of entries.
        for (usize j = i + 1; j < entries_.size(); ++j) {
            entries_[j - 1] = entries_[j];
        }
        entries_.pop_back();
        return ok();
    }
    return fail(ErrorCode::NotFound, "no such camera stack entry");
}

void CameraStack::advance(f32 delta_seconds) noexcept {
    usize write = 0;
    for (usize read = 0; read < entries_.size(); ++read) {
        Record& record = entries_[read];
        record.elapsed += math::max(delta_seconds, 0.0F);
        const bool finished = record.releasing && (effective_weight(record) <= 0.0F) &&
                              (record.entry.blend_out.duration_seconds <= 0.0F ||
                               record.elapsed >= record.entry.blend_out.duration_seconds);
        if (finished) {
            continue;
        }
        if (write != read) {
            entries_[write] = entries_[read];
        }
        ++write;
    }
    while (entries_.size() > write) {
        entries_.pop_back();
    }
}

const CameraStack::Record* CameraStack::find(StackEntryId id) const noexcept {
    for (const Record& record : entries_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

CameraStack::Record* CameraStack::find(StackEntryId id) noexcept {
    // The const overload, un-consted. Two identical loops would be two places for the search to
    // change.
    return const_cast<Record*>(static_cast<const CameraStack*>(this)->find(id));
}

const StackEntry& CameraStack::entry_at(usize index) const noexcept {
    CY_ASSERT_MSG(index < entries_.size(), "camera stack index out of range");
    return entries_[index].entry;
}

StackEntryId CameraStack::id_at(usize index) const noexcept {
    if (index >= entries_.size()) {
        return kInvalidStackEntry;
    }
    return entries_[index].id;
}

f32 CameraStack::weight_at(usize index) const noexcept {
    if (index >= entries_.size()) {
        return 0.0F;
    }
    return effective_weight(entries_[index]);
}

Status CameraStack::blend(Span<const EvaluatedCamera> evaluated, EvaluatedCamera& out,
                          Array<StackContribution>* report) const noexcept {
    if (evaluated.size() != entries_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "the camera stack was given a different number of evaluated cameras than it "
                    "has entries");
    }
    if (entries_.empty()) {
        return fail(ErrorCode::NotFound, "the camera stack is empty");
    }
    if (report != nullptr) {
        report->clear();
        if (Status reserved = report->reserve(entries_.size()); !reserved) {
            return reserved;
        }
    }

    // THE LOWEST-PRIORITY ENTRY IS THE BASE AND ITS WEIGHT DOES NOT ATTENUATE IT. There is nothing
    // below it to blend toward, so a base at half weight would otherwise blend half way to a
    // default-constructed camera at the world origin.
    out = evaluated[0];

    for (usize i = 0; i < entries_.size(); ++i) {
        const Record& record = entries_[i];
        const f32 weight = effective_weight(record);

        StackContribution contribution;
        contribution.id = record.id;
        contribution.rig = record.entry.rig;
        contribution.kind = record.entry.kind;
        contribution.priority = record.entry.priority;
        contribution.weight = (i == 0) ? 1.0F : weight;

        if (i != 0 && weight > 0.0F) {
            const Vec3 before_position = out.pose.translation;
            const Quat before_rotation = out.pose.rotation;
            const f32 before_fov = out.lens.vertical_fov_radians();
            const BlendPolicy& policy =
                record.releasing ? record.entry.blend_out : record.entry.blend_in;
            blend_channels(out, evaluated[i], weight, policy, record.entry.kind);
            contribution.position_delta = length(out.pose.translation - before_position);
            contribution.rotation_delta_radians = angle_between(before_rotation, out.pose.rotation);
            contribution.fov_delta_radians = out.lens.vertical_fov_radians() - before_fov;
        }

        if (report != nullptr) {
            if (Status pushed = report->push_back(contribution); !pushed) {
                return pushed;
            }
        }
    }

    // The identity and the cut travel with the HIGHEST-PRIORITY entry, not with the blend: a
    // temporal history follows the camera that is in control, and averaging two history identities
    // would name a third view that never existed.
    out.history_id = evaluated[entries_.size() - 1].history_id;
    out.cut_epoch = evaluated[entries_.size() - 1].cut_epoch;
    out.last_cut = evaluated[entries_.size() - 1].last_cut;
    return ok();
}

}  // namespace cy::camera
