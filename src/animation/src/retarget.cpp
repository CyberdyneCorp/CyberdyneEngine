#include <cy/animation/retarget.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

constexpr const char* kChainNames[] = {"Root",    "Spine",    "Neck",    "Head",
                                       "LeftArm", "RightArm", "LeftLeg", "RightLeg"};

static_assert(sizeof(kChainNames) / sizeof(kChainNames[0]) == static_cast<usize>(Chain::Count),
              "every chain has a spelling");

[[nodiscard]] Quat safe_conjugate(const Quat& q) noexcept {
    return length_squared(q) <= math::kSmallLength ? Quat::identity() : conjugate(normalize(q));
}

/// The model-space placement of a joint under a local pose.
[[nodiscard]] Transform model_of(const Skeleton& skeleton, u16 joint,
                                 Span<const Transform> local) noexcept {
    Transform accumulated = Transform::identity();
    u16 cursor = joint;
    while (cursor != kInvalidJoint && cursor < local.size()) {
        accumulated = local[cursor] * accumulated;
        cursor = skeleton.joints()[cursor].parent;
    }
    return accumulated;
}

}  // namespace

const char* chain_name(Chain chain) noexcept {
    const auto index = static_cast<usize>(chain);
    return index < static_cast<usize>(Chain::Count) ? kChainNames[index] : "?";
}

RetargetProfile::RetargetProfile(Allocator& allocator) noexcept : entries_(allocator) {}

Status RetargetProfile::map_chain(Chain chain, Span<const JointPair> pairs) noexcept {
    for (const JointPair& pair : pairs) {
        Entry entry;
        entry.pair = pair;
        entry.chain = chain;
        // Only the root chain carries translation across. Every other joint keeps the target's own
        // bone length, which is what keeps the target's silhouette its own.
        entry.root_translation = chain == Chain::Root;
        if (Status pushed = entries_.push_back(entry); !pushed) {
            return pushed;
        }
    }
    built_ = false;
    return ok();
}

Status RetargetProfile::build(const Skeleton& source, const Skeleton& target,
                              RetargetReport& report) noexcept {
    if (!source.finalized() || !target.finalized()) {
        return fail(ErrorCode::InvalidArgument,
                    "both skeletons are finalized before a profile is "
                    "built over them");
    }
    report = RetargetReport{};
    bool chains[static_cast<usize>(Chain::Count)] = {};

    f32 source_height = 0.0F;
    f32 target_height = 0.0F;
    for (Entry& entry : entries_) {
        if (entry.pair.source >= source.joint_count()) {
            ++report.joints_unmapped_source;
            continue;
        }
        if (entry.pair.target >= target.joint_count()) {
            ++report.joints_unmapped_target;
            continue;
        }
        const Transform& source_rest = source.bind_model()[entry.pair.source];
        const Transform& target_rest = target.bind_model()[entry.pair.target];
        // What takes the source's rest orientation to the target's. A pose is transferred as
        // `target_model = source_model * offset`, so two skeletons whose rest poses differ by a
        // ninety-degree twist do not transfer that twist into every frame.
        entry.offset = normalize(safe_conjugate(source_rest.rotation) * target_rest.rotation);
        ++report.joints_mapped;
        chains[static_cast<usize>(entry.chain)] = true;
        if (entry.root_translation) {
            source_height = math::max(source_height, source_rest.translation.y);
            target_height = math::max(target_height, target_rest.translation.y);
        }
    }
    for (const bool mapped : chains) {
        report.chains_mapped += mapped ? 1U : 0U;
    }
    height_scale_ = source_height > math::kSmallLength ? target_height / source_height : 1.0F;
    report.height_scale = height_scale_;
    report_ = report;
    built_ = true;
    return ok();
}

Status RetargetProfile::retarget_pose(const Skeleton& source, const Skeleton& target,
                                      Span<const Transform> source_local,
                                      Span<Transform> target_local) const noexcept {
    if (!built_) {
        return fail(ErrorCode::InvalidArgument, "the profile has not been built");
    }
    if (source_local.size() < source.joint_count() || target_local.size() < target.joint_count()) {
        return fail(ErrorCode::BufferTooSmall, "a pose is one transform per joint");
    }
    // Entries are applied in the target's joint order so a parent's model rotation is already the
    // retargeted one when its child is written.
    for (u16 joint = 0; joint < target.joint_count(); ++joint) {
        for (const Entry& entry : entries_) {
            if (entry.pair.target != joint || entry.pair.source >= source.joint_count()) {
                continue;
            }
            const Transform source_model = model_of(source, entry.pair.source, source_local);
            const u16 parent = target.joints()[joint].parent;
            const Quat parent_rotation = parent == kInvalidJoint
                                             ? Quat::identity()
                                             : model_of(target, parent, target_local).rotation;
            const Quat wanted = normalize(source_model.rotation * entry.offset);
            target_local[joint].rotation = normalize(safe_conjugate(parent_rotation) * wanted);
            if (entry.root_translation) {
                target_local[joint].translation =
                    source_local[entry.pair.source].translation * height_scale_;
            }
            break;
        }
    }
    return ok();
}

bool RetargetProfile::maps_target(u16 target_joint, bool& carries_translation) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.pair.target == target_joint) {
            carries_translation = entry.root_translation;
            return true;
        }
    }
    carries_translation = false;
    return false;
}

u16 RetargetProfile::source_of(u16 target_joint) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.pair.target == target_joint) {
            return entry.pair.source;
        }
    }
    return kInvalidJoint;
}

namespace {

/// Sample the source clip at `time` and map it onto the target, into `target_local`.
[[nodiscard]] Status bake_frame(const RetargetProfile& profile, const Skeleton& source,
                                const Skeleton& target, const Clip& clip, f32 time,
                                ClipCursor& cursor, Span<Transform> source_local,
                                Span<Transform> target_local) noexcept {
    source.reference_pose(source_local);
    SampleStats stats;
    if (Status sampled =
            clip.sample(time, JointMask::all(source.joint_count()), cursor, source_local, stats);
        !sampled) {
        return sampled;
    }
    target.reference_pose(target_local);
    return profile.retarget_pose(source, target, source_local, target_local);
}

/// One frame's worth of the target's local pose, in the flat frame-major array the authoring pass
/// reads back.
struct BakedFrames {
    Span<const Transform> poses;
    u16 joints = 0;
    u32 frames = 0;
    f32 sample_rate = 30.0F;

    [[nodiscard]] const Transform& at(u32 frame, u16 joint) const noexcept {
        return poses[(static_cast<usize>(frame) * joints) + joint];
    }
    [[nodiscard]] f32 time_of(u32 frame) const noexcept {
        return static_cast<f32>(frame) / sample_rate;
    }
};

/// Sample every frame of the source clip and map it onto the target, into `baked`.
[[nodiscard]] Status bake_frames(Allocator& allocator, const RetargetProfile& profile,
                                 const Skeleton& source, const Skeleton& target, const Clip& clip,
                                 const BakedFrames& layout, Span<Transform> baked) noexcept {
    Array<Transform> source_local(allocator);
    Array<Transform> target_local(allocator);
    if (Status sized = source_local.resize(source.joint_count()); !sized) {
        return sized;
    }
    if (Status sized = target_local.resize(layout.joints); !sized) {
        return sized;
    }
    ClipCursor cursor(allocator);
    for (u32 frame = 0; frame < layout.frames; ++frame) {
        if (Status ran = bake_frame(profile, source, target, clip, layout.time_of(frame), cursor,
                                    source_local.span(), target_local.span());
            !ran) {
            return ran;
        }
        for (u16 joint = 0; joint < layout.joints; ++joint) {
            baked[(static_cast<usize>(frame) * layout.joints) + joint] = target_local[joint];
        }
    }
    return ok();
}

/// Author one joint's rotation or translation track from the baked frames.
[[nodiscard]] Status author_track(Clip& out, TrackKind kind, u16 joint,
                                  const BakedFrames& frames) noexcept {
    const Interpolation interpolation =
        kind == TrackKind::Rotation ? Interpolation::Spherical : Interpolation::Linear;
    Expected<u32, Error> track = out.add_joint_track(kind, joint, interpolation);
    if (!track) {
        return Status{make_unexpected(track.error())};
    }
    for (u32 frame = 0; frame < frames.frames; ++frame) {
        const Transform& pose = frames.at(frame, joint);
        const Vec4 value =
            kind == TrackKind::Rotation
                ? Vec4{pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w}
                : Vec4{pose.translation.x, pose.translation.y, pose.translation.z, 0.0F};
        if (Status added = out.add_key(*track, frames.time_of(frame), value); !added) {
            return added;
        }
    }
    return ok();
}

/// The target joint the source's root motion joint maps to, or `kInvalidJoint`.
[[nodiscard]] u16 mapped_root(const RetargetProfile& profile, u16 source_root,
                              u16 target_joints) noexcept {
    for (u16 joint = 0; joint < target_joints; ++joint) {
        if (profile.source_of(joint) == source_root) {
            return joint;
        }
    }
    return kInvalidJoint;
}

}  // namespace

Status bake_clip(Allocator& allocator, const RetargetProfile& profile, const Skeleton& source,
                 const Skeleton& target, const Clip& clip, f32 sample_rate,
                 const CompressionSettings& settings, Clip& out) noexcept {
    if (!profile.built()) {
        return fail(ErrorCode::InvalidArgument, "the profile has not been built");
    }
    if (!clip.compressed()) {
        return fail(ErrorCode::InvalidArgument, "a clip is baked from its compressed form");
    }
    if (sample_rate <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "a sample rate is positive");
    }

    BakedFrames frames;
    frames.joints = target.joint_count();
    frames.frames =
        static_cast<u32>(math::max(2.0F, std::floor(clip.duration() * sample_rate) + 1.0F));
    frames.sample_rate = sample_rate;

    Array<Transform> baked(allocator);
    if (Status sized = baked.resize(static_cast<usize>(frames.frames) * frames.joints); !sized) {
        return sized;
    }
    if (Status ran = bake_frames(allocator, profile, source, target, clip, frames, baked.span());
        !ran) {
        return ran;
    }
    frames.poses = baked.span();

    out.set_name(clip.name());
    out.set_duration(clip.duration());
    out.set_loop_mode(clip.loop_mode());
    out.set_sample_rate_hint(sample_rate);

    for (u16 joint = 0; joint < frames.joints; ++joint) {
        bool translation = false;
        if (!profile.maps_target(joint, translation)) {
            continue;
        }
        if (Status authored = author_track(out, TrackKind::Rotation, joint, frames); !authored) {
            return authored;
        }
        if (translation) {
            if (Status authored = author_track(out, TrackKind::Translation, joint, frames);
                !authored) {
                return authored;
            }
        }
    }

    // The root motion track follows the mapping like every other joint: the target's own root joint
    // carries it, and the height scale has already been applied to its translation.
    if (clip.has_root_motion()) {
        const u16 root = mapped_root(profile, clip.root_motion_joint(), frames.joints);
        if (root != kInvalidJoint) {
            out.set_root_motion_joint(root);
        }
    }
    return out.compress(settings);
}

}  // namespace cy::animation
