// SPDX-License-Identifier: MIT
#include "units.h"

#include <cy/abi/host.h>

#include <algorithm>
#include <utility>

#include "level.h"

namespace sample::rts {
namespace {

using cy::f32;
using cy::Vec3;

[[nodiscard]] cy::ecs::QueryDesc agent_query(cy::Allocator& allocator,
                                             cy::ecs::ComponentTypeId agent) noexcept {
    cy::ecs::QueryDesc desc(allocator);
    (void)desc.read(agent);
    return desc;
}

/// The capsule's centre sits half its height above the agent's feet.
[[nodiscard]] cy::Transform body_pose(const cy::navigation::NavAgent& agent) noexcept {
    const Vec3 feet = agent.position;
    return cy::Transform::from_translation(
        Vec3{feet.x, feet.y + (agent.avoidance.height * 0.5F), feet.z});
}

}  // namespace

// --- NodeMotion ----------------------------------------------------------------------------------

bool NodeMotion::position(CyEntity entity, Vec3& out) const noexcept {
    const cy::scene::Node node = tree_->node(cy::abi::from_abi(entity));
    if (!node.valid()) {
        return false;
    }
    out = node.local_transform().translation;
    return true;
}

Vec3 NodeMotion::drive(CyEntity entity, Vec3 from, Vec3 velocity, f32 dt) noexcept {
    // Units walk on a flat level, so the height is kept and only the plane moves.
    const Vec3 to{from.x + (velocity.x * dt), from.y, from.z + (velocity.z * dt)};
    const cy::scene::Node node = tree_->node(cy::abi::from_abi(entity));
    if (!node.valid()) {
        return from;
    }
    cy::Transform placed = node.local_transform();
    placed.translation = to;
    if (!node.set_local_transform(placed)) {
        return from;
    }
    return to;
}

// --- UnitBodies ----------------------------------------------------------------------------------

UnitBodies::UnitBodies(cy::Allocator& allocator, cy::ecs::World& world,
                       const cy::navigation::NavComponents& components,
                       cy::physics::PhysicsServer& server,
                       cy::physics::WorldHandle physics_world) noexcept
    : components_(components),
      server_(&server),
      physics_world_(physics_world),
      agents_(world, agent_query(allocator, components.agent)),
      units_(allocator) {}

UnitBodies::~UnitBodies() {
    clear();
}

cy::Status UnitBodies::sync() noexcept {
    for (Unit& unit : units_) {
        unit.seen = false;
    }
    cy::Status failure = cy::ok();
    cy::Status walked = agents_.for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
        const auto entities = chunk.entities();
        const auto agents = chunk.read<cy::navigation::NavAgent>(components_.agent);
        for (cy::u32 index = 0; index < chunk.count(); ++index) {
            const cy::Status followed = follow(cy::abi::to_abi(entities[index]), agents[index]);
            if (!followed && failure) {
                failure = followed;
            }
        }
    });
    if (!walked) {
        return walked;
    }
    sweep();
    return failure;
}

cy::Status UnitBodies::follow(CyEntity entity, const cy::navigation::NavAgent& agent) noexcept {
    Unit* unit = find(entity);
    if (unit == nullptr) {
        return create(entity, agent);
    }
    unit->seen = true;
    return server_->set_body_transform(unit->body, body_pose(agent),
                                       cy::physics::TeleportMode::Teleport);
}

cy::Status UnitBodies::create(CyEntity entity, const cy::navigation::NavAgent& agent) noexcept {
    cy::physics::ShapeDescription capsule;
    capsule.type = cy::physics::ShapeType::Capsule;
    capsule.radius = agent.avoidance.radius;
    capsule.half_height = std::max(0.0F, (agent.avoidance.height * 0.5F) - agent.avoidance.radius);
    const auto shape = server_->create_shape(capsule);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    cy::physics::ColliderDescription collider;
    collider.shape = *shape;
    collider.filter.layer = kUnitLayer;
    cy::physics::BodyDescription description;
    description.motion = cy::physics::MotionType::Kinematic;
    description.transform = body_pose(agent);
    description.colliders = &collider;
    description.collider_count = 1;
    description.user_data = entity;
    const auto body = server_->create_body(physics_world_, description);
    if (!body) {
        (void)server_->destroy_shape(*shape);
        return cy::make_unexpected(body.error());
    }
    if (cy::Status added = units_.push_back(Unit{entity, *body, *shape, true}); !added) {
        (void)server_->destroy_body(*body);
        (void)server_->destroy_shape(*shape);
        return added;
    }
    std::ranges::sort(units_, [](const Unit& a, const Unit& b) { return a.entity < b.entity; });
    return cy::ok();
}

void UnitBodies::sweep() noexcept {
    for (cy::usize index = units_.size(); index > 0; --index) {
        Unit& unit = units_[index - 1U];
        if (!unit.seen) {
            release(unit);
            units_.erase(index - 1U);
        }
    }
}

void UnitBodies::release(Unit& unit) noexcept {
    (void)server_->destroy_body(unit.body);
    (void)server_->destroy_shape(unit.shape);
    unit.body = cy::physics::BodyHandle();
}

void UnitBodies::clear() noexcept {
    for (Unit& unit : units_) {
        release(unit);
    }
    units_.clear();
}

UnitBodies::Unit* UnitBodies::find(CyEntity entity) noexcept {
    for (Unit& unit : units_) {
        if (unit.entity == entity) {
            return &unit;
        }
    }
    return nullptr;
}

cy::physics::BodyHandle UnitBodies::body_of(CyEntity entity) const noexcept {
    for (const Unit& unit : units_) {
        if (unit.entity == entity) {
            return unit.body;
        }
    }
    return cy::physics::BodyHandle{};
}

// --- LevelBodies ---------------------------------------------------------------------------------

void LevelBodies::add_prop(CyEntity entity, cy::physics::BodyHandle body) noexcept {
    if (prop_count_ < kProps) {
        props_[prop_count_++] = Prop{entity, body};
    }
}

cy::physics::BodyHandle LevelBodies::body_of(CyEntity entity) const noexcept {
    for (cy::u32 index = 0; index < prop_count_; ++index) {
        if (props_[index].entity == entity) {
            return props_[index].body;
        }
    }
    const cy::physics::BodyHandle unit = units_->body_of(entity);
    return unit.is_null() ? characters_->body_of(entity) : unit;
}

// --- VeterancyRoll -------------------------------------------------------------------------------

cy::Expected<cy::ecs::SystemId, cy::Error> VeterancyRoll::install(
    cy::ecs::Schedule& schedule, cy::ecs::ComponentTypeId veterancy) noexcept {
    veterancy_ = veterancy;
    cy::ecs::QueryDesc desc(*allocator_);
    if (cy::Status declared = desc.read(veterancy); !declared) {
        return cy::make_unexpected(declared.error());
    }
    cy::ecs::SystemDesc system;
    system.name = kName;
    system.body = &VeterancyRoll::body;
    system.user = this;
    // THE QUERY IS THE DECLARATION, as system.h asks: the access is the query's own.
    system.access = desc.access();
    auto query = cy::make_unique<cy::ecs::Query>(*allocator_, *world_, std::move(desc));
    if (!query) {
        return cy::make_unexpected(query.error());
    }
    query_ = std::move(*query);
    return schedule.add(cy::ecs::Stage::Simulation, system);
}

void VeterancyRoll::body(const cy::ecs::SystemContext& context) noexcept {
    auto* self = static_cast<VeterancyRoll*>(context.user);
    self->rows_ = 0;
    self->most_ = 0.0F;
    (void)self->query_->for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
        // `Veterancy` is one f32, `ticks`, so its column is a column of floats.
        const auto ticks = chunk.read<cy::f32>(self->veterancy_);
        for (cy::u32 index = 0; index < chunk.count(); ++index) {
            self->most_ = std::max(self->most_, ticks[index]);
            ++self->rows_;
        }
    });
}

}  // namespace sample::rts
