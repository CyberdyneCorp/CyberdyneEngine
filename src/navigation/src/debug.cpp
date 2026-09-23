// Extracting debug primitives from navigation data without a renderer dependency.

#include <cy/navigation/debug.h>

#include <cmath>
#include <limits>

namespace cy::navigation {
namespace {

void draw_polygon(const NavMesh& mesh, PolyRef ref, NavDebugSink& sink) noexcept {
    Vec3 corners[kMaxPolyVertices];
    const u32 size = mesh.poly_vertices(ref, corners, kMaxPolyVertices);
    sink.polygon(ref, Span<const Vec3>(corners, size), mesh.effective_area(ref));
}

void draw_adjacency(const NavMesh& mesh, PolyRef ref, NavDebugSink& sink) noexcept {
    for (PolyRef adjacent : mesh.poly_neighbours(ref)) {
        if (!adjacent.valid() || adjacent < ref) {
            continue;
        }
        Vec3 a;
        Vec3 b;
        if (mesh.portal(ref, adjacent, a, b)) {
            sink.adjacency(ref, adjacent, a, b);
        }
    }
}

void draw_tile(const NavMesh& mesh, u32 slot, NavDebugFlags flags, NavDebugSink& sink) noexcept {
    const u32 count = mesh.tile_poly_count(slot);
    if (count == 0) {
        return;
    }
    if (has_flag(flags, NavDebugFlags::Tiles)) {
        sink.tile(mesh.tile_coord(slot), mesh.tile_bounds(slot));
    }
    for (u32 index = 0; index < count; ++index) {
        const PolyRef ref = mesh.tile_poly(slot, index);
        if (has_flag(flags, NavDebugFlags::Polygons)) {
            draw_polygon(mesh, ref, sink);
        }
        if (has_flag(flags, NavDebugFlags::Adjacency)) {
            draw_adjacency(mesh, ref, sink);
        }
    }
}

}  // namespace

void draw_navigation_mesh(const NavMesh& mesh, NavDebugFlags flags, NavDebugSink& sink) noexcept {
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        draw_tile(mesh, slot, flags, sink);
    }
    if (has_flag(flags, NavDebugFlags::Links)) {
        for (LinkId id = 0; id < mesh.link_count(); ++id) {
            if (const NavLink* link = mesh.link(id); link != nullptr) {
                sink.link(id, link->from, link->to, link->action);
            }
        }
    }
    if (has_flag(flags, NavDebugFlags::Obstacles)) {
        for (ObstacleId id = 0; id < mesh.obstacle_capacity(); ++id) {
            if (const NavObstacleShape* obstacle = mesh.obstacle(id); obstacle != nullptr) {
                sink.obstacle(id, *obstacle);
            }
        }
    }
}

void draw_navigation_path(const PathCorridor& corridor, Span<const PathPoint> points,
                          NavDebugFlags flags, NavDebugSink& sink) noexcept {
    if (has_flag(flags, NavDebugFlags::Corridors)) {
        for (PolyRef ref : corridor.polys()) {
            sink.corridor(ref);
        }
    }
    if (has_flag(flags, NavDebugFlags::Paths)) {
        for (usize index = 1; index < points.size(); ++index) {
            sink.path_segment(points[index - 1].position, points[index].position);
        }
    }
}

void draw_navigation_agents(Span<const NavDebugAgent> agents, NavDebugFlags flags,
                            NavDebugSink& sink) noexcept {
    for (const NavDebugAgent& agent : agents) {
        if (has_flag(flags, NavDebugFlags::Avoidance)) {
            sink.velocity(agent.id, agent.position, agent.desired_velocity,
                          agent.adjusted_velocity);
        }
        if (has_flag(flags, NavDebugFlags::Neighbours)) {
            for (NavDebugNeighbour neighbour : agent.neighbours) {
                sink.neighbour(agent.id, neighbour.id, agent.position, neighbour.position);
            }
        }
    }
}

void NavMetrics::record_query(u64 duration_ns, const PathResult& result, f32 path_length) noexcept {
    queries_.fetch_add(1, std::memory_order_relaxed);
    failed_queries_.fetch_add(result.found ? 0 : 1, std::memory_order_relaxed);
    partial_queries_.fetch_add(result.partial ? 1 : 0, std::memory_order_relaxed);
    query_time_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
    if (std::isfinite(path_length) && path_length > 0.0F) {
        const f64 millimetres = static_cast<f64>(path_length) * 1000.0;
        if (millimetres < static_cast<f64>(std::numeric_limits<u64>::max())) {
            path_length_mm_.fetch_add(static_cast<u64>(millimetres), std::memory_order_relaxed);
        }
    }
}

void NavMetrics::record_agent_pass(u32 repaths) noexcept {
    agent_passes_.fetch_add(1, std::memory_order_relaxed);
    repaths_.fetch_add(repaths, std::memory_order_relaxed);
}

void NavMetrics::record_tile_rebuild(u64 duration_ns) noexcept {
    tile_rebuilds_.fetch_add(1, std::memory_order_relaxed);
    tile_rebuild_time_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
}

NavStatistics NavMetrics::snapshot() const noexcept {
    NavStatistics result;
    result.queries = queries_.load(std::memory_order_relaxed);
    result.failed_queries = failed_queries_.load(std::memory_order_relaxed);
    result.partial_queries = partial_queries_.load(std::memory_order_relaxed);
    result.query_time_ns = query_time_ns_.load(std::memory_order_relaxed);
    result.path_length_mm = path_length_mm_.load(std::memory_order_relaxed);
    result.agent_passes = agent_passes_.load(std::memory_order_relaxed);
    result.repaths = repaths_.load(std::memory_order_relaxed);
    result.tile_rebuilds = tile_rebuilds_.load(std::memory_order_relaxed);
    result.tile_rebuild_time_ns = tile_rebuild_time_ns_.load(std::memory_order_relaxed);
    return result;
}

}  // namespace cy::navigation
