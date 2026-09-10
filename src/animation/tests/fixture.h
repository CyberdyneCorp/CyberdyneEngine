#pragma once
// The skeleton and the clips every animation suite is written against. M8.b section 5.
//
// One twelve-joint biped: a root, a hip chain up to a head, one arm whose three joints are a
// two-bone chain, and one leg. Two joints carry a bone level of detail — a finger dropped first and
// a head dropped next — because a level of detail that drops nothing is not one.

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <string_view>

namespace cy::animation::testing {

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

/// The joints of the fixture skeleton, by name, so a case does not index by a number.
enum Joints : u16 {
    kRoot = 0,
    kHips,
    kSpine,
    kChest,
    kHead,
    kShoulder,
    kUpperArm,
    kLowerArm,
    kFinger,
    kUpperLeg,
    kLowerLeg,
    kFoot,
    kJointCount,
};

/// `prefix` lets a case build two skeletons whose joints share no name at all, which is what
/// retargeting by semantic chain has to work across.
inline Status build_biped(Skeleton& skeleton, const char* prefix = "", f32 scale = 1.0F) noexcept {
    struct Row {
        const char* name = "";
        u16 parent = kInvalidJoint;
        Vec3 translation{0.0F, 0.0F, 0.0F};
        u8 dropped_at = kBoneLodLevels;
    };
    const Row rows[] = {
        {"root", kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}, kBoneLodLevels},
        {"hips", kRoot, Vec3{0.0F, 1.0F, 0.0F}, kBoneLodLevels},
        {"spine", kHips, Vec3{0.0F, 0.4F, 0.0F}, kBoneLodLevels},
        {"chest", kSpine, Vec3{0.0F, 0.4F, 0.0F}, kBoneLodLevels},
        {"head", kChest, Vec3{0.0F, 0.3F, 0.0F}, 2},
        {"shoulder", kChest, Vec3{-0.2F, 0.2F, 0.0F}, kBoneLodLevels},
        {"upper_arm", kShoulder, Vec3{-0.3F, 0.0F, 0.0F}, kBoneLodLevels},
        {"lower_arm", kUpperArm, Vec3{-0.3F, 0.0F, 0.0F}, kBoneLodLevels},
        {"finger", kLowerArm, Vec3{-0.1F, 0.0F, 0.0F}, 1},
        {"upper_leg", kHips, Vec3{-0.1F, -0.1F, 0.0F}, kBoneLodLevels},
        {"lower_leg", kUpperLeg, Vec3{0.0F, -0.45F, 0.0F}, kBoneLodLevels},
        {"foot", kLowerLeg, Vec3{0.0F, -0.45F, 0.0F}, kBoneLodLevels},
    };
    char buffer[64] = {};
    for (const Row& row : rows) {
        usize length = 0;
        for (const char* cursor = prefix; *cursor != '\0' && length + 1 < sizeof(buffer);
             ++cursor) {
            buffer[length++] = *cursor;
        }
        for (const char* cursor = row.name; *cursor != '\0' && length + 1 < sizeof(buffer);
             ++cursor) {
            buffer[length++] = *cursor;
        }
        buffer[length] = '\0';
        const Transform bind = Transform::from_translation(
            Vec3{row.translation.x * scale, row.translation.y * scale, row.translation.z * scale});
        Expected<u16, Error> joint = skeleton.add_joint(
            Name::intern(std::string_view(buffer, length)), row.parent, bind, row.dropped_at);
        if (!joint) {
            return Status{make_unexpected(joint.error())};
        }
        if (Status bounds = skeleton.set_bounds(
                *joint,
                Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.05F, 0.05F, 0.05F}));
            !bounds) {
            return bounds;
        }
    }
    return skeleton.finalize();
}

/// A clip that walks the root forward at one metre per second and swings the arm, with a footstep
/// event and a foot-down marker on it. `duration` seconds, keyed at 10 Hz.
inline Status build_walk(Clip& clip, f32 duration = 1.0F, bool root_motion = true) noexcept {
    clip.set_name(Name::intern("walk"));
    clip.set_duration(duration);
    clip.set_loop_mode(LoopMode::Loop);
    clip.set_sample_rate_hint(10.0F);
    if (root_motion) {
        clip.set_root_motion_joint(kRoot);
    }

    const auto frames = static_cast<u32>(duration * 10.0F) + 1U;
    Expected<u32, Error> root =
        clip.add_joint_track(TrackKind::Translation, kRoot, Interpolation::Linear);
    if (!root) {
        return Status{make_unexpected(root.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        if (Status added = clip.add_key(*root, time, Vec4{0.0F, 0.0F, -time, 0.0F}); !added) {
            return added;
        }
    }

    Expected<u32, Error> arm =
        clip.add_joint_track(TrackKind::Rotation, kUpperArm, Interpolation::Spherical);
    if (!arm) {
        return Status{make_unexpected(arm.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        const f32 angle = 0.5F * std::sin(time * 6.2831853F / duration);
        const Quat rotation = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, angle);
        if (Status added =
                clip.add_key(*arm, time, Vec4{rotation.x, rotation.y, rotation.z, rotation.w});
            !added) {
            return added;
        }
    }

    if (Status marker = clip.add_marker(Name::intern("foot_down"), duration * 0.5F); !marker) {
        return marker;
    }
    if (Status event = clip.add_event(Name::intern("footstep"), duration * 0.5F); !event) {
        return event;
    }
    return clip.compress(CompressionSettings{});
}

/// A clip that only raises the arm — no root motion, no leg tracks — used as an upper-body layer.
inline Status build_aim(Clip& clip) noexcept {
    clip.set_name(Name::intern("aim"));
    clip.set_duration(1.0F);
    clip.set_loop_mode(LoopMode::Loop);
    Expected<u32, Error> arm =
        clip.add_joint_track(TrackKind::Rotation, kUpperArm, Interpolation::Spherical);
    if (!arm) {
        return Status{make_unexpected(arm.error())};
    }
    const Quat up = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, 1.2F);
    if (Status added = clip.add_key(*arm, 0.0F, Vec4{up.x, up.y, up.z, up.w}); !added) {
        return added;
    }
    if (Status added = clip.add_key(*arm, 1.0F, Vec4{up.x, up.y, up.z, up.w}); !added) {
        return added;
    }
    return clip.compress(CompressionSettings{});
}

}  // namespace cy::animation::testing
