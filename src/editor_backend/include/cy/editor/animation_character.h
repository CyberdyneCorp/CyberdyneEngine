// SPDX-License-Identifier: MIT
// cy/editor/animation_character.h — the character the animation panel plays a graph on: the
// built-in mannequin, or a project's own imported one. Issue #29 (animation) and #112's gaps.
//
// A CHARACTER is a skeleton, the clips a graph can name, and a mesh to draw. The MANNEQUIN is
// built in code (twelve joints, `idle`, `walk`, `run`, `wave`, one box per bone), so a project with
// nothing imported can still author a graph. A PROJECT CHARACTER is what the importer cooked from
// the project's own model files (`tools/import`, steps 7 and 8, and M11.b's skin): a `skeleton/`
// sub-asset, the `animation/` sub-assets of every file whose clips were cooked for that skeleton,
// and the model's skinned `mesh/` sub-asset. The records are the runtime's own
// (`cy/animation/cooked.h`), read through an `AnimationAssetSource` the host provides — the editor
// window's runtime reads `<project>/.cy/cooked/<id>.cyasset`, a test reads memory.
//
// A CLIP IS NAMED AS THE GRAPH NAMES IT: the editor sends each clip with the name a `pose.clip`
// node gives it — the leaf of its sub-asset name, `animation/Walking` -> `Walking` — because the
// name a cooked clip carries is the FBX stack's, and every Mixamo export calls its stack
// `mixamo.com`. A clip whose tracks mean other joints than the skeleton's
// (`animation::clip_matches_skeleton`) is refused alone, by name, and the rest are kept.
//
// THE GRAPH'S EVENTS REPLACE THE CLIP'S. `build_clips` decodes every clip again and gives it the
// events its `pose.clip` node authors (`AnimationClipEvent`), in time order, so the preview fires
// what the author placed and the bake writes exactly that into the cooked clip a game loads.
//
// A MODEL WITH NO SKIN is drawn as one box per bone, from each joint towards its first child, so a
// skeleton-only import (an animation-only FBX) still shows what its clips do.

#pragma once

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>
#include <cy/core/math/vec.h>
#include <cy/editor/animation_service.h>

namespace cy::editor {

/// A character's mesh in its bind pose, four influences per vertex.
struct AnimationPreviewMesh {
    explicit AnimationPreviewMesh(Allocator& allocator) noexcept
        : positions(allocator),
          normals(allocator),
          joints(allocator),
          weights(allocator),
          indices(allocator) {}

    void clear() noexcept {
        positions.clear();
        normals.clear();
        joints.clear();
        weights.clear();
        indices.clear();
    }

    Array<Vec3> positions;
    Array<Vec3> normals;
    /// Four joint indices per vertex.
    Array<u16> joints;
    /// Four weights per vertex, summing to one.
    Array<f32> weights;
    Array<u32> indices;
};

/// Where a project character's cooked records come from. Implemented by the host.
class AnimationAssetSource {
public:
    AnimationAssetSource() = default;
    virtual ~AnimationAssetSource() = default;
    AnimationAssetSource(const AnimationAssetSource&) = delete;
    AnimationAssetSource& operator=(const AnimationAssetSource&) = delete;
    AnimationAssetSource(AnimationAssetSource&&) = delete;
    AnimationAssetSource& operator=(AnimationAssetSource&&) = delete;

    /// The record of cooked asset `id`, without its cooked header.
    [[nodiscard]] virtual Status read(AssetId id, Array<u8>& payload) noexcept = 0;
    /// The skinned mesh `id` in its bind pose, its joints the skeleton's. Refused for a mesh with
    /// no skin.
    [[nodiscard]] virtual Status read_mesh(AssetId id, AnimationPreviewMesh& out) noexcept = 0;
};

/// A skeleton, its clips and its mesh. See the file comment.
class AnimationCharacter final : public AnimationClipCatalogue {
public:
    explicit AnimationCharacter(Allocator& allocator) noexcept;
    ~AnimationCharacter() override;

    /// Build the mannequin.
    [[nodiscard]] Status load_mannequin() noexcept;
    /// Load a project character through `source`. A nil skeleton loads the mannequin. Refused,
    /// with this character emptied, when the skeleton or the mesh does not load.
    [[nodiscard]] Status load(const AnimationCharacterRequest& request,
                              AnimationAssetSource& source) noexcept;

    [[nodiscard]] Span<const AnimationClipInfo> clips() const noexcept override {
        return infos_.span();
    }
    [[nodiscard]] u32 joint_count() const noexcept override { return skeleton_.joint_count(); }

    [[nodiscard]] const animation::Skeleton& skeleton() const noexcept { return skeleton_; }
    /// The skeleton's joint names, in joint order: what a cooked clip's tracks are checked and
    /// written against.
    [[nodiscard]] Span<const Name> joint_names() const noexcept { return joint_names_.span(); }
    [[nodiscard]] const AnimationPreviewMesh& mesh() const noexcept { return mesh_; }
    [[nodiscard]] std::string_view model() const noexcept { return {model_.data(), model_.size()}; }
    [[nodiscard]] bool from_project() const noexcept { return project_; }
    [[nodiscard]] bool skinned() const noexcept { return skinned_; }
    [[nodiscard]] Span<const AnimationClipRefusal> refused() const noexcept {
        return refused_.span();
    }
    /// Every clip, named as the graph names it, with `events` in place of its own, into `out`.
    [[nodiscard]] Status build_clips(Span<const AnimationClipEvent> events,
                                     Array<animation::Clip>& out) const noexcept;

private:
    /// A project clip as cooked, decoded again whenever it is given events.
    struct ProjectClip {
        explicit ProjectClip(Allocator& allocator) noexcept : payload(allocator) {}

        Name name;
        Array<u8> payload;
    };

    void reset() noexcept;
    [[nodiscard]] Status take_clip(const AnimationCharacterClip& wanted,
                                   AnimationAssetSource& source) noexcept;
    [[nodiscard]] Status finish_skeleton() noexcept;
    [[nodiscard]] Status bone_boxes() noexcept;

    Allocator* allocator_;
    animation::Skeleton skeleton_;
    Array<Name> joint_names_;
    Array<AnimationClipInfo> infos_;
    Array<ProjectClip> project_clips_;
    Array<AnimationClipRefusal> refused_;
    AnimationPreviewMesh mesh_;
    Array<char> model_;
    bool project_ = false;
    bool skinned_ = false;
};

}  // namespace cy::editor
