// SPDX-License-Identifier: MIT
// The animation panel's preview character. See cy/editor/animation_preview.h.

#include <cy/editor/animation_preview.h>

#include <cy/core/math/quat.h>

#include <cmath>
#include <iterator>
#include <utility>

namespace cy::editor {
namespace {

namespace animation = cy::animation;
namespace pose = graph::pose;

[[nodiscard]] bool same_events(Span<const AnimationClipEvent> a,
                               Span<const AnimationClipEvent> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index].clip != b[index].clip || a[index].event != b[index].event ||
            a[index].time != b[index].time) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_parameters(Span<const AnimationParameter> a,
                                   Span<const AnimationParameter> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index].name != b[index].name || a[index].value != b[index].value) {
            return false;
        }
    }
    return true;
}

}  // namespace

AnimationPreview::AnimationPreview(Allocator& allocator) noexcept
    : allocator_(&allocator),
      character_(std::make_unique<AnimationCharacter>(allocator)),
      clips_(allocator),
      events_(allocator),
      program_(allocator),
      buffer_(allocator),
      parameters_(allocator),
      local_(allocator),
      model_(allocator),
      skinning_(allocator),
      fired_(allocator) {}

AnimationPreview::~AnimationPreview() = default;

Status AnimationPreview::initialize() noexcept {
    if (Status built = character_->load_mannequin(); !built) {
        return built;
    }
    if (Status adopted = adopt_character(); !adopted) {
        return adopted;
    }
    initialized_ = true;
    return finish();
}

Status AnimationPreview::adopt_character() noexcept {
    if (Status built = rebuild_clips({}); !built) {
        return built;
    }
    const usize joints = character_->joint_count();
    if (Status sized = local_.resize(joints); !sized) {
        return sized;
    }
    if (Status sized = model_.resize(joints); !sized) {
        return sized;
    }
    if (Status sized = skinning_.resize(joints); !sized) {
        return sized;
    }
    character_->skeleton().reference_pose(local_.span());
    return ok();
}

Status AnimationPreview::set_character(const AnimationCharacterRequest& request) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the preview character was never built");
    }
    if (!request.skeleton.is_nil() && source_ == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "this host reads no project assets, so only the mannequin can be previewed");
    }
    auto loaded = std::make_unique<AnimationCharacter>(*allocator_);
    if (request.skeleton.is_nil()) {
        if (Status built = loaded->load_mannequin(); !built) {
            return built;
        }
    } else if (Status read = loaded->load(request, *source_); !read) {
        return read;
    }
    // The rig, the instance and the scratch point at the old skeleton and clips: drop them first.
    rig_.reset();
    instance_.reset();
    scratch_.reset();
    character_ = std::move(loaded);
    ++mesh_generation_;
    stop();
    state_ = AnimationPreviewState{};
    fired_.clear();
    if (Status adopted = adopt_character(); !adopted) {
        return adopted;
    }
    return finish();
}

Span<const AnimationClipInfo> AnimationPreview::clips() const noexcept {
    return character_->clips();
}

u32 AnimationPreview::joint_count() const noexcept {
    return character_->joint_count();
}

const animation::Clip* AnimationPreview::clip(Name name) const noexcept {
    for (const animation::Clip& candidate : clips_) {
        if (candidate.name() == name) {
            return &candidate;
        }
    }
    return nullptr;
}

Status AnimationPreview::rebuild_clips(Span<const AnimationClipEvent> events) noexcept {
    // The rig points at the clips: drop it before they move.
    rig_.reset();
    instance_.reset();
    scratch_.reset();
    if (Status built = character_->build_clips(events, clips_); !built) {
        return built;
    }
    events_.clear();
    return events_.append(events);
}

Status AnimationPreview::bind(pose::PoseProgram&& program) noexcept {
    program_ = std::move(program);
    Array<const animation::Clip*> table(*allocator_);
    for (const pose::ClipRef& reference : program_.clips()) {
        animation::Clip* found = nullptr;
        for (animation::Clip& candidate : clips_) {
            found = candidate.name() == reference.name ? &candidate : found;
        }
        // A project clip loops or holds as its node says, as the bake cooks it for a game.
        if (found != nullptr && character_->from_project()) {
            found->set_loop_mode(reference.looping ? animation::LoopMode::Loop
                                                   : animation::LoopMode::None);
        }
        if (Status pushed = table.push_back(found); !pushed) {
            return pushed;
        }
    }
    rig_ = std::make_unique<animation::AnimationRig>(*allocator_);
    instance_ = std::make_unique<animation::AnimationInstance>(*allocator_);
    scratch_ = std::make_unique<animation::PoseScratch>(*allocator_);
    cursor_ = std::make_unique<animation::ClipCursor>(*allocator_);
    if (Status bound = rig_->bind(character_->skeleton(), program_, table.span()); !bound) {
        return bound;
    }
    return scratch_->prepare(*rig_);
}

Status AnimationPreview::preview(pose::PoseProgram&& program,
                                 const AnimationPreviewRequest& request) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the preview character was never built");
    }
    const bool events_kept = same_events(events_.span(), request.events);
    const bool continuing =
        state_.active && program.digest() == state_.program_digest && events_kept &&
        same_parameters(parameters_.span(), request.parameters) && request.focus == state_.focus &&
        request.focus_clip == state_.focus_clip && request.time >= state_.time;
    const f32 previous = state_.time;
    if (!events_kept) {
        if (Status rebuilt = rebuild_clips(request.events); !rebuilt) {
            return rebuilt;
        }
    }
    parameters_.clear();
    if (Status copied = parameters_.append(request.parameters); !copied) {
        return copied;
    }
    state_.program_digest = program.digest();
    if (Status bound = bind(std::move(program)); !bound) {
        return bound;
    }
    state_.active = true;
    state_.playing = request.playing;
    state_.focus = request.focus;
    state_.focus_clip = request.focus_clip;
    pending_ = 0.0F;
    if (request.focus != 0) {
        const animation::Clip* focused = clip(request.focus_clip);
        if (focused == nullptr) {
            return fail(ErrorCode::NotFound, "the preview character has no clip of that name");
        }
        focus_clip_ = static_cast<u32>(focused - clips_.data());
        state_.length = focused->duration();
        return show_clip(std::fmin(request.time, focused->duration()), previous, continuing);
    }
    state_.length = kAnimationGraphPreviewSeconds;
    if (Status restarted = restart_machine(); !restarted) {
        return restarted;
    }
    if (Status ran = run_machine(request.time, continuing ? previous : request.time); !ran) {
        return ran;
    }
    state_.time = request.time;
    return evaluate_machine();
}

void AnimationPreview::stop() noexcept {
    state_.active = false;
    state_.playing = false;
    pending_ = 0.0F;
}

Status AnimationPreview::show_clip(f32 time, f32 previous, bool continuing) noexcept {
    const animation::Clip& focused = clips_[focus_clip_];
    state_.time = time;
    state_.state = 0xFFFFU;
    state_.state_name = Name{};
    state_.target = 0xFFFFU;
    state_.target_name = Name{};
    state_.blend = 0.0F;
    if (continuing && time > previous) {
        buffer_.clear();
        if (Status emitted = animation::emit_events(focused, 0, previous, time,
                                                    animation::EventPolicy::Emit, buffer_);
            !emitted) {
            return emitted;
        }
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, event.normalised_time * focused.duration());
        }
    }
    character_->skeleton().reference_pose(local_.span());
    if (Status reset = cursor_->reset(focused.track_count()); !reset) {
        return reset;
    }
    animation::SampleStats stats;
    if (Status sampled = focused.sample_unwrapped(time, character_->skeleton().retained(0),
                                                  *cursor_, local_.span(), stats);
        !sampled) {
        return sampled;
    }
    return finish();
}

Status AnimationPreview::restart_machine() noexcept {
    if (Status prepared = instance_->prepare(*rig_); !prepared) {
        return prepared;
    }
    for (const AnimationParameter& parameter : parameters_) {
        // A parameter the program does not read changes nothing: the panel may still hold one an
        // edit removed.
        (void)instance_->set_parameter(*rig_, parameter.name, parameter.value);
    }
    state_.time = 0.0F;
    return ok();
}

Status AnimationPreview::run_machine(f32 seconds, f32 report_after) noexcept {
    f32 remaining = seconds;
    while (remaining > 0.0F) {
        const f32 step = std::fmin(kAnimationPreviewStep, remaining);
        if (Status stepped = step_machine(step, report_after); !stepped) {
            return stepped;
        }
        remaining -= step;
    }
    return ok();
}

Status AnimationPreview::step_machine(f32 step, f32 report_after) noexcept {
    buffer_.clear();
    if (Status advanced = animation::advance(*rig_, *instance_, step, &buffer_); !advanced) {
        return advanced;
    }
    state_.time += step;
    if (state_.time > report_after) {
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, state_.time);
        }
    }
    return ok();
}

Status AnimationPreview::evaluate_machine() noexcept {
    character_->skeleton().reference_pose(local_.span());
    animation::EvaluationStats stats;
    if (Status evaluated =
            animation::evaluate(*rig_, *instance_, 0, *scratch_, local_.span(), stats);
        !evaluated) {
        return evaluated;
    }
    const pose::PoseInstance& machine = instance_->machine();
    const Span<const pose::PoseState> states = program_.states();
    state_.state = machine.state;
    state_.state_name = machine.state < states.size() ? states[machine.state].name : Name{};
    state_.target = machine.target;
    state_.target_name = machine.target < states.size() ? states[machine.target].name : Name{};
    const f32 duration = pose::transition_duration(program_, machine);
    state_.blend = machine.target != 0xFFFFU && duration > 0.0F
                       ? std::fmin(machine.blend_elapsed / duration, 1.0F)
                       : 0.0F;
    return finish();
}

Status AnimationPreview::tick(f32 seconds) noexcept {
    if (!state_.active || !state_.playing || !(seconds > 0.0F)) {
        return ok();
    }
    if (state_.focus != 0) {
        const animation::Clip& focused = clips_[focus_clip_];
        const f32 previous = state_.time;
        f32 next = previous + seconds;
        if (focused.loop_mode() == animation::LoopMode::None) {
            next = std::fmin(next, focused.duration());
            state_.playing = next < focused.duration();
        }
        buffer_.clear();
        if (Status emitted = animation::emit_events(focused, 0, previous, next,
                                                    animation::EventPolicy::Emit, buffer_);
            !emitted) {
            return emitted;
        }
        for (const animation::EmittedEvent& event : buffer_.events()) {
            remember(event, event.normalised_time * focused.duration());
        }
        const f32 wrapped = focused.loop_mode() == animation::LoopMode::None
                                ? next
                                : std::fmod(next, focused.duration());
        return show_clip(wrapped, wrapped, false);
    }
    pending_ += seconds;
    while (pending_ >= kAnimationPreviewStep) {
        pending_ -= kAnimationPreviewStep;
        if (state_.time + kAnimationPreviewStep > kAnimationGraphPreviewSeconds) {
            if (Status restarted = restart_machine(); !restarted) {
                return restarted;
            }
        }
        if (Status stepped = step_machine(kAnimationPreviewStep, -1.0F); !stepped) {
            return stepped;
        }
    }
    return evaluate_machine();
}

void AnimationPreview::remember(const animation::EmittedEvent& event, f32 at) noexcept {
    if (fired_.size() >= kAnimationPreviewEvents) {
        fired_.erase(0);
    }
    AnimationFiredEvent fired;
    fired.sequence = ++sequence_;
    fired.name = event.name;
    fired.normalised_time = event.normalised_time;
    fired.at = at;
    (void)fired_.push_back(fired);
}

Status AnimationPreview::finish() noexcept {
    const animation::Skeleton& skeleton = character_->skeleton();
    const animation::JointMask& every = skeleton.retained(0);
    skeleton.to_model(local_.span(), every, model_.span());
    skeleton.to_skinning(model_.span(), every, skinning_.span());
    state_.pose_digest = animation_pose_digest(local_.span());
    ++state_.generation;
    return ok();
}

}  // namespace cy::editor
