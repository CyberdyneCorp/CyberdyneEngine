// SPDX-License-Identifier: MIT
// cy/editor/animation_rig.h — a graph baked into the rig a game loads. Issue #112's gaps (#29).
//
// THE AUTHORED EVENTS REACH THE GAME HERE. The animation panel places a clip's events on its
// `pose.clip` node; the preview gives them to the clip it plays, and nothing a game loads carried
// them. `animation.bake` cooks a graph for a project character into the records the runtime loads
// (`cy/animation/cooked.h`, through `AnimationLibrary`):
//
//   program.cyasset      the compiled program, so the game links no compiler;
//   clips/<n>.cyasset    every clip the program names, decoded from the character's cooked clip,
//                        renamed to the name the graph gives it, looping or holding as its node
//                        says, with the graph's authored events IN PLACE of the clip's own;
//   rig.cyrig            this manifest: the rig's name, the character's cooked skeleton and mesh
//                        by asset id, the program and each clip by name and file.
//
// Each record is a cooked asset (`assets::write_cooked_asset`, kind animation), written under
// `<project>/.cy/cooked/animation/<rig>/` by the editor. A host loads the rig through
// `AnimationLibrary` and registers it under `rig` — the editor window's Play does, so a Swift
// behaviour's `Animator.attach(to:rig:)` plays it and `Animation.events(for:)` delivers the
// authored events at the times they were placed.
//
// THE MANIFEST, `cyrig 1`, one entry per line, every value a double-quoted string with no quote
// or line break inside it:
//
//     cyrig 1
//     rig "hero"
//     model "characters/hero.fbx"
//     skeleton "<32 hex digits>"
//     mesh "<32 hex digits, or empty>"
//     program "program.cyasset"
//     clip "Walking" "clips/0.cyasset"

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/core/values/name.h>
#include <cy/editor/animation_character.h>
#include <cy/editor/animation_service.h>

#include <string_view>

namespace cy::editor {

/// The manifest's file name inside a rig's directory.
inline constexpr std::string_view kAnimationRigManifest = "rig.cyrig";
/// Where the editor writes a project's baked rigs, project-relative; each is a directory.
inline constexpr std::string_view kAnimationRigDirectory = ".cy/cooked/animation";

/// One clip of a baked rig: the name the program binds it by, and its file in the rig directory.
struct AnimationRigClip {
    Name name;
    Name path;
};

/// A `cyrig 1` manifest.
struct AnimationRigManifest {
    explicit AnimationRigManifest(Allocator& allocator) noexcept : clips(allocator) {}

    Name rig;
    Name model;
    AssetId skeleton;
    /// Nil when the character has no skinned mesh.
    AssetId mesh;
    Name program;
    Array<AnimationRigClip> clips;
};

/// Write `manifest` as `cyrig 1` text into `out`.
[[nodiscard]] Status write_animation_rig(const AnimationRigManifest& manifest,
                                         Array<char>& out) noexcept;
/// Read `cyrig 1` text into `out`, replacing all it held. Refused for any other text, and for a
/// manifest that names no rig, skeleton or program.
[[nodiscard]] Status read_animation_rig(std::string_view text, AnimationRigManifest& out) noexcept;

/// `animation.bake`'s cook over a project's cooked assets. See the file comment.
class AnimationRigBaker final : public AnimationBakeRuntime {
public:
    AnimationRigBaker(Allocator& allocator, AnimationAssetSource& source) noexcept
        : allocator_(&allocator), source_(&source) {}

    [[nodiscard]] Status bake(const AnimationBakeRequest& request,
                              AnimationBakeResult& out) noexcept override;

private:
    Allocator* allocator_;
    AnimationAssetSource* source_;
};

}  // namespace cy::editor
