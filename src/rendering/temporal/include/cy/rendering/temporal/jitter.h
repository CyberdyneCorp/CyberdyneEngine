#pragma once
// The jitter sequence, owned by the framework and by nothing else. Task 8.3.
//
// `temporal-rendering` — "Jitter": a low discrepancy sequence (Halton by default) with a
// configurable length; the current and previous frame's offsets exposed to every consumer; applied
// ONCE, by the framework; and disabled coherently when no consumer requires it.
//
// The reason this is a file rather than three lines inside a TAA pass is the requirement's own
// scenario: "WHEN both temporal anti-aliasing and temporal upscaling are active THEN one jitter
// offset SHALL be applied to the projection, and both SHALL read it". Two passes that each jittered
// would produce an image that is the sum of two sub-pixel offsets and a history that neither of
// them can reproject, and the symptom is a softness that survives every knob either pass has.
//
// PINNED MODE IS A GOLDEN-IMAGE REQUIREMENT, NOT A DEBUG AID. "The framework SHALL support a pinned
// mode in which jitter follows a fixed sequence from a fixed starting index and history is
// deterministic, so golden-image tests and capture produce reproducible output." A sequence indexed
// by a free-running frame counter is reproducible only if the capture starts on the same frame,
// which no test can promise; `JitterSequence::pin(index)` is what makes the promise keepable.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// The Halton radical inverse in `base`, for `index`. Exposed because the sequence length and the
/// bases are configurable and a consumer occasionally needs to reproduce one offset — a capture
/// tool, or a test asserting the sequence rather than the class around it.
[[nodiscard]] f32 halton(u32 index, u32 base) noexcept;

struct JitterConfig {
    /// How many samples before the sequence repeats. 8 is the usual choice for temporal
    /// antialiasing and 16 for a 2x upscale, and it is configurable because the right answer is a
    /// function of how many frames of history the consumers actually keep.
    u32 length = 8;
    /// The two Halton bases. Coprime, or the sequence collapses onto a line.
    u32 base_x = 2;
    u32 base_y = 3;
    /// Scales the offset. 1.0 spreads samples over one whole pixel, which is what a box
    /// reconstruction wants; a narrower spread trades aliasing for sharpness.
    f32 spread = 1.0F;
};

/// The sequence. Owned by `TemporalFramework`; there is deliberately no way for a pass to make one
/// of its own that the framework would not know about.
class JitterSequence {
public:
    void configure(const JitterConfig& config) noexcept;

    [[nodiscard]] const JitterConfig& config() const noexcept { return config_; }

    /// Advance one frame. `enabled` is the framework's answer to "does any consumer need jitter" —
    /// when it is false the offset is exactly zero and the sequence does not advance, which is what
    /// "the projection SHALL be unjittered" means for a frame that later turns jitter back on.
    void advance(bool enabled) noexcept;

    /// Pin the sequence to a fixed starting index. Deterministic from here on, whatever frame the
    /// capture started on.
    void pin(u32 index) noexcept;

    void unpin() noexcept;

    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /// Sub-pixel offset for this frame, in pixels, in [-0.5, 0.5] before `spread`.
    [[nodiscard]] Vec2 current() const noexcept { return current_; }

    /// The previous frame's offset. A consumer that reprojects needs both: the history was rendered
    /// with the previous offset and comparing it against the current one without removing them is
    /// the most common source of a half-pixel of permanent smear.
    [[nodiscard]] Vec2 previous() const noexcept { return previous_; }

    [[nodiscard]] u32 index() const noexcept { return index_; }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// The offset expressed in normalised device coordinates for a target of this size, which is
    /// what actually goes into the projection matrix. Two pixels of a `width`-wide target span 2/w
    /// of NDC, so the factor is 2/size and not 1/size — the missing 2 is a bug that halves the
    /// sample spread and looks exactly like a sequence that is too short.
    [[nodiscard]] Vec2 ndc_offset(u32 width, u32 height) const noexcept;

private:
    JitterConfig config_;
    Vec2 current_{0.0F, 0.0F};
    Vec2 previous_{0.0F, 0.0F};
    u32 index_ = 0;
    bool pinned_ = false;
    bool enabled_ = false;
};

}  // namespace cy::rendering
