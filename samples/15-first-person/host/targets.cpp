// SPDX-License-Identifier: MIT
// targets.cpp — the targets the engine's AI drives. See targets.h.

#include "targets.h"

#include <cy/core/math/scalar.h>

#include <cmath>
#include <numbers>

namespace sample::fps {
namespace {

using cy::f32;
using cy::Name;
using cy::u32;
using cy::Vec3;
namespace behaviour = cy::graph::behaviour;

constexpr f32 kPi = std::numbers::pi_v<f32>;
/// Metres a second a target walks its patrol, strafes, and staggers back.
constexpr f32 kPatrolSpeed = 1.2F;
constexpr f32 kStrafeSpeed = 1.6F;
constexpr f32 kStaggerSpeed = 1.0F;
/// Radians a second a target turns: to face where it walks, and to face the player.
constexpr f32 kWalkTurn = 4.0F;
constexpr f32 kEngageTurn = 6.0F;
/// How far an engaged target strays from its lane, and how long one strafe lasts.
constexpr f32 kStrafeLeash = 2.0F;
constexpr f32 kStrafeSeconds = 1.25F;
/// A hit staggers for this long; a fall takes this long.
constexpr f32 kFlinchSeconds = 0.35F;
constexpr f32 kFallSeconds = 0.7F;
/// Where a target's eyes are above its feet.
constexpr f32 kEyeHeight = 1.5F;
constexpr u32 kSolidQueryMask = (1U << kWorldLayer) | (1U << kPropLayer) | (1U << kTargetLayer);

[[nodiscard]] cy::graph::Literal name_literal(const char* text) noexcept {
    cy::graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(text);
    return literal;
}

[[nodiscard]] Vec3 forward_of(f32 yaw) noexcept {
    return Vec3{std::sin(yaw), 0.0F, -std::cos(yaw)};
}

/// The yaw that faces from `from` to `to` on the ground, or `fallback` when they coincide.
[[nodiscard]] f32 yaw_towards(Vec3 from, Vec3 to, f32 fallback) noexcept {
    const f32 dx = to.x - from.x;
    const f32 dz = to.z - from.z;
    if ((dx * dx) + (dz * dz) < 1e-6F) {
        return fallback;
    }
    return std::atan2(dx, -dz);
}

/// Turn `yaw` towards `goal` by at most `step` radians, the short way round.
[[nodiscard]] f32 turn(f32 yaw, f32 goal, f32 step) noexcept {
    f32 delta = goal - yaw;
    while (delta > kPi) {
        delta -= 2.0F * kPi;
    }
    while (delta < -kPi) {
        delta += 2.0F * kPi;
    }
    return yaw + cy::math::clamp(delta, -step, step);
}

[[nodiscard]] f32 planar_length(Vec3 v) noexcept {
    return std::sqrt((v.x * v.x) + (v.z * v.z));
}

[[nodiscard]] cy::u64 fold(cy::u64 digest, cy::u64 value) noexcept {
    for (u32 byte = 0; byte < 8U; ++byte) {
        digest ^= (value >> (byte * 8U)) & 0xFFULL;
        digest *= 0x100000001B3ULL;
    }
    return digest;
}

}  // namespace

const char* target_mode_name(TargetMode mode) noexcept {
    switch (mode) {
        case TargetMode::Patrol:
            return "patrol";
        case TargetMode::Engage:
            return "engage";
        case TargetMode::Flinch:
            return "flinch";
        case TargetMode::Down:
            return "down";
        case TargetMode::Count:
            break;
    }
    return "?";
}

cy::Transform target_placement(const TargetState& state) noexcept {
    // TOPPLED ABOUT ITS BACK EDGE: the pivot is the heel's back edge, so a fallen target lies on
    // its back with its depth's half above the floor rather than half through it.
    const cy::Quat facing = cy::Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, -state.yaw);
    const cy::Quat tilt =
        cy::Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, state.fall * kPi * 0.5F);
    const Vec3 pivot{0.0F, 0.0F, kTargetHalf.z};
    const Vec3 centre = pivot + (tilt * (Vec3{0.0F, kTargetHalf.y, 0.0F} - pivot));
    cy::Transform placement;
    placement.rotation = facing * tilt;
    placement.translation = state.feet + (facing * centre);
    return placement;
}

cy::Transform head_placement(const cy::Transform& body) noexcept {
    cy::Transform head = body;
    head.translation =
        body.translation +
        (body.rotation * Vec3{0.0F, kTargetHalf.y + kTargetHeadHalf.y + 0.02F, 0.0F});
    return head;
}

// --- Sight: the perception batch, answered by the physics world ---------------------------------

class Targets::Sight final : public cy::ai::VisibilityHost {
public:
    Sight(const cy::physics::PhysicsServer& server, cy::physics::WorldHandle world,
          const ArenaBodies& bodies, const u32* observers) noexcept
        : server_(&server), world_(world), bodies_(&bodies), observers_(observers) {}

    void trace_batch(cy::Span<const cy::ai::VisibilityQuery> queries,
                     cy::Span<bool> results) override {
        for (cy::usize index = 0; index < queries.size() && index < results.size(); ++index) {
            results[index] = clear(queries[index]);
            ++traced;
        }
    }

    u32 traced = 0;

private:
    /// Nothing solid between the target's eyes and the player's: walls, the beam, props and the
    /// other targets block; the player's own capsule is not on the mask, so it does not.
    [[nodiscard]] bool clear(const cy::ai::VisibilityQuery& query) const noexcept {
        const Vec3 span = query.to - query.from;
        const f32 length = cy::length(span);
        if (length < 1e-3F) {
            return true;
        }
        cy::physics::RayCastInput ray;
        ray.origin = query.from;
        ray.direction = span * (1.0F / length);
        ray.max_distance = length;
        const cy::physics::BodyHandle own = bodies_->target_body(observers_[query.first_observer]);
        cy::physics::QueryFilter filter;
        filter.filter.layer = static_cast<cy::u8>(kTargetLayer);
        filter.filter.mask = kSolidQueryMask;
        filter.ignore = &own;
        filter.ignore_count = 1;
        const auto hit = server_->raycast(world_, ray, filter);
        return hit.has_value() && hit->body.is_null();
    }

    const cy::physics::PhysicsServer* server_;
    cy::physics::WorldHandle world_;
    const ArenaBodies* bodies_;
    const u32* observers_;
};

// --- The tree's host: one target's conditions, and the task it chose ---------------------------

class Targets::Brain final : public behaviour::BehaviourHost {
public:
    explicit Brain(TargetState* states) noexcept : states_(states) {}

    void begin(u32 index, bool down) noexcept {
        index_ = index;
        down_ = down;
    }

    behaviour::BtStatus run_task(Name task, f32 /*dt*/) override {
        TargetState& state = states_[index_];
        if (task == fall_) {
            state.mode = TargetMode::Down;
        } else if (task == flinch_) {
            state.mode = TargetMode::Flinch;
        } else if (task == engage_) {
            state.mode = TargetMode::Engage;
        } else {
            state.mode = TargetMode::Patrol;
        }
        // Every task is one tick long, so the tree is walked from its root every tick and the
        // first condition that holds decides.
        return behaviour::BtStatus::Success;
    }

    [[nodiscard]] bool test_condition(Name condition) override {
        const TargetState& state = states_[index_];
        if (condition == down_name_) {
            return down_;
        }
        if (condition == hit_) {
            return state.flinch > 0.0F;
        }
        return condition == contact_ && state.knows;
    }

    [[nodiscard]] f32 score(Name /*task*/) override { return 0.0F; }

private:
    TargetState* states_;
    u32 index_ = 0;
    bool down_ = false;
    Name fall_ = Name::intern("fall");
    Name flinch_ = Name::intern("flinch");
    Name engage_ = Name::intern("engage");
    Name down_name_ = Name::intern("down");
    Name hit_ = Name::intern("hit");
    Name contact_ = Name::intern("contact");
};

// --- Targets ------------------------------------------------------------------------------------

Targets::Targets(cy::Allocator& allocator, cy::physics::PhysicsServer& server,
                 cy::physics::WorldHandle world, ArenaBodies& bodies) noexcept
    : allocator_(&allocator),
      server_(&server),
      world_(world),
      bodies_(&bodies),
      registry_(allocator),
      sink_(allocator),
      graph_(allocator, Name::intern("target")) {}

Targets::~Targets() = default;

cy::Status Targets::compile() noexcept {
    if (cy::Status registered = behaviour::register_behaviour_nodes(registry_); !registered) {
        return registered;
    }
    struct NodeRow {
        cy::graph::NodeKey key;
        const char* type;
        const char* task;
    };
    constexpr NodeRow kNodes[] = {
        {1, "ai.root", nullptr},         {2, "ai.selector", nullptr}, {3, "ai.sequence", nullptr},
        {4, "ai.condition", "down"},     {5, "ai.task", "fall"},      {6, "ai.sequence", nullptr},
        {7, "ai.condition", "hit"},      {8, "ai.task", "flinch"},    {9, "ai.sequence", nullptr},
        {10, "ai.condition", "contact"}, {11, "ai.task", "engage"},   {12, "ai.task", "patrol"},
    };
    struct LinkRow {
        cy::graph::NodeKey from;
        cy::graph::NodeKey to;
        const char* pin;
    };
    // Children in the order they are linked: the selector tries the sequences top to bottom.
    constexpr LinkRow kLinks[] = {
        {2, 1, "child"},     {3, 2, "children"},  {4, 3, "children"},  {5, 3, "children"},
        {6, 2, "children"},  {7, 6, "children"},  {8, 6, "children"},  {9, 2, "children"},
        {10, 9, "children"}, {11, 9, "children"}, {12, 2, "children"},
    };
    for (const NodeRow& row : kNodes) {
        if (auto added = graph_.add_node(row.key, Name::intern(row.type)); !added) {
            return cy::make_unexpected(added.error());
        }
        if (row.task != nullptr) {
            if (auto set =
                    graph_.set_property(row.key, Name::intern("task"), name_literal(row.task));
                !set) {
                return cy::make_unexpected(set.error());
            }
        }
    }
    for (const LinkRow& row : kLinks) {
        if (auto linked =
                graph_.connect(row.from, Name::intern("node"), row.to, Name::intern(row.pin));
            !linked) {
            return cy::make_unexpected(linked.error());
        }
    }
    graph_.resolve(registry_);
    auto compiled = behaviour::compile_behaviour(graph_, registry_, sink_);
    if (!compiled) {
        return cy::make_unexpected(compiled.error());
    }
    program_ = std::make_unique<behaviour::BehaviourProgram>(std::move(*compiled));
    return cy::ok();
}

cy::Status Targets::build(const cy::ecs::Entity (&entities)[kTargetCount]) noexcept {
    if (cy::Status compiled = compile(); !compiled) {
        return compiled;
    }
    runtime_ =
        std::make_unique<cy::ai::AiRuntime>(*allocator_, cy::ai::AiBudget{}, cy::ai::TierPolicy{});
    perception_ =
        std::make_unique<cy::ai::PerceptionScheduler>(*allocator_, cy::ai::PerceptionParams{});
    brain_ = std::make_unique<Brain>(states_);
    const auto first = runtime_->reserve_slots(*program_, kTargetCount);
    if (!first) {
        return cy::make_unexpected(first.error());
    }
    cy::ai::KnowledgeParams knowledge;
    knowledge.capacity = 4;
    // A sighting is held for about four seconds out of sight before it is forgotten.
    knowledge.decay_per_tick = 0.004F;
    knowledge_.clear();
    knowledge_.reserve(kTargetCount);
    const cy::Span<const TargetSpot> spots = target_spots();
    for (u32 index = 0; index < kTargetCount; ++index) {
        entities_[index] = entities[index];
        agents_[index].graph = Name::intern("target");
        agents_[index].seed = 0x7A26E7ULL + index;
        ai_states_[index].slot = *first + index;
        sensors_[index].sight_range = 24.0F;
        sensors_[index].field_of_view_degrees = 120.0F;
        sensors_[index].own_faction = 2;
        sensors_[index].factions_of_interest = 1;
        knowledge_.emplace_back(*allocator_, knowledge);
        TargetState& state = states_[index];
        state.feet = spots[index].home;
        state.yaw = yaw_towards(spots[index].home, spots[index].patrol, 0.0F);
    }
    return cy::ok();
}

cy::u64 Targets::program_digest() const noexcept {
    return program_ == nullptr ? 0U : program_->digest();
}

void Targets::feel(u32 index, u32 tick, const TargetHealth& health,
                   cy::ecs::Entity player) noexcept {
    TargetState& state = states_[index];
    if (!health.present || health.hits <= state.hits_seen) {
        return;
    }
    // A HIT IS A PERCEPTION of the player at the shot's source: `SenseKind::Damage`, at full
    // confidence, whatever the target could see.
    state.hits_felt += static_cast<u32>(health.hits - state.hits_seen);
    state.hits_seen = health.hits;
    state.flinch = kFlinchSeconds;
    (void)knowledge_[index].perceive(player, health.source, Vec3{}, cy::ai::SenseKind::Damage, 1.0F,
                                     1.0F, tick);
}

cy::Status Targets::tick(u32 tick, f32 dt, const PlayerSighting& player,
                         const TargetHealth (&health)[kTargetCount]) noexcept {
    // WHAT THE GAME WROTE: hits felt, and who is down.
    bool down[kTargetCount] = {};
    for (u32 index = 0; index < kTargetCount; ++index) {
        feel(index, tick, health[index], player.entity);
        down[index] = states_[index].mode == TargetMode::Down ||
                      (health[index].present && health[index].points <= 0.0F);
    }

    // WHAT EACH STANDING TARGET SEES: one batched update over all of them.
    cy::ecs::Entity entities[kTargetCount];
    Vec3 eyes[kTargetCount];
    Vec3 forward[kTargetCount];
    cy::ai::PerceptionSensors sensors[kTargetCount];
    f32 importance[kTargetCount];
    cy::ai::AiTier tiers[kTargetCount];
    cy::ai::KnowledgeStore* stores[kTargetCount];
    u32 observers[kTargetCount];
    u32 standing = 0;
    for (u32 index = 0; index < kTargetCount; ++index) {
        if (down[index]) {
            continue;
        }
        entities[standing] = entities_[index];
        eyes[standing] = states_[index].feet + Vec3{0.0F, kEyeHeight, 0.0F};
        forward[standing] = forward_of(states_[index].yaw);
        sensors[standing] = sensors_[index];
        importance[standing] = 1.0F;
        tiers[standing] = cy::ai::AiTier::Full;
        stores[standing] = &knowledge_[index];
        observers[standing] = index;
        ++standing;
    }
    cy::ai::PerceptionTarget seen;
    seen.entity = player.entity;
    seen.position = player.eyes;
    seen.velocity = player.velocity;
    seen.faction = 1;
    const cy::ai::ObserverColumns columns{
        {entities, standing},   {eyes, standing},  {forward, standing}, {sensors, standing},
        {importance, standing}, {tiers, standing}, {stores, standing}};
    Sight sight(*server_, world_, *bodies_, observers);
    cy::ai::PerceptionReport perceived;
    if (cy::Status updated = perception_->update(tick, columns, {&seen, 1}, sight, perceived);
        !updated) {
        return updated;
    }
    for (u32 column = 0; column < standing; ++column) {
        sensors_[observers[column]] = sensors[column];
    }
    sightings_ += perceived.sightings;
    traces_ += sight.traced;

    // WHAT THE TREE DECIDES, one target at a time so its conditions are that target's.
    for (u32 index = 0; index < kTargetCount; ++index) {
        (void)knowledge_[index].age(tick);
        TargetState& state = states_[index];
        const cy::ai::KnowledgeEntry* known = knowledge_[index].best_target();
        state.knows = known != nullptr;
        if (known != nullptr) {
            state.known = known->last_position;
            if (state.first_sighting == 0U && known->sense == cy::ai::SenseKind::Vision) {
                state.first_sighting = tick;
            }
        }
        brain_->begin(index, down[index]);
        cy::ai::ThinkReport thought;
        cy::ai::KnowledgeStore* store = &knowledge_[index];
        if (cy::Status ran =
                runtime_->think(tick, *program_, {&entities_[index], 1}, {&agents_[index], 1},
                                {&ai_states_[index], 1}, {&blackboards_[index], 1}, {&store, 1},
                                *brain_, dt, thought);
            !ran) {
            return ran;
        }
        think_hash_ = fold(think_hash_, thought.state_hash);
        move(index, tick, dt);
    }
    return cy::ok();
}

void Targets::move(u32 index, u32 tick, f32 dt) noexcept {
    TargetState& state = states_[index];
    ++state.ticks_in[static_cast<cy::usize>(state.mode)];
    const TargetSpot& spot = target_spots()[index];
    switch (state.mode) {
        case TargetMode::Down: {
            if (state.fall == 0.0F) {
                (void)bodies_->fell(index);
            }
            state.fall = cy::math::min(1.0F, state.fall + (dt / kFallSeconds));
            break;
        }
        case TargetMode::Flinch: {
            state.flinch = cy::math::max(0.0F, state.flinch - dt);
            state.yaw = turn(state.yaw, yaw_towards(state.feet, state.known, state.yaw),
                             kEngageTurn * 2.0F * dt);
            state.feet = state.feet - (forward_of(state.yaw) * (kStaggerSpeed * dt));
            break;
        }
        case TargetMode::Engage: {
            state.yaw =
                turn(state.yaw, yaw_towards(state.feet, state.known, state.yaw), kEngageTurn * dt);
            // A strafe across the line of fire, its side flipping on a fixed beat of ticks, held
            // to a leash round the target's own spot.
            const auto beat = static_cast<u32>(kStrafeSeconds / dt);
            const f32 side = ((tick / beat) + index) % 2U == 0U ? 1.0F : -1.0F;
            const Vec3 right{std::cos(state.yaw), 0.0F, std::sin(state.yaw)};
            Vec3 next = state.feet + (right * (side * kStrafeSpeed * dt));
            const Vec3 away = next - spot.home;
            const f32 stray = planar_length(away);
            if (stray > kStrafeLeash) {
                next = spot.home + (away * (kStrafeLeash / stray));
            }
            state.feet = next;
            break;
        }
        case TargetMode::Patrol:
        case TargetMode::Count: {
            // There and back: the leg is decided by the tick, so a patrol replays exactly.
            const Vec3 leg = spot.patrol - spot.home;
            const f32 length = planar_length(leg);
            const f32 period = 2.0F * length / kPatrolSpeed;
            const f32 time = static_cast<f32>(tick) * dt;
            const f32 phase = std::fmod(time, period) / period;
            const Vec3 goal = phase < 0.5F ? spot.patrol : spot.home;
            const Vec3 to_goal = goal - state.feet;
            const f32 distance = planar_length(to_goal);
            if (distance > 1e-3F) {
                const f32 step = cy::math::min(distance, kPatrolSpeed * dt);
                state.feet = state.feet + (to_goal * (step / distance));
                state.yaw =
                    turn(state.yaw, yaw_towards(state.feet, goal, state.yaw), kWalkTurn * dt);
            }
            break;
        }
    }
    state.feet.y = 0.0F;
    (void)bodies_->place_target(index, target_placement(state));
}

}  // namespace sample::fps
