// SPDX-License-Identifier: MIT
// The cook half of an animated character. See cy/import/animation_cook.h.

#include <cy/import/animation_cook.h>

#if defined(CY_IMPORT_ANIMATION)

#    include <cy/animation/cooked.h>
#    include <cy/animation/retarget.h>
#    include <cy/animation/retarget_build.h>
#    include <cy/core/memory/system_allocator.h>
#    include <cy/graph/locomotion.h>
#    include <cy/import/fbx_skeleton.h>

#    include <string_view>
#    include <utility>

namespace cy::import {
namespace {

namespace pose = graph::pose;

[[nodiscard]] Allocator& memory() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

/// The sub-asset whose name begins with `prefix`, or null.
[[nodiscard]] const SubAsset* find_prefixed(const ImportResult& result,
                                            std::string_view prefix) noexcept {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view().starts_with(prefix)) {
            return &produced;
        }
    }
    return nullptr;
}

[[nodiscard]] Span<const u8> payload_of(const SubAsset& produced) noexcept {
    return {produced.payload.data(), produced.payload.size()};
}

[[nodiscard]] Status read_rig(const ImportResult& result, animation::Skeleton& skeleton,
                              animation::SkeletonProfile& humanoid) noexcept {
    const SubAsset* found = find_prefixed(result, kSkeletonSubAssetPrefix);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound,
                    "an animation source produced no skeleton; step 7 imports one from every "
                    "node that carries a bone attribute, so a file without one is not a rig");
    }
    return animation::decode_skeleton(payload_of(*found), skeleton, humanoid);
}

[[nodiscard]] Status read_clip(const ImportResult& result, animation::Clip& clip,
                               Array<Name>& joints) noexcept {
    const SubAsset* found = find_prefixed(result, "animation/");
    if (found == nullptr) {
        return fail(ErrorCode::NotFound,
                    "an animation source produced no clip; step 8 skips a stack whose every track "
                    "the codec collapsed to one key");
    }
    return animation::decode_clip(payload_of(*found), clip, joints);
}

/// Keep only the tracks that address a joint of a skeleton of `joints` joints. Step 8 records a
/// track per animated NODE, and a node that is not a bone — a mesh's own transform — has no joint
/// of the skeleton to drive.
[[nodiscard]] Status keep_skeleton_tracks(animation::Clip& clip, u16 joints) noexcept {
    Array<animation::TrackDesc> kept(memory());
    for (const animation::TrackDesc& track : clip.tracks()) {
        if (track.joint < joints) {
            if (Status pushed = kept.push_back(track); !pushed) {
                return pushed;
            }
        }
    }
    if (kept.size() == clip.tracks().size()) {
        return ok();
    }
    Array<animation::PackedKey> keys(memory());
    if (Status copied = keys.append(clip.keys()); !copied) {
        return copied;
    }
    return clip.adopt_compressed(kept.span(), keys.span());
}

[[nodiscard]] bool is_locomotion_name(std::string_view name) noexcept {
    return name == "idle" || name == "walk" || name == "run" || name == "die";
}

[[nodiscard]] Status check_spec(const AnimationCookSpec& spec) noexcept {
    if (spec.rig == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an animation cook names the import of its rig");
    }
    if (spec.clips.size() != pose::kLocomotionStateCount) {
        return fail(ErrorCode::InvalidArgument,
                    "a locomotion character is exactly four clips: idle, walk, run and die");
    }
    for (usize index = 0; index < spec.clips.size(); ++index) {
        const AnimationClipSource& clip = spec.clips[index];
        if (clip.import == nullptr || !is_locomotion_name(clip.name)) {
            return fail(ErrorCode::InvalidArgument,
                        "every locomotion clip names its import and one of idle, walk, run, die");
        }
        for (usize other = 0; other < index; ++other) {
            if (spec.clips[other].name == clip.name) {
                return fail(ErrorCode::AlreadyExists, "a locomotion clip is named twice");
            }
        }
    }
    return ok();
}

/// One clip onto the character: as it is when its rig is the character's, retargeted and baked
/// when it is not.
[[nodiscard]] Status cook_clip(const AnimationClipSource& source,
                               const animation::Skeleton& character,
                               const animation::SkeletonProfile& humanoid,
                               Span<const Name> character_joints,
                               CookedAnimationClip& out) noexcept {
    animation::Skeleton source_rig(memory());
    animation::SkeletonProfile source_humanoid;
    if (Status read = read_rig(*source.import, source_rig, source_humanoid); !read) {
        return read;
    }
    animation::Clip clip(memory());
    Array<Name> source_joints(memory());
    if (Status read = read_clip(*source.import, clip, source_joints); !read) {
        return read;
    }
    out.name = source.name;
    out.duration = clip.duration();
    out.source_tracks = clip.track_count();
    out.source_keys = static_cast<u32>(clip.keys().size());

    // MEASURED BEFORE IT IS RECONCILED, and reported, so a reader sees what the retarget absorbed.
    const animation::RigMatch match = animation::compare_rigs(source_rig, character);
    out.rest_difference_degrees = match.worst_rest_rotation_degrees;
    out.rest_difference_metres = match.worst_rest_translation;
    animation::Clip cooked(memory());
    if (match.congruent && match.same_rest_pose) {
        // The same rig: joint i of the source IS joint i of the character, so the clip plays as it
        // was imported, under the character's joint names.
        cooked = std::move(clip);
        if (Status kept = keep_skeleton_tracks(cooked, character.joint_count()); !kept) {
            return kept;
        }
    } else {
        animation::RetargetProfile profile(memory());
        animation::RetargetBuildReport report;
        if (Status built = animation::build_retarget_profile(source_rig, source_humanoid, character,
                                                             humanoid, profile, report);
            !built) {
            return built;
        }
        out.retargeted = true;
        out.retarget_pairs = report.pairs;
        out.height_scale = profile.height_scale();
        // At the source's own sample rate, so a 30 Hz export is baked at 30 Hz and not resampled to
        // a number this file invented.
        if (Status baked = animation::bake_clip(memory(), profile, source_rig, character, clip,
                                                clip.sample_rate_hint(),
                                                animation::CompressionSettings{}, cooked);
            !baked) {
            return baked;
        }
        out.worst_rotation_degrees = cooked.report().worst_rotation_degrees;
    }
    cooked.set_name(Name::intern(source.name));
    cooked.set_duration(out.duration);
    cooked.set_loop_mode(source.looping ? animation::LoopMode::Loop : animation::LoopMode::None);
    out.tracks = cooked.track_count();
    out.keys = static_cast<u32>(cooked.keys().size());
    return animation::encode_clip(cooked, character_joints, out.bytes);
}

[[nodiscard]] pose::LocomotionSpec locomotion_spec(const AnimationCookSpec& spec,
                                                   const CookedAnimationSet& out) noexcept {
    pose::LocomotionSpec locomotion;
    locomotion.name = Name::intern(spec.name);
    for (usize index = 0; index < spec.clips.size(); ++index) {
        pose::LocomotionClip clip;
        clip.clip = Name::intern(spec.clips[index].name);
        clip.duration = out.clips[index].duration;
        clip.looping = spec.clips[index].looping;
        const std::string_view name = spec.clips[index].name;
        if (name == "idle") {
            locomotion.idle = clip;
        } else if (name == "walk") {
            locomotion.walk = clip;
        } else if (name == "run") {
            locomotion.run = clip;
        } else {
            locomotion.die = clip;
        }
    }
    locomotion.idle_to_walk = spec.blends.idle_to_walk;
    locomotion.walk_to_run = spec.blends.walk_to_run;
    locomotion.run_to_walk = spec.blends.run_to_walk;
    locomotion.walk_to_idle = spec.blends.walk_to_idle;
    locomotion.to_die = spec.blends.to_die;
    return locomotion;
}

}  // namespace

Status cook_locomotion_set(const AnimationCookSpec& spec, CookedAnimationSet& out) noexcept {
    if (Status checked = check_spec(spec); !checked) {
        return checked;
    }
    animation::Skeleton character(memory());
    animation::SkeletonProfile humanoid;
    if (Status read = read_rig(*spec.rig, character, humanoid); !read) {
        return read;
    }
    out.joints = character.joint_count();
    out.humanoid_mapped = humanoid.mapped_count();
    Array<Name> joints(memory());
    for (const animation::Joint& joint : character.joints()) {
        if (Status pushed = joints.push_back(joint.name); !pushed) {
            return pushed;
        }
    }

    out.clips.clear();
    out.clips.resize(spec.clips.size());
    for (usize index = 0; index < spec.clips.size(); ++index) {
        if (Status cooked =
                cook_clip(spec.clips[index], character, humanoid, joints.span(), out.clips[index]);
            !cooked) {
            return cooked;
        }
    }

    graph::DiagnosticSink sink(memory());
    Expected<pose::PoseProgram, Error> program = pose::compile_locomotion(
        memory(), locomotion_spec(spec, out), character.joint_count(), sink);
    if (!program) {
        return Status{make_unexpected(program.error())};
    }
    out.skeleton.clear();
    out.program.clear();
    if (Status written = animation::encode_skeleton(character, humanoid, out.skeleton); !written) {
        return written;
    }
    return animation::encode_program(*program, out.program);
}

}  // namespace cy::import

#else

namespace cy::import {

// `-D CY_ANIMATION=OFF` removed the runtime, so there is no skeleton, clip or program to cook to.
Status cook_locomotion_set(const AnimationCookSpec& spec, CookedAnimationSet& out) noexcept {
    (void)spec;
    (void)out;
    return fail(ErrorCode::Unsupported,
                "this build was configured with CY_ANIMATION=OFF, so there is no animation runtime "
                "to cook a character for");
}

}  // namespace cy::import

#endif
