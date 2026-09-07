#include <cy/rendering/temporal/jitter.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {

f32 halton(u32 index, u32 base) noexcept {
    if (base < 2) {
        return 0.0F;
    }
    f32 fraction = 1.0F;
    f32 result = 0.0F;
    u32 remaining = index;
    while (remaining > 0) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(remaining % base);
        remaining /= base;
    }
    return result;
}

void JitterSequence::configure(const JitterConfig& config) noexcept {
    config_ = config;
    config_.length = math::max(config_.length, 1U);
    config_.base_x = math::max(config_.base_x, 2U);
    config_.base_y = math::max(config_.base_y, 2U);
    index_ = 0;
    current_ = Vec2{0.0F, 0.0F};
    previous_ = Vec2{0.0F, 0.0F};
}

void JitterSequence::advance(bool enabled) noexcept {
    previous_ = current_;
    enabled_ = enabled;
    if (!enabled) {
        // Exactly zero, and the sequence does not advance. A frame that turns jitter back on
        // resumes where it left off rather than at whatever index a free-running counter reached.
        current_ = Vec2{0.0F, 0.0F};
        return;
    }
    if (!pinned_) {
        index_ = (index_ + 1U) % config_.length;
    }
    // Halton is indexed from one: index zero is the radical inverse of nothing, which is 0.0, and a
    // sequence whose first sample is the pixel centre wastes one frame of convergence.
    const u32 sample = index_ + 1U;
    current_ = Vec2{(halton(sample, config_.base_x) - 0.5F) * config_.spread,
                    (halton(sample, config_.base_y) - 0.5F) * config_.spread};
}

void JitterSequence::pin(u32 index) noexcept {
    pinned_ = true;
    index_ = index % config_.length;
}

void JitterSequence::unpin() noexcept {
    pinned_ = false;
}

Vec2 JitterSequence::ndc_offset(u32 width, u32 height) const noexcept {
    const f32 w = static_cast<f32>(math::max(width, 1U));
    const f32 h = static_cast<f32>(math::max(height, 1U));
    return Vec2{current_.x * 2.0F / w, current_.y * 2.0F / h};
}

}  // namespace cy::rendering
