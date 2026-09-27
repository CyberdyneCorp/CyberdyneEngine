// SPDX-License-Identifier: MIT
#pragma once
// Last frame's instance rows, kept so the depth prepass derives PER-OBJECT motion.
// `temporal-rendering` — "Motion vectors are derived, not authored".
//
// ================================================================================================
// WHY THE BINDINGS KEEP THE ROWS, AND NOT THE CALLER
// ================================================================================================
//
// The requirement's first sentence is the design: motion vectors are "derived from data the
// renderer already holds: current and previous instance transforms". Every caller of this layer
// already hands `FrameBindings::upload` this frame's rows; last frame's are the rows it handed the
// frame before. So the bindings keep them, and a moving object's motion needs nothing from anybody
// — not a second span, not a flag, not a per-system "previous transform" field that one system
// remembers to fill and the next does not. That is the requirement's "without per-system effort"
// made structural rather than asked for.
//
// A ROW IS KEYED BY ITS SLOT AND CONFIRMED BY ITS STABLE IDENTITY. The rows are indexed by the
// instance's GPU scene slot (`cyDrawInstances[...].instanceSlot`), and a slot is reused when an
// instance is destroyed and another created. A caller that passes `FrameUpload::instance_ids`
// gets a new instance in a reused slot treated as NEW — its previous row is its current one, so it
// carries camera motion and no invented object motion from whatever lived there before. A caller
// that passes none gets rows matched by slot alone, which is exact for a scene whose slots are not
// recycled between two frames.
//
// ================================================================================================
// THE ROWS ARE CAMERA-RELATIVE, SO LAST FRAME'S ARE REBASED
// ================================================================================================
//
// Design.md §3: positions are camera-RELATIVE and never world. Last frame's rows are relative to
// last frame's camera, and the prepass pushes a previous position through a previous projection
// that is expressed about THIS frame's camera (`previous_relative_to_clip` in frame_recorder.cpp).
// So each kept row's translation is moved by the camera's displacement:
//
//     relative_now = world - camera_now = relative_then - (camera_now - camera_then)
//
// A still camera moves every row by exactly zero, which is what keeps a still frame's previous rows
// equal to its current ones bit for bit — and the prepass's velocity, and the temporal resolve that
// reads it, byte-identical to the frame before this existed.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::pipeline {

/// What one `InstanceHistory::advance` did, for a caller that wants to see the derivation work.
struct InstanceHistoryReport {
    /// Rows written after the current ones: the previous placement of every current row.
    u32 previous_rows = 0;
    /// Rows whose previous placement differs from their current one, bit for bit. Under a still
    /// camera these are exactly the instances that moved; under a moving one the rebase rounds.
    u32 moved = 0;
    /// Rows with no usable history: a slot seen for the first time, or reused by a new identity,
    /// or every row after a cut. Their previous row is their current one.
    u32 new_rows = 0;
};

/// The rows one frame uploaded, kept for the next.
///
/// NOT THREAD-SAFE and one per view, like `FrameBindings`, which owns one.
class InstanceHistory {
public:
    explicit InstanceHistory(Allocator& allocator) noexcept;

    InstanceHistory(const InstanceHistory&) = delete;
    InstanceHistory& operator=(const InstanceHistory&) = delete;
    InstanceHistory(InstanceHistory&&) = delete;
    InstanceHistory& operator=(InstanceHistory&&) = delete;

    /// Write each current row's PREVIOUS placement into `previous` — which must be as long as
    /// `current` — and then keep `current` for the next frame.
    ///
    /// `ids` is empty or parallel to `current`. `camera_motion` is this frame's camera position
    /// minus last frame's, in world metres. `cut` discards the history: a teleport or a cinematic
    /// cut has no previous frame worth reprojecting, and the temporal framework rejects history for
    /// the frame anyway, so every previous row is its current one.
    [[nodiscard]] Status advance(Span<const InstanceTransform> current, Span<const u64> ids,
                                 Vec3 camera_motion, bool cut,
                                 Span<InstanceTransform> previous) noexcept;

    /// Forget everything. The next frame's every row is new.
    void reset() noexcept;

    [[nodiscard]] const InstanceHistoryReport& report() const noexcept { return report_; }
    /// How many rows are kept — last frame's row count.
    [[nodiscard]] u32 kept() const noexcept { return static_cast<u32>(rows_.size()); }

private:
    Array<InstanceTransform> rows_;
    Array<u64> ids_;
    InstanceHistoryReport report_{};
};

/// One kept row, moved from last frame's camera to this frame's. Exposed for the unit suite.
[[nodiscard]] InstanceTransform rebase_previous_row(const InstanceTransform& row,
                                                    Vec3 camera_motion) noexcept;

}  // namespace cy::rendering::pipeline
