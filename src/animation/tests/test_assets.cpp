// SPDX-License-Identifier: MIT
// Cooked skeletons, clips and pose programs: their records round-trip, and the library loads them
// through a real asset system, binds rigs by asset id, refuses what does not match and swaps a
// reloaded clip under live instances. Issue #76 stage 2.
//
// INTEGRATION: the library cases start a job system, an async service and an asset system, and
// the program cases compile the locomotion machine.

#include <cy/animation/cooked.h>
#include <cy/animation/library.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/test/test.h>

#include "locomotion_fixture.h"

#include <cmath>
#include <cstring>
#include <string_view>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;
namespace pose = cy::graph::pose;

namespace {

constexpr f32 kTick = 1.0F / 60.0F;

[[nodiscard]] bool same_bytes(const Array<u8>& a, const Array<u8>& b) noexcept {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

[[nodiscard]] bool same_pose(Span<const Transform> a, Span<const Transform> b) noexcept {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(Transform)) == 0;
}

/// The skeleton's joint names, in order: what a cook writes into every clip it cooks for it.
[[nodiscard]] Array<Name> joint_names(const Skeleton& skeleton) {
    Array<Name> names(allocator());
    for (const Joint& joint : skeleton.joints()) {
        CY_REQUIRE(names.push_back(joint.name).has_value());
    }
    return names;
}

/// One instance of a rig, posed tick by tick through the locomotion schedule.
struct Player {
    Player(const AnimationRig& rig_in)
        : rig(&rig_in), instance(allocator()), scratch(allocator()), local(allocator()) {
        CY_REQUIRE(instance.prepare(*rig).has_value());
        CY_REQUIRE(scratch.prepare(*rig).has_value());
        CY_REQUIRE(local.resize(rig->skeleton().joint_count()).has_value());
    }

    void tick(pose::LocomotionState state) {
        CY_REQUIRE(request_state(state, [this](Name name, f32 value) noexcept {
                       return instance.set_parameter(*rig, name, value);
                   }).has_value());
        CY_REQUIRE(advance(*rig, instance, kTick, nullptr).has_value());
        rig->skeleton().reference_pose(local.span());
        EvaluationStats stats;
        CY_REQUIRE(evaluate(*rig, instance, 0, scratch, local.span(), stats).has_value());
    }

    const AnimationRig* rig;
    AnimationInstance instance;
    PoseScratch scratch;
    Array<Transform> local;
};

[[nodiscard]] pose::LocomotionState scheduled(u32 tick) noexcept {
    if (tick >= 200) {
        return pose::LocomotionState::Die;
    }
    if (tick >= 120) {
        return pose::LocomotionState::Run;
    }
    if (tick >= 30) {
        return pose::LocomotionState::Walk;
    }
    return pose::LocomotionState::Idle;
}

/// A job system, an async service, a namespace with one memory mount, and an asset system over
/// them: the whole stack a game loads through.
struct Assets {
    Assets() {
        jobs::JobSystemConfig job_config;
        job_config.worker_count = 2;
        CY_REQUIRE(workers.start(job_config).has_value());
        CY_REQUIRE(async.start(workers).has_value());
        Expected<UniquePtr<assets::MemoryMount>, Error> memory =
            make_unique<assets::MemoryMount>(allocator(), "animation");
        CY_REQUIRE(memory.has_value());
        CY_REQUIRE(
            files.mount_owned(std::move(*memory), assets::mount_priority::kMemory).has_value());
        CY_REQUIRE(system.start(workers, async, files, assets::AssetSystemConfig{}).has_value());
    }
    ~Assets() {
        system.shutdown();
        async.stop();
        workers.shutdown();
    }
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    /// Put cooked bytes where the asset system looks for `id`.
    void put(AssetId id, const Array<u8>& bytes) {
        Expected<assets::VirtualPath, Error> path = assets::package_entry_path(id, {});
        CY_REQUIRE(path.has_value());
        CY_REQUIRE(files.write(*path, bytes.data(), bytes.size()).has_value());
    }

    jobs::JobSystem workers;
    jobs::AsyncService async;
    assets::VirtualFileSystem files;
    assets::AssetSystem system;
};

constexpr AssetId kSkeleton{0xA11E, 1};
constexpr AssetId kProgram{0xA11E, 2};
constexpr AssetId kIdle{0xA11E, 10};
constexpr AssetId kWalk{0xA11E, 11};
constexpr AssetId kRun{0xA11E, 12};
constexpr AssetId kDie{0xA11E, 13};

/// Cook the locomotion rig into `assets`: the skeleton, the program and the four clips, under the
/// ids above. `skip` leaves one clip out.
void cook(const LocomotionRig& locomotion, Assets& assets, AssetId skip = AssetId{}) {
    const Array<Name> joints = joint_names(locomotion.skeleton);
    SkeletonProfile humanoid;
    humanoid.map(HumanoidJoint::Hips, kHips);
    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_skeleton(locomotion.skeleton, humanoid, bytes).has_value());
    assets.put(kSkeleton, bytes);
    bytes.clear();
    CY_REQUIRE(encode_program(locomotion.program, bytes).has_value());
    assets.put(kProgram, bytes);
    const struct {
        AssetId id;
        const Clip* clip;
    } clips[] = {{kIdle, &locomotion.idle},
                 {kWalk, &locomotion.walk},
                 {kRun, &locomotion.run},
                 {kDie, &locomotion.die}};
    for (const auto& entry : clips) {
        if (entry.id == skip) {
            continue;
        }
        bytes.clear();
        CY_REQUIRE(encode_clip(*entry.clip, joints.span(), bytes).has_value());
        assets.put(entry.id, bytes);
    }
}

const AssetId kAllClips[] = {kIdle, kWalk, kRun, kDie};

}  // namespace

CY_TEST_CASE("cooked animation: a skeleton round-trips byte for byte, with its humanoid profile") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    skeleton.set_name(Name::intern("biped"));
    SkeletonProfile humanoid;
    humanoid.map(HumanoidJoint::Hips, kHips);
    humanoid.map(HumanoidJoint::Head, kHead);

    Array<u8> first(allocator());
    CY_REQUIRE(encode_skeleton(skeleton, humanoid, first).has_value());
    Skeleton read(allocator());
    SkeletonProfile read_humanoid;
    CY_REQUIRE(decode_skeleton(first.span(), read, read_humanoid).has_value());
    Array<u8> second(allocator());
    CY_REQUIRE(encode_skeleton(read, read_humanoid, second).has_value());
    CY_CHECK(same_bytes(first, second));

    CY_CHECK(read.finalized());
    CY_CHECK_EQ(read.name(), Name::intern("biped"));
    CY_REQUIRE_EQ(read.joint_count(), skeleton.joint_count());
    for (u16 joint = 0; joint < skeleton.joint_count(); ++joint) {
        CY_CHECK_EQ(read.joints()[joint].name, skeleton.joints()[joint].name);
        CY_CHECK_EQ(read.joints()[joint].parent, skeleton.joints()[joint].parent);
        CY_CHECK_EQ(read.joints()[joint].dropped_at, skeleton.joints()[joint].dropped_at);
    }
    CY_CHECK_EQ(read_humanoid.resolve(HumanoidJoint::Head), static_cast<u16>(kHead));
    CY_CHECK_EQ(read_humanoid.resolve(HumanoidJoint::Chest), kInvalidJoint);

    // A record cut short, or one that claims more joints than it has, is refused before anything
    // is sized by it.
    Skeleton refused(allocator());
    SkeletonProfile refused_humanoid;
    CY_CHECK_FALSE(
        decode_skeleton(first.span().subspan(0, first.size() - 3), refused, refused_humanoid)
            .has_value());
}

CY_TEST_CASE(
    "cooked animation: a clip round-trips its keys, events and markers, and samples the same") {
    Skeleton skeleton(allocator());
    CY_REQUIRE(build_biped(skeleton).has_value());
    Clip walk(allocator());
    CY_REQUIRE(build_walk(walk).has_value());
    const Array<Name> joints = joint_names(skeleton);

    Array<u8> first(allocator());
    CY_REQUIRE(encode_clip(walk, joints.span(), first).has_value());
    Clip read(allocator());
    Array<Name> read_joints(allocator());
    CY_REQUIRE(decode_clip(first.span(), read, read_joints).has_value());
    Array<u8> second(allocator());
    CY_REQUIRE(encode_clip(read, read_joints.span(), second).has_value());
    CY_CHECK(same_bytes(first, second));

    CY_CHECK_EQ(read.name(), walk.name());
    CY_CHECK_EQ(read.duration(), walk.duration());
    CY_CHECK_EQ(read.root_motion_joint(), walk.root_motion_joint());
    CY_REQUIRE_EQ(read.events().size(), 1U);
    CY_CHECK_EQ(read.events()[0].name, Name::intern("footstep"));
    CY_CHECK_EQ(read.markers().size(), 1U);

    // THE KEYS ARE ADOPTED, NOT RE-FITTED: the loaded clip samples the cooked one's bits.
    Array<Transform> a(allocator());
    Array<Transform> b(allocator());
    CY_REQUIRE(a.resize(kJointCount).has_value());
    CY_REQUIRE(b.resize(kJointCount).has_value());
    ClipCursor cursor_a(allocator());
    ClipCursor cursor_b(allocator());
    SampleStats stats;
    for (u32 step = 0; step <= 20; ++step) {
        const f32 time = static_cast<f32>(step) * 0.05F;
        skeleton.reference_pose(a.span());
        skeleton.reference_pose(b.span());
        CY_REQUIRE(
            walk.sample(time, JointMask::all(kJointCount), cursor_a, a.span(), stats).has_value());
        CY_REQUIRE(
            read.sample(time, JointMask::all(kJointCount), cursor_b, b.span(), stats).has_value());
        CY_CHECK(same_pose(a.span(), b.span()));
    }
    u16 mismatch = kInvalidJoint;
    CY_CHECK(clip_matches_skeleton(read, read_joints.span(), skeleton, mismatch));
}

CY_TEST_CASE(
    "cooked animation: a program round-trips and plays exactly as the one the compiler made") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());

    Array<u8> first(allocator());
    CY_REQUIRE(encode_program(locomotion.program, first).has_value());
    Expected<pose::PoseProgram, Error> read = decode_program(allocator(), first.span());
    CY_REQUIRE(read.has_value());
    Array<u8> second(allocator());
    CY_REQUIRE(encode_program(*read, second).has_value());
    CY_CHECK(same_bytes(first, second));
    CY_CHECK_EQ(read->digest(), locomotion.program.digest());
    CY_CHECK_EQ(read->states().size(), locomotion.program.states().size());

    AnimationRig loaded(allocator());
    CY_REQUIRE(loaded.bind(locomotion.skeleton, *read, locomotion.table.span()).has_value());
    Player compiled(locomotion.rig);
    Player cooked(loaded);
    for (u32 tick = 0; tick < 300; ++tick) {
        compiled.tick(scheduled(tick));
        cooked.tick(scheduled(tick));
        CY_REQUIRE(same_pose(compiled.local.span(), cooked.local.span()));
    }

    // A record cut short, and one that is not a program at all, are refused at load rather than
    // read past their end a frame later.
    CY_CHECK_FALSE(
        decode_program(allocator(), first.span().subspan(0, first.size() - 1)).has_value());
    Array<u8> other(allocator());
    CY_REQUIRE(other.append(first.span()).has_value());
    other[0] ^= 0xFFU;
    CY_CHECK_FALSE(decode_program(allocator(), other.span()).has_value());
}

CY_TEST_CASE(
    "animation library: a rig bound from asset ids poses exactly as the rig built in memory") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Assets assets;
    cook(locomotion, assets);

    AnimationLibrary library(allocator(), assets.system);
    // The clips are given in another order than the program names them: binding is by name.
    const AssetId shuffled[] = {kDie, kRun, kIdle, kWalk};
    Expected<const AnimationRig*, Error> rig =
        library.rig(RigAssets{kSkeleton, kProgram, Span<const AssetId>(shuffled, 4)});
    CY_REQUIRE(rig.has_value());
    CY_CHECK_EQ(library.stats().clips, 4U);
    CY_REQUIRE(library.humanoid(kSkeleton) != nullptr);
    CY_CHECK_EQ(library.humanoid(kSkeleton)->resolve(HumanoidJoint::Hips), static_cast<u16>(kHips));

    Player built(locomotion.rig);
    Player loaded(**rig);
    for (u32 tick = 0; tick < 300; ++tick) {
        built.tick(scheduled(tick));
        loaded.tick(scheduled(tick));
        CY_REQUIRE(same_pose(built.local.span(), loaded.local.span()));
        const Vec3 built_travel = built.instance.travelled();
        const Vec3 loaded_travel = loaded.instance.travelled();
        CY_REQUIRE(std::memcmp(&built_travel, &loaded_travel, sizeof(Vec3)) == 0);
    }

    // Asked again, the same ids are the same objects: one load per id.
    Expected<const Clip*, Error> walk = library.clip(kWalk);
    CY_REQUIRE(walk.has_value());
    CY_CHECK_EQ(library.stats().clips, 4U);
}

CY_TEST_CASE("animation library: a clip the program names and nobody gave is refused, by name") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Assets assets;
    cook(locomotion, assets);
    AnimationLibrary library(allocator(), assets.system);

    const AssetId three[] = {kIdle, kWalk, kRun};
    Expected<const AnimationRig*, Error> rig =
        library.rig(RigAssets{kSkeleton, kProgram, Span<const AssetId>(three, 3)});
    CY_REQUIRE_FALSE(rig.has_value());
    CY_CHECK(rig.error().code == ErrorCode::NotFound);
    CY_CHECK(std::string_view(rig.error().message).find("'die'") != std::string_view::npos);
}

CY_TEST_CASE(
    "animation library: a missing asset and a clip cooked for another rig are refused with a "
    "reason") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Assets assets;
    cook(locomotion, assets, kRun);
    AnimationLibrary library(allocator(), assets.system);

    // The run was never cooked: its id names nothing in the namespace.
    Expected<const AnimationRig*, Error> missing =
        library.rig(RigAssets{kSkeleton, kProgram, Span<const AssetId>(kAllClips, 4)});
    CY_REQUIRE_FALSE(missing.has_value());
    char text[AssetId::kTextLength + 1] = {};
    (void)kRun.format(text);
    CY_CHECK(std::string_view(missing.error().message).find(text) != std::string_view::npos);

    // A run cooked against another skeleton's joint names: same counts, other joints. `bind`
    // would accept it, because it checks only counts.
    Skeleton other(allocator());
    CY_REQUIRE(build_biped(other, "x_").has_value());
    const Array<Name> foreign = joint_names(other);
    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_clip(locomotion.run, foreign.span(), bytes).has_value());
    const AssetId foreign_run{0xA11E, 42};
    assets.put(foreign_run, bytes);
    const AssetId with_foreign[] = {kIdle, kWalk, foreign_run, kDie};
    Expected<const AnimationRig*, Error> mismatched =
        library.rig(RigAssets{kSkeleton, kProgram, Span<const AssetId>(with_foreign, 4)});
    CY_REQUIRE_FALSE(mismatched.has_value());
    CY_CHECK(std::string_view(mismatched.error().message).find("x_") != std::string_view::npos);
    CY_CHECK_GE(library.stats().refusals, 2U);

    // Bytes that are not a cooked program at all.
    const AssetId garbage{0xA11E, 99};
    Array<u8> noise(allocator());
    CY_REQUIRE(noise.resize(64).has_value());
    assets.put(garbage, noise);
    CY_CHECK_FALSE(library.program(garbage).has_value());
}

CY_TEST_CASE(
    "animation library: a reloaded clip is swapped under live instances, and a bad reload keeps "
    "the old") {
    LocomotionRig locomotion(allocator());
    CY_REQUIRE(locomotion.build().has_value());
    Assets assets;
    cook(locomotion, assets);
    AnimationLibrary library(allocator(), assets.system);
    CY_REQUIRE(library.watch().has_value());
    Expected<const AnimationRig*, Error> rig =
        library.rig(RigAssets{kSkeleton, kProgram, Span<const AssetId>(kAllClips, 4)});
    CY_REQUIRE(rig.has_value());
    Expected<const Clip*, Error> idle_before = library.clip(kIdle);
    CY_REQUIRE(idle_before.has_value());

    // The instance stands in the idle, which samples the `aim` clip: the arm raised.
    Player player(**rig);
    for (u32 tick = 0; tick < 10; ++tick) {
        player.tick(pose::LocomotionState::Idle);
    }
    const Quat raised = player.local[kUpperArm].rotation;

    // The artist re-exports the idle with the arm lowered — same name, same joints — and the file
    // watcher's reload lands. The instance is NOT re-prepared.
    Clip lowered(allocator());
    lowered.set_name(Name::intern("idle"));
    lowered.set_duration(1.0F);
    Expected<u32, Error> arm =
        lowered.add_joint_track(TrackKind::Rotation, kUpperArm, Interpolation::Spherical);
    CY_REQUIRE(arm.has_value());
    const Quat down = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.6F);
    CY_REQUIRE(lowered.add_key(*arm, 0.0F, Vec4{down.x, down.y, down.z, down.w}).has_value());
    CY_REQUIRE(lowered.add_key(*arm, 1.0F, Vec4{down.x, down.y, down.z, down.w}).has_value());
    CY_REQUIRE(lowered.compress(CompressionSettings{}).has_value());
    const Array<Name> joints = joint_names(locomotion.skeleton);
    Array<u8> bytes(allocator());
    CY_REQUIRE(encode_clip(lowered, joints.span(), bytes).has_value());
    assets.put(kIdle, bytes);
    CY_REQUIRE(assets.system.reload(kIdle).has_value());
    CY_CHECK_EQ(library.stats().reloads_applied, 1U);

    // Same object, new content, and the instance plays it.
    Expected<const Clip*, Error> idle_after = library.clip(kIdle);
    CY_REQUIRE(idle_after.has_value());
    CY_CHECK(*idle_after == *idle_before);
    player.tick(pose::LocomotionState::Idle);
    CY_CHECK(std::fabs(player.local[kUpperArm].rotation.x - down.x) < 1e-3F);
    CY_CHECK(std::fabs(player.local[kUpperArm].rotation.x - raised.x) > 0.1F);

    // A reload that does not decode is refused, and the working clip plays on.
    Array<u8> torn(allocator());
    CY_REQUIRE(torn.append(bytes.span().subspan(0, bytes.size() / 2)).has_value());
    assets.put(kIdle, torn);
    (void)assets.system.reload(kIdle);
    CY_CHECK_EQ(library.stats().reloads_refused, 1U);
    player.tick(pose::LocomotionState::Idle);
    CY_CHECK(std::fabs(player.local[kUpperArm].rotation.x - down.x) < 1e-3F);
}
