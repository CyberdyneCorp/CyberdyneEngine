// SPDX-License-Identifier: MIT
// The animation panel's preview character. See cy/editor/animation_preview.h.

#include <cy/editor/animation_preview.h>

#include <cy/core/math/quat.h>

#include <cmath>
#include <iterator>
#include <utility>

namespace cy::editor {
namespace {

namespace animation = cy::animation;
namespace pose = graph::pose;

enum Joint : u16 {
    kRoot,
    kHips,
    kSpine,
    kHead,
    kUpperArmLeft,
    kForearmLeft,
    kUpperArmRight,
    kForearmRight,
    kThighLeft,
    kShinLeft,
    kThighRight,
    kShinRight,
    kJoints
};

struct JointRow {
    const char* name = nullptr;
    u16 parent = 0;
    Vec3 offset;
};

constexpr JointRow kSkeleton[kJoints] = {
    {"root", animation::kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}},
    {"hips", kRoot, Vec3{0.0F, 0.95F, 0.0F}},
    {"spine", kHips, Vec3{0.0F, 0.18F, 0.0F}},
    {"head", kSpine, Vec3{0.0F, 0.5F, 0.0F}},
    {"upper_arm_left", kSpine, Vec3{-0.24F, 0.42F, 0.0F}},
    {"forearm_left", kUpperArmLeft, Vec3{0.0F, -0.3F, 0.0F}},
    {"upper_arm_right", kSpine, Vec3{0.24F, 0.42F, 0.0F}},
    {"forearm_right", kUpperArmRight, Vec3{0.0F, -0.3F, 0.0F}},
    {"thigh_left", kHips, Vec3{-0.11F, 0.0F, 0.0F}},
    {"shin_left", kThighLeft, Vec3{0.0F, -0.46F, 0.0F}},
    {"thigh_right", kHips, Vec3{0.11F, 0.0F, 0.0F}},
    {"shin_right", kThighRight, Vec3{0.0F, -0.46F, 0.0F}},
};

/// One box of the mesh: the bone it moves with, its centre relative to that joint's bind position,
/// and its half extents.
struct BoxRow {
    u16 joint = 0;
    Vec3 centre;
    Vec3 half;
};

constexpr BoxRow kBoxes[] = {
    {kHips, Vec3{0.0F, 0.06F, 0.0F}, Vec3{0.16F, 0.1F, 0.1F}},
    {kSpine, Vec3{0.0F, 0.24F, 0.0F}, Vec3{0.19F, 0.24F, 0.11F}},
    {kHead, Vec3{0.0F, 0.13F, 0.0F}, Vec3{0.1F, 0.13F, 0.11F}},
    {kUpperArmLeft, Vec3{0.0F, -0.15F, 0.0F}, Vec3{0.05F, 0.15F, 0.05F}},
    {kForearmLeft, Vec3{0.0F, -0.14F, 0.0F}, Vec3{0.045F, 0.14F, 0.045F}},
    {kUpperArmRight, Vec3{0.0F, -0.15F, 0.0F}, Vec3{0.05F, 0.15F, 0.05F}},
    {kForearmRight, Vec3{0.0F, -0.14F, 0.0F}, Vec3{0.045F, 0.14F, 0.045F}},
    {kThighLeft, Vec3{0.0F, -0.23F, 0.0F}, Vec3{0.07F, 0.23F, 0.07F}},
    {kShinLeft, Vec3{0.0F, -0.22F, 0.02F}, Vec3{0.06F, 0.24F, 0.08F}},
    {kThighRight, Vec3{0.0F, -0.23F, 0.0F}, Vec3{0.07F, 0.23F, 0.07F}},
    {kShinRight, Vec3{0.0F, -0.22F, 0.02F}, Vec3{0.06F, 0.24F, 0.08F}},
};

enum class Motion : u8 { Idle, Walk, Run, Wave };

struct ClipRow {
    const char* name;
    f32 duration;
    bool looping;
    Motion motion;
};

constexpr ClipRow kClips[] = {
    {"idle", 2.0F, true, Motion::Idle},
    {"walk", 1.0F, true, Motion::Walk},
    {"run", 0.6F, true, Motion::Run},
    {"wave", 1.5F, false, Motion::Wave},
};

constexpr f32 kTau = 6.28318530718F;
constexpr f32 kKeyRate = 30.0F;
constexpr Vec3 kAcross{1.0F, 0.0F, 0.0F};
constexpr Vec3 kForward{0.0F, 0.0F, 1.0F};

/// One joint's rotation about one axis, as a function of time.
struct Swing {
    u16 joint = 0;
    Vec3 axis;
    f32 (*angle)(f32 time) = nullptr;
};

f32 idle_spine(f32 time) {
    return 0.05F * std::sin(time * kTau * 0.5F);
}
f32 idle_arm_left(f32 time) {
    return -0.08F - (0.02F * std::sin(time * kTau * 0.5F));
}
f32 idle_arm_right(f32 time) {
    return 0.08F + (0.02F * std::sin(time * kTau * 0.5F));
}
f32 idle_head(f32 time) {
    return 0.04F * std::sin((time * kTau * 0.5F) + 1.0F);
}

/// The stride, as a phase from 0 to 1 over a clip's length.
f32 stride(f32 phase, f32 amplitude, f32 offset) {
    return amplitude * std::sin((phase + offset) * kTau);
}
f32 knee(f32 phase, f32 amplitude, f32 offset) {
    return amplitude * std::fmax(0.0F, std::sin((phase + offset + 0.25F) * kTau));
}

f32 walk_thigh_left(f32 time) {
    return stride(time, 0.5F, 0.0F);
}
f32 walk_thigh_right(f32 time) {
    return stride(time, 0.5F, 0.5F);
}
f32 walk_shin_left(f32 time) {
    return knee(time, 0.6F, 0.0F);
}
f32 walk_shin_right(f32 time) {
    return knee(time, 0.6F, 0.5F);
}
f32 walk_arm_left(f32 time) {
    return stride(time, 0.4F, 0.5F);
}
f32 walk_arm_right(f32 time) {
    return stride(time, 0.4F, 0.0F);
}
f32 walk_forearm(f32 /*time*/) {
    return -0.3F;
}

constexpr f32 kRunLength = 0.6F;
f32 run_thigh_left(f32 time) {
    return stride(time / kRunLength, 0.9F, 0.0F);
}
f32 run_thigh_right(f32 time) {
    return stride(time / kRunLength, 0.9F, 0.5F);
}
f32 run_shin_left(f32 time) {
    return knee(time / kRunLength, 1.2F, 0.0F);
}
f32 run_shin_right(f32 time) {
    return knee(time / kRunLength, 1.2F, 0.5F);
}
f32 run_arm_left(f32 time) {
    return stride(time / kRunLength, 0.8F, 0.5F);
}
f32 run_arm_right(f32 time) {
    return stride(time / kRunLength, 0.8F, 0.0F);
}
f32 run_forearm(f32 /*time*/) {
    return -1.2F;
}

f32 wave_arm(f32 time) {
    return 2.6F * std::fmin(time / 0.4F, 1.0F);
}
f32 wave_forearm(f32 time) {
    return time < 0.4F ? 0.0F : 0.45F * std::sin((time - 0.4F) * kTau * 2.0F);
}

constexpr Swing kIdle[] = {
    {kSpine, kForward, &idle_spine},
    {kHead, kAcross, &idle_head},
    {kUpperArmLeft, kForward, &idle_arm_left},
    {kUpperArmRight, kForward, &idle_arm_right},
};
constexpr Swing kWalk[] = {
    {kThighLeft, kAcross, &walk_thigh_left},  {kThighRight, kAcross, &walk_thigh_right},
    {kShinLeft, kAcross, &walk_shin_left},    {kShinRight, kAcross, &walk_shin_right},
    {kUpperArmLeft, kAcross, &walk_arm_left}, {kUpperArmRight, kAcross, &walk_arm_right},
    {kForearmLeft, kAcross, &walk_forearm},   {kForearmRight, kAcross, &walk_forearm},
};
constexpr Swing kRun[] = {
    {kThighLeft, kAcross, &run_thigh_left},  {kThighRight, kAcross, &run_thigh_right},
    {kShinLeft, kAcross, &run_shin_left},    {kShinRight, kAcross, &run_shin_right},
    {kUpperArmLeft, kAcross, &run_arm_left}, {kUpperArmRight, kAcross, &run_arm_right},
    {kForearmLeft, kAcross, &run_forearm},   {kForearmRight, kAcross, &run_forearm},
};
constexpr Swing kWave[] = {
    {kUpperArmRight, kForward, &wave_arm},
    {kForearmRight, kForward, &wave_forearm},
};

[[nodiscard]] Span<const Swing> swings_of(Motion motion) noexcept {
    switch (motion) {
        case Motion::Idle:
            return {kIdle, std::size(kIdle)};
        case Motion::Walk:
            return {kWalk, std::size(kWalk)};
        case Motion::Run:
            return {kRun, std::size(kRun)};
        case Motion::Wave:
            return {kWave, std::size(kWave)};
    }
    return {};
}

[[nodiscard]] Status key_swing(animation::Clip& clip, const Swing& swing) noexcept {
    Expected<u32, Error> track = clip.add_joint_track(animation::TrackKind::Rotation, swing.joint,
                                                      animation::Interpolation::Spherical);
    if (!track) {
        return make_unexpected(track.error());
    }
    const auto frames = static_cast<u32>(std::lround(clip.duration() * kKeyRate)) + 1U;
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = std::fmin(static_cast<f32>(frame) / kKeyRate, clip.duration());
        const Quat rotation = Quat::from_axis_angle(swing.axis, swing.angle(time));
        if (Status added =
                clip.add_key(*track, time, Vec4{rotation.x, rotation.y, rotation.z, rotation.w});
            !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status author_clip(animation::Clip& clip, const ClipRow& row,
                                 Span<const AnimationClipEvent> events) noexcept {
    clip.set_name(Name::intern(row.name));
    clip.set_duration(row.duration);
    clip.set_loop_mode(row.looping ? animation::LoopMode::Loop : animation::LoopMode::None);
    clip.set_sample_rate_hint(kKeyRate);
    for (const Swing& swing : swings_of(row.motion)) {
        if (Status keyed = key_swing(clip, swing); !keyed) {
            return keyed;
        }
    }
    for (const AnimationClipEvent& event : events) {
        if (event.clip != clip.name()) {
            continue;
        }
        if (Status added = clip.add_event(event.event, event.time); !added) {
            return added;
        }
    }
    return clip.compress(animation::CompressionSettings{});
}

/// One face of a box: four corners counter-clockwise seen from outside, and two triangles.
[[nodiscard]] Status add_face(AnimationPreviewMesh& mesh, u16 joint, Vec3 centre, Vec3 normal,
                              Vec3 u, Vec3 v) noexcept {
    const auto first = static_cast<u32>(mesh.positions.size());
    const Vec3 corners[4] = {centre - u - v, centre + u - v, centre + u + v, centre - u + v};
    for (const Vec3& corner : corners) {
        if (!mesh.positions.push_back(corner) || !mesh.normals.push_back(normal)) {
            return fail(ErrorCode::OutOfMemory, "the preview mesh did not allocate");
        }
        const u16 joints[4] = {joint, 0, 0, 0};
        const f32 weights[4] = {1.0F, 0.0F, 0.0F, 0.0F};
        if (!mesh.joints.append({joints, 4}) || !mesh.weights.append({weights, 4})) {
            return fail(ErrorCode::OutOfMemory, "the preview mesh did not allocate");
        }
    }
    const u32 triangles[6] = {first, first + 1, first + 2, first, first + 2, first + 3};
    return mesh.indices.append({triangles, 6});
}

[[nodiscard]] Status add_box(AnimationPreviewMesh& mesh, const BoxRow& box,
                             Vec3 joint_position) noexcept {
    const Vec3 centre = joint_position + box.centre;
    const Vec3 x{box.half.x, 0.0F, 0.0F};
    const Vec3 y{0.0F, box.half.y, 0.0F};
    const Vec3 z{0.0F, 0.0F, box.half.z};
    struct Face {
        Vec3 normal;
        Vec3 offset;
        Vec3 u;
        Vec3 v;
    };
    // u x v points along the normal on every face, so each is counter-clockwise from outside.
    const Face faces[6] = {
        {Vec3{1.0F, 0.0F, 0.0F}, x, Vec3{0.0F, 0.0F, -box.half.z}, y},
        {Vec3{-1.0F, 0.0F, 0.0F}, x * -1.0F, z, y},
        {Vec3{0.0F, 1.0F, 0.0F}, y, x, Vec3{0.0F, 0.0F, -box.half.z}},
        {Vec3{0.0F, -1.0F, 0.0F}, y * -1.0F, x, z},
        {Vec3{0.0F, 0.0F, 1.0F}, z, x, y},
        {Vec3{0.0F, 0.0F, -1.0F}, z * -1.0F, x * -1.0F, y},
    };
    for (const Face& face : faces) {
        if (Status added =
                add_face(mesh, box.joint, centre + face.offset, face.normal, face.u, face.v);
            !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] bool same_events(Span<const AnimationClipEvent> a,
                               Span<const AnimationClipEvent> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index].clip != b[index].clip || a[index].event != b[index].event ||
            a[index].time != b[index].time) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_parameters(Span<const AnimationParameter> a,
                                   Span<const AnimationParameter> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index].name != b[index].name || a[index].value != b[index].value) {
            return false;
        }
    }
    return true;
}

}  // namespace

AnimationPreview::AnimationPreview(Allocator& allocator) noexcept
    : allocator_(&allocator),
      skeleton_(allocator),
      clips_(allocator),
      infos_(allocator),
      events_(allocator),
      mesh_(allocator),
      program_(allocator),
      buffer_(allocator),
      parameters_(allocator),
      local_(allocator),
      model_(allocator),
      skinning_(allocator),
      fired_(allocator) {}

AnimationPreview::~AnimationPreview() = default;

Status AnimationPreview::initialize() noexcept {
    skeleton_.set_name(Name::intern("preview_mannequin"));
    Vec3 positions[kJoints] = {};
    for (u16 joint = 0; joint < kJoints; ++joint) {
        const JointRow& row = kSkeleton[joint];
        Expected<u16, Error> added = skeleton_.add_joint(Name::intern(row.name), row.parent,
                                                         Transform::from_translation(row.offset));
        if (!added) {
            return make_unexpected(added.error());
        }
        positions[joint] = row.parent == animation::kInvalidJoint
                               ? row.offset
                               : positions[row.parent] + row.offset;
    }
    if (Status finalized = skeleton_.finalize(); !finalized) {
        return finalized;
    }
    for (const BoxRow& box : kBoxes) {
        if (Status added = add_box(mesh_, box, positions[box.joint]); !added) {
            return added;
        }
    }
    for (const ClipRow& row : kClips) {
        AnimationClipInfo info;
        info.name = Name::intern(row.name);
        info.duration = row.duration;
        info.looping = row.looping;
        if (Status pushed = infos_.push_back(info); !pushed) {
            return pushed;
        }
    }
    if (Status built = rebuild_clips({}); !built) {
        return built;
    }
    const usize joints = skeleton_.joint_count();
    if (Status sized = local_.resize(joints); !sized) {
        return sized;
    }
    if (Status sized = model_.resize(joints); !sized) {
        return sized;
    }
    if (Status sized = skinning_.resize(joints); !sized) {
        return sized;
    }
    skeleton_.reference_pose(local_.span());
    initialized_ = true;
    return finish();
}

Span<const AnimationClipInfo> AnimationPreview::clips() const noexcept {
    return infos_.span();
}

u32 AnimationPreview::joint_count() const noexcept {
    return skeleton_.joint_count();
}

const animation::Clip* AnimationPreview::clip(Name name) const noexcept {
    for (const animation::Clip& candidate : clips_) {
        if (candidate.name() == name) {
            return &candidate;
        }
    }
    return nullptr;
}

Status AnimationPreview::rebuild_clips(Span<const AnimationClipEvent> events) noexcept {
    // The rig points at the clips: drop it before they move.
    rig_.reset();
    instance_.reset();
    scratch_.reset();
    clips_.clear();
    for (const ClipRow& row : kClips) {
        animation::Clip clip(*allocator_);
        if (Status authored = author_clip(clip, row, events); !authored) {
            return authored;
        }
        if (Status pushed = clips_.push_back(std::move(clip)); !pushed) {
            return pushed;
        }
    }
    events_.clear();
    return events_.append(events);
}

Status AnimationPreview::bind(pose::PoseProgram&& program) noexcept {
    program_ = std::move(program);
    Array<const animation::Clip*> table(*allocator_);
    for (const pose::ClipRef& reference : program_.clips()) {
        if (Status pushed = table.push_back(clip(reference.name)); !pushed) {
            return pushed;
        }
    }
    rig_ = std::make_unique<animation::AnimationRig>(*allocator_);
    instance_ = std::make_unique<animation::AnimationInstance>(*allocator_);
    scratch_ = std::make_unique<animation::PoseScratch>(*allocator_);
    cursor_ = std::make_unique<animation::ClipCursor>(*allocator_);
    if (Status bound = rig_->bind(skeleton_, program_, table.span()); !bound) {
        return bound;
    }
    return scratch_->prepare(*rig_);
}

Status AnimationPreview::preview(pose::PoseProgram&& program,
                                 const AnimationPreviewRequest& request) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the preview character was never built");
    }
    const bool events_kept = same_events(events_.span(), request.events);
    const bool continuing =
        state_.active && program.digest() == state_.program_digest && events_kept &&
        same_parameters(parameters_.span(), request.parameters) && request.focus == state_.focus &&
        request.focus_clip == state_.focus_clip && request.time >= state_.time;
    const f32 previous = state_.time;
    if (!events_kept) {
        if (Status rebuilt = rebuild_clips(request.events); !rebuilt) {
            return rebuilt;
        }
    }
    parameters_.clear();
    if (Status copied = parameters_.append(request.parameters); !copied) {
        return copied;
    }
    state_.program_digest = program.digest();
    if (Status bound = bind(std::move(program)); !bound) {
        return bound;
    }
    state_.active = true;
    state_.playing = request.playing;
    state_.focus = request.focus;
    state_.focus_clip = request.focus_clip;
    pending_ = 0.0F;
    if (request.focus != 0) {
        const animation::Clip* focused = clip(request.focus_clip);
        if (focused == nullptr) {
            return fail(ErrorCode::NotFound, "the preview character has no clip of that name");
        }
        focus_clip_ = static_cast<u32>(focused - clips_.data());
        state_.length = focused->duration();
        return show_clip(std::fmin(request.time, focused->duration()), previous, continuing);
    }
    state_.length = kAnimationGraphPreviewSeconds;
    if (Status restarted = restart_machine(); !restarted) {
        return restarted;
    }
    if (Status ran = run_machine(request.time, continuing ? previous : request.time); !ran) {
        return ran;
    }
    state_.time = request.time;
    return evaluate_machine();
}

void AnimationPreview::stop() noexcept {
    state_.active = false;
    state_.playing = false;
    pending_ = 0.0F;
}

Status AnimationPreview::show_clip(f32 time, f32 previous, bool continuing) noexcept {
    const animation::Clip& focused = clips_[focus_clip_];
    state_.time = time;
    state_.state = 0xFFFFU;
    state_.state_name = Name{};
    state_.target = 0xFFFFU;
    state_.target_name = Name{};
    state_.blend = 0.0F;
    if (continuing && time > previous) {
        buffer_.clear();
        if (Status emitted = animation::emit_events(focused, 0, previous, time,
                                                    animation::EventPolicy::Emit, buffer_);
            !emitted) {
            return emitted;
        }
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, event.normalised_time * focused.duration());
        }
    }
    skeleton_.reference_pose(local_.span());
    if (Status reset = cursor_->reset(focused.track_count()); !reset) {
        return reset;
    }
    animation::SampleStats stats;
    if (Status sampled =
            focused.sample_unwrapped(time, skeleton_.retained(0), *cursor_, local_.span(), stats);
        !sampled) {
        return sampled;
    }
    return finish();
}

Status AnimationPreview::restart_machine() noexcept {
    if (Status prepared = instance_->prepare(*rig_); !prepared) {
        return prepared;
    }
    for (const AnimationParameter& parameter : parameters_) {
        // A parameter the program does not read changes nothing: the panel may still hold one an
        // edit removed.
        (void)instance_->set_parameter(*rig_, parameter.name, parameter.value);
    }
    state_.time = 0.0F;
    return ok();
}

Status AnimationPreview::run_machine(f32 seconds, f32 report_after) noexcept {
    f32 remaining = seconds;
    while (remaining > 0.0F) {
        const f32 step = std::fmin(kAnimationPreviewStep, remaining);
        if (Status stepped = step_machine(step, report_after); !stepped) {
            return stepped;
        }
        remaining -= step;
    }
    return ok();
}

Status AnimationPreview::step_machine(f32 step, f32 report_after) noexcept {
    buffer_.clear();
    if (Status advanced = animation::advance(*rig_, *instance_, step, &buffer_); !advanced) {
        return advanced;
    }
    state_.time += step;
    if (state_.time > report_after) {
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, state_.time);
        }
    }
    return ok();
}

Status AnimationPreview::evaluate_machine() noexcept {
    skeleton_.reference_pose(local_.span());
    animation::EvaluationStats stats;
    if (Status evaluated =
            animation::evaluate(*rig_, *instance_, 0, *scratch_, local_.span(), stats);
        !evaluated) {
        return evaluated;
    }
    const pose::PoseInstance& machine = instance_->machine();
    const Span<const pose::PoseState> states = program_.states();
    state_.state = machine.state;
    state_.state_name = machine.state < states.size() ? states[machine.state].name : Name{};
    state_.target = machine.target;
    state_.target_name = machine.target < states.size() ? states[machine.target].name : Name{};
    const f32 duration = pose::transition_duration(program_, machine);
    state_.blend = machine.target != 0xFFFFU && duration > 0.0F
                       ? std::fmin(machine.blend_elapsed / duration, 1.0F)
                       : 0.0F;
    return finish();
}

Status AnimationPreview::tick(f32 seconds) noexcept {
    if (!state_.active || !state_.playing || !(seconds > 0.0F)) {
        return ok();
    }
    if (state_.focus != 0) {
        const animation::Clip& focused = clips_[focus_clip_];
        const f32 previous = state_.time;
        f32 next = previous + seconds;
        if (focused.loop_mode() == animation::LoopMode::None) {
            next = std::fmin(next, focused.duration());
            state_.playing = next < focused.duration();
        }
        buffer_.clear();
        if (Status emitted = animation::emit_events(focused, 0, previous, next,
                                                    animation::EventPolicy::Emit, buffer_);
            !emitted) {
            return emitted;
        }
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, event.normalised_time * focused.duration());
        }
        const f32 wrapped = focused.loop_mode() == animation::LoopMode::None
                                ? next
                                : std::fmod(next, focused.duration());
        return show_clip(wrapped, wrapped, false);
    }
    pending_ += seconds;
    while (pending_ >= kAnimationPreviewStep) {
        pending_ -= kAnimationPreviewStep;
        if (state_.time + kAnimationPreviewStep > kAnimationGraphPreviewSeconds) {
            if (Status restarted = restart_machine(); !restarted) {
                return restarted;
            }
        }
        if (Status stepped = step_machine(kAnimationPreviewStep, -1.0F); !stepped) {
            return stepped;
        }
    }
    return evaluate_machine();
}

void AnimationPreview::remember(const animation::EmittedEvent& event, f32 at) noexcept {
    if (fired_.size() >= kAnimationPreviewEvents) {
        fired_.erase(0);
    }
    AnimationFiredEvent fired;
    fired.sequence = ++sequence_;
    fired.name = event.name;
    fired.normalised_time = event.normalised_time;
    fired.at = at;
    (void)fired_.push_back(fired);
}

Status AnimationPreview::finish() noexcept {
    const animation::JointMask& every = skeleton_.retained(0);
    skeleton_.to_model(local_.span(), every, model_.span());
    skeleton_.to_skinning(model_.span(), every, skinning_.span());
    state_.pose_digest = animation_pose_digest(local_.span());
    ++state_.generation;
    return ok();
}

}  // namespace cy::editor
