#pragma once
// Sparse 3D navigation and a representation-neutral point-path query surface.

#include <cy/core/memory/hash_map.h>
#include <cy/navigation/query.h>

namespace cy::navigation {

struct VolumeCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    friend constexpr bool operator==(VolumeCoord, VolumeCoord) noexcept = default;
};

struct VolumeCell {
    VolumeCoord coord;
    AreaType area = kAreaGround;
    f32 cost = 1.0F;
};

/// Only navigable cells are stored. Six-connected adjacency prevents a route from cutting through
/// a blocked corner, and coordinates/costs are independent of any surface NavMesh.
class NavVolume {
public:
    NavVolume(Allocator& allocator, f32 cell_size) noexcept;

    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }
    [[nodiscard]] u32 version() const noexcept { return version_; }
    [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    [[nodiscard]] Status set_cell(VolumeCoord coord, AreaType area = kAreaGround,
                                  f32 cost = 1.0F) noexcept;
    [[nodiscard]] Status remove_cell(VolumeCoord coord) noexcept;
    [[nodiscard]] const VolumeCell* cell(VolumeCoord coord) const noexcept;
    [[nodiscard]] Vec3 center(VolumeCoord coord) const noexcept;
    [[nodiscard]] bool nearest(Vec3 position, Vec3 extents, AreaMask areas,
                               VolumeCoord& coord) const noexcept;

    /// A* with PathFilter/PathResult shared with surface queries. A partial path ends at the
    /// reachable cell closest to the target. The point path is three-dimensional.
    [[nodiscard]] PathResult find_path(Vec3 start, Vec3 end, Vec3 extents, const PathFilter& filter,
                                       Array<Vec3>& points) const noexcept;

private:
    Allocator* allocator_ = nullptr;
    HashMap<u64, VolumeCell> cells_;
    f32 cell_size_ = 1.0F;
    u32 version_ = 0;
};

enum class NavigationRepresentation : u8 { Surface = 0, Volume };

/// Both representations answer the same filter/result/point-path query. The surface branch runs
/// polygon A* and the funnel; the volume branch runs sparse-voxel A*.
class NavigationSpace {
public:
    explicit NavigationSpace(const NavMesh& mesh) noexcept : mesh_(&mesh) {}
    explicit NavigationSpace(const NavVolume& volume) noexcept : volume_(&volume) {}

    [[nodiscard]] NavigationRepresentation representation() const noexcept {
        return volume_ == nullptr ? NavigationRepresentation::Surface
                                  : NavigationRepresentation::Volume;
    }
    [[nodiscard]] const NavVolume* volume() const noexcept { return volume_; }
    [[nodiscard]] Allocator& allocator() const noexcept;
    [[nodiscard]] PathResult find_path(Vec3 start, Vec3 end, Vec3 extents, const PathFilter& filter,
                                       Array<Vec3>& points) const noexcept;

private:
    const NavMesh* mesh_ = nullptr;
    const NavVolume* volume_ = nullptr;
};

/// Deterministic asynchronous delivery for either navigation representation. A query issued on
/// tick N is delivered on N + latency, in submission order.
class SpatialPathQueue {
public:
    SpatialPathQueue(Allocator& allocator, NavigationSpace space, u32 latency_ticks) noexcept;

    [[nodiscard]] NavigationRepresentation representation() const noexcept {
        return space_.representation();
    }
    [[nodiscard]] const NavigationSpace& space() const noexcept { return space_; }
    void set_metrics(NavMetrics* metrics) noexcept { metrics_ = metrics; }
    [[nodiscard]] Expected<QueryId, Error> submit(u64 owner, Vec3 start, Vec3 end, Vec3 extents,
                                                  const PathFilter& filter, u32 tick) noexcept;
    [[nodiscard]] Status cancel(QueryId id) noexcept;
    [[nodiscard]] u32 update(u32 tick) noexcept;
    [[nodiscard]] bool consume(QueryId id, Array<Vec3>& points, PathResult& result) noexcept;
    [[nodiscard]] Span<const QueryId> completed() const noexcept { return completed_.span(); }
    [[nodiscard]] QueryState state(QueryId id) const noexcept;
    [[nodiscard]] u64 owner(QueryId id) const noexcept;
    [[nodiscard]] u32 pending() const noexcept;

private:
    struct Entry {
        u64 owner = 0;
        Vec3 start;
        Vec3 end;
        Vec3 extents;
        PathFilter filter;
        Array<Vec3> points;
        PathResult result;
        u32 deliver_tick = 0;
        QueryState state = QueryState::Pending;

        explicit Entry(Allocator& allocator) noexcept : points(allocator) {}
    };

    NavigationSpace space_;
    Array<Entry> entries_;
    Array<QueryId> completed_;
    u32 latency_ = 1;
    NavMetrics* metrics_ = nullptr;
};

}  // namespace cy::navigation
