// SPDX-License-Identifier: MIT
#ifndef CY_IMPORT_ANIMATION_COOK_H
#define CY_IMPORT_ANIMATION_COOK_H
// The cook half of an animated character: imported rigs and clips in, the three cooked records the
// runtime loads out. Issue #76 stage 2.
//
// ================================================================================================
// WHAT IT DOES
// ================================================================================================
//
// Steps 7 and 8 of the importer produce one skeleton record and one clip record per source file,
// each against that FILE's rig. A character is assembled from several such files — a Mixamo
// character arrives as one export per motion — so before anything can play, three things have to
// happen that belong to no single import:
//
//   1. the character's skeleton is chosen (the rig of the file named `rig`);
//   2. every clip is brought onto it: used as it is when its rig is the character's (same
//      hierarchy, same rest pose — `compare_rigs`), and otherwise RETARGETED by the measured
//      correspondence and baked (`build_retarget_profile`, `bake_clip`), which is what
//      `retarget.h` recommends for the combinations a game ships;
//   3. the locomotion machine is compiled over the clips' names and durations
//      (`compile_locomotion`) — cook time, so the runtime never compiles.
//
// The output is BYTES in the records `cy/animation/cooked.h` reads: a skeleton, one clip per
// source named as the program refers to it, and the program. The `animation` producer in
// tools/build/ runs this as a build-graph node; samples/09b-animated-character runs the same
// function, which is what makes the sample's character the one a cook would ship.
//
// This header names no animation type, so it compiles in every build. With `CY_ANIMATION=OFF`
// there is no runtime to cook against and `cook_locomotion_set` refuses with `Unsupported`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>

#include <string>
#include <vector>

namespace cy::import {

#if defined(CY_IMPORT_ANIMATION)
inline constexpr bool kAnimationCookAvailable = true;
#else
inline constexpr bool kAnimationCookAvailable = false;
#endif

/// One clip of the set: the import that produced it, and the name the program calls it by.
struct AnimationClipSource {
    const ImportResult* import = nullptr;
    /// `idle`, `walk`, `run` or `die` for the locomotion machine.
    std::string name;
    /// False holds the clip on its last frame — a death — instead of looping it. FBX carries no
    /// loop flag, so this is the cook's decision and is made here, once.
    bool looping = true;
};

/// `graph::pose::LocomotionSpec`'s blend durations, in seconds, without naming the type.
struct LocomotionBlends {
    f32 idle_to_walk = 0.20F;
    f32 walk_to_run = 0.15F;
    f32 run_to_walk = 0.25F;
    f32 walk_to_idle = 0.25F;
    f32 to_die = 0.30F;
};

struct AnimationCookSpec {
    std::string name = "locomotion";
    /// The import whose skeleton is the character's.
    const ImportResult* rig = nullptr;
    /// Exactly the four locomotion clips, in any order.
    std::vector<AnimationClipSource> clips;
    LocomotionBlends blends;
};

/// One cooked clip and what cooking it measured, so a report can say what was done to it.
struct CookedAnimationClip {
    std::string name;
    Array<u8> bytes;
    /// True when the source rig was not the character's and the clip was baked onto it.
    bool retargeted = false;
    /// What `compare_rigs` measured between the source rig and the character's, before the
    /// retarget reconciled them.
    f32 rest_difference_degrees = 0.0F;
    f32 rest_difference_metres = 0.0F;
    u32 retarget_pairs = 0;
    f32 height_scale = 1.0F;
    /// The source clip as imported, and the clip as cooked.
    f32 duration = 0.0F;
    u32 source_tracks = 0;
    u32 source_keys = 0;
    u32 tracks = 0;
    u32 keys = 0;
    f32 worst_rotation_degrees = 0.0F;
};

struct CookedAnimationSet {
    Array<u8> skeleton;
    Array<u8> program;
    std::vector<CookedAnimationClip> clips;
    u32 joints = 0;
    u32 humanoid_mapped = 0;
};

/// Cook a locomotion character: the rig's skeleton, the four clips on it, and the compiled
/// machine. Fails, naming what is wrong, on a source with no skeleton or no clip, a clip whose rig
/// cannot be mapped onto the character's, a missing or duplicated locomotion clip name, and a blend
/// duration the locomotion builder refuses.
[[nodiscard]] Status cook_locomotion_set(const AnimationCookSpec& spec,
                                         CookedAnimationSet& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_ANIMATION_COOK_H
