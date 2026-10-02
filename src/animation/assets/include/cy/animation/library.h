// SPDX-License-Identifier: MIT
#pragma once
// Cooked skeletons, clips and programs loaded through the asset system and bound into rigs by
// asset id. Issue #76 stage 2.
//
// ================================================================================================
// WHAT THIS IS, AND WHERE IT SITS
// ================================================================================================
//
// `AssetSystem` (src/core/assets/) loads cooked BYTES: it reads, decompresses, coalesces and
// reference-counts an asset by id, and it knows nothing of what the bytes mean — `AssetData` is
// "the cooked blob; a typed layer needs types". This is the typed layer for animation: it asks the
// asset system for an id, decodes the payload with `cooked.h` into the runtime object, keeps the
// object for as long as the library lives, and hands out a stable pointer to it.
//
// BINDING IS BY NAME, AND A MISS IS A REFUSAL. A compiled program names its clips (`ClipRef::name`)
// and `AnimationRig::bind` takes a table parallel to them in which a null entry is legal and
// silently samples the reference pose. So `rig()` matches every clip the program names against the
// clips it was given BY NAME and refuses a miss, naming the clip; and it checks every clip's
// tracks against the skeleton's joints by the joint names the cooked clip carries, refusing a clip
// cooked for another rig, naming the joint. Each refusal is also emitted as a diagnostic in the
// "animation" category, so it reaches a log and an editor without the caller printing it.
//
// HOT RELOAD. `watch()` registers with the asset system's reload notification. When a clip's
// cooked bytes are replaced (`AssetSystem::reload`, which the editor's file watcher drives), the
// new clip is decoded and checked against every rig that uses it, and only then moved INTO THE SAME
// `Clip` object, so every rig's table and every instance keeps the pointer it holds; the rigs are
// rebound so a changed duration reaches the clocks. A reload that does not decode, or that no
// longer matches a skeleton using it, is refused and the working clip stays. A skeleton or a
// program is not swapped under live instances — their state is laid out from both — so their
// reloads are refused with a diagnostic; rebuild the rig instead.

#include <cy/animation/evaluate.h>
#include <cy/animation/skeleton.h>
#include <cy/core/assets/asset_system.h>
#include <cy/core/base/diagnostic_sink.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/values/asset_id.h>

namespace cy::animation {

/// The asset ids one rig is bound from.
struct RigAssets {
    AssetId skeleton;
    AssetId program;
    /// Every clip the rig may need. Matched to the program's clip table BY NAME; order does not
    /// matter, and a clip the program does not name is ignored.
    Span<const AssetId> clips;
};

struct AnimationLibraryStats {
    u32 skeletons = 0;
    u32 clips = 0;
    u32 programs = 0;
    u32 rigs = 0;
    u64 refusals = 0;
    u64 reloads_applied = 0;
    u64 reloads_refused = 0;
};

class AnimationLibrary {
public:
    AnimationLibrary(Allocator& allocator, assets::AssetSystem& assets) noexcept;
    ~AnimationLibrary();

    AnimationLibrary(const AnimationLibrary&) = delete;
    AnimationLibrary& operator=(const AnimationLibrary&) = delete;
    AnimationLibrary(AnimationLibrary&&) = delete;
    AnimationLibrary& operator=(AnimationLibrary&&) = delete;

    /// Load and decode, once per id. The pointer is stable for the library's life.
    [[nodiscard]] Expected<const Skeleton*, Error> skeleton(AssetId id) noexcept;
    [[nodiscard]] Expected<const Clip*, Error> clip(AssetId id) noexcept;
    [[nodiscard]] Expected<const graph::pose::PoseProgram*, Error> program(AssetId id) noexcept;
    /// The humanoid profile the cooked skeleton carried, or null when `id` is not loaded.
    [[nodiscard]] const SkeletonProfile* humanoid(AssetId id) const noexcept;

    /// Load the three kinds and bind a rig, refusing a clip the program names and the list lacks,
    /// and a clip whose tracks mean other joints than the skeleton's.
    [[nodiscard]] Expected<const AnimationRig*, Error> rig(const RigAssets& wanted) noexcept;

    /// Be told when a loaded asset's bytes are replaced. Idempotent.
    [[nodiscard]] Status watch() noexcept;

    /// The text of the last refusal, which a refused call's `Error::message` points at. Valid
    /// until the next refusal or the library's destruction.
    [[nodiscard]] const char* last_refusal() const noexcept { return refusal_; }
    [[nodiscard]] const AnimationLibraryStats& stats() const noexcept { return stats_; }

private:
    struct LoadedSkeleton;
    struct LoadedClip;
    struct LoadedProgram;
    struct BoundRig;

    [[nodiscard]] Expected<Ref<assets::AssetData>, Error> load(AssetId id) noexcept;
    [[nodiscard]] LoadedSkeleton* find_skeleton(AssetId id) const noexcept;
    [[nodiscard]] LoadedClip* find_clip(AssetId id) const noexcept;
    [[nodiscard]] LoadedProgram* find_program(AssetId id) const noexcept;
    [[nodiscard]] Expected<LoadedClip*, Error> load_clip(AssetId id) noexcept;
    [[nodiscard]] Status bind_table(BoundRig& rig, Span<LoadedClip* const> available) noexcept;
    [[nodiscard]] Status reload_clip(LoadedClip& loaded) noexcept;
    static void on_reload(void* user, const assets::ReloadEvent& event) noexcept;

    /// Record a refusal: its text, the diagnostic, and an `Error` whose message is that text.
    [[nodiscard]] Error refuse(ErrorCode code, const char* format, ...) noexcept
        CY_PRINTF_FORMAT(3, 4);

    Allocator* allocator_;
    assets::AssetSystem* assets_;
    Array<UniquePtr<LoadedSkeleton>> skeletons_;
    Array<UniquePtr<LoadedClip>> clips_;
    Array<UniquePtr<LoadedProgram>> programs_;
    Array<UniquePtr<BoundRig>> rigs_;
    AnimationLibraryStats stats_;
    char refusal_[256] = {};
    bool watching_ = false;
};

}  // namespace cy::animation
