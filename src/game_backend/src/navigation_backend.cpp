// SPDX-License-Identifier: MIT
// The `navigation` adapter: `cy::abi::game::NavigationBackend` over `cy::navigation`'s path
// queries, `PathQueue` and `Crowd`, and the agent system that joins them. `add-swift-game-api`.
// See include/cy/game_backend/navigation_backend.h for the steps of `update()` and why they are in
// that order, and tests/test_navigation_backend.cpp for the proof.

#include <cy/game_backend/navigation_backend.h>

#include <cy/abi/errors.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace cy::game_backend {
namespace {

using navigation::NavAgent;
using navigation::NavPathStatus;
using navigation::PathPoint;
using navigation::QueryId;
using navigation::QueryState;

// The ABI mirrors these enums value for value; a reorder on either side is a build failure here.
static_assert(static_cast<u32>(NavPathStatus::Idle) == CY_NAV_PATH_STATUS_IDLE);
static_assert(static_cast<u32>(NavPathStatus::Computing) == CY_NAV_PATH_STATUS_COMPUTING);
static_assert(static_cast<u32>(NavPathStatus::Following) == CY_NAV_PATH_STATUS_FOLLOWING);
static_assert(static_cast<u32>(NavPathStatus::Arrived) == CY_NAV_PATH_STATUS_ARRIVED);
static_assert(static_cast<u32>(NavPathStatus::Failed) == CY_NAV_PATH_STATUS_FAILED);
static_assert(static_cast<u32>(QueryState::Pending) == CY_NAV_QUERY_PENDING);
static_assert(static_cast<u32>(QueryState::Ready) == CY_NAV_QUERY_READY);
static_assert(static_cast<u32>(QueryState::Consumed) == CY_NAV_QUERY_CONSUMED);
static_assert(static_cast<u32>(QueryState::Cancelled) == CY_NAV_QUERY_CANCELLED);

/// A script query's owner in the queue. An agent's query is owned by its entity, which is never
/// zero, so a script can never poll an agent's path out from under it.
constexpr u64 kScriptOwner = 0;
constexpr u32 kMaxPriority = 255;

Vec3 vec3(const f32* value) noexcept {
    return Vec3{value[0], value[1], value[2]};
}

void write3(f32* out, Vec3 value) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

f32 horizontal_distance(Vec3 a, Vec3 b) noexcept {
    return length(Vec3{b.x - a.x, 0.0F, b.z - a.z});
}

CyNavQuery pack_query(u32 world, QueryId id) noexcept {
    return (static_cast<u64>(world) << 32U) | (static_cast<u64>(id) + 1U);
}

u32 world_of(CyNavQuery query) noexcept {
    return static_cast<u32>(query >> 32U);
}

QueryId id_of(CyNavQuery query) noexcept {
    return static_cast<QueryId>((query & 0xFFFFFFFFULL) - 1U);
}

navigation::AreaMask areas_of(u64 mask) noexcept {
    return mask == 0U ? navigation::kAllAreas : (mask & navigation::kAllAreas);
}

navigation::CapabilityMask capabilities_of(u64 mask) noexcept {
    return mask == 0U ? navigation::kAllCapabilities : mask;
}

/// Where a straightened path ends: the target, or — for a partial search — the centre of the last
/// polygon the search reached, which is what `PathQueue`'s own path length measures to.
Vec3 path_end(const navigation::NavMesh& mesh, const navigation::PathCorridor& corridor,
              const navigation::PathResult& result, Vec3 target) noexcept {
    if (!result.partial || corridor.empty()) {
        return target;
    }
    const navigation::NavPoly* last = mesh.poly(corridor.polys()[corridor.size() - 1U]);
    return last != nullptr ? last->centre : target;
}

/// Straighten a search's corridor into `points` and describe it in `out`.
Status straighten_into(const navigation::NavMesh& mesh, const navigation::PathCorridor& corridor,
                       const navigation::PathResult& result, Vec3 start, Vec3 target,
                       Array<PathPoint>& points, CyNavPathResult& out) noexcept {
    points.clear();
    out.flags = (result.found ? CY_NAV_PATH_FOUND : 0U) |
                (result.partial ? CY_NAV_PATH_PARTIAL : 0U) |
                (result.budget_exceeded ? CY_NAV_PATH_BUDGET_EXCEEDED : 0U);
    out.cost = result.cost;
    out.state = CY_NAV_QUERY_READY;
    out.point_count = 0;
    out.length = 0.0F;
    if (!result.found || corridor.empty()) {
        return ok();
    }
    if (Status made = navigation::straighten(mesh, corridor, start,
                                             path_end(mesh, corridor, result, target), points);
        !made) {
        return made;
    }
    out.point_count = static_cast<u32>(points.size());
    for (usize index = 1; index < points.size(); ++index) {
        out.length += length(points[index].position - points[index - 1U].position);
    }
    return ok();
}

void copy_points(Span<const PathPoint> points, Span<f32> out) noexcept {
    const usize count = std::min(points.size(), out.size() / 3U);
    for (usize index = 0; index < count; ++index) {
        write3(&out[index * 3U], points[index].position);
    }
}

/// Where `key` goes in `sorted`, ordered by `projection`.
template <class T, class Key, class Projection>
usize sorted_index(const Array<T>& sorted, const Key& key, Projection projection) noexcept {
    return static_cast<usize>(std::ranges::lower_bound(sorted, key, {}, projection) -
                              sorted.begin());
}

/// Insert `value` at `index`, keeping everything after it in order.
template <class T>
Status insert_at(Array<T>& sorted, usize index, T value) noexcept {
    if (Status pushed = sorted.push_back(std::move(value)); !pushed) {
        return pushed;
    }
    std::ranges::rotate(sorted.begin() + index, sorted.end() - 1, sorted.end());
    return ok();
}

navigation::PathFilter filter_of(const CyNavPathRequest& request) noexcept {
    navigation::PathFilter filter;
    filter.areas = areas_of(request.area_mask);
    filter.capabilities = capabilities_of(request.capabilities);
    if (request.node_budget != 0U) {
        filter.node_budget = request.node_budget;
    }
    return filter;
}

/// `CyNavAgentParams` onto an agent, zero fields taking the engine's defaults.
void apply_params(const CyNavAgentParams& params, NavAgent& value) noexcept {
    const navigation::AvoidanceParams defaults;
    const auto pick = [](f32 given, f32 fallback) noexcept {
        return given > 0.0F ? given : fallback;
    };
    value.avoidance.radius = pick(params.radius, defaults.radius);
    value.avoidance.height = pick(params.height, defaults.height);
    value.avoidance.max_speed = pick(params.max_speed, defaults.max_speed);
    value.avoidance.max_acceleration = pick(params.max_acceleration, defaults.max_acceleration);
    value.avoidance.priority = static_cast<u8>(std::min(params.priority, kMaxPriority));
    value.arrival_distance = pick(params.arrival_distance, NavAgent{}.arrival_distance);
    value.areas = areas_of(params.area_mask);
    value.capabilities = capabilities_of(params.capabilities);
}

void raise_event(NavAgent& agent, NavPathStatus status) noexcept {
    agent.status = status;
    agent.event_pending = true;
}

}  // namespace

NavigationAdapter::NavigationAdapter(Allocator& allocator, ecs::World& world,
                                     const navigation::NavComponents& components,
                                     navigation::NavWorlds& worlds,
                                     const abi::game::GameClock& clock, Config config) noexcept
    : allocator_(&allocator),
      world_(&world),
      components_(components),
      worlds_(&worlds),
      clock_(&clock),
      config_(config),
      agents_(allocator),
      queries_(allocator),
      used_worlds_(allocator),
      crowds_(allocator) {}

NavigationAdapter::NavigationAdapter(Allocator& allocator, ecs::World& world,
                                     const navigation::NavComponents& components,
                                     navigation::NavWorlds& worlds,
                                     const abi::game::GameClock& clock) noexcept
    : NavigationAdapter(allocator, world, components, worlds, clock, Config{}) {}

// --- Lookups
// ---------------------------------------------------------------------------------------

NavigationAdapter::Agent* NavigationAdapter::find_agent(u64 entity) noexcept {
    const usize index = sorted_index(agents_, entity, &Agent::entity);
    return index < agents_.size() && agents_[index].entity == entity ? &agents_[index] : nullptr;
}

const NavigationAdapter::Agent* NavigationAdapter::find_agent(u64 entity) const noexcept {
    const usize index = sorted_index(agents_, entity, &Agent::entity);
    return index < agents_.size() && agents_[index].entity == entity ? &agents_[index] : nullptr;
}

NavigationAdapter::ScriptQuery* NavigationAdapter::find_query(CyNavQuery id) noexcept {
    const usize index = sorted_index(queries_, id, &ScriptQuery::id);
    return index < queries_.size() && queries_[index].id == id ? &queries_[index] : nullptr;
}

navigation::Crowd* NavigationAdapter::find_crowd(u32 world) noexcept {
    for (WorldCrowd& entry : crowds_.span()) {
        if (entry.world == world) {
            return &entry.crowd;
        }
    }
    return nullptr;
}

const navigation::Crowd* NavigationAdapter::find_crowd(u32 world) const noexcept {
    for (const WorldCrowd& entry : crowds_.span()) {
        if (entry.world == world) {
            return &entry.crowd;
        }
    }
    return nullptr;
}

navigation::Crowd* NavigationAdapter::crowd_for(u32 world) noexcept {
    if (navigation::Crowd* existing = find_crowd(world); existing != nullptr) {
        return existing;
    }
    // Kept in world order, so the crowds step in the same order on every run.
    const usize index = sorted_index(crowds_, world, &WorldCrowd::world);
    if (!insert_at(crowds_, index,
                   WorldCrowd{world, navigation::Crowd(*allocator_, config_.crowd_cell_size)})) {
        return nullptr;
    }
    return &crowds_[index].crowd;
}

Status NavigationAdapter::note_world(u32 world) noexcept {
    const usize index = sorted_index(used_worlds_, world, std::identity{});
    if (index < used_worlds_.size() && used_worlds_[index] == world) {
        return ok();
    }
    return insert_at(used_worlds_, index, world);
}

Vec3 NavigationAdapter::extents_of(const CyNavPathRequest& request) const noexcept {
    const Vec3 extents = vec3(request.extents);
    return (extents.x == 0.0F && extents.y == 0.0F && extents.z == 0.0F) ? config_.default_extents
                                                                         : extents;
}

// --- Path queries
// ------------------------------------------------------------------------------------

CyResult NavigationAdapter::find_path(const CyNavPathRequest& request, Span<f32> out_points_xyz,
                                      CyNavPathResult& out_result) noexcept {
    const navigation::NavMesh* mesh = worlds_->mesh(request.world);
    if (mesh == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "no navigation mesh is bound for that world");
    }
    // Per-call scratch: this entry is callable from a job worker, so nothing here is shared.
    navigation::PathCorridor corridor(*allocator_);
    Array<PathPoint> points(*allocator_);
    const Vec3 start = vec3(request.start);
    const Vec3 end = vec3(request.end);
    const navigation::PathResult result =
        navigation::find_path(*mesh, start, end, extents_of(request), filter_of(request), corridor);
    if (Status made = straighten_into(*mesh, corridor, result, start, end, points, out_result);
        !made) {
        return abi::report(made.error());
    }
    copy_points(points.span(), out_points_xyz);
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::request_path(const CyNavPathRequest& request,
                                         CyNavQuery& out_query) noexcept {
    navigation::PathQueue* queue = worlds_->queue(request.world);
    if (queue == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "no navigation path queue is bound for that world");
    }
    const Expected<QueryId, Error> submitted =
        queue->submit(kScriptOwner, vec3(request.start), vec3(request.end), extents_of(request),
                      filter_of(request), static_cast<u32>(clock_->tick));
    if (!submitted) {
        return abi::report(submitted.error());
    }
    if (Status noted = note_world(request.world); !noted) {
        (void)queue->cancel(*submitted);
        return abi::report(noted.error());
    }
    const CyNavQuery id = pack_query(request.world, *submitted);
    ScriptQuery record(*allocator_);
    record.id = id;
    record.start = vec3(request.start);
    record.end = vec3(request.end);
    record.result.struct_size = static_cast<u32>(sizeof(CyNavPathResult));
    if (Status inserted =
            insert_at(queries_, sorted_index(queries_, id, &ScriptQuery::id), std::move(record));
        !inserted) {
        (void)queue->cancel(*submitted);
        return abi::report(inserted.error());
    }
    out_query = id;
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::take_ready(ScriptQuery& query) noexcept {
    const u32 world = world_of(query.id);
    navigation::PathQueue* queue = worlds_->queue(world);
    const navigation::NavMesh* mesh = worlds_->mesh(world);
    if (queue == nullptr || mesh == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "the query's navigation world is no longer bound");
    }
    navigation::PathCorridor corridor(*allocator_);
    navigation::PathResult result;
    if (!queue->consume(id_of(query.id), corridor, result)) {
        return abi::report(CY_RESULT_NOT_FOUND, "the path query has no result to take");
    }
    Array<PathPoint> points(*allocator_);
    if (Status made =
            straighten_into(*mesh, corridor, result, query.start, query.end, points, query.result);
        !made) {
        return abi::report(made.error());
    }
    if (!query.points.resize(points.size() * 3U)) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "navigation query path");
    }
    copy_points(points.span(), query.points.span());
    query.taken = true;
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::poll_path(CyNavQuery query, Span<f32> out_points_xyz,
                                      CyNavPathResult& out_result) noexcept {
    ScriptQuery* record = find_query(query);
    if (record == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "no such path query: it was consumed, retired or never issued");
    }
    const auto index = static_cast<usize>(record - queries_.begin());
    if (record->cancelled) {
        out_result.state = CY_NAV_QUERY_CANCELLED;
        queries_.erase(index);  // retired: answered CANCELLED once
        return CY_RESULT_OK;
    }
    if (!record->taken) {
        const navigation::PathQueue* queue = worlds_->queue(world_of(query));
        if (queue == nullptr) {
            return abi::report(CY_RESULT_NOT_FOUND,
                               "the query's navigation world is no longer bound");
        }
        if (queue->state(id_of(query)) == QueryState::Pending) {
            out_result.state = CY_NAV_QUERY_PENDING;
            return CY_RESULT_OK;
        }
        if (const CyResult taken = take_ready(*record); taken != CY_RESULT_OK) {
            return taken;
        }
    }
    out_result = record->result;
    // Left READY, and nothing written, when the caller's buffer cannot hold the whole path.
    if (out_points_xyz.size() < record->points.size()) {
        return CY_RESULT_OK;
    }
    std::ranges::copy(record->points, out_points_xyz.begin());
    queries_.erase(index);
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::cancel_path(CyNavQuery query) noexcept {
    ScriptQuery* record = find_query(query);
    if (record == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND,
                           "no such path query: it was consumed, retired or never issued");
    }
    if (record->taken) {
        // Its path was already out of the queue; dropping it is the whole of cancelling it.
        queries_.erase(static_cast<usize>(record - queries_.begin()));
        return CY_RESULT_OK;
    }
    if (navigation::PathQueue* queue = worlds_->queue(world_of(query)); queue != nullptr) {
        (void)queue->cancel(id_of(query));
        // A search that already completed is not cancelled by the queue; take its result so the
        // corridor does not sit in the queue for ever.
        navigation::PathCorridor corridor(*allocator_);
        navigation::PathResult result;
        (void)queue->consume(id_of(query), corridor, result);
    }
    record->cancelled = true;
    return CY_RESULT_OK;
}

// --- Agents: the ABI half
// --------------------------------------------------------------------------

CyResult NavigationAdapter::agent_configure(CyEntity entity, const CyNavAgentParams& params,
                                            bool& out_structural) noexcept {
    out_structural = false;
    const ecs::Entity target = ecs::Entity::from_bits(entity);
    if (entity == CY_ENTITY_NULL || !world_->is_alive(target)) {
        return abi::report(CY_RESULT_NOT_FOUND, "nav_agent_configure: the entity is not alive");
    }
    if (worlds_->mesh(params.world) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "no navigation mesh is bound for that world");
    }
    navigation::Crowd* crowd = crowd_for(params.world);
    if (crowd == nullptr || !note_world(params.world)) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "navigation crowd");
    }

    NavAgent value;
    if (const auto* existing = world_->get<NavAgent>(target, components_.agent);
        existing != nullptr) {
        value = *existing;
    } else if (motion_ != nullptr) {
        (void)motion_->position(entity, value.position);
    }
    apply_params(params, value);

    Agent* agent = find_agent(entity);
    if (agent != nullptr && agent->world != params.world) {
        leave_world(*agent, value);
    }
    value.world = params.world;
    if (const CyResult stored = store_component(target, value, out_structural);
        stored != CY_RESULT_OK) {
        return stored;
    }
    if (agent == nullptr) {
        agent = add_record(entity);
        if (agent == nullptr) {
            return abi::report(CY_RESULT_OUT_OF_MEMORY, "navigation agent record");
        }
    }
    agent->world = params.world;
    return join_crowd(*agent, *crowd, value);
}

void NavigationAdapter::leave_world(Agent& agent, NavAgent& value) noexcept {
    // A different navigation world is a different mesh: whatever it was doing is void.
    if (navigation::Crowd* old = find_crowd(agent.world); old != nullptr) {
        (void)old->remove(agent.crowd);
    }
    agent.crowd = navigation::kInvalidCrowdAgent;
    agent.path.clear();
    agent.order = Order::None;
    value.status = NavPathStatus::Idle;
    value.event_pending = false;
}

CyResult NavigationAdapter::store_component(ecs::Entity entity, const NavAgent& value,
                                            bool& out_structural) noexcept {
    if (world_->has(entity, components_.agent)) {
        const Status set = world_->set(entity, components_.agent, value);
        return set ? CY_RESULT_OK : abi::report(set.error());
    }
    const Status added = world_->add(entity, components_.agent, &value);
    if (!added) {
        return abi::report(added.error());
    }
    out_structural = true;
    return CY_RESULT_OK;
}

NavigationAdapter::Agent* NavigationAdapter::add_record(u64 entity) noexcept {
    Agent record(*allocator_);
    record.entity = entity;
    const usize index = sorted_index(agents_, entity, &Agent::entity);
    if (!insert_at(agents_, index, std::move(record))) {
        return nullptr;
    }
    return &agents_[index];
}

CyResult NavigationAdapter::join_crowd(Agent& agent, navigation::Crowd& crowd,
                                       const NavAgent& value) noexcept {
    if (agent.crowd != navigation::kInvalidCrowdAgent) {
        if (navigation::CrowdAgent* member = crowd.agent(agent.crowd); member != nullptr) {
            member->params = value.avoidance;
        }
        return CY_RESULT_OK;
    }
    const Expected<navigation::CrowdAgentId, Error> added =
        crowd.add(value.position, value.avoidance);
    if (!added) {
        return abi::report(added.error());
    }
    agent.crowd = *added;
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::agent_move_to(CyEntity entity, const f32* target_xyz) noexcept {
    Agent* agent = find_agent(entity);
    if (agent == nullptr || !world_->has(ecs::Entity::from_bits(entity), components_.agent)) {
        return abi::report(CY_RESULT_NOT_FOUND, "nav_agent_move_to: the entity is not an agent");
    }
    // Applied in the next update, never here: see the header.
    agent->order = Order::Move;
    agent->order_target = vec3(target_xyz);
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::agent_stop(CyEntity entity) noexcept {
    Agent* agent = find_agent(entity);
    if (agent == nullptr || !world_->has(ecs::Entity::from_bits(entity), components_.agent)) {
        return abi::report(CY_RESULT_NOT_FOUND, "nav_agent_stop: the entity is not an agent");
    }
    agent->order = Order::Stop;
    return CY_RESULT_OK;
}

CyResult NavigationAdapter::agent_state(CyEntity entity,
                                        CyNavAgentState& out_state) const noexcept {
    const Agent* agent = find_agent(entity);
    const NavAgent* component =
        agent != nullptr ? world_->get<NavAgent>(ecs::Entity::from_bits(entity), components_.agent)
                         : nullptr;
    if (component == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "nav_agent_state: the entity is not an agent");
    }
    out_state.status = static_cast<u32>(component->status);
    out_state.flags = component->event_pending ? CY_NAV_AGENT_EVENT : 0U;
    write3(out_state.position, component->position);
    write3(out_state.target, component->target);

    if (component->status == NavPathStatus::Following) {
        const navigation::Crowd* crowd = find_crowd(agent->world);
        const navigation::CrowdAgent* member =
            crowd != nullptr ? crowd->agent(agent->crowd) : nullptr;
        if (member != nullptr) {
            write3(out_state.velocity, member->velocity);
        }
        f32 remaining = 0.0F;
        Vec3 from = component->position;
        for (usize index = component->path_cursor; index < agent->path.size(); ++index) {
            remaining += length(agent->path[index].position - from);
            from = agent->path[index].position;
        }
        out_state.remaining_distance = remaining;
    } else if (component->status == NavPathStatus::Computing) {
        out_state.remaining_distance = length(component->target - component->position);
    }
    return CY_RESULT_OK;
}

// --- Agents: the fixed step
// ------------------------------------------------------------------------

void NavigationAdapter::remove_agent(usize index) noexcept {
    if (navigation::Crowd* crowd = find_crowd(agents_[index].world); crowd != nullptr) {
        (void)crowd->remove(agents_[index].crowd);
    }
    agents_.erase(index);
}

void NavigationAdapter::prune_and_refresh() noexcept {
    for (usize index = agents_.size(); index > 0; --index) {
        const Agent& agent = agents_[index - 1U];
        const ecs::Entity entity = ecs::Entity::from_bits(agent.entity);
        if (!world_->is_alive(entity) || !world_->has(entity, components_.agent)) {
            remove_agent(index - 1U);
        }
    }
    for (Agent& agent : agents_.span()) {
        auto* component =
            world_->get_mut<NavAgent>(ecs::Entity::from_bits(agent.entity), components_.agent);
        // The event lasts from the update that raised it to the next one: exactly one tick.
        component->event_pending = false;
        if (motion_ != nullptr) {
            (void)motion_->position(agent.entity, component->position);
        }
        if (navigation::Crowd* crowd = find_crowd(agent.world); crowd != nullptr) {
            if (navigation::CrowdAgent* member = crowd->agent(agent.crowd); member != nullptr) {
                member->position = component->position;
                member->desired_velocity = Vec3{};
            }
        }
    }
}

void NavigationAdapter::apply_orders(u32 tick) noexcept {
    for (Agent& agent : agents_.span()) {
        if (agent.order == Order::None) {
            continue;
        }
        auto* component =
            world_->get_mut<NavAgent>(ecs::Entity::from_bits(agent.entity), components_.agent);
        navigation::PathQueue* queue = worlds_->queue(agent.world);
        if (component->status == NavPathStatus::Computing && queue != nullptr) {
            (void)queue->cancel(component->query);
        }
        component->query = navigation::kInvalidQuery;
        component->path_cursor = 0;
        agent.path.clear();
        const Order order = agent.order;
        agent.order = Order::None;

        if (order == Order::Stop) {
            component->status = NavPathStatus::Idle;
            component->target = component->position;
            continue;
        }
        component->target = agent.order_target;
        if (horizontal_distance(component->position, component->target) <=
            component->arrival_distance) {
            raise_event(*component, NavPathStatus::Arrived);
            continue;
        }
        if (queue == nullptr) {
            raise_event(*component, NavPathStatus::Failed);
            continue;
        }
        navigation::PathFilter filter;
        filter.costs = component->costs;
        filter.areas = component->areas;
        filter.capabilities = component->capabilities;
        const Expected<QueryId, Error> issued =
            queue->submit(agent.entity, component->position, component->target,
                          config_.default_extents, filter, tick);
        if (!issued) {
            raise_event(*component, NavPathStatus::Failed);
            continue;
        }
        component->query = *issued;
        component->status = NavPathStatus::Computing;
        component->last_repath_tick = tick;
    }
}

void NavigationAdapter::advance_queues(u32 tick) noexcept {
    for (const u32 world : used_worlds_.span()) {
        if (navigation::PathQueue* queue = worlds_->queue(world); queue != nullptr) {
            (void)queue->update(tick);
        }
    }
}

void NavigationAdapter::take_paths() noexcept {
    for (Agent& agent : agents_.span()) {
        auto* component =
            world_->get_mut<NavAgent>(ecs::Entity::from_bits(agent.entity), components_.agent);
        if (component->status != NavPathStatus::Computing) {
            continue;
        }
        navigation::PathQueue* queue = worlds_->queue(agent.world);
        const navigation::NavMesh* mesh = worlds_->mesh(agent.world);
        if (queue == nullptr || mesh == nullptr ||
            queue->state(component->query) == QueryState::Cancelled) {
            raise_event(*component, NavPathStatus::Failed);
            continue;
        }
        if (queue->state(component->query) != QueryState::Ready) {
            continue;
        }
        navigation::PathCorridor corridor(*allocator_);
        navigation::PathResult result;
        (void)queue->consume(component->query, corridor, result);
        component->query = navigation::kInvalidQuery;
        CyNavPathResult described{};
        const Status made = straighten_into(*mesh, corridor, result, component->position,
                                            component->target, agent.path, described);
        if (!made || agent.path.empty()) {
            raise_event(*component, NavPathStatus::Failed);
            continue;
        }
        component->status = NavPathStatus::Following;
        component->path_cursor = 0;
        component->corridor_version = mesh->version();
    }
}

void NavigationAdapter::steer(f32 dt) noexcept {
    for (Agent& agent : agents_.span()) {
        auto* component =
            world_->get_mut<NavAgent>(ecs::Entity::from_bits(agent.entity), components_.agent);
        if (component->status != NavPathStatus::Following) {
            continue;
        }
        const PathPoint& last = agent.path[agent.path.size() - 1U];
        if (horizontal_distance(component->position, last.position) <=
            component->arrival_distance) {
            agent.path.clear();
            component->path_cursor = 0;
            raise_event(*component, NavPathStatus::Arrived);
            continue;
        }
        u32 cursor = component->path_cursor;
        Vec3 desired = navigation::follow_path(agent.path.span(), component->position,
                                               component->avoidance.max_speed,
                                               component->arrival_distance, cursor);
        // Arrive rather than overshoot: on the last leg, no faster than reaches the point this
        // tick.
        const f32 to_end = horizontal_distance(component->position, last.position);
        const f32 speed = length(desired);
        if (dt > 0.0F && speed * dt > to_end && speed > 0.0F) {
            desired = desired * (to_end / (speed * dt));
        }
        component->path_cursor = cursor;
        if (navigation::Crowd* crowd = find_crowd(agent.world); crowd != nullptr) {
            crowd->set_desired_velocity(agent.crowd, desired);
        }
    }
    for (WorldCrowd& entry : crowds_.span()) {
        navigation::CrowdReport report;
        (void)entry.crowd.step(dt, report);
    }
}

void NavigationAdapter::move(f32 dt) noexcept {
    for (Agent& agent : agents_.span()) {
        auto* component =
            world_->get_mut<NavAgent>(ecs::Entity::from_bits(agent.entity), components_.agent);
        navigation::Crowd* crowd = find_crowd(agent.world);
        navigation::CrowdAgent* member = crowd != nullptr ? crowd->agent(agent.crowd) : nullptr;
        if (component->status != NavPathStatus::Following || member == nullptr) {
            continue;
        }
        const Vec3 velocity = member->velocity;
        const Vec3 moved = motion_ != nullptr
                               ? motion_->drive(agent.entity, component->position, velocity, dt)
                               : component->position + (velocity * dt);
        component->position = moved;
        member->position = moved;
    }
}

Status NavigationAdapter::update(f32 dt) noexcept {
    const auto tick = static_cast<u32>(clock_->tick);
    prune_and_refresh();
    apply_orders(tick);
    advance_queues(tick);
    take_paths();
    steer(dt);
    move(dt);
    return ok();
}

void bind(cy::abi::Host& host, NavigationAdapter* adapter) noexcept {
    host.game.navigation = adapter;
}

}  // namespace cy::game_backend
