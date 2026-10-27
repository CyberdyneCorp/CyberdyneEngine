// SPDX-License-Identifier: MIT
// bodies.cpp — the arena in the physics world. See bodies.h.

#include "bodies.h"

namespace sample::fps {
namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;

/// Every layer but the fallen one: what the world, a prop and a live target collide with.
constexpr u32 kSolidMask = ~(1U << kFallenLayer);

}  // namespace

ArenaBodies::ArenaBodies(cy::physics::PhysicsServer& server,
                         cy::physics::WorldHandle world) noexcept
    : server_(&server), world_(world) {}

ArenaBodies::~ArenaBodies() {
    clear();
}

cy::Expected<ArenaBodies::Owned, cy::Error> ArenaBodies::make(CyEntity entity, Vec3 centre,
                                                              Vec3 half,
                                                              cy::physics::MotionType motion,
                                                              u32 layer, f32 mass) noexcept {
    cy::physics::ShapeDescription box;
    box.type = cy::physics::ShapeType::Box;
    box.half_extents = half;
    const auto shape = server_->create_shape(box);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    cy::physics::ColliderDescription collider;
    collider.shape = *shape;
    collider.filter.layer = static_cast<cy::u8>(layer);
    collider.filter.mask = kSolidMask;
    cy::physics::BodyDescription body;
    body.motion = motion;
    body.mass = mass;
    body.transform = cy::Transform::from_translation(centre);
    body.colliders = &collider;
    body.collider_count = 1;
    body.user_data = entity;
    const auto created = server_->create_body(world_, body);
    if (!created) {
        (void)server_->destroy_shape(*shape);
        return cy::make_unexpected(created.error());
    }
    return Owned{entity, *created, *shape};
}

cy::Status ArenaBodies::build(CyEntity arena, const CyEntity (&props)[kPropCount],
                              const CyEntity (&targets)[kTargetCount]) noexcept {
    const cy::Span<const ArenaBox> boxes = arena_boxes();
    if (boxes.size() > sizeof(statics_) / sizeof(statics_[0])) {
        return cy::fail(cy::ErrorCode::OutOfRange, "the arena has more boxes than the map holds");
    }
    for (const ArenaBox& box : boxes) {
        auto made =
            make(arena, box.centre, box.half, cy::physics::MotionType::Static, kWorldLayer, 0.0F);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        statics_[static_count_++] = *made;
    }
    const cy::Span<const PropBox> prop_list = prop_boxes();
    for (u32 index = 0; index < kPropCount; ++index) {
        const PropBox& prop = prop_list[index];
        auto made = make(props[index], prop.centre, prop.half, cy::physics::MotionType::Dynamic,
                         kPropLayer, prop.mass);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        props_[index] = *made;
    }
    const cy::Span<const TargetSpot> spots = target_spots();
    for (u32 index = 0; index < kTargetCount; ++index) {
        const Vec3 centre = spots[index].home + Vec3{0.0F, kTargetHalf.y, 0.0F};
        auto made = make(targets[index], centre, kTargetHalf, cy::physics::MotionType::Kinematic,
                         kTargetLayer, 0.0F);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        targets_[index] = *made;
    }
    return cy::ok();
}

void ArenaBodies::clear() noexcept {
    const auto release = [this](Owned& owned) noexcept {
        if (!owned.body.is_null()) {
            (void)server_->destroy_body(owned.body);
        }
        if (!owned.shape.is_null()) {
            (void)server_->destroy_shape(owned.shape);
        }
        owned = Owned{};
    };
    for (u32 index = 0; index < static_count_; ++index) {
        release(statics_[index]);
    }
    static_count_ = 0;
    for (Owned& owned : props_) {
        release(owned);
    }
    for (Owned& owned : targets_) {
        release(owned);
    }
}

cy::Transform ArenaBodies::prop(u32 index) const noexcept {
    const auto state = server_->body_state(props_[index].body);
    return state ? state->transform : cy::Transform::from_translation(prop_boxes()[index].centre);
}

cy::Status ArenaBodies::place_target(u32 index, const cy::Transform& placement) noexcept {
    return server_->set_body_transform(targets_[index].body, placement,
                                       cy::physics::TeleportMode::Interpolate);
}

cy::Status ArenaBodies::fell(u32 index) noexcept {
    cy::physics::CollisionFilter fallen;
    fallen.layer = static_cast<cy::u8>(kFallenLayer);
    fallen.mask = 1U << kFallenLayer;
    return server_->set_body_filter(targets_[index].body, fallen);
}

cy::physics::BodyHandle ArenaBodies::body_of(CyEntity entity) const noexcept {
    if (entity == CY_ENTITY_NULL) {
        return {};
    }
    for (const Owned& owned : props_) {
        if (owned.entity == entity) {
            return owned.body;
        }
    }
    for (const Owned& owned : targets_) {
        if (owned.entity == entity) {
            return owned.body;
        }
    }
    return characters_ == nullptr ? cy::physics::BodyHandle{} : characters_->body_of(entity);
}

}  // namespace sample::fps
