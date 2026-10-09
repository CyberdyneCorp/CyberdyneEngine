// SPDX-License-Identifier: MIT
// The characters the animation panel plays a graph on. See cy/editor/animation_character.h.

#include <cy/editor/animation_character.h>

#include <cy/animation/cooked.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace cy::editor {
namespace {

[[nodiscard]] Allocator& clip_allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

namespace animation = cy::animation;

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

/// Give `clip` the events authored for `name`, in time order (a stable sort, so two events at one
/// time fire in the order they were written).
[[nodiscard]] Status add_events(animation::Clip& clip, Name name,
                                Span<const AnimationClipEvent> events) noexcept {
    Array<AnimationClipEvent> mine(clip_allocator());
    for (const AnimationClipEvent& event : events) {
        if (event.clip == name) {
            if (Status pushed = mine.push_back(event); !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::stable_sort(mine, [](const AnimationClipEvent& a, const AnimationClipEvent& b) {
        return a.time < b.time;
    });
    for (const AnimationClipEvent& event : mine) {
        if (Status added = clip.add_event(event.event, event.time); !added) {
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
    if (Status added = add_events(clip, clip.name(), events); !added) {
        return added;
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

/// A small box around a segment from `from` to `to`, bound to `joint`: a bone of a skeleton with
/// no skin. Axis-aligned, which shows where the bone is and how it moves; no more is claimed.
[[nodiscard]] Status add_bone(AnimationPreviewMesh& mesh, u16 joint, Vec3 from, Vec3 to) noexcept {
    constexpr f32 kThickness = 0.025F;
    BoxRow box;
    box.joint = joint;
    box.centre = (to - from) * 0.5F;
    box.half = Vec3{(std::fabs(to.x - from.x) * 0.5F) + kThickness,
                    (std::fabs(to.y - from.y) * 0.5F) + kThickness,
                    (std::fabs(to.z - from.z) * 0.5F) + kThickness};
    return add_box(mesh, box, from);
}

}  // namespace

AnimationCharacter::AnimationCharacter(Allocator& allocator) noexcept
    : allocator_(&allocator),
      skeleton_(allocator),
      joint_names_(allocator),
      infos_(allocator),
      project_clips_(allocator),
      refused_(allocator),
      mesh_(allocator),
      model_(allocator) {}

AnimationCharacter::~AnimationCharacter() = default;

void AnimationCharacter::reset() noexcept {
    skeleton_ = animation::Skeleton(*allocator_);
    joint_names_.clear();
    infos_.clear();
    project_clips_.clear();
    refused_.clear();
    mesh_.clear();
    model_.clear();
    project_ = false;
    skinned_ = false;
}

Status AnimationCharacter::finish_skeleton() noexcept {
    for (const animation::Joint& joint : skeleton_.joints()) {
        if (Status pushed = joint_names_.push_back(joint.name); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AnimationCharacter::load_mannequin() noexcept {
    reset();
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
    if (Status named = finish_skeleton(); !named) {
        return named;
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
    return ok();
}

Status AnimationCharacter::take_clip(const AnimationCharacterClip& wanted,
                                     AnimationAssetSource& source) noexcept {
    const auto refuse = [this, &wanted](const char* reason) {
        return refused_.push_back(AnimationClipRefusal{wanted.name, reason});
    };
    for (const AnimationClipInfo& taken : infos_) {
        if (taken.name == wanted.name) {
            return refuse("another clip of the character already has this name");
        }
    }
    ProjectClip clip(*allocator_);
    clip.name = wanted.name;
    if (Status read = source.read(wanted.id, clip.payload); !read) {
        return refuse("its cooked clip could not be read; import its model again");
    }
    animation::Clip decoded(*allocator_);
    Array<Name> joints(*allocator_);
    if (Status read = animation::decode_clip(clip.payload.span(), decoded, joints); !read) {
        return refuse("its cooked clip does not decode");
    }
    u16 offending = animation::kInvalidJoint;
    if (!animation::clip_matches_skeleton(decoded, joints.span(), skeleton_, offending)) {
        return refuse("it was cooked for another skeleton: its tracks move joints this one lacks");
    }
    AnimationClipInfo info;
    info.name = wanted.name;
    info.duration = decoded.duration();
    info.looping = decoded.loop_mode() == animation::LoopMode::Loop;
    if (Status pushed = infos_.push_back(info); !pushed) {
        return pushed;
    }
    return project_clips_.push_back(std::move(clip));
}

Status AnimationCharacter::bone_boxes() noexcept {
    const Span<const animation::Joint> joints = skeleton_.joints();
    const Span<const Transform> model = skeleton_.bind_model();
    for (usize index = 0; index < joints.size(); ++index) {
        const auto joint = static_cast<u16>(index);
        const Vec3 at = model[joint].translation;
        const u16 parent = joints[joint].parent;
        // A joint with no parent is a small cube; every other one is the bone from its parent.
        const Vec3 from = parent == animation::kInvalidJoint ? at : model[parent].translation;
        const u16 owner = parent == animation::kInvalidJoint ? joint : parent;
        if (Status added = add_bone(mesh_, owner, from, at); !added) {
            return added;
        }
    }
    return ok();
}

Status AnimationCharacter::load(const AnimationCharacterRequest& request,
                                AnimationAssetSource& source) noexcept {
    if (request.skeleton.is_nil()) {
        return load_mannequin();
    }
    reset();
    project_ = true;
    if (Status named = model_.append({request.model.data(), request.model.size()}); !named) {
        return named;
    }
    Array<u8> payload(*allocator_);
    if (Status read = source.read(request.skeleton, payload); !read) {
        reset();
        return fail(ErrorCode::NotFound,
                    "the character's cooked skeleton could not be read; import its model again");
    }
    animation::SkeletonProfile humanoid;
    if (Status decoded = animation::decode_skeleton(payload.span(), skeleton_, humanoid);
        !decoded) {
        reset();
        return fail(ErrorCode::InvalidArgument, "the character's cooked skeleton does not decode");
    }
    if (Status named = finish_skeleton(); !named) {
        reset();
        return named;
    }
    for (const AnimationCharacterClip& wanted : request.clips) {
        if (Status taken = take_clip(wanted, source); !taken) {
            reset();
            return taken;
        }
    }
    if (request.mesh.is_nil()) {
        if (Status boxed = bone_boxes(); !boxed) {
            reset();
            return boxed;
        }
        return ok();
    }
    if (Status read = source.read_mesh(request.mesh, mesh_); !read) {
        reset();
        return fail(ErrorCode::InvalidArgument,
                    "the character's mesh could not be read as a skinned mesh of its skeleton");
    }
    const bool bound = std::ranges::all_of(
        mesh_.joints, [this](const u16 joint) { return joint < skeleton_.joint_count(); });
    if (!bound || mesh_.joints.size() != mesh_.positions.size() * 4U ||
        mesh_.weights.size() != mesh_.positions.size() * 4U) {
        reset();
        return fail(ErrorCode::InvalidArgument,
                    "the character's mesh is bound to joints its skeleton does not have");
    }
    skinned_ = true;
    return ok();
}

Status AnimationCharacter::build_clips(Span<const AnimationClipEvent> events,
                                       Array<animation::Clip>& out) const noexcept {
    out.clear();
    if (!project_) {
        for (const ClipRow& row : kClips) {
            animation::Clip clip(*allocator_);
            if (Status authored = author_clip(clip, row, events); !authored) {
                return authored;
            }
            if (Status pushed = out.push_back(std::move(clip)); !pushed) {
                return pushed;
            }
        }
        return ok();
    }
    for (const ProjectClip& source : project_clips_) {
        animation::Clip clip(*allocator_);
        Array<Name> joints(*allocator_);
        if (Status decoded = animation::decode_clip(source.payload.span(), clip, joints);
            !decoded) {
            return decoded;
        }
        clip.set_name(source.name);
        clip.clear_events();
        if (Status added = add_events(clip, source.name, events); !added) {
            return added;
        }
        if (Status pushed = out.push_back(std::move(clip)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::editor
