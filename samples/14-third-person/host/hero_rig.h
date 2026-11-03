// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_HERO_RIG_H
#define CY_SAMPLE_THIRD_PERSON_HERO_RIG_H
// hero_rig.h — the hero's animation, authored in code, cooked, and loaded through the asset system.
//
// THE HOST IS ALSO THE COOK, as samples/13-rts-api/host/worker_rig.h is: a skeleton, six clips and
// a program compiled from a pose graph are encoded as the cooked records (`cy/animation/cooked.h`),
// written into a memory mount under fixed asset ids, and LOADED BACK by id through `AssetSystem`
// and `AnimationLibrary`. What the game animates is what the loader read.
//
// THE SKELETON. Twenty joints of a biped standing on the model's ground plane, facing −Z (the way
// the engine's characters face): a root, the pelvis, spine, chest, neck and head; each arm's
// shoulder, elbow and wrist; each leg's hip, knee, ankle and toe. The ankles stand 8 cm over the
// ground, which is foot placement's `ankle_height`.
//
// THE CLIPS, every one a function of its phase keyed at 30 Hz:
//
//   idle   2.4 s, looping   breathing, a weight shift
//   walk   1.1 s, looping   a 1.8 m/s stride; markers `l_down` at 0 and `r_down` at 0.55 s, a
//                           `footstep` event at each (parameter 0 left, 1 right)
//   run    0.7 s, looping   a 5.5 m/s stride, a lean, the arms pumping; the same markers and
//                           events at its own times
//   jump   0.45 s, held     the legs tucked, the arms up
//   fall   0.8 s, looping   the legs apart, the arms out, a slow flail
//   land   0.35 s, held     a crouch that rises
//
// THE PROGRAM. Four states the game picks with `Animator.play`, no transitions:
//
//   locomotion   a ONE-DIMENSIONAL BLEND SPACE over `speed` — idle at 0, walk at 1.8, run at 5.5
//                metres a second — SYNCHRONISED by the markers, so the walk and the run put their
//                feet down together at every speed between; through FOOT PLACEMENT on both legs,
//                its weight the parameter `feet`
//   jump, fall   their clips
//   land         its clip through foot placement
//
// Foot placement asks the AnimationSystem's ground probe — the host hands it the physics world —
// where the ground is under each ankle, lowers the hips to the lower foot, and bends each knee to
// put its foot there: on a stair's tread, on the hill's slope.

#include <cy/animation/evaluate.h>
#include <cy/animation/library.h>
#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>

#include <memory>

namespace sample::tps {

/// The name the host registers the rig under, which the game attaches by.
inline constexpr const char* kHeroRig = "hero";

/// The hero's joints, in the skeleton's order. Parents come before their children.
enum HeroJoint : cy::u16 {
    kRoot = 0,
    kPelvis,
    kSpine,
    kChest,
    kNeck,
    kHead,
    kShoulderL,
    kElbowL,
    kWristL,
    kShoulderR,
    kElbowR,
    kWristR,
    kHipL,
    kKneeL,
    kAnkleL,
    kToeL,
    kHipR,
    kKneeR,
    kAnkleR,
    kToeR,
    kHeroJoints,
};

/// Every joint's name and its bind position in the model's metres (the bind rotations are the
/// identity, so a joint's model position is the sum of its chain's offsets).
struct HeroJointRow {
    const char* name = "";
    cy::u16 parent = 0;
    cy::Vec3 offset;
};
[[nodiscard]] const HeroJointRow* hero_skeleton() noexcept;
/// Joint `joint`'s bind position in the model.
[[nodiscard]] cy::Vec3 hero_bind_position(cy::u16 joint) noexcept;

/// Speeds the locomotion space places its samples at, metres a second.
inline constexpr cy::f32 kWalkSpeed = 1.8F;
inline constexpr cy::f32 kRunSpeed = 5.5F;

class HeroRig {
public:
    explicit HeroRig(cy::Allocator& allocator) noexcept;
    ~HeroRig();

    HeroRig(const HeroRig&) = delete;
    HeroRig& operator=(const HeroRig&) = delete;
    HeroRig(HeroRig&&) = delete;
    HeroRig& operator=(HeroRig&&) = delete;

    /// Cook the rig into the memory mount and load it back through the asset system.
    [[nodiscard]] cy::Status load() noexcept;

    /// The bound rig the loader produced. Valid after `load`, for the life of this object.
    [[nodiscard]] const cy::animation::AnimationRig& rig() const noexcept { return *rig_; }
    /// Records cooked, and what the library loaded.
    [[nodiscard]] cy::u32 records() const noexcept { return records_; }
    [[nodiscard]] const cy::animation::AnimationLibraryStats& stats() const noexcept;

    void shutdown() noexcept;

    /// The job system and async service the rig loads through, which the host's streamed
    /// ambience refills on too: a process has one job system. Valid after `load`.
    [[nodiscard]] cy::jobs::JobSystem& jobs() noexcept { return workers_; }
    [[nodiscard]] cy::jobs::AsyncService& async() noexcept { return async_; }

private:
    [[nodiscard]] cy::Status cook() noexcept;
    [[nodiscard]] cy::Status store(cy::AssetId id, const cy::Array<cy::u8>& bytes) noexcept;

    cy::Allocator* allocator_;
    cy::jobs::JobSystem workers_;
    cy::jobs::AsyncService async_;
    cy::assets::VirtualFileSystem files_;
    cy::assets::AssetSystem assets_;
    std::unique_ptr<cy::animation::AnimationLibrary> library_;
    const cy::animation::AnimationRig* rig_ = nullptr;
    cy::u32 records_ = 0;
    bool started_ = false;
};

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_HERO_RIG_H
