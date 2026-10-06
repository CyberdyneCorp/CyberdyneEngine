// SPDX-License-Identifier: MIT
// worker_rig.h — the worker's animation, authored in code, cooked, and loaded through the asset
// system. Issue #76 stage 4.
//
// THE HOST IS ALSO THE COOK. A shipped game loads a rig a build step cooked; this sample has no
// content pipeline of its own, so it builds the rig the way that step would — a skeleton, three
// clips and a three-state program compiled from a pose graph — encodes each as the cooked record
// (`cy/animation/cooked.h`), writes the records into a memory mount under fixed asset ids, and then
// LOADS them back by id through `AssetSystem` and `AnimationLibrary`, binding the program's clip
// table by name. Nothing the game animates is the object the cook built: it is what the loader
// read.
//
// THE RIG. Eight joints (a root, hips, spine, head, two arms, two legs) and three states:
//
//   idle    two seconds, looping: the spine sways.
//   walk    one second, looping: the legs swing against the arms; a `footstep` event at each
//           footfall, a quarter and three quarters through.
//   cheer   0.8 s, held: both arms go up; a `cheer_done` event near its end.
//
// The program has NO TRANSITIONS. Which state a worker is in is the Swift game's decision, made
// with `Animator.play`, which crossfades to any state whatever the program says.

#ifndef CY_SAMPLE_RTS_API_WORKER_RIG_H
#define CY_SAMPLE_RTS_API_WORKER_RIG_H

#include <cy/animation/evaluate.h>
#include <cy/animation/library.h>
#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/allocator.h>

#include <memory>

namespace sample::rts {

/// The name the host registers the rig under, which the game attaches by.
inline constexpr const char* kWorkerRig = "worker";
/// The worker skeleton's joint count.
inline constexpr cy::u32 kWorkerJoints = 8;

class WorkerRig {
public:
    explicit WorkerRig(cy::Allocator& allocator) noexcept;
    ~WorkerRig();

    WorkerRig(const WorkerRig&) = delete;
    WorkerRig& operator=(const WorkerRig&) = delete;
    WorkerRig(WorkerRig&&) = delete;
    WorkerRig& operator=(WorkerRig&&) = delete;

    /// Cook the rig into the memory mount and load it back through the asset system.
    [[nodiscard]] cy::Status load() noexcept;

    /// The bound rig the loader produced. Valid after `load`, for the life of this object.
    [[nodiscard]] const cy::animation::AnimationRig& rig() const noexcept { return *rig_; }
    /// How the rig arrived: records cooked, and what the library loaded.
    [[nodiscard]] cy::u32 records() const noexcept { return records_; }
    [[nodiscard]] const cy::animation::AnimationLibraryStats& stats() const noexcept;

    void shutdown() noexcept;

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

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_WORKER_RIG_H
