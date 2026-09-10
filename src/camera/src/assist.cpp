// Aim assistance and the director. M8.b task 7.3.

#include <cy/camera/assist.h>

#include <algorithm>
#include <cmath>

namespace cy::camera {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

[[nodiscard]] Vec3 safe_direction(Vec3 direction) noexcept {
    return normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
}

}  // namespace

Vec2 apply_aim_assist(const AimAssistSettings& settings, Vec2 raw_look, Vec3 view_direction,
                      Span<const AimCandidate> candidates, f32 dt,
                      AimAssistReport& report) noexcept {
    report.raw_look = raw_look;
    report.assisted_look = raw_look;
    report.candidate = AimAssistReport::kNoCandidate;
    report.candidate_angle = 0.0F;
    report.active = false;
    if (candidates.empty() || dt <= 0.0F) {
        return raw_look;
    }

    const Vec3 view = safe_direction(view_direction);
    u32 best = AimAssistReport::kNoCandidate;
    f32 best_score = -1.0F;
    f32 best_angle = 0.0F;
    for (usize index = 0; index < candidates.size(); ++index) {
        const Vec3 direction = safe_direction(candidates[index].direction);
        const f32 angle = std::acos(clampf(dot(view, direction), -1.0F, 1.0F));
        if (angle > settings.capture_radians) {
            continue;
        }
        const f32 score = candidates[index].weight * (settings.capture_radians - angle);
        if (score > best_score) {
            best_score = score;
            best = static_cast<u32>(index);
            best_angle = angle;
        }
    }
    if (best == AimAssistReport::kNoCandidate) {
        return raw_look;
    }

    Vec2 assisted = raw_look;
    // SLOWDOWN: the look scales down near a candidate, so a stick sweeps past it more slowly.
    if (best_angle <= settings.slowdown_radians && settings.slowdown < 1.0F) {
        const f32 nearness = 1.0F - (best_angle / (settings.slowdown_radians + 1e-6F));
        const f32 factor = 1.0F - ((1.0F - settings.slowdown) * nearness);
        assisted.x *= factor;
        assisted.y *= factor;
    }
    // MAGNETISM: a bend toward the candidate PER SECOND, frame-rate independent for the same reason
    // camera smoothing is.
    if (settings.magnetism > 0.0F) {
        const Vec3 target = safe_direction(candidates[best].direction);
        const Vec3 offset = target - (view * dot(view, target));
        const Vec3 right =
            normalized_or(cross(view, Vec3{0.0F, 1.0F, 0.0F}), Vec3{1.0F, 0.0F, 0.0F});
        const Vec3 up = cross(right, view);
        const f32 pull = settings.magnetism * dt;
        assisted.x += dot(offset, right) * pull;
        assisted.y += dot(offset, up) * pull;
    }
    if (settings.snap && best_angle <= settings.slowdown_radians * 0.25F) {
        assisted = Vec2{0.0F, 0.0F};
    }

    report.assisted_look = assisted;
    report.candidate = best;
    report.candidate_angle = best_angle;
    report.active = true;
    return assisted;
}

DirectorState::DirectorState(Allocator& allocator) noexcept {
    (void)allocator;  // The memory is fixed-size on purpose; see the header.
}

f32 DirectorState::penalty_of(Name shot) const noexcept {
    for (const Memory& entry : memory_) {
        if (entry.shot == shot) {
            return entry.penalty;
        }
    }
    return 0.0F;
}

void DirectorState::remember(Name shot, f32 penalty) noexcept {
    for (Memory& entry : memory_) {
        if (entry.shot == shot) {
            entry.penalty = penalty;
            return;
        }
    }
    // Replace the faintest memory: the shot the director has least reason to avoid.
    Memory* weakest = &memory_[0];
    for (Memory& entry : memory_) {
        if (entry.penalty < weakest->penalty) {
            weakest = &entry;
        }
    }
    weakest->shot = shot;
    weakest->penalty = penalty;
}

Status DirectorState::score_shots(Span<const ShotCandidate> candidates,
                                  const DirectorWeights& weights,
                                  Array<ShotScore>& out) const noexcept {
    out.clear();
    for (const ShotCandidate& candidate : candidates) {
        ShotScore score;
        score.name = candidate.name;
        score.repetition_penalty = penalty_of(candidate.name) * weights.repetition;
        score.score = (candidate.visibility * weights.visibility) +
                      (candidate.importance * weights.importance) +
                      (candidate.activity * weights.activity) - score.repetition_penalty;
        if (Status pushed = out.push_back(score); !pushed) {
            return pushed;
        }
    }
    // Insertion sort by score, descending, and STABLE: two shots with equal scores keep the
    // caller's order, so a director does not oscillate between two identical viewpoints.
    for (usize index = 1; index < out.size(); ++index) {
        const ShotScore held = out[index];
        usize position = index;
        while (position > 0 && out[position - 1].score < held.score) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = held;
    }
    return ok();
}

void DirectorState::advance(f32 dt) noexcept {
    if (dt <= 0.0F) {
        return;
    }
    shot_seconds_ += dt;
    const f32 half_life = (decay_half_life_ > 0.0F) ? decay_half_life_ : 1.0F;
    const f32 decay = std::pow(0.5F, dt / half_life);
    for (Memory& entry : memory_) {
        entry.penalty *= decay;
        if (entry.penalty < 1e-4F) {
            entry.penalty = 0.0F;
        }
    }
}

Name DirectorState::select(Span<const ShotScore> scores, const DirectorWeights& weights) noexcept {
    changed_ = false;
    decay_half_life_ = weights.repetition_half_life;
    if (scores.empty()) {
        return current_;
    }
    const ShotScore& best = scores[0];
    if (current_.is_empty()) {
        current_ = best.name;
        shot_seconds_ = 0.0F;
        changed_ = true;
        return current_;
    }
    if (best.name == current_) {
        return current_;
    }
    // A MINIMUM SHOT LENGTH AND A MARGIN. Without both, a director changes its mind whenever two
    // shots trade places by a thousandth, which is the oscillation the repetition term alone does
    // not prevent.
    if (shot_seconds_ < weights.minimum_shot_seconds) {
        return current_;
    }
    f32 current_score = -1e9F;
    for (const ShotScore& score : scores) {
        if (score.name == current_) {
            current_score = score.score;
            break;
        }
    }
    if (best.score < current_score + weights.hysteresis) {
        return current_;
    }
    // The shot being LEFT is what earns a repetition penalty, so a director does not come straight
    // back to it.
    remember(current_, 1.0F);
    current_ = best.name;
    shot_seconds_ = 0.0F;
    changed_ = true;
    return current_;
}

}  // namespace cy::camera
