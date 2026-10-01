// SPDX-License-Identifier: MIT
// The character adapter: `cy::abi::game::CharacterBackend` over `cy::physics::CharacterController`.
// `add-swift-m12-gaps`. See include/cy/game_backend/character_backend.h, and
// tests/test_character_backend.cpp for the proof.

#include <cy/game_backend/character_backend.h>

#include <cy/abi/errors.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/ownership.h>
#include <cy/servers/physics/server.h>

namespace cy::game_backend {
namespace {

Vec3 vec3(const f32* value) noexcept {
    return Vec3{value[0], value[1], value[2]};
}

void write3(f32* out, Vec3 value) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

/// A non-zero field replaces the default; zero keeps it. `CyCharacterDesc`'s rule.
void take(f32 value, f32& field) noexcept {
    if (value != 0.0F) {
        field = value;
    }
}

Transform start_of(const CyPose& pose) noexcept {
    Transform transform = Transform::from_translation(vec3(pose.position));
    const Quat raw{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    const f32 length_sq = (raw.x * raw.x) + (raw.y * raw.y) + (raw.z * raw.z) + (raw.w * raw.w);
    transform.rotation = length_sq <= 1e-12F ? Quat::identity() : normalize(raw);
    return transform;
}

physics::CharacterDescription description_of(CyEntity entity,
                                             const CyCharacterDesc& desc) noexcept {
    physics::CharacterDescription out;
    take(desc.radius, out.radius);
    take(desc.height, out.height);
    take(desc.max_slope_radians, out.max_slope_radians);
    take(desc.step_offset, out.step_offset);
    take(desc.skin_width, out.skin_width);
    take(desc.gravity_scale, out.gravity_scale);
    take(desc.mass, out.mass);
    take(desc.push_force, out.push_force);
    out.mode = (desc.flags & CY_CHARACTER_FLOATING) != 0U ? physics::CharacterMode::Floating
                                                          : physics::CharacterMode::Grounded;
    out.push_dynamic_bodies = (desc.flags & CY_CHARACTER_NO_PUSH) == 0U;
    out.filter.layer = static_cast<u8>(desc.layer);
    out.filter.mask = desc.mask == 0U ? 0xFFFFFFFFU : desc.mask;
    out.start = start_of(desc.start);
    out.user_data = static_cast<physics::UserData>(entity);
    return out;
}

}  // namespace

CharacterAdapter::CharacterAdapter(Allocator& allocator, physics::PhysicsServer& server,
                                   physics::WorldHandle world) noexcept
    : allocator_(&allocator), server_(&server), world_(world), characters_(allocator) {}

CharacterAdapter::~CharacterAdapter() {
    for (Character& character : characters_) {
        release(character);
    }
}

void CharacterAdapter::release(Character& character) noexcept {
    if (character.controller != nullptr) {
        character.controller->~CharacterController();
        allocator_->deallocate(static_cast<void*>(character.controller),
                               sizeof(physics::CharacterController),
                               alignof(physics::CharacterController));
        character.controller = nullptr;
    }
}

usize CharacterAdapter::lower_bound(CyEntity entity) const noexcept {
    usize low = 0;
    usize high = characters_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2U);
        if (characters_[middle].entity < entity) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

const CharacterAdapter::Character* CharacterAdapter::find(CyEntity entity) const noexcept {
    const usize index = lower_bound(entity);
    if (index < characters_.size() && characters_[index].entity == entity) {
        return &characters_[index];
    }
    return nullptr;
}

const physics::CharacterController* CharacterAdapter::controller(CyEntity entity) const noexcept {
    const Character* found = find(entity);
    return found == nullptr ? nullptr : found->controller;
}

physics::BodyHandle CharacterAdapter::body_of(CyEntity entity) const noexcept {
    const Character* found = find(entity);
    return found == nullptr ? physics::BodyHandle{} : found->controller->body();
}

CyResult CharacterAdapter::create(CyEntity entity, const CyCharacterDesc& desc) noexcept {
    if (server_->stepping()) {
        return abi::report(CY_RESULT_UNAVAILABLE, "the physics step is running");
    }
    if (find(entity) != nullptr) {
        return abi::report(CY_RESULT_ALREADY_EXISTS, "the entity already has a character");
    }
    Expected<UniquePtr<physics::CharacterController>, Error> made =
        make_unique<physics::CharacterController>(*allocator_, *server_, world_);
    if (!made) {
        return abi::report(made.error());
    }
    if (Status created = made.value()->create(description_of(entity, desc)); !created) {
        return abi::report(created.error());
    }
    // Insert in entity order. Grown first, then shifted up from the back, so a failed allocation
    // leaves the array as it was.
    const usize index = lower_bound(entity);
    if (Status grown = characters_.resize(characters_.size() + 1U); !grown) {
        return abi::report(grown.error());
    }
    for (usize slot = characters_.size() - 1U; slot > index; --slot) {
        characters_[slot] = characters_[slot - 1U];
    }
    characters_[index] = Character{entity, made.value().release()};
    return CY_RESULT_OK;
}

CyResult CharacterAdapter::destroy(CyEntity entity) noexcept {
    if (server_->stepping()) {
        return abi::report(CY_RESULT_UNAVAILABLE, "the physics step is running");
    }
    const usize index = lower_bound(entity);
    if (index >= characters_.size() || characters_[index].entity != entity) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity has no character");
    }
    release(characters_[index]);
    for (usize slot = index; slot + 1U < characters_.size(); ++slot) {
        characters_[slot] = characters_[slot + 1U];
    }
    (void)characters_.resize(characters_.size() - 1U);  // shrinking never allocates
    return CY_RESULT_OK;
}

CyResult CharacterAdapter::move(CyEntity entity, const CyCharacterInput& input,
                                f32 delta) noexcept {
    if (server_->stepping()) {
        return abi::report(CY_RESULT_UNAVAILABLE,
                           "the physics step is running; characters move before or after it");
    }
    const Character* found = find(entity);
    if (found == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity has no character");
    }
    physics::CharacterInput step;
    step.desired_velocity = vec3(input.desired_velocity);
    step.jump = (input.flags & CY_CHARACTER_JUMP) != 0U;
    step.jump_speed = input.jump_speed;
    if (Status moved = found->controller->move(delta, step); !moved) {
        return abi::report(moved.error());
    }
    return CY_RESULT_OK;
}

CyResult CharacterAdapter::state(CyEntity entity, CyCharacterState& out) const noexcept {
    const Character* found = find(entity);
    if (found == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the entity has no character");
    }
    const physics::CharacterState& moved = found->controller->state();
    out.ground = static_cast<u32>(moved.ground);
    out.flags = (moved.touching_ceiling ? CY_CHARACTER_TOUCHING_CEILING : 0U) |
                (moved.touching_wall ? CY_CHARACTER_TOUCHING_WALL : 0U) |
                (moved.stepped_up ? CY_CHARACTER_STEPPED_UP : 0U);
    out.ground_entity = CY_ENTITY_NULL;
    if (!moved.ground_body.is_null()) {
        const Expected<physics::UserData, Error> owner = server_->body_user_data(moved.ground_body);
        out.ground_entity = owner ? static_cast<CyEntity>(*owner) : CY_ENTITY_NULL;
    }
    write3(out.position, moved.transform.translation);
    write3(out.velocity, moved.velocity);
    write3(out.ground_normal, moved.ground_normal);
    write3(out.platform_velocity, moved.platform_velocity);
    return CY_RESULT_OK;
}

void bind_characters(cy::abi::Host& host, CharacterAdapter* adapter) noexcept {
    host.game.characters = adapter;
}

static_assert(static_cast<u32>(physics::GroundState::Grounded) == CY_GROUND_GROUNDED);
static_assert(static_cast<u32>(physics::GroundState::OnSteepSlope) == CY_GROUND_STEEP_SLOPE);
static_assert(static_cast<u32>(physics::GroundState::InAir) == CY_GROUND_IN_AIR);

}  // namespace cy::game_backend
