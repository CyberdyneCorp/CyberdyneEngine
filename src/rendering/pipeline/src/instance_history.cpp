// SPDX-License-Identifier: MIT
// Last frame's instance rows, kept so the depth prepass derives per-object motion.

#include <cy/rendering/pipeline/instance_history.h>

#include <cstring>

namespace cy::rendering::pipeline {
namespace {

/// Whether two rows place a mesh identically. Bitwise, because "the same placement" here means
/// "the prepass computes the same position", and that is a statement about bits.
[[nodiscard]] bool same_placement(const InstanceTransform& a, const InstanceTransform& b) noexcept {
    // `bugprone-suspicious-memory-comparison` is right that a float has no unique representation,
    // and that is the point here, as the comment above says.
    // NOLINTBEGIN(bugprone-suspicious-memory-comparison)
    return std::memcmp(a.row0, b.row0, sizeof(a.row0)) == 0 &&
           std::memcmp(a.row1, b.row1, sizeof(a.row1)) == 0 &&
           std::memcmp(a.row2, b.row2, sizeof(a.row2)) == 0;
    // NOLINTEND(bugprone-suspicious-memory-comparison)
}

/// Whether slot `slot` holds the same instance it held last frame.
[[nodiscard]] bool same_instance(Span<const u64> kept, Span<const u64> ids, usize slot) noexcept {
    if (ids.empty() && kept.empty()) {
        return true;
    }
    return !ids.empty() && slot < kept.size() && kept[slot] == ids[slot];
}

}  // namespace

InstanceTransform rebase_previous_row(const InstanceTransform& row, Vec3 camera_motion) noexcept {
    InstanceTransform rebased = row;
    // The fourth column of each row is the translation; subtracting zero leaves it bit for bit.
    rebased.row0[3] = row.row0[3] - camera_motion.x;
    rebased.row1[3] = row.row1[3] - camera_motion.y;
    rebased.row2[3] = row.row2[3] - camera_motion.z;
    return rebased;
}

InstanceHistory::InstanceHistory(Allocator& allocator) noexcept
    : rows_(allocator), ids_(allocator) {}

Status InstanceHistory::advance(Span<const InstanceTransform> current, Span<const u64> ids,
                                Vec3 camera_motion, bool cut,
                                Span<InstanceTransform> previous) noexcept {
    if (previous.size() != current.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "instance history: the previous rows must be as many as the current ones");
    }
    if (!ids.empty() && ids.size() != current.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "instance history: the stable identities must be parallel to the rows");
    }
    report_ = InstanceHistoryReport{};
    report_.previous_rows = static_cast<u32>(current.size());
    const Span<const InstanceTransform> kept = rows_.span();
    const Span<const u64> kept_ids = ids_.span();
    for (usize slot = 0; slot < current.size(); ++slot) {
        const bool known = !cut && slot < kept.size() && same_instance(kept_ids, ids, slot);
        if (!known) {
            previous[slot] = current[slot];
            ++report_.new_rows;
            continue;
        }
        previous[slot] = rebase_previous_row(kept[slot], camera_motion);
        // Tint and the unused word travel with the current row: only the placement is history.
        std::memcpy(previous[slot].tint, current[slot].tint, sizeof(previous[slot].tint));
        // Its previous placement is not its current one. Under a still camera that is exactly the
        // objects that moved in the world; under a moving one the rebase rounds, and a still object
        // may count.
        if (!same_placement(previous[slot], current[slot])) {
            ++report_.moved;
        }
    }

    if (Status sized = rows_.resize(current.size()); !sized) {
        return sized;
    }
    if (!current.empty()) {
        std::memcpy(rows_.data(), current.data(), current.size() * sizeof(InstanceTransform));
    }
    if (Status sized = ids_.resize(ids.size()); !sized) {
        return sized;
    }
    if (!ids.empty()) {
        std::memcpy(ids_.data(), ids.data(), ids.size() * sizeof(u64));
    }
    return ok();
}

void InstanceHistory::reset() noexcept {
    rows_.clear();
    ids_.clear();
    report_ = InstanceHistoryReport{};
}

}  // namespace cy::rendering::pipeline
