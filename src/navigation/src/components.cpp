// Registering navigation's components in a world, and the bulk pass over them. See
// cy/navigation/components.h.

#include <cy/ecs/query.h>
#include <cy/navigation/components.h>

namespace cy::navigation {
namespace {

template <class T>
[[nodiscard]] Status bind(World& world, const char* name, ComponentTypeId& out) noexcept {
    Expected<ComponentTypeId, Error> id = world.components().register_builtin(
        name, static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T)));
    if (!id) {
        return make_unexpected(id.error());
    }
    out = *id;
    return ok();
}

}  // namespace

const char* nav_path_status_name(NavPathStatus status) noexcept {
    switch (status) {
        case NavPathStatus::Idle:
            return "Idle";
        case NavPathStatus::Computing:
            return "Computing";
        case NavPathStatus::Following:
            return "Following";
        case NavPathStatus::Arrived:
            return "Arrived";
        case NavPathStatus::Failed:
            return "Failed";
        case NavPathStatus::Count:
            break;
    }
    return "unknown";
}

Expected<NavComponents, Error> NavComponents::register_all(World& world) noexcept {
    NavComponents ids;
    // The order is the id order and therefore the serialized descriptor table's order. Fixed
    // deliberately; see the header.
    if (Status bound = bind<NavMeshSurface>(world, kNavMeshSurfaceComponentName, ids.surface);
        !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<NavAgent>(world, kNavAgentComponentName, ids.agent); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<NavObstacle>(world, kNavObstacleComponentName, ids.obstacle); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<NavLinkComponent>(world, kNavLinkComponentName, ids.link); !bound) {
        return make_unexpected(bound.error());
    }
    if (Status bound = bind<NavArea>(world, kNavAreaComponentName, ids.area); !bound) {
        return make_unexpected(bound.error());
    }
    return ids;
}

namespace {

/// One agent's bookkeeping: is its corridor still valid, has it arrived, and does it need a path?
/// Returns false when nothing more is to be done for it this tick.
[[nodiscard]] bool refresh_agent(NavAgent& agent, const NavMesh& mesh, u32 tick,
                                 u32 repath_interval, NavAgentReport& report) noexcept {
    // A corridor whose polygons no longer resolve is `navigation`'s "the agent's path SHALL be
    // invalidated cleanly and a repath triggered": the version comparison is the cheap half and
    // `poly()` is the exact one.
    const bool mesh_moved = agent.corridor_version != mesh.version();
    const bool poly_gone = agent.poly.valid() && mesh.poly(agent.poly) == nullptr;
    if ((mesh_moved || poly_gone) && agent.status == NavPathStatus::Following) {
        agent.status = NavPathStatus::Idle;
        ++report.corridors_invalidated;
    }

    if (agent.status == NavPathStatus::Arrived || agent.status == NavPathStatus::Failed) {
        // The event lasts exactly one pass, so a reader sees it once and never twice.
        agent.event_pending = false;
        return false;
    }
    // A field follower issues no query at all; that is the whole saving of the tier.
    if (agent.status == NavPathStatus::Computing || agent.status == NavPathStatus::Following ||
        agent.follows_field) {
        return false;
    }
    // `navigation`: "subject to a repath rate limit".
    if (tick < agent.last_repath_tick + repath_interval && agent.last_repath_tick != 0) {
        ++report.repaths_rate_limited;
        return false;
    }
    return true;
}

/// Snap the agent to the mesh, answer whether it has arrived, and issue its query.
[[nodiscard]] Status request_path(NavAgent& agent, Entity entity, const NavMesh& mesh,
                                  PathQueue& queue, u32 tick, NavAgentReport& report) noexcept {
    const Vec3 extents{2.0F, 4.0F, 2.0F};
    Vec3 nearest;
    agent.poly = mesh.find_nearest(agent.position, extents, agent.areas, nearest);
    const PolyRef destination = mesh.find_nearest(agent.target, extents, agent.areas, nearest);
    if (!agent.poly.valid() || !destination.valid()) {
        agent.status = NavPathStatus::Failed;
        agent.event_pending = true;
        ++report.failed;
        return ok();
    }
    if (length(agent.target - agent.position) <= agent.arrival_distance) {
        agent.status = NavPathStatus::Arrived;
        agent.event_pending = true;
        ++report.arrived;
        return ok();
    }

    PathFilter filter;
    filter.costs = agent.costs;
    filter.areas = agent.areas;
    filter.capabilities = agent.capabilities;
    const Expected<QueryId, Error> issued =
        queue.submit(entity.bits(), agent.position, agent.target, extents, filter, tick);
    if (!issued) {
        agent.status = NavPathStatus::Failed;
        agent.event_pending = true;
        ++report.failed;
        return ok();
    }
    agent.query = *issued;
    agent.status = NavPathStatus::Computing;
    agent.last_repath_tick = tick;
    agent.corridor_version = mesh.version();
    ++report.repaths_issued;
    return ok();
}

}  // namespace

Status update_agents(World& world, const NavComponents& components, const NavMesh& mesh,
                     PathQueue& queue, u32 tick, u32 repath_interval,
                     NavAgentReport& report) noexcept {
    return update_agents(world, components, mesh, queue, 0, tick, repath_interval, report);
}

Status update_agents(World& world, const NavComponents& components, const NavMesh& mesh,
                     PathQueue& queue, u32 navigation_world, u32 tick, u32 repath_interval,
                     NavAgentReport& report) noexcept {
    report = NavAgentReport{};
    if (!components.registered()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "navigation's components are not registered in this world"});
    }

    ecs::QueryDesc desc(world.allocator());
    if (Status declared = desc.write(components.agent); !declared) {
        return declared;
    }
    ecs::Query query(world, std::move(desc));

    // One pass over packed chunks. `navigation`: "Crowd movement SHALL be a scheduled,
    // data-oriented system operating over agent components in bulk"; the same is true of the
    // bookkeeping that feeds it.
    NavAgentReport* out = &report;
    const NavMesh* subject = &mesh;
    PathQueue* pending = &queue;
    return query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<const ecs::Entity> entities = chunk.entities();
        const Span<NavAgent> agents = chunk.write<NavAgent>(components.agent);
        for (u32 row = 0; row < agents.size(); ++row) {
            if (agents[row].world != navigation_world ||
                agents[row].representation != NavigationRepresentation::Surface) {
                continue;
            }
            ++out->agents;
            if (!refresh_agent(agents[row], *subject, tick, repath_interval, *out)) {
                continue;
            }
            (void)request_path(agents[row], entities[row], *subject, *pending, tick, *out);
        }
    });
}

namespace {

[[nodiscard]] bool refresh_volume_agent(NavAgent& agent, const NavVolume& volume, u32 tick,
                                        u32 repath_interval, NavAgentReport& report) noexcept {
    if (agent.status == NavPathStatus::Following && agent.corridor_version != volume.version()) {
        agent.status = NavPathStatus::Idle;
        ++report.corridors_invalidated;
    }
    if (agent.status == NavPathStatus::Arrived || agent.status == NavPathStatus::Failed) {
        agent.event_pending = false;
        return false;
    }
    if (agent.status == NavPathStatus::Computing || agent.status == NavPathStatus::Following ||
        agent.follows_field) {
        return false;
    }
    if (tick < agent.last_repath_tick + repath_interval && agent.last_repath_tick != 0) {
        ++report.repaths_rate_limited;
        return false;
    }
    return true;
}

void request_volume_path(NavAgent& agent, Entity entity, const NavVolume& volume,
                         SpatialPathQueue& queue, u32 tick, NavAgentReport& report) noexcept {
    if (length(agent.target - agent.position) <= agent.arrival_distance) {
        agent.status = NavPathStatus::Arrived;
        agent.event_pending = true;
        ++report.arrived;
        return;
    }
    PathFilter filter;
    filter.costs = agent.costs;
    filter.areas = agent.areas;
    filter.capabilities = agent.capabilities;
    const Expected<QueryId, Error> issued = queue.submit(
        entity.bits(), agent.position, agent.target, Vec3{2.0F, 2.0F, 2.0F}, filter, tick);
    if (!issued) {
        agent.status = NavPathStatus::Failed;
        agent.event_pending = true;
        ++report.failed;
        return;
    }
    agent.query = *issued;
    agent.status = NavPathStatus::Computing;
    agent.last_repath_tick = tick;
    agent.corridor_version = volume.version();
    ++report.repaths_issued;
}

[[nodiscard]] Status update_volume_agents(World& world, const NavComponents& components,
                                          const NavVolume& volume, SpatialPathQueue& queue,
                                          u32 navigation_world, u32 tick, u32 repath_interval,
                                          NavAgentReport& report) noexcept {
    report = NavAgentReport{};
    ecs::QueryDesc desc(world.allocator());
    if (Status declared = desc.write(components.agent); !declared) {
        return declared;
    }
    ecs::Query query(world, std::move(desc));
    return query.for_each_chunk([&](ecs::QueryChunk& chunk) noexcept {
        const Span<const ecs::Entity> entities = chunk.entities();
        const Span<NavAgent> agents = chunk.write<NavAgent>(components.agent);
        for (u32 row = 0; row < agents.size(); ++row) {
            NavAgent& agent = agents[row];
            if (agent.world != navigation_world ||
                agent.representation != NavigationRepresentation::Volume) {
                continue;
            }
            ++report.agents;
            if (refresh_volume_agent(agent, volume, tick, repath_interval, report)) {
                request_volume_path(agent, entities[row], volume, queue, tick, report);
            }
        }
    });
}

void accumulate(NavAgentReport& total, const NavAgentReport& partial) noexcept {
    total.agents += partial.agents;
    total.repaths_issued += partial.repaths_issued;
    total.repaths_rate_limited += partial.repaths_rate_limited;
    total.corridors_invalidated += partial.corridors_invalidated;
    total.arrived += partial.arrived;
    total.failed += partial.failed;
}

}  // namespace

Status NavWorlds::bind(u32 id, NavMesh& mesh, PathQueue& queue) noexcept {
    if (&queue.mesh() != &mesh) {
        return fail(ErrorCode::InvalidArgument, "navigation world queue searches a different mesh");
    }
    if (this->mesh(id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "navigation world id is already bound");
    }
    return bindings_.push_back(Binding{id, &mesh, &queue});
}

Status NavWorlds::unbind(u32 id) noexcept {
    for (usize index = 0; index < bindings_.size(); ++index) {
        if (bindings_[index].id == id) {
            bindings_.remove_unordered(index);
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "navigation world id is not bound");
}

NavMesh* NavWorlds::mesh(u32 id) const noexcept {
    for (const Binding& binding : bindings_.span()) {
        if (binding.id == id) {
            return binding.mesh;
        }
    }
    return nullptr;
}

PathQueue* NavWorlds::queue(u32 id) const noexcept {
    for (const Binding& binding : bindings_.span()) {
        if (binding.id == id) {
            return binding.queue;
        }
    }
    return nullptr;
}

Expected<ObstacleId, Error> NavWorlds::add_obstacle(u32 id,
                                                    const NavObstacleShape& shape) noexcept {
    NavMesh* subject = mesh(id);
    if (subject == nullptr) {
        return fail(ErrorCode::NotFound, "navigation world id is not bound");
    }
    return subject->add_obstacle(shape);
}

Status NavWorlds::remove_obstacle(u32 id, ObstacleId obstacle) noexcept {
    NavMesh* subject = mesh(id);
    return subject == nullptr ? fail(ErrorCode::NotFound, "navigation world id is not bound")
                              : subject->remove_obstacle(obstacle);
}

Expected<LinkId, Error> NavWorlds::add_link(u32 id, const NavLink& link, Vec3 snap) noexcept {
    NavMesh* subject = mesh(id);
    if (subject == nullptr) {
        return fail(ErrorCode::NotFound, "navigation world id is not bound");
    }
    return subject->add_link(link, snap);
}

Status NavWorlds::remove_link(u32 id, LinkId link) noexcept {
    NavMesh* subject = mesh(id);
    return subject == nullptr ? fail(ErrorCode::NotFound, "navigation world id is not bound")
                              : subject->remove_link(link);
}

Status NavWorlds::bind_volume(u32 id, NavVolume& volume, SpatialPathQueue& queue) noexcept {
    if (queue.space().volume() != &volume) {
        return fail(ErrorCode::InvalidArgument,
                    "navigation volume queue searches a different volume");
    }
    if (this->volume(id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "navigation volume world id is already bound");
    }
    return volume_bindings_.push_back(VolumeBinding{id, &volume, &queue});
}

Status NavWorlds::unbind_volume(u32 id) noexcept {
    for (usize index = 0; index < volume_bindings_.size(); ++index) {
        if (volume_bindings_[index].id == id) {
            volume_bindings_.remove_unordered(index);
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "navigation volume world id is not bound");
}

NavVolume* NavWorlds::volume(u32 id) const noexcept {
    for (const VolumeBinding& binding : volume_bindings_.span()) {
        if (binding.id == id) {
            return binding.volume;
        }
    }
    return nullptr;
}

SpatialPathQueue* NavWorlds::volume_queue(u32 id) const noexcept {
    for (const VolumeBinding& binding : volume_bindings_.span()) {
        if (binding.id == id) {
            return binding.queue;
        }
    }
    return nullptr;
}

Status NavWorlds::update(World& world, const NavComponents& components, u32 tick,
                         u32 repath_interval, NavAgentReport& report) noexcept {
    report = NavAgentReport{};
    for (const Binding& binding : bindings_.span()) {
        NavAgentReport partial;
        if (Status updated = update_agents(world, components, *binding.mesh, *binding.queue,
                                           binding.id, tick, repath_interval, partial);
            !updated) {
            return updated;
        }
        accumulate(report, partial);
    }
    for (const VolumeBinding& binding : volume_bindings_.span()) {
        NavAgentReport partial;
        if (Status updated =
                update_volume_agents(world, components, *binding.volume, *binding.queue, binding.id,
                                     tick, repath_interval, partial);
            !updated) {
            return updated;
        }
        accumulate(report, partial);
    }
    return ok();
}

}  // namespace cy::navigation
