// SPDX-License-Identifier: MIT
// The animation adapter: `cy::abi::game::AnimationBackend` over `cy::animation::AnimationSystem`.
// Issue #76 stage 4. See include/cy/game_backend/animation_backend.h, and
// tests/test_animation_backend.cpp for the proof.

#include <cy/game_backend/animation_backend.h>

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>

#include <algorithm>

namespace cy::game_backend {
namespace {

using abi::from_abi;
using abi::to_abi;

void write3(f32* out, Vec3 value) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

[[nodiscard]] animation::RootMotionMode system_mode(CyRootMotionMode mode) noexcept {
    switch (mode) {
        case CY_ROOT_MOTION_TRANSFORM:
            return animation::RootMotionMode::ApplyToTransform;
        case CY_ROOT_MOTION_ACCUMULATE:
        case CY_ROOT_MOTION_CHARACTER:
            return animation::RootMotionMode::Controller;
        case CY_ROOT_MOTION_EXTRACT:
            return animation::RootMotionMode::ExtractOnly;
        case CY_ROOT_MOTION_IGNORE:
            break;
    }
    return animation::RootMotionMode::Ignore;
}

void write_motion(const animation::RootDelta& delta, Vec3 travelled, CyRootMotion& out) noexcept {
    write3(out.translation, delta.translation);
    out.rotation[0] = delta.rotation.x;
    out.rotation[1] = delta.rotation.y;
    out.rotation[2] = delta.rotation.z;
    out.rotation[3] = delta.rotation.w;
    out.distance = delta.distance;
    out.contacts = delta.contacts;
    write3(out.travelled, travelled);
}

/// A name a script passed, as the engine's interned one. Never interns: a name nobody interned is
/// a name no program, clip or skeleton carries, which `find`'s empty answer already says.
[[nodiscard]] Name known(const char* text) noexcept {
    return Name::find(text);
}

/// The system's refusal, as the ABI's result.
[[nodiscard]] CyResult answer(const Status& status) noexcept {
    return status.has_value() ? CY_RESULT_OK : abi::report(status.error());
}

}  // namespace

AnimationAdapter::AnimationAdapter(Allocator& allocator, ecs::World& world,
                                   animation::AnimationSystem& system,
                                   ecs::ComponentTypeId animator, scene::SceneTree* tree) noexcept
    : world_(&world),
      system_(&system),
      animator_(animator),
      tree_(tree),
      rigs_(allocator),
      driven_(allocator),
      events_(allocator) {}

Status AnimationAdapter::add_rig(const char* name, animation::RigId rig) noexcept {
    if (name == nullptr || *name == '\0') {
        return fail(ErrorCode::InvalidArgument, "a rig is registered under a name");
    }
    if (rig >= system_->rig_count()) {
        return fail(ErrorCode::NotFound, "the animation system has no such rig");
    }
    const Name interned = Name::intern(name);
    for (const Rig& existing : rigs_) {
        if (existing.name == interned) {
            return fail(ErrorCode::AlreadyExists, "a rig is already registered under that name");
        }
    }
    return rigs_.push_back(Rig{interned, rig});
}

const animation::Animator* AnimationAdapter::animator_of(CyEntity entity) const noexcept {
    return world_->get<animation::Animator>(from_abi(entity), animator_);
}

CyResult AnimationAdapter::missing(CyEntity entity) const noexcept {
    (void)entity;
    return abi::report(CY_RESULT_NOT_FOUND, "this entity has no animator");
}

// --- Lifetime
// -------------------------------------------------------------------------------------

CyResult AnimationAdapter::attach(CyEntity entity, const CyAnimatorDesc& desc) noexcept {
    const ecs::Entity target = from_abi(entity);
    if (!world_->is_alive(target)) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity is not alive");
    }
    if (world_->has(target, animator_)) {
        return abi::report(CY_RESULT_ALREADY_EXISTS, "the entity already has an animator");
    }
    const Name name = known(desc.rig);
    animation::RigId rig = animation::kNoRig;
    for (const Rig& candidate : rigs_) {
        if (!name.is_empty() && candidate.name == name) {
            rig = candidate.id;
        }
    }
    if (rig == animation::kNoRig) {
        return abi::report(CY_RESULT_NOT_FOUND, "no rig is registered under that name");
    }
    const auto mode = static_cast<CyRootMotionMode>(desc.root_motion);
    if (mode == CY_ROOT_MOTION_CHARACTER) {
        CyCharacterState probe{};
        probe.struct_size = static_cast<u32>(sizeof(CyCharacterState));
        if (characters_ == nullptr || characters_->state(entity, probe) != CY_RESULT_OK) {
            return abi::report(CY_RESULT_NOT_FOUND,
                               "CY_ROOT_MOTION_CHARACTER needs the entity to own a character");
        }
    }
    animation::Animator settings;
    settings.rig = rig;
    settings.tier = static_cast<animation::LodTier>(desc.tier);
    settings.events = (desc.flags & CY_ANIMATOR_SUPPRESS_EVENTS) != 0U
                          ? animation::EventPolicy::Suppress
                          : animation::EventPolicy::Emit;
    settings.root_motion = system_mode(mode);
    settings.play_rate = desc.play_rate == 0.0F ? 1.0F : desc.play_rate;
    if (Status added = world_->add(target, animator_, &settings); !added) {
        return abi::report(added.error());
    }
    // THE INSTANCE EXISTS WHEN THE CALL RETURNS: the system syncs now rather than at the next
    // tick, so the script that attached can play a state in the same callback.
    if (Status synced = system_->sync(); !synced) {
        return abi::report(synced.error());
    }
    if (system_->instance(target) == nullptr) {
        return abi::report(CY_RESULT_INTERNAL,
                           "the animation system did not instance the animator");
    }
    if (mode == CY_ROOT_MOTION_CHARACTER) {
        return write_mode(entity, mode);
    }
    return CY_RESULT_OK;
}

CyResult AnimationAdapter::detach(CyEntity entity) noexcept {
    const ecs::Entity target = from_abi(entity);
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    if (Status removed = world_->remove(target, animator_); !removed) {
        return abi::report(removed.error());
    }
    forget_driven(entity);
    return answer(system_->sync());
}

// --- Requests
// -------------------------------------------------------------------------------------

CyResult AnimationAdapter::play(CyEntity entity, const char* state, f32 seconds) noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    return answer(system_->play(from_abi(entity), known(state), seconds));
}

CyResult AnimationAdapter::stop(CyEntity entity, f32 seconds) noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    return answer(system_->stop(from_abi(entity), seconds));
}

CyResult AnimationAdapter::set_parameter(CyEntity entity, const char* parameter,
                                         f32 value) noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    return answer(system_->set_parameter(from_abi(entity), known(parameter), value));
}

CyResult AnimationAdapter::fire_trigger(CyEntity entity, const char* parameter) noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    return answer(system_->fire_trigger(from_abi(entity), known(parameter)));
}

// --- Reads
// ----------------------------------------------------------------------------------------

CyResult AnimationAdapter::parameter(CyEntity entity, const char* parameter,
                                     f32& out) const noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    Expected<f32, Error> value = system_->parameter(from_abi(entity), known(parameter));
    if (!value.has_value()) {
        return abi::report(value.error());
    }
    out = *value;
    return CY_RESULT_OK;
}

CyResult AnimationAdapter::state(CyEntity entity, CyAnimatorState& out) const noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    Expected<animation::AnimatorStatus, Error> status = system_->status(from_abi(entity));
    if (!status.has_value()) {
        return abi::report(status.error());
    }
    out.state = abi::game::name_hash(status->state_name.c_str());
    out.flags = 0;
    out.target = 0;
    out.blend_weight = 0.0F;
    if (status->target != 0xFFFFU) {
        out.flags |= CY_ANIMATOR_BLENDING;
        out.target = abi::game::name_hash(status->target_name.c_str());
        out.blend_weight = status->blend_weight;
    }
    if (status->requested) {
        out.flags |= CY_ANIMATOR_REQUESTED;
    }
    out.state_time = status->state_time;
    return CY_RESULT_OK;
}

CyResult AnimationAdapter::root_motion(CyEntity entity, CyRootMotion& out) const noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    const ecs::Entity target = from_abi(entity);
    write_motion(system_->root_motion(target), system_->travelled(target), out);
    return CY_RESULT_OK;
}

CyResult AnimationAdapter::take_root_motion(CyEntity entity, CyRootMotion& out) noexcept {
    const animation::Animator* animator = animator_of(entity);
    if (animator == nullptr) {
        return missing(entity);
    }
    // ONLY AN ACCUMULATING ANIMATOR IS TAKEN FROM. A character's motion is the adapter's to take,
    // and taking it here too would move the character by half of it.
    const bool driven = std::binary_search(driven_.begin(), driven_.end(), entity);
    if (animator->root_motion != animation::RootMotionMode::Controller || driven) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "root motion is taken only from an animator in "
                           "CY_ROOT_MOTION_ACCUMULATE");
    }
    const ecs::Entity target = from_abi(entity);
    write_motion(system_->take_root_motion(target), system_->travelled(target), out);
    return CY_RESULT_OK;
}

CyResult AnimationAdapter::set_root_motion(CyEntity entity, CyRootMotionMode mode) noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    if (mode == CY_ROOT_MOTION_CHARACTER) {
        CyCharacterState probe{};
        probe.struct_size = static_cast<u32>(sizeof(CyCharacterState));
        if (characters_ == nullptr || characters_->state(entity, probe) != CY_RESULT_OK) {
            return abi::report(CY_RESULT_NOT_FOUND,
                               "CY_ROOT_MOTION_CHARACTER needs the entity to own a character");
        }
    }
    return write_mode(entity, mode);
}

CyResult AnimationAdapter::write_mode(CyEntity entity, CyRootMotionMode mode) noexcept {
    auto* animator = world_->get_mut<animation::Animator>(from_abi(entity), animator_);
    if (animator == nullptr) {
        return missing(entity);
    }
    animator->root_motion = system_mode(mode);
    forget_driven(entity);
    if (mode == CY_ROOT_MOTION_CHARACTER) {
        const auto* at = std::lower_bound(driven_.begin(), driven_.end(), entity);
        const auto index = static_cast<usize>(at - driven_.begin());
        if (Status pushed = driven_.push_back(entity); !pushed) {
            return abi::report(pushed.error());
        }
        for (usize move = driven_.size() - 1U; move > index; --move) {
            driven_[move] = driven_[move - 1U];
        }
        driven_[index] = entity;
    }
    // The system copies the component's settings onto the instance at its next sync; doing it now
    // makes the choice take effect at the next tick whatever order the stages run in.
    return answer(system_->sync());
}

void AnimationAdapter::forget_driven(CyEntity entity) noexcept {
    for (usize index = 0; index < driven_.size(); ++index) {
        if (driven_[index] == entity) {
            driven_.erase(index);
            return;
        }
    }
}

Transform AnimationAdapter::placement_of(CyEntity entity) const noexcept {
    if (tree_ == nullptr) {
        return Transform{};
    }
    const auto* placed =
        world_->get<scene::WorldTransform>(from_abi(entity), tree_->components().world_transform);
    return placed == nullptr ? Transform{} : placed->value;
}

CyResult AnimationAdapter::joint_pose(CyEntity entity, const char* joint,
                                      CyPose& out) const noexcept {
    if (animator_of(entity) == nullptr) {
        return missing(entity);
    }
    Expected<Mat4, Error> model = system_->joint_model_matrix(from_abi(entity), known(joint));
    if (!model.has_value()) {
        return abi::report(model.error());
    }
    // Model space to the world through the entity's placement.
    Expected<Transform, Error> decomposed = decompose(placement_of(entity).to_matrix() * *model);
    if (!decomposed.has_value()) {
        return abi::report(decomposed.error());
    }
    write3(out.position, decomposed->translation);
    out.rotation[0] = decomposed->rotation.x;
    out.rotation[1] = decomposed->rotation.y;
    out.rotation[2] = decomposed->rotation.z;
    out.rotation[3] = decomposed->rotation.w;
    return CY_RESULT_OK;
}

// --- Per frame and per tick
// -------------------------------------------------------------------------

Status AnimationAdapter::begin_frame() noexcept {
    events_.clear();
    // EXACTLY ONCE: the events the system holds are those of the ticks since its last evaluation,
    // and they are this frame's only if a tick ran since the last frame looked.
    const u64 ticks = system_->stats().ticks_total;
    if (ticks == ticks_seen_) {
        return ok();
    }
    ticks_seen_ = ticks;
    for (const animation::EmittedEvent& event : system_->events().events()) {
        CyAnimationEvent out{};
        out.entity = to_abi(system_->entity_of(event));
        out.name = abi::game::name_hash(event.name.c_str());
        out.normalised_time = event.normalised_time;
        out.parameter = event.parameter;
        if (Status pushed = events_.push_back(out); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status AnimationAdapter::update(f32 tick_seconds) noexcept {
    if (driven_.empty() || characters_ == nullptr || tick_seconds <= 0.0F) {
        return ok();
    }
    for (const CyEntity entity : driven_) {
        // The delta is in the character's own frame; its placement turns it into the world, and a
        // velocity over the tick is what a character controller moves by.
        const animation::RootDelta delta = system_->take_root_motion(from_abi(entity));
        const Vec3 world = placement_of(entity).rotation * delta.translation;
        CyCharacterInput input{};
        input.struct_size = static_cast<u32>(sizeof(CyCharacterInput));
        write3(input.desired_velocity, world * (1.0F / tick_seconds));
        if (const CyResult moved = characters_->move(entity, input, tick_seconds);
            moved != CY_RESULT_OK) {
            return fail(abi::to_error_code(moved), "a root-motion character move was refused");
        }
    }
    return ok();
}

void bind_animation(cy::abi::Host& host, AnimationAdapter* adapter) noexcept {
    host.game.animation = adapter;
}

}  // namespace cy::game_backend
