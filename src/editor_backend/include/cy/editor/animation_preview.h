// SPDX-License-Identifier: MIT
// cy/editor/animation_preview.h — the character the animation panel previews on. Issue #29.
//
// THE ENGINE EVALUATES; THE EDITOR SHOWS. `animation.preview.set` hands this object a compiled
// program and what to show, and it evaluates the pose the way a game's frame does: the program's
// state machine and clocks through `animation::advance`, the pose through `animation::evaluate`,
// the model and skinning matrices through the skeleton. A host draws the skinned mesh with those
// matrices in its viewport (the editor window's runtime does, through `SkinnedScene`), so what the
// panel previews is what the engine evaluates and draws, not a picture the editor made.
//
// THE CHARACTER is the mannequin until the editor names a project's own
// (`animation.character.set`): `cy/editor/animation_character.h` says what each is. A host that
// can read the project's cooked assets hands the preview an `AnimationAssetSource`; one that cannot
// plays the mannequin alone, and a project character is refused by name.
//
// The clips carry no events of their own: a clip's events are the graph's (`events` on its
// `pose.clip` node), given to the clip at each preview, so what fires is what was authored. A
// project clip also loops or holds as its node says, as the bake writes it for a game; the
// mannequin's clips keep their own.

#pragma once

#include <cy/animation/clip.h>
#include <cy/animation/evaluate.h>
#include <cy/animation/skeleton.h>
#include <cy/core/math/matrix.h>
#include <cy/editor/animation_character.h>
#include <cy/editor/animation_service.h>

#include <memory>

namespace cy::editor {

/// The engine's animation preview. See the file comment.
class AnimationPreview final : public AnimationPreviewRuntime {
public:
    explicit AnimationPreview(Allocator& allocator) noexcept;
    ~AnimationPreview() override;

    /// Build the mannequin. Refused when the skeleton, a clip or the mesh cannot be built.
    [[nodiscard]] Status initialize() noexcept;

    /// Where a project character's cooked records are read. Null: the mannequin alone.
    void set_source(AnimationAssetSource* source) noexcept { source_ = source; }

    [[nodiscard]] Span<const AnimationClipInfo> clips() const noexcept override;
    [[nodiscard]] u32 joint_count() const noexcept override;
    [[nodiscard]] Status set_character(const AnimationCharacterRequest& request) noexcept override;
    [[nodiscard]] std::string_view character_model() const noexcept override {
        return character_->model();
    }
    [[nodiscard]] bool character_from_project() const noexcept override {
        return character_->from_project();
    }
    [[nodiscard]] bool character_skinned() const noexcept override { return character_->skinned(); }
    [[nodiscard]] Span<const AnimationClipRefusal> refused_clips() const noexcept override {
        return character_->refused();
    }
    [[nodiscard]] Status preview(graph::pose::PoseProgram&& program,
                                 const AnimationPreviewRequest& request) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] const AnimationPreviewState& state() const noexcept override { return state_; }
    [[nodiscard]] Span<const Transform> pose() const noexcept override { return local_.span(); }
    [[nodiscard]] Span<const AnimationFiredEvent> events() const noexcept override {
        return fired_.span();
    }

    /// Advance a playing preview by `seconds` of wall time: a clip plays on, looping or holding
    /// as it does in a game, and a state machine runs in `kAnimationPreviewStep` steps.
    [[nodiscard]] Status tick(f32 seconds) noexcept;

    /// The matrices a skinning pass multiplies each vertex by, `model * inverse bind`, one per
    /// joint.
    [[nodiscard]] Span<const Mat4> skinning_matrices() const noexcept { return skinning_.span(); }
    /// Each joint's model-space transform this evaluation.
    [[nodiscard]] Span<const Transform> model_pose() const noexcept { return model_.span(); }
    [[nodiscard]] const AnimationPreviewMesh& mesh() const noexcept { return character_->mesh(); }
    [[nodiscard]] const animation::Skeleton& skeleton() const noexcept {
        return character_->skeleton();
    }
    /// Changes whenever another character is set, so a host uploads its mesh again.
    [[nodiscard]] u64 mesh_generation() const noexcept { return mesh_generation_; }
    /// The character being played.
    [[nodiscard]] const AnimationCharacter& character() const noexcept { return *character_; }
    /// The character's clip of that name with the authored events of the last preview, or null.
    [[nodiscard]] const animation::Clip* clip(Name name) const noexcept;

private:
    [[nodiscard]] Status rebuild_clips(Span<const AnimationClipEvent> events) noexcept;
    [[nodiscard]] Status bind(graph::pose::PoseProgram&& program) noexcept;
    [[nodiscard]] Status show_clip(f32 time, f32 previous, bool continuing) noexcept;
    [[nodiscard]] Status restart_machine() noexcept;
    [[nodiscard]] Status run_machine(f32 seconds, f32 report_after) noexcept;
    [[nodiscard]] Status step_machine(f32 step, f32 report_after) noexcept;
    [[nodiscard]] Status evaluate_machine() noexcept;
    [[nodiscard]] Status finish() noexcept;
    void remember(const animation::EmittedEvent& event, f32 at) noexcept;

    /// Size the pose buffers for the character and show its reference pose.
    [[nodiscard]] Status adopt_character() noexcept;

    Allocator* allocator_;
    std::unique_ptr<AnimationCharacter> character_;
    AnimationAssetSource* source_ = nullptr;
    Array<animation::Clip> clips_;
    Array<AnimationClipEvent> events_;
    graph::pose::PoseProgram program_;
    std::unique_ptr<animation::AnimationRig> rig_;
    std::unique_ptr<animation::AnimationInstance> instance_;
    std::unique_ptr<animation::PoseScratch> scratch_;
    std::unique_ptr<animation::ClipCursor> cursor_;
    animation::EventBuffer buffer_;
    Array<AnimationParameter> parameters_;
    Array<Transform> local_;
    Array<Transform> model_;
    Array<Mat4> skinning_;
    Array<AnimationFiredEvent> fired_;
    AnimationPreviewState state_;
    /// The focused clip's index in `clips_`, while a clip is focused.
    u32 focus_clip_ = 0;
    /// Wall time a playing state machine has not yet spent in whole steps.
    f32 pending_ = 0.0F;
    u64 sequence_ = 0;
    u64 mesh_generation_ = 1;
    bool initialized_ = false;
};

}  // namespace cy::editor
