// SPDX-License-Identifier: MIT
// cy/editor/animation_service.h — the `animation.*` operations of the editor backend service: the
// animation panel's engine side. Issue #29, animation.
//
// The editor authors a pose graph as CyberGraph text (`cygraph 1`, `cy/graph/text.h`) over
// `cy::graph::pose`'s vocabulary and owns none of what it means. The engine answers:
//
//   animation.catalogue.get  ()                      -> the pose vocabulary: types, pins,
//   properties animation.compile        (u32 1, text source)    -> the compiled program, or what
//   refused it animation.preview.set    (u32 1, text source,    -> the preview's state after
//   evaluating the
//                             u64 focus, f32 time,      graph (or the focused clip) at `time`
//                             u8 playing, u32 n,
//                             n x (text name, f32))
//   animation.preview.get    ()                      -> the preview's state
//   animation.preview.stop   ()                      -> the preview's state, inactive
//
// THE CATALOGUE is the material catalogue's schema 3 (`material.catalogue.get`), as `script.*`'s
// is, so one decoder reads all three. Its node types are `graph::pose::register_pose_nodes`', with
// their pins — a state's `state` output is what a transition's `from` and `to` take — and their
// properties. A `pose.clip`'s `clip` is a choice among the preview character's clips when the host
// has one, so the palette offers exactly what the preview can play. A property's `semantic` is
// `literal:<type>`, the literal type its value is written at in the graph's text.
//
// A CLIP'S EVENTS are its `pose.clip` node's `events` property: `name@seconds` items separated by
// `;` (`footstep@0.25; footstep@0.75`). A cooked program has no event table — events belong to the
// clip — so the preview gives the clip the node's events, and the compile checks each one.
//
// A COMPILE always completes. Its reply: u32 1, u8 compiled, u64 the source's semantic digest, u64
// the program's digest, u32 joints, u32 instructions; u32 states and per state (text name, u64
// node, u32 transitions); u32 transitions and per transition (u64 node, u32 from, u32 to, text
// condition, f32 duration, u32 priority, u8 interruption: 0 none, 1 higher priority, 2 any); u32
// clips and per clip (text name, f32 duration, u8 looping, u8 known to the preview, u32 events);
// u32 parameters and per parameter (text name, u8 kind: 0 set by the author, 1 a clip's clock the
// runtime advances); then the diagnostics as `script.compile` writes them (u32 count; per
// diagnostic u8 severity, text code, u64 node, text pin, text message, text detail, u64 related
// node). `compiled` is 0 when any diagnostic is an error.
//
// The compile is `graph::pose::compile_pose` after `graph::validate`, and adds the authoring checks
// a compiled program cannot carry: a transition wired to fewer than two states, a transition whose
// blend is not positive (a cut pops the pose: `locomotion.h`'s decision 3, and a game that wants
// one asks `Animator.play` for it), a transition with no condition, a state with no pose, a clip
// the preview character does not have, and an event that does not parse or lies outside its clip.
//
// THE PREVIEW STATE: u32 1, u8 active, u8 playing, u64 focus, text focus clip, f32 time, f32
// length, u32 state, text state name, u32 target (0xFFFF: no blend), text target name, f32 blend,
// u64 program digest, u64 pose digest, u64 generation; u32 joints and per joint ten f32 (local
// translation x y z, rotation x y z w, scale x y z); u32 events and per event, oldest first (u64
// sequence, text name, f32 normalised time, f32 preview time). The pose digest is FNV-1a over the
// joints' bits, so two evaluations agree exactly or differ. `generation` counts evaluations.
//
// FOCUS zero previews the state machine: from its entry state, the parameters as given, advanced
// from zero to `time` in steps of `kAnimationPreviewStep` (the last one shorter). A focus naming a
// `pose.clip` node previews that clip alone, sampled at `time`. While `playing` the host advances
// the preview each frame; a graph preview restarts after `kAnimationGraphPreviewSeconds`.
//
// Refusals (`u32 1, text code, text detail`): animation.request.malformed,
// animation.schema.unsupported, animation.operation.unsupported, animation.preview.unavailable (no
// preview runtime: the host has no character, or the build has no animation), animation.preview.
// uncompiled (the graph has an error; `animation.compile` names it), animation.preview.focus (the
// focus is not a clip node of the graph), animation.preview.failed (the runtime refused it).

#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/graph/lower_pose.h>

#include <array>
#include <string_view>

namespace cy::editor {

/// The request and reply format every `animation.*` payload begins with.
inline constexpr u32 kAnimationWireFormat = 1;

/// Every operation `animation.*` serves, in the order `capabilities.get` lists them.
inline constexpr std::array<std::string_view, 5> kAnimationOperations{
    "animation.catalogue.get", "animation.compile", "animation.preview.set",
    "animation.preview.get", "animation.preview.stop"};

/// The step a state machine preview is advanced in, so a scrub lands on the pose a run of the
/// same ticks reaches: sixty per second.
inline constexpr f32 kAnimationPreviewStep = 1.0F / 60.0F;
/// How long a playing state machine preview runs before it starts again from its entry state.
inline constexpr f32 kAnimationGraphPreviewSeconds = 4.0F;
/// The most recent events the preview keeps for its state.
inline constexpr u32 kAnimationPreviewEvents = 16;

/// One clip the preview can play.
struct AnimationClipInfo {
    Name name;
    f32 duration = 0.0F;
    bool looping = true;
};

/// One authored event, as a `pose.clip` node's `events` property gives it.
struct AnimationClipEvent {
    Name clip;
    Name event;
    f32 time = 0.0F;
};

/// One parameter the author set for a preview.
struct AnimationParameter {
    Name name;
    f32 value = 0.0F;
};

/// What `animation.preview.set` asks the host's preview to show.
struct AnimationPreviewRequest {
    /// The `pose.clip` node previewed alone, or zero for the state machine.
    u64 focus = 0;
    /// The focused node's clip.
    Name focus_clip;
    f32 time = 0.0F;
    bool playing = false;
    /// Every clip's authored events, which replace whatever the clip carried.
    Span<const AnimationClipEvent> events;
    Span<const AnimationParameter> parameters;
};

/// One event the preview's playback crossed.
struct AnimationFiredEvent {
    u64 sequence = 0;
    Name name;
    f32 normalised_time = 0.0F;
    /// The preview time it was crossed at.
    f32 at = 0.0F;
};

/// The preview's scalars. Its pose and its recent events are `AnimationPreviewRuntime::pose` and
/// `::events`.
struct AnimationPreviewState {
    bool active = false;
    bool playing = false;
    u64 focus = 0;
    Name focus_clip;
    f32 time = 0.0F;
    /// The focused clip's duration, or `kAnimationGraphPreviewSeconds`.
    f32 length = 0.0F;
    u16 state = 0;
    Name state_name;
    /// The state being blended towards, or `0xFFFF`.
    u16 target = 0xFFFFU;
    Name target_name;
    /// How far the blend has gone, 0 to 1; zero with no blend.
    f32 blend = 0.0F;
    u64 program_digest = 0;
    /// FNV-1a over the pose's bits.
    u64 pose_digest = 0;
    /// Evaluations so far.
    u64 generation = 0;
};

/// The host's preview character, as the animation panel reaches it. Implemented by
/// `AnimationPreview` (`cy/editor/animation_preview.h`) in a build with animation; the service
/// never evaluates a pose itself.
class AnimationPreviewRuntime {
public:
    AnimationPreviewRuntime() = default;
    virtual ~AnimationPreviewRuntime() = default;
    AnimationPreviewRuntime(const AnimationPreviewRuntime&) = delete;
    AnimationPreviewRuntime& operator=(const AnimationPreviewRuntime&) = delete;
    AnimationPreviewRuntime(AnimationPreviewRuntime&&) = delete;
    AnimationPreviewRuntime& operator=(AnimationPreviewRuntime&&) = delete;

    /// The clips a graph can name.
    [[nodiscard]] virtual Span<const AnimationClipInfo> clips() const noexcept = 0;
    /// The skeleton's joint count, which the graph is compiled for.
    [[nodiscard]] virtual u32 joint_count() const noexcept = 0;
    /// Show `program` as `request` says, replacing what was shown.
    [[nodiscard]] virtual Status preview(graph::pose::PoseProgram&& program,
                                         const AnimationPreviewRequest& request) noexcept = 0;
    /// Show nothing.
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual const AnimationPreviewState& state() const noexcept = 0;
    /// The evaluated pose, local to each joint's parent.
    [[nodiscard]] virtual Span<const Transform> pose() const noexcept = 0;
    /// The most recent events, oldest first.
    [[nodiscard]] virtual Span<const AnimationFiredEvent> events() const noexcept = 0;
};

/// Why an `animation.*` request was refused, or an empty code when it was answered.
struct AnimationRefusal {
    const char* code = nullptr;
    const char* detail = nullptr;

    [[nodiscard]] bool refused() const noexcept { return code != nullptr; }
};

/// The pose vocabulary in the material catalogue's schema 3; `preview` (may be null) supplies the
/// clip choices. Deterministic.
[[nodiscard]] Status encode_animation_catalogue(const AnimationPreviewRuntime* preview,
                                                Array<u8>& out) noexcept;

/// Compile `source` and encode the reply described above, checking clips against `preview` when
/// there is one. Fails only for memory.
[[nodiscard]] Status encode_animation_compile(const AnimationPreviewRuntime* preview,
                                              std::string_view source, Array<u8>& out) noexcept;

/// Encode the preview's state. `preview` may be null, which encodes "inactive".
[[nodiscard]] Status encode_animation_state(const AnimationPreviewRuntime* preview,
                                            Array<u8>& out) noexcept;

/// The FNV-1a digest of a pose's bits, as the preview state carries it.
[[nodiscard]] u64 animation_pose_digest(Span<const Transform> pose) noexcept;

/// Read a `pose.clip` node's `events` property: `name@seconds` items separated by `;`. False, with
/// nothing appended, when it does not parse.
[[nodiscard]] bool parse_animation_events(std::string_view text, Name clip,
                                          Array<AnimationClipEvent>& out) noexcept;

/// Answer one `animation.*` request into `reply`.
[[nodiscard]] AnimationRefusal answer_animation(AnimationPreviewRuntime* preview,
                                                std::string_view operation, Span<const u8> payload,
                                                Array<u8>& reply) noexcept;

}  // namespace cy::editor
