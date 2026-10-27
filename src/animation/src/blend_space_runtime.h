// SPDX-License-Identifier: MIT
#pragma once
// The runtime half of blend spaces and their sync groups, shared by evaluate.cpp's walkers.
// Private to src/animation/src.

#include <cy/animation/evaluate.h>

namespace cy::animation::detail {

/// The weights of blend space `space` at the instance's parameters, into `out` (at least
/// `graph::pose::kMaxBlendSamples` entries). False when the program has no such space.
[[nodiscard]] bool space_weights(const AnimationRig& rig, u16 space, Span<const f32> parameters,
                                 Span<f32> out) noexcept;

/// The sample of `space` with the most weight: the first of equals. What a space's events and
/// its leader are taken from.
[[nodiscard]] u32 heaviest_sample(const AnimationRig& rig, u16 space,
                                  Span<const f32> parameters) noexcept;

/// Advance every synchronised space's shared phase by `step` seconds of playback and set its
/// clips' clocks from it. `before` is `previous_times()`: the clocks as they were when the step
/// began. Called by `advance` after the clocks advanced on their own and before root motion and
/// events read the interval.
void advance_sync(const AnimationRig& rig, AnimationInstance& instance, f32 step) noexcept;

/// Put synchronised space `space` at the start of its cycle: the phase where its first clip is at
/// time zero, every other clip at the corresponding point of its own stride.
void restart_sync(const AnimationRig& rig, AnimationInstance& instance, u16 space) noexcept;

}  // namespace cy::animation::detail
