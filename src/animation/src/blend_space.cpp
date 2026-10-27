// SPDX-License-Identifier: MIT
// Blend spaces at run time: their weights, the shared clock of a synchronised space, and the
// foot placements and aims a rig resolves by joint name. Issue #92. See cy/graph/lower_pose.h for
// what a blend space and its sync are; this file is how the runtime plays them.

#include <cy/animation/evaluate.h>

#include <cy/core/math/scalar.h>

#include "blend_space_runtime.h"

#include <cmath>

namespace cy::animation {
namespace {

using graph::pose::BlendSpace;
using graph::pose::kMaxBlendSamples;

/// The most sync markers one clip's stride is followed through. A clip with more is synchronised
/// by normalised time instead: a locomotion cycle has two foot-downs, and thirty-two is a dance.
constexpr u32 kMaxSyncMarkers = 32;
/// A stride segment shorter than this is two markers at one instant, which no phase can divide.
constexpr f32 kShortestSegment = 1e-4F;

struct MarkerRun {
    ClipMarker markers[kMaxSyncMarkers];
    u32 count = 0;
};

/// The clip's markers in time order; equal times keep the clip's own order. False for more than
/// `kMaxSyncMarkers`.
[[nodiscard]] bool sorted_markers(const Clip& clip, MarkerRun& out) noexcept {
    const Span<const ClipMarker> markers = clip.markers();
    if (markers.size() > kMaxSyncMarkers) {
        return false;
    }
    out.count = static_cast<u32>(markers.size());
    for (u32 index = 0; index < out.count; ++index) {
        out.markers[index] = markers[index];
    }
    // An insertion sort: a stride has a handful of markers, and equal times keep their order.
    for (u32 outer = 1; outer < out.count; ++outer) {
        const ClipMarker moving = out.markers[outer];
        u32 inner = outer;
        for (; inner > 0 && out.markers[inner - 1].time > moving.time; --inner) {
            out.markers[inner] = out.markers[inner - 1];
        }
        out.markers[inner] = moving;
    }
    return true;
}

/// The rotation of `run` whose names read as `reference`'s from its first: walk and run that start
/// on opposite feet still correspond, one segment along. `out_of_step` when none does.
constexpr u32 kOutOfStep = 0xFFFFFFFFU;
[[nodiscard]] u32 corresponding_rotation(const MarkerRun& reference,
                                         const MarkerRun& run) noexcept {
    if (run.count != reference.count) {
        return kOutOfStep;
    }
    for (u32 rotation = 0; rotation < run.count; ++rotation) {
        bool same = true;
        for (u32 index = 0; index < run.count && same; ++index) {
            same =
                run.markers[(rotation + index) % run.count].name == reference.markers[index].name;
        }
        if (same) {
            return rotation;
        }
    }
    return kOutOfStep;
}

/// One clip sample of a synchronised space, before its times are stored.
struct Candidate {
    u16 sample = 0;
    u16 time_param = 0;
    u16 time_slot = 0;
    const Clip* clip = nullptr;
};

[[nodiscard]] f32 wrap_time(f32 value, f32 duration) noexcept {
    if (duration <= 0.0F) {
        return 0.0F;
    }
    const f32 wrapped = value - (std::floor(value / duration) * duration);
    return wrapped >= duration ? 0.0F : wrapped;
}

/// The view of one sample of a synchronised space: its segment starts and its length.
struct Stride {
    Span<const f32> starts;
    f32 duration = 1.0F;

    [[nodiscard]] u32 segments() const noexcept { return static_cast<u32>(starts.size()); }
    [[nodiscard]] f32 segment(u32 index) const noexcept {
        const u32 last = segments() - 1;
        return index < last ? starts[index + 1] - starts[index]
                            : starts[0] + duration - starts[last];
    }
    /// The clip time, unwrapped, at marker-space phase `phase`.
    [[nodiscard]] f32 time_at(f32 phase) const noexcept {
        const auto count = static_cast<f32>(segments());
        const f32 cycles = std::floor(phase / count);
        const f32 within = phase - (cycles * count);
        u32 index = static_cast<u32>(within);
        index = index < segments() ? index : segments() - 1;
        const f32 fraction = within - static_cast<f32>(index);
        return (cycles * duration) + starts[index] + (fraction * segment(index));
    }
    /// The phase at clip time `time`, within one cycle.
    [[nodiscard]] f32 phase_of(f32 time) const noexcept {
        f32 at = wrap_time(time, duration);
        if (at < starts[0]) {
            at += duration;
        }
        u32 index = 0;
        for (u32 candidate = 0; candidate < segments(); ++candidate) {
            if (starts[candidate] <= at) {
                index = candidate;
            }
        }
        const f32 fraction = math::clamp((at - starts[index]) / segment(index), 0.0F, 1.0F);
        return static_cast<f32>(index) + fraction;
    }
};

[[nodiscard]] Stride stride_of(const AnimationRig& rig, const AnimationRig::SyncSpace& space,
                               const AnimationRig::SyncSample& sample) noexcept {
    return Stride{rig.sync_times().subspan(sample.first_time, space.segments), sample.duration};
}

[[nodiscard]] f32 parameter_value(Span<const f32> parameters, u16 index) noexcept {
    return index < parameters.size() ? parameters[index] : 0.0F;
}

/// Each sample's share of the space's cadence: its weight among the synchronised samples, or an
/// equal share when all the weight is on samples that are not clips.
void cadence_weights(const AnimationRig& rig, const AnimationRig::SyncSpace& space,
                     Span<const f32> weights, f32 (&out)[kMaxBlendSamples]) noexcept {
    f32 total = 0.0F;
    for (u32 index = 0; index < space.sample_count; ++index) {
        total += weights[rig.sync_samples()[space.first_sample + index].sample];
    }
    for (u32 index = 0; index < space.sample_count; ++index) {
        const f32 weight = weights[rig.sync_samples()[space.first_sample + index].sample];
        out[index] = total > 0.0F ? weight / total : 1.0F / static_cast<f32>(space.sample_count);
    }
}

/// The phase `step` seconds of playback after `phase`: through each segment at the weighted mean
/// of the samples' lengths of it.
[[nodiscard]] f32 advanced_phase(const AnimationRig& rig, const AnimationRig::SyncSpace& space,
                                 const f32 (&cadence)[kMaxBlendSamples], f32 phase,
                                 f32 step) noexcept {
    f32 remaining = step;
    // A step longer than many strides is not walked segment by segment for ever: past this many
    // segments the rest is taken at the last segment's rate.
    const u32 bound = (4U * space.segments) + 4U;
    for (u32 walked = 0; remaining > 0.0F; ++walked) {
        const u32 index = static_cast<u32>(phase) % space.segments;
        f32 length = 0.0F;
        for (u32 sample = 0; sample < space.sample_count; ++sample) {
            const AnimationRig::SyncSample& entry = rig.sync_samples()[space.first_sample + sample];
            length += cadence[sample] * stride_of(rig, space, entry).segment(index);
        }
        if (length <= 0.0F) {
            break;
        }
        const f32 fraction = phase - std::floor(phase);
        const f32 to_end = (1.0F - fraction) * length;
        if (remaining < to_end || walked >= bound) {
            phase += remaining / length;
            break;
        }
        phase = std::floor(phase) + 1.0F;
        remaining -= to_end;
    }
    return phase;
}

[[nodiscard]] const AnimationRig::SyncSpace* sync_of(const AnimationRig& rig, u16 space) noexcept {
    for (const AnimationRig::SyncSpace& entry : rig.sync_spaces()) {
        if (entry.space == space) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

// --- Binding
// --------------------------------------------------------------------------------------

Status AnimationRig::bind_sync() noexcept {
    sync_spaces_.clear();
    sync_samples_.clear();
    sync_times_.clear();
    const PoseProgram& program = *program_;
    for (usize index = 0; index < program.blend_spaces().size(); ++index) {
        const BlendSpace& space = program.blend_spaces()[index];
        if (!space.sync) {
            continue;
        }
        // THE CLIP SAMPLES, each with a looping clock of its own. Anything else in the space — a
        // nested blend, a held clip, a clip on a clock another sample already plays — keeps its
        // own clock: there is nothing to align it by.
        Candidate candidates[kMaxBlendSamples];
        u32 count = 0;
        for (u32 sample = 0; sample < space.sample_count; ++sample) {
            const PoseValue value = program.blend_samples()[space.first_sample + sample].value;
            const PoseInstruction& instruction = program.code()[value];
            if (instruction.op != PoseOp::SampleClip || instruction.clip >= clips_.size() ||
                clips_[instruction.clip] == nullptr) {
                continue;
            }
            bool fresh = true;
            for (u32 other = 0; other < count; ++other) {
                fresh = fresh && candidates[other].time_param != instruction.time_param;
            }
            u16 slot = 0xFFFFU;
            for (usize entry = 0; entry < times_.size(); ++entry) {
                if (times_[entry].parameter == instruction.time_param && times_[entry].looping) {
                    slot = static_cast<u16>(entry);
                }
            }
            if (!fresh || slot == 0xFFFFU || clips_[instruction.clip]->duration() <= 0.0F) {
                continue;
            }
            candidates[count++] = Candidate{static_cast<u16>(sample), instruction.time_param, slot,
                                            clips_[instruction.clip]};
        }
        if (count == 0) {
            continue;
        }

        // MARKER CORRESPONDENCE, or normalised time when any clip's markers do not correspond.
        MarkerRun runs[kMaxBlendSamples];
        u32 rotations[kMaxBlendSamples] = {};
        bool aligned = sorted_markers(*candidates[0].clip, runs[0]) && runs[0].count != 0;
        for (u32 candidate = 0; candidate < count && aligned; ++candidate) {
            aligned = sorted_markers(*candidates[candidate].clip, runs[candidate]);
            rotations[candidate] =
                aligned ? corresponding_rotation(runs[0], runs[candidate]) : kOutOfStep;
            aligned = aligned && rotations[candidate] != kOutOfStep;
        }
        const u32 segments = aligned ? runs[0].count : 1U;
        f32 starts[kMaxBlendSamples][kMaxSyncMarkers] = {};
        for (u32 candidate = 0; candidate < count && aligned; ++candidate) {
            const f32 duration = candidates[candidate].clip->duration();
            for (u32 segment = 0; segment < segments; ++segment) {
                const u32 source = rotations[candidate] + segment;
                const f32 time = runs[candidate].markers[source % segments].time;
                starts[candidate][segment] = source >= segments ? time + duration : time;
            }
            const Stride stride{Span<const f32>(starts[candidate], segments), duration};
            for (u32 segment = 0; segment < segments; ++segment) {
                aligned = aligned && stride.segment(segment) > kShortestSegment;
            }
        }
        const u32 stored_segments = aligned ? segments : 1U;

        SyncSpace entry;
        entry.space = static_cast<u16>(index);
        entry.segments = static_cast<u16>(stored_segments);
        entry.first_sample = static_cast<u32>(sync_samples_.size());
        entry.sample_count = count;
        for (u32 candidate = 0; candidate < count; ++candidate) {
            SyncSample sample;
            sample.sample = candidates[candidate].sample;
            sample.time_param = candidates[candidate].time_param;
            sample.time_slot = candidates[candidate].time_slot;
            sample.duration = candidates[candidate].clip->duration();
            sample.first_time = static_cast<u32>(sync_times_.size());
            if (Status pushed = sync_samples_.push_back(sample); !pushed) {
                return pushed;
            }
            for (u32 segment = 0; segment < stored_segments; ++segment) {
                if (Status pushed =
                        sync_times_.push_back(aligned ? starts[candidate][segment] : 0.0F);
                    !pushed) {
                    return pushed;
                }
            }
        }
        if (Status pushed = sync_spaces_.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AnimationRig::bind_modifiers() noexcept {
    const PoseProgram& program = *program_;
    const Skeleton& skeleton = *skeleton_;
    unresolved_ = 0;
    const auto resolve = [this, &skeleton](Name name) noexcept {
        const u16 joint = skeleton.find(name);
        unresolved_ += joint == kInvalidJoint ? 1U : 0U;
        return joint;
    };

    feet_.clear();
    for (const graph::pose::FootPlacementRef& foot : program.foot_placements()) {
        FootPlacementSetup setup;
        setup.pelvis = resolve(foot.pelvis);
        for (u32 leg = 0; leg < 2; ++leg) {
            setup.legs[leg].root = resolve(foot.leg[leg][0]);
            setup.legs[leg].mid = resolve(foot.leg[leg][1]);
            setup.legs[leg].tip = resolve(foot.leg[leg][2]);
            setup.legs[leg].pole = Vec3{foot.pole[0], foot.pole[1], foot.pole[2]};
        }
        setup.ankle_height = foot.ankle_height;
        setup.probe_above = foot.probe_above;
        setup.probe_below = foot.probe_below;
        setup.max_pelvis_drop = foot.max_pelvis_drop;
        setup.max_foot_raise = foot.max_foot_raise;
        setup.smoothing = foot.smoothing;
        setup.max_tilt = foot.max_tilt;
        if (Status pushed = feet_.push_back(setup); !pushed) {
            return pushed;
        }
    }

    aims_.clear();
    for (const graph::pose::AimRef& aim : program.aims()) {
        AimChain chain;
        chain.count = aim.joint_count;
        bool resolved = true;
        for (u32 joint = 0; joint < aim.joint_count; ++joint) {
            chain.joints[joint] = resolve(aim.joints[joint]);
            resolved = resolved && chain.joints[joint] != kInvalidJoint;
        }
        chain.count = resolved ? chain.count : 0;
        chain.axis = Vec3{aim.axis[0], aim.axis[1], aim.axis[2]};
        chain.max_angle = aim.max_angle;
        if (Status pushed = aims_.push_back(chain); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AnimationRig::set_foot_placement(u16 index, const FootPlacementSetup& setup) noexcept {
    if (index >= feet_.size()) {
        return fail(ErrorCode::OutOfRange, "the program has no such foot placement");
    }
    feet_[index] = setup;
    return ok();
}

const FootPlacementSetup* AnimationRig::foot_placement(u16 index) const noexcept {
    return index < feet_.size() && feet_[index].valid() ? &feet_[index] : nullptr;
}

Status AnimationRig::set_aim(u16 index, const AimChain& chain) noexcept {
    if (index >= aims_.size()) {
        return fail(ErrorCode::OutOfRange, "the program has no such aim");
    }
    aims_[index] = chain;
    return ok();
}

const AimChain* AnimationRig::aim(u16 index) const noexcept {
    return index < aims_.size() && aims_[index].valid() ? &aims_[index] : nullptr;
}

// --- The instance's foot state
// --------------------------------------------------------------------

FootPlacementState* AnimationInstance::foot_state(u16 index) noexcept {
    return index < feet_.size() ? &feet_[index] : nullptr;
}

const FootPlacementReport* AnimationInstance::foot_report(u16 index) const noexcept {
    return index < foot_reports_.size() ? &foot_reports_[index] : nullptr;
}

FootPlacementReport* AnimationInstance::foot_report(u16 index) noexcept {
    return index < foot_reports_.size() ? &foot_reports_[index] : nullptr;
}

// --- Playing a space
// ------------------------------------------------------------------------------

namespace detail {

bool space_weights(const AnimationRig& rig, u16 space, Span<const f32> parameters,
                   Span<f32> out) noexcept {
    return graph::pose::blend_space_weights(rig.program(), space, parameters, out);
}

u32 heaviest_sample(const AnimationRig& rig, u16 space, Span<const f32> parameters) noexcept {
    f32 weights[kMaxBlendSamples] = {};
    if (!space_weights(rig, space, parameters, Span<f32>(weights, kMaxBlendSamples))) {
        return 0;
    }
    const u32 count = rig.program().blend_spaces()[space].sample_count;
    u32 best = 0;
    for (u32 index = 1; index < count; ++index) {
        best = weights[index] > weights[best] ? index : best;
    }
    return best;
}

void advance_sync(const AnimationRig& rig, AnimationInstance& instance, f32 step) noexcept {
    const Span<f32> parameters = instance.parameters();
    const Span<const f32> before = instance.previous_times();
    for (const AnimationRig::SyncSpace& space : rig.sync_spaces()) {
        const BlendSpace& authored = rig.program().blend_spaces()[space.space];
        if (authored.phase_param >= parameters.size()) {
            continue;
        }
        f32 weights[kMaxBlendSamples] = {};
        if (!space_weights(rig, space.space, parameters, Span<f32>(weights, kMaxBlendSamples))) {
            continue;
        }
        f32 cadence[kMaxBlendSamples] = {};
        cadence_weights(rig, space, Span<const f32>(weights, kMaxBlendSamples), cadence);

        // Reversed playback holds the space where it is: the phase only runs forwards.
        const f32 from = parameters[authored.phase_param];
        const f32 to = step > 0.0F ? advanced_phase(rig, space, cadence, from, step) : from;
        for (u32 index = 0; index < space.sample_count; ++index) {
            const AnimationRig::SyncSample& sample = rig.sync_samples()[space.first_sample + index];
            const Stride stride = stride_of(rig, space, sample);
            // The clock moves by what the phase moved it, re-anchored to the phase's own mapping
            // each tick so rounding cannot accumulate into drift between the clips.
            const f32 exact = stride.time_at(to) - stride.time_at(from);
            const f32 start = parameter_value(before, sample.time_slot);
            f32 forward = wrap_time(stride.time_at(to), sample.duration) - start;
            forward += sample.duration * std::round((exact - forward) / sample.duration);
            if (sample.time_param < parameters.size()) {
                parameters[sample.time_param] = start + math::max(forward, 0.0F);
            }
        }
        const auto segments = static_cast<f32>(space.segments);
        parameters[authored.phase_param] = to - (std::floor(to / segments) * segments);
    }
}

void restart_sync(const AnimationRig& rig, AnimationInstance& instance, u16 space) noexcept {
    const AnimationRig::SyncSpace* entry = sync_of(rig, space);
    const Span<f32> parameters = instance.parameters();
    const BlendSpace& authored = rig.program().blend_spaces()[space];
    if (entry == nullptr || entry->sample_count == 0 || authored.phase_param >= parameters.size()) {
        return;
    }
    const AnimationRig::SyncSample& first = rig.sync_samples()[entry->first_sample];
    const f32 phase = stride_of(rig, *entry, first).phase_of(0.0F);
    parameters[authored.phase_param] = phase;
    for (u32 index = 0; index < entry->sample_count; ++index) {
        const AnimationRig::SyncSample& sample = rig.sync_samples()[entry->first_sample + index];
        if (sample.time_param < parameters.size()) {
            parameters[sample.time_param] =
                wrap_time(stride_of(rig, *entry, sample).time_at(phase), sample.duration);
        }
    }
}

}  // namespace detail
}  // namespace cy::animation
