#pragma once
// Renderer-independent navigation debug command stream.

#include <cy/navigation/query.h>

#include <atomic>

namespace cy::navigation {

enum class NavDebugFlags : u32 {
    None = 0,
    Polygons = 1U << 0U,
    Tiles = 1U << 1U,
    Adjacency = 1U << 2U,
    Links = 1U << 3U,
    Corridors = 1U << 4U,
    Paths = 1U << 5U,
    Avoidance = 1U << 6U,
    Neighbours = 1U << 7U,
    Obstacles = 1U << 8U,
    All = 0x1FFU,
};

[[nodiscard]] constexpr NavDebugFlags operator|(NavDebugFlags a, NavDebugFlags b) noexcept {
    return static_cast<NavDebugFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has_flag(NavDebugFlags set, NavDebugFlags one) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(one)) != 0U;
}

/// Calls are synchronous. A sink must copy a polygon's temporary corner span if it retains it.
class NavDebugSink {
public:
    virtual ~NavDebugSink() = default;
    virtual void polygon(PolyRef, Span<const Vec3>, AreaType) noexcept {}
    virtual void tile(TileCoord, Aabb) noexcept {}
    virtual void adjacency(PolyRef, PolyRef, Vec3, Vec3) noexcept {}
    virtual void link(LinkId, Vec3, Vec3, Name) noexcept {}
    virtual void corridor(PolyRef) noexcept {}
    virtual void path_segment(Vec3, Vec3) noexcept {}
    virtual void velocity(u64, Vec3, Vec3, Vec3) noexcept {}
    virtual void neighbour(u64, u64, Vec3, Vec3) noexcept {}
    virtual void obstacle(ObstacleId, const NavObstacleShape&) noexcept {}
};

struct NavDebugNeighbour {
    u64 id = 0;
    Vec3 position;
};

struct NavDebugAgent {
    u64 id = 0;
    Vec3 position;
    Vec3 desired_velocity;
    Vec3 adjusted_velocity;
    Span<const NavDebugNeighbour> neighbours;
};

void draw_navigation_mesh(const NavMesh& mesh, NavDebugFlags flags, NavDebugSink& sink) noexcept;
void draw_navigation_path(const PathCorridor& corridor, Span<const PathPoint> points,
                          NavDebugFlags flags, NavDebugSink& sink) noexcept;
void draw_navigation_agents(Span<const NavDebugAgent> agents, NavDebugFlags flags,
                            NavDebugSink& sink) noexcept;

/// Cumulative diagnostics. Timing fields are observational and never feed path decisions.
struct NavStatistics {
    u64 queries = 0;
    u64 failed_queries = 0;
    u64 partial_queries = 0;
    u64 query_time_ns = 0;
    u64 path_length_mm = 0;
    u64 agent_passes = 0;
    u64 repaths = 0;
    u64 tile_rebuilds = 0;
    u64 tile_rebuild_time_ns = 0;

    [[nodiscard]] f64 mean_query_ms() const noexcept {
        return queries == 0 ? 0.0 : static_cast<f64>(query_time_ns) / (queries * 1'000'000.0);
    }
    [[nodiscard]] f64 mean_path_length() const noexcept {
        return queries == 0 ? 0.0 : static_cast<f64>(path_length_mm) / (queries * 1000.0);
    }
    [[nodiscard]] f64 repaths_per_pass() const noexcept {
        return agent_passes == 0 ? 0.0 : static_cast<f64>(repaths) / agent_passes;
    }
    [[nodiscard]] f64 mean_rebuild_ms() const noexcept {
        return tile_rebuilds == 0
                   ? 0.0
                   : static_cast<f64>(tile_rebuild_time_ns) / (tile_rebuilds * 1'000'000.0);
    }
};

/// Atomic so independent world queues can report into one diagnostics view. A snapshot taken while
/// writers run is approximate; it is for display, not simulation state.
class NavMetrics {
public:
    void record_query(u64 duration_ns, const PathResult& result, f32 path_length) noexcept;
    void record_agent_pass(u32 repaths) noexcept;
    void record_tile_rebuild(u64 duration_ns) noexcept;
    [[nodiscard]] NavStatistics snapshot() const noexcept;

private:
    std::atomic<u64> queries_{0};
    std::atomic<u64> failed_queries_{0};
    std::atomic<u64> partial_queries_{0};
    std::atomic<u64> query_time_ns_{0};
    std::atomic<u64> path_length_mm_{0};
    std::atomic<u64> agent_passes_{0};
    std::atomic<u64> repaths_{0};
    std::atomic<u64> tile_rebuilds_{0};
    std::atomic<u64> tile_rebuild_time_ns_{0};
};

}  // namespace cy::navigation
