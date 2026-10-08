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
// THE CHARACTER is built in code, as `samples/13-rts-api`'s worker is: a twelve-joint mannequin
// (root, hips, spine, head, two arms of two bones, two legs of two bones), four clips, and a mesh
// of one box per bone, each vertex bound to its bone. The editor imports no skinned model yet, so a
// project's own character cannot be previewed; the panel says so.
//
//   idle  2 s, looping: the spine sways and the arms hang.
//   walk  1 s, looping: legs and arms swing against each other, knees bend on the passing leg.
//   run   0.6 s, looping: the walk's swing, wider, with the elbows bent.
//   wave  1.5 s, held: the right arm rises and the forearm waves.
//
// The clips carry no events of their own: a clip's events are the graph's (`events` on its
// `pose.clip` node), given to the clip at each preview, so what fires is what was authored.

#pragma once

#include <cy/animation/clip.h>
#include <cy/animation/evaluate.h>
#include <cy/animation/skeleton.h>
#include <cy/core/math/matrix.h>
#include <cy/editor/animation_service.h>

#include <memory>

namespace cy::editor {

/// The preview character's mesh in its bind pose: one box per bone, four influences per vertex.
struct AnimationPreviewMesh {
    explicit AnimationPreviewMesh(Allocator& allocator) noexcept
        : positions(allocator),
          normals(allocator),
          joints(allocator),
          weights(allocator),
          indices(allocator) {}

    Array<Vec3> positions;
    Array<Vec3> normals;
    /// Four joint indices per vertex.
    Array<u16> joints;
    /// Four weights per vertex, summing to one.
    Array<f32> weights;
    Array<u32> indices;
};

/// The engine's animation preview. See the file comment.
class AnimationPreview final : public AnimationPreviewRuntime {
public:
    explicit AnimationPreview(Allocator& allocator) noexcept;
    ~AnimationPreview() override;

    /// Build the character. Refused when the skeleton, a clip or the mesh cannot be built.
    [[nodiscard]] Status initialize() noexcept;

    [[nodiscard]] Span<const AnimationClipInfo> clips() const noexcept override;
    [[nodiscard]] u32 joint_count() const noexcept override;
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
    [[nodiscard]] const AnimationPreviewMesh& mesh() const noexcept { return mesh_; }
    [[nodiscard]] const animation::Skeleton& skeleton() const noexcept { return skeleton_; }
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

    Allocator* allocator_;
    animation::Skeleton skeleton_;
    Array<animation::Clip> clips_;
    Array<AnimationClipInfo> infos_;
    Array<AnimationClipEvent> events_;
    AnimationPreviewMesh mesh_;
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
    bool initialized_ = false;
};

}  // namespace cy::editor
