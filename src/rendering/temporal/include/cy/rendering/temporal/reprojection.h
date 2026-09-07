#pragma once
// Reprojection and disocclusion classification, computed once and read by everybody. Task 8.3.
//
// `temporal-rendering` — "Reprojection and disocclusion": the framework classifies each pixel's
// history as valid, disoccluded, out of frame, or unrepresentable, and "consumers SHALL be able to
// read that classification rather than each deriving it, so that disocclusion handling is
// consistent across effects".
//
// The consistency is the point and it is a real defect when it is missing: temporal antialiasing
// and screen-space reflections that disagree about whether a pixel's history is valid produce an
// edge where one of them accumulates and the other does not, which reads as a shimmering outline
// around every moving object and cannot be attributed to either pass by looking at it.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// The four states, in the order `temporal-rendering` names them.
enum class HistoryState : u8 {
    /// Reprojection landed on the same surface. History may be accumulated.
    Valid = 0,
    /// Reprojection landed on a different surface — geometry revealed from behind an occluder.
    Disoccluded,
    /// Reprojection landed off the screen.
    OutOfFrame,
    /// The surface's motion cannot be expressed as a screen-space vector.
    Unrepresentable,
    Count,
};

[[nodiscard]] const char* history_state_name(HistoryState state) noexcept;

/// True for the one state in which a consumer may read history. Written once so that no consumer
/// invents its own set of "close enough" states.
[[nodiscard]] inline bool history_usable(HistoryState state) noexcept {
    return state == HistoryState::Valid;
}

struct ReprojectionInputs {
    Vec2 current_uv{0.0F, 0.0F};
    /// From `derive_surface_motion()`. `history_uv = current_uv + motion`.
    Vec2 motion{0.0F, 0.0F};
    bool representable = true;
    /// Linear view-space depth of this pixel now, and of the history sample it reprojects onto.
    f32 current_depth = 0.0F;
    f32 history_depth = 0.0F;
    /// Relative depth difference above which the two are different surfaces. Relative rather than
    /// absolute because a fixed epsilon is either useless at a hundred metres or useless at one.
    f32 depth_tolerance = 0.05F;
    /// False when the framework has invalidated history this frame. A consumer never has to check
    /// this separately — invalidation reaches it as `Unrepresentable`, which is the only state that
    /// means "reconstruct spatially" whatever the reason.
    bool history_valid = true;
};

struct ReprojectionResult {
    HistoryState state = HistoryState::Unrepresentable;
    Vec2 history_uv{0.0F, 0.0F};
};

[[nodiscard]] ReprojectionResult classify_history(const ReprojectionInputs& inputs) noexcept;

/// Counts per state, for the diagnostic the specification requires: "the fraction of pixels
/// classified into each history state".
struct ClassificationCounts {
    u32 counts[static_cast<usize>(HistoryState::Count)] = {};

    void record(HistoryState state) noexcept;
    void reset() noexcept;
    [[nodiscard]] u32 total() const noexcept;
    [[nodiscard]] f32 fraction(HistoryState state) const noexcept;
};

}  // namespace cy::rendering
