// SPDX-License-Identifier: MIT
// The worker's rig: authored, cooked, loaded. See worker_rig.h.

#include "worker_rig.h"

#include <cy/animation/clip.h>
#include <cy/animation/cooked.h>
#include <cy/animation/skeleton.h>
#include <cy/core/assets/package.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/ownership.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>

#include <cmath>
#include <utility>

namespace sample::rts {
namespace {

using cy::AssetId;
using cy::f32;
using cy::Name;
using cy::Quat;
using cy::Status;
using cy::u16;
using cy::u32;
using cy::Vec3;
using cy::Vec4;
namespace animation = cy::animation;
namespace graph = cy::graph;

/// Fixed ids, outside the reserved placeholder namespace: the records are cooked again on every
/// run and nothing persists a reference to them, so a minted id would buy nothing.
constexpr AssetId kSkeletonId{0x13A0'0000'0000'0001ULL, 1};
constexpr AssetId kProgramId{0x13A0'0000'0000'0001ULL, 2};
constexpr AssetId kClipIds[] = {
    {0x13A0'0000'0000'0001ULL, 10}, {0x13A0'0000'0000'0001ULL, 11}, {0x13A0'0000'0000'0001ULL, 12}};

enum Joint : u16 { kRoot, kHips, kSpine, kHead, kArmLeft, kArmRight, kLegLeft, kLegRight, kJoints };
static_assert(kJoints == kWorkerJoints, "the header's joint count is the skeleton's");

struct JointRow {
    const char* name;
    u16 parent;
    Vec3 offset;
};

constexpr JointRow kSkeleton[kJoints] = {
    {"root", animation::kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}},
    {"hips", kRoot, Vec3{0.0F, 0.9F, 0.0F}},
    {"spine", kHips, Vec3{0.0F, 0.3F, 0.0F}},
    {"head", kSpine, Vec3{0.0F, 0.5F, 0.0F}},
    {"arm_left", kSpine, Vec3{-0.25F, 0.4F, 0.0F}},
    {"arm_right", kSpine, Vec3{0.25F, 0.4F, 0.0F}},
    {"leg_left", kHips, Vec3{-0.12F, 0.0F, 0.0F}},
    {"leg_right", kHips, Vec3{0.12F, 0.0F, 0.0F}},
};

/// One state: its clip, how long it lasts and whether it loops.
struct StateRow {
    const char* state;
    const char* clip;
    f32 duration;
    bool looping;
};

constexpr StateRow kStates[] = {
    {"idle", "worker_idle", 2.0F, true},
    {"walk", "worker_walk", 1.0F, true},
    {"cheer", "worker_cheer", 0.8F, false},
};
constexpr u32 kStateCount = sizeof(kStates) / sizeof(kStates[0]);

[[nodiscard]] Vec4 rotation_about(Vec3 axis, f32 radians) noexcept {
    const Quat rotation = Quat::from_axis_angle(axis, radians);
    return Vec4{rotation.x, rotation.y, rotation.z, rotation.w};
}

/// A rotation track on `joint`, keyed at 10 Hz from `angle(t)` about `axis`.
template <class Angle>
[[nodiscard]] Status key_rotation(animation::Clip& clip, u16 joint, Vec3 axis, Angle&& angle) {
    cy::Expected<u32, cy::Error> track = clip.add_joint_track(animation::TrackKind::Rotation, joint,
                                                              animation::Interpolation::Spherical);
    if (!track) {
        return Status{cy::make_unexpected(track.error())};
    }
    const auto frames = static_cast<u32>(std::lround(clip.duration() * 10.0F)) + 1U;
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        if (Status added = clip.add_key(*track, time, rotation_about(axis, angle(time))); !added) {
            return added;
        }
    }
    return cy::ok();
}

[[nodiscard]] Status author_clip(animation::Clip& clip, const StateRow& row) noexcept {
    clip.set_name(Name::intern(row.clip));
    clip.set_duration(row.duration);
    clip.set_loop_mode(row.looping ? animation::LoopMode::Loop : animation::LoopMode::None);
    clip.set_sample_rate_hint(10.0F);
    const Vec3 across{1.0F, 0.0F, 0.0F};
    const Vec3 forward{0.0F, 0.0F, 1.0F};
    constexpr f32 kTau = 6.2831853F;
    const std::string_view state = row.state;
    Status authored = cy::ok();
    if (state == "idle") {
        authored = key_rotation(clip, kSpine, forward, [](f32 time) noexcept {
            return 0.06F * std::sin(time * kTau * 0.5F);
        });
    } else if (state == "walk") {
        const auto swing = [](f32 phase) noexcept {
            return [phase](f32 time) noexcept { return 0.55F * std::sin((time + phase) * kTau); };
        };
        authored = key_rotation(clip, kLegLeft, across, swing(0.0F));
        if (authored) {
            authored = key_rotation(clip, kLegRight, across, swing(0.5F));
        }
        if (authored) {
            authored = key_rotation(clip, kArmLeft, across, swing(0.5F));
        }
        if (authored) {
            authored = key_rotation(clip, kArmRight, across, swing(0.0F));
        }
        if (authored) {
            authored = clip.add_event(Name::intern("footstep"), 0.25F);
        }
        if (authored) {
            authored = clip.add_event(Name::intern("footstep"), 0.75F);
        }
    } else {
        // Both arms up over the first half, held there.
        const auto raise = [](f32 sign) noexcept {
            return [sign](f32 time) noexcept { return sign * 2.6F * std::fmin(time / 0.4F, 1.0F); };
        };
        authored = key_rotation(clip, kArmLeft, forward, raise(-1.0F));
        if (authored) {
            authored = key_rotation(clip, kArmRight, forward, raise(1.0F));
        }
        if (authored) {
            authored = clip.add_event(Name::intern("cheer_done"), 0.7F);
        }
    }
    if (!authored) {
        return authored;
    }
    return clip.compress(animation::CompressionSettings{});
}

[[nodiscard]] graph::Literal literal_name(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] graph::Literal literal_number(f32 value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("float");
    literal.value = graph::Immediate::scalar(value);
    return literal;
}

[[nodiscard]] graph::Literal literal_bool(bool value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("bool");
    literal.value.mask = value ? 1U : 0U;
    return literal;
}

/// The three states, each one clip, and no transition between them: the game picks the state.
[[nodiscard]] cy::Expected<graph::pose::PoseProgram, cy::Error> compile_program(
    cy::Allocator& allocator) noexcept {
    graph::NodeRegistry registry(allocator);
    if (Status registered = graph::pose::register_pose_nodes(registry); !registered) {
        return cy::make_unexpected(registered.error());
    }
    graph::Graph authored(allocator, Name::intern("worker"));
    for (u32 index = 0; index < kStateCount; ++index) {
        const StateRow& row = kStates[index];
        // Ascending keys, so idle — the lowest — is the entry state.
        const auto state_node = static_cast<graph::NodeKey>((index + 1U) * 10U);
        const graph::NodeKey clip_node = state_node + 1U;
        char clock[48];
        (void)std::snprintf(clock, sizeof(clock), "clock_%s", row.state);
        Status built = authored.add_node(clip_node, Name::intern("pose.clip"));
        const std::pair<const char*, graph::Literal> properties[] = {
            {"clip", literal_name(row.clip)},
            {"duration", literal_number(row.duration)},
            {"loop", literal_bool(row.looping)},
            {"time_parameter", literal_name(clock)},
        };
        for (const auto& [property, value] : properties) {
            if (built) {
                built = authored.set_property(clip_node, Name::intern(property), value);
            }
        }
        if (built) {
            built = authored.add_node(state_node, Name::intern("pose.state"));
        }
        if (built) {
            built =
                authored.set_property(state_node, Name::intern("name"), literal_name(row.state));
        }
        if (built) {
            built =
                authored.connect(clip_node, Name::intern("pose"), state_node, Name::intern("pose"));
        }
        if (!built) {
            return cy::make_unexpected(built.error());
        }
    }
    authored.resolve(registry);
    graph::DiagnosticSink sink(allocator);
    return graph::pose::compile_pose(authored, registry, kJoints, sink);
}

}  // namespace

WorkerRig::WorkerRig(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

WorkerRig::~WorkerRig() {
    shutdown();
}

void WorkerRig::shutdown() noexcept {
    rig_ = nullptr;
    library_.reset();
    if (started_) {
        assets_.shutdown();
        async_.stop();
        workers_.shutdown();
        started_ = false;
    }
}

const cy::animation::AnimationLibraryStats& WorkerRig::stats() const noexcept {
    static const cy::animation::AnimationLibraryStats kNone{};
    return library_ ? library_->stats() : kNone;
}

Status WorkerRig::store(AssetId id, const cy::Array<cy::u8>& bytes) noexcept {
    cy::Expected<cy::assets::VirtualPath, cy::Error> path = cy::assets::package_entry_path(id, {});
    if (!path) {
        return Status{cy::make_unexpected(path.error())};
    }
    ++records_;
    return files_.write(*path, bytes.data(), bytes.size());
}

Status WorkerRig::cook() noexcept {
    // THE SKELETON.
    animation::Skeleton skeleton(*allocator_);
    skeleton.set_name(Name::intern("worker"));
    for (const JointRow& row : kSkeleton) {
        if (cy::Expected<u16, cy::Error> added = skeleton.add_joint(
                Name::intern(row.name), row.parent, cy::Transform::from_translation(row.offset));
            !added) {
            return Status{cy::make_unexpected(added.error())};
        }
    }
    if (Status finalized = skeleton.finalize(); !finalized) {
        return finalized;
    }
    cy::Array<cy::u8> bytes(*allocator_);
    if (Status encoded = animation::encode_skeleton(skeleton, animation::SkeletonProfile{}, bytes);
        !encoded) {
        return encoded;
    }
    if (Status stored = store(kSkeletonId, bytes); !stored) {
        return stored;
    }

    // THE CLIPS, each naming the skeleton's joints so the loader can check they move the joints
    // they were authored for.
    Name joints[kJoints];
    for (u16 joint = 0; joint < kJoints; ++joint) {
        joints[joint] = Name::intern(kSkeleton[joint].name);
    }
    for (u32 index = 0; index < kStateCount; ++index) {
        animation::Clip clip(*allocator_);
        if (Status authored = author_clip(clip, kStates[index]); !authored) {
            return authored;
        }
        bytes.clear();
        if (Status encoded =
                animation::encode_clip(clip, cy::Span<const Name>(joints, kJoints), bytes);
            !encoded) {
            return encoded;
        }
        if (Status stored = store(kClipIds[index], bytes); !stored) {
            return stored;
        }
    }

    // THE PROGRAM, compiled here at cook time; the game links no compiler to run it.
    cy::Expected<graph::pose::PoseProgram, cy::Error> program = compile_program(*allocator_);
    if (!program) {
        return Status{cy::make_unexpected(program.error())};
    }
    bytes.clear();
    if (Status encoded = animation::encode_program(*program, bytes); !encoded) {
        return encoded;
    }
    return store(kProgramId, bytes);
}

Status WorkerRig::load() noexcept {
    cy::jobs::JobSystemConfig config;
    config.worker_count = 1;
    if (Status started = workers_.start(config); !started) {
        return started;
    }
    if (Status started = async_.start(workers_); !started) {
        return started;
    }
    cy::Expected<cy::UniquePtr<cy::assets::MemoryMount>, cy::Error> memory =
        cy::make_unique<cy::assets::MemoryMount>(*allocator_, "cooked-worker");
    if (!memory) {
        return Status{cy::make_unexpected(memory.error())};
    }
    if (cy::Expected<cy::assets::MountId, cy::Error> mounted =
            files_.mount_owned(std::move(*memory), cy::assets::mount_priority::kMemory);
        !mounted) {
        return Status{cy::make_unexpected(mounted.error())};
    }
    if (Status started = assets_.start(workers_, async_, files_, cy::assets::AssetSystemConfig{});
        !started) {
        return started;
    }
    started_ = true;
    if (Status cooked = cook(); !cooked) {
        return cooked;
    }
    library_ = std::make_unique<cy::animation::AnimationLibrary>(*allocator_, assets_);
    const cy::animation::RigAssets wanted{kSkeletonId, kProgramId,
                                          cy::Span<const AssetId>(kClipIds, kStateCount)};
    cy::Expected<const cy::animation::AnimationRig*, cy::Error> rig = library_->rig(wanted);
    if (!rig) {
        return Status{cy::make_unexpected(rig.error())};
    }
    rig_ = *rig;
    return cy::ok();
}

}  // namespace sample::rts
