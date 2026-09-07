#include <cy/rendering/temporal/reprojection.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {

const char* history_state_name(HistoryState state) noexcept {
    switch (state) {
        case HistoryState::Valid:
            return "Valid";
        case HistoryState::Disoccluded:
            return "Disoccluded";
        case HistoryState::OutOfFrame:
            return "OutOfFrame";
        case HistoryState::Unrepresentable:
            return "Unrepresentable";
        case HistoryState::Count:
            break;
    }
    return "Unknown";
}

ReprojectionResult classify_history(const ReprojectionInputs& inputs) noexcept {
    ReprojectionResult result;
    result.history_uv =
        Vec2{inputs.current_uv.x + inputs.motion.x, inputs.current_uv.y + inputs.motion.y};

    // The order is not arbitrary. An invalidated history and an unrepresentable surface both mean
    // "reconstruct spatially", and they are tested first so that a consumer never has to combine a
    // state with a separate validity flag — which is the shape that lets one consumer forget.
    if (!inputs.history_valid || !inputs.representable) {
        result.state = HistoryState::Unrepresentable;
        return result;
    }
    if (result.history_uv.x < 0.0F || result.history_uv.x > 1.0F || result.history_uv.y < 0.0F ||
        result.history_uv.y > 1.0F) {
        result.state = HistoryState::OutOfFrame;
        return result;
    }
    // Relative depth, because a fixed epsilon is either useless at a hundred metres or useless at
    // one. The reference is the nearer of the two: a background pixel disoccluded by a foreground
    // one must be classified from the scale of the surface that is actually there.
    const f32 reference = math::max(math::min(inputs.current_depth, inputs.history_depth), 1e-4F);
    const f32 difference = std::fabs(inputs.current_depth - inputs.history_depth);
    result.state = difference > reference * inputs.depth_tolerance ? HistoryState::Disoccluded
                                                                   : HistoryState::Valid;
    return result;
}

void ClassificationCounts::record(HistoryState state) noexcept {
    const auto index = static_cast<usize>(state);
    if (index < static_cast<usize>(HistoryState::Count)) {
        ++counts[index];
    }
}

void ClassificationCounts::reset() noexcept {
    for (u32& count : counts) {
        count = 0;
    }
}

u32 ClassificationCounts::total() const noexcept {
    u32 sum = 0;
    for (const u32 count : counts) {
        sum += count;
    }
    return sum;
}

f32 ClassificationCounts::fraction(HistoryState state) const noexcept {
    const u32 all = total();
    if (all == 0) {
        return 0.0F;
    }
    return static_cast<f32>(counts[static_cast<usize>(state)]) / static_cast<f32>(all);
}

}  // namespace cy::rendering
