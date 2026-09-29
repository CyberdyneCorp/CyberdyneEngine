// SPDX-License-Identifier: MIT
#pragma once
// The editor-window runtime's side of navigation authoring. Issue #28, tasks 3.1, 3.2, 3.4 and 3.5
// of `implement-issue-28-navigation-authoring`.
//
// ================================================================================================
// WHO DOES WHAT
// ================================================================================================
//
// The engine's `NavigationService` (src/editor_backend) runs every bake, rebake, query and pick.
// This file only supplies it with the authored world and carries its results to the frame:
//
//   `AuthoredNavigationSource` implements `NavigationSourceRuntime` over the `.cyworld` the editor
//   has open. It gathers world-space triangles from `MeshRenderer` nodes (the same `.cyprim` and
//   cooked-mesh paths `AuthoredFrame::load_mesh` reads), reads the Nav* components by name, stores
//   and loads the content-addressed `<project>/navigation/<identity>.cynavmesh` sidecar, and turns
//   a pixel of a published frame into a world ray from the view that frame was rendered with.
//
//   `NavigationDriver` watches the synced document. When a Nav* component, or a mesh inside a
//   surface, changes, it sends `navigation.update` for the union of the old and new bounds, in
//   process; when the recorded bake identity changes (an undone bake) it sends `navigation.status`
//   so the service reloads that sidecar; when the document stops recording a bake for a world (the
//   first bake undone, or the world removed) it sends `navigation.clear`, so no query answers on a
//   mesh the document does not record; when a world's overlay flags change it sends
//   `navigation.overlay.set`. It also keeps the editor's last test path and flow field per world
//   for the overlay. The overlay itself is `nav_overlay.h`.
//
// ================================================================================================
// THE AUTHORED NAMES (the document schema the editor's `navigation.*` commands write)
// ================================================================================================
//
//   NavigationWorld  world (int), agent_radius, agent_height, max_slope, step_height, cell_size,
//                    cell_height, tile_size (float), layers, tags (int), backend (int: 1 engine,
//                    2 recast), overlay (int: NavDebugFlags bits), bake_identity,
//                    source_fingerprint (int: the u64 bit pattern, or decimal/0x text)
//   NavMeshSurface   world (int), bounds.min, bounds.max (vec3, node-local), exclude (bool)
//   NavArea          world (int), bounds.min, bounds.max (vec3, node-local), area (int),
//                    cost (float)
//   NavObstacle      world (int), shape.offset (vec3), shape.radius, shape.height (float),
//                    shape.half_extents (vec3; a box when the radius is zero), area (int,
//                    default 63: carved out). The centre is the node's world translation plus the
//                    offset, so the transform gizmo moves an obstacle.
//   NavLink          world (int), from, to (vec3, node-local), cost (float), area (int),
//                    bidirectional (bool), requires_capabilities (int), action (text)
//
// A missing field takes the engine default. A MeshRenderer contributes its triangles to every
// navigation world with an including surface that overlaps the mesh's world bounds.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/editor/navigation_service.h>
#include <cy/navigation/bake.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/model.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nav_overlay.h"

namespace cy::sample::editor_window {

/// One `NavigationWorld` as the document records it.
struct NavWorldRecord {
    u32 world = 0;
    navigation::NavBakeSettings settings;
    u32 overlay = 0;
    u64 bake_identity = 0;
    u64 source_fingerprint = 0;
};

/// Every `NavigationWorld` of `world`, in node order. A world id declared twice keeps the first.
[[nodiscard]] Status read_navigation_worlds(const scene::serialization::World& world,
                                            Array<NavWorldRecord>& out) noexcept;

/// What a node contributes to navigation, for change tracking.
enum class NavRole : u8 { Mesh, Surface, Area, Obstacle, Link };

/// One node's contribution: its world-space bounds and a digest of everything that shapes it.
struct NavNodeState {
    u64 identity = 0;
    NavRole role = NavRole::Mesh;
    /// The navigation world. Unused for a mesh, which belongs to every world whose surface it
    /// overlaps.
    u32 world = 0;
    /// An including surface (only meaningful for `Surface`).
    bool including = false;
    u64 digest = 0;
    Aabb bounds;
};

/// A world and the region a document change dirtied in it.
struct NavDirtyRegion {
    u32 world = 0;
    Aabb region;
};

/// The union of the old and new bounds of every changed contribution, per world. A mesh change
/// dirties each world with an including surface (before or after) that overlaps the mesh.
[[nodiscard]] std::vector<NavDirtyRegion> dirty_regions(const std::vector<NavNodeState>& before,
                                                        const std::vector<NavNodeState>& after);

class AuthoredNavigationSource final : public editor::NavigationSourceRuntime {
public:
    /// `project` is where mesh references resolve and where the sidecars go.
    AuthoredNavigationSource(Allocator& allocator, std::string project) noexcept;
    ~AuthoredNavigationSource() override;
    AuthoredNavigationSource(const AuthoredNavigationSource&) = delete;
    AuthoredNavigationSource& operator=(const AuthoredNavigationSource&) = delete;
    AuthoredNavigationSource(AuthoredNavigationSource&&) = delete;
    AuthoredNavigationSource& operator=(AuthoredNavigationSource&&) = delete;

    /// The world the seam reads. Null declares no navigation world.
    void bind(const scene::serialization::World* world) noexcept { world_ = world; }
    [[nodiscard]] const scene::serialization::World* bound() const noexcept { return world_; }
    /// Where `navigation/<identity>.cynavmesh` is written and read; the project by default.
    void set_sidecar_root(std::string root) { sidecar_root_ = std::move(root); }

    /// Remembers the view frame `frame` was rendered with, for `pick_ray`. The last 64 are kept.
    void record_frame(u64 frame, const render::View& view, Vec3 eye);
    /// The view of `frame`, or of the latest frame when `frame` is zero. Null when not held.
    [[nodiscard]] const NavOverlayView* frame_view(u64 frame) const noexcept;

    /// Every navigation-relevant contribution of the bound world, for `dirty_regions`.
    [[nodiscard]] Status describe(std::vector<NavNodeState>& out) noexcept;

    // NavigationSourceRuntime.
    [[nodiscard]] Status worlds(Array<u32>& out) noexcept override;
    [[nodiscard]] Status gather(u32 world, editor::NavSourceBuffers& out) noexcept override;
    [[nodiscard]] Status obstacles(u32 world,
                                   Array<navigation::NavObstacleShape>& out) noexcept override;
    [[nodiscard]] Status links(u32 world, Array<navigation::NavLink>& out) noexcept override;
    [[nodiscard]] Status store_bake(u64 identity, Span<const u8> bytes,
                                    Array<char>& out_path) noexcept override;
    [[nodiscard]] Status load_bake(u64 identity, Array<u8>& out) noexcept override;
    [[nodiscard]] Expected<Ray, Error> pick_ray(u32 viewport, u64 frame, f32 x,
                                                f32 y) noexcept override;

    /// Mesh loads that failed, so a missing asset is reported rather than silently unwalkable.
    [[nodiscard]] u32 mesh_failures() const noexcept { return mesh_failures_; }

private:
    struct LoadedMesh;
    struct Frame {
        u64 identity = 0;
        NavOverlayView view;
    };

    /// The mesh a reference names, loaded once. Null when it cannot be read.
    [[nodiscard]] const LoadedMesh* mesh(const std::string& reference) noexcept;
    [[nodiscard]] Status world_matrices(Array<Mat4>& out) const noexcept;
    [[nodiscard]] Status gather_meshes(Span<const Mat4> matrices,
                                       editor::NavSourceBuffers& out) noexcept;

    Allocator* allocator_;
    std::string project_;
    std::string sidecar_root_;
    const scene::serialization::World* world_ = nullptr;
    std::map<std::string, std::unique_ptr<LoadedMesh>> meshes_;
    std::deque<Frame> frames_;
    u32 mesh_failures_ = 0;
};

/// The last `navigation.update` the runtime sent, as the service answered it.
struct NavUpdateResult {
    u32 world = 0;
    u64 identity = 0;
    std::vector<navigation::TileCoord> rebuilt;
    std::vector<navigation::TileCoord> marked;
};

/// The runtime's own navigation requests, and what the frame draws per world.
class NavigationDriver {
public:
    /// The runtime's request ids carry this bit; the editor's never do.
    static constexpr u64 kInternalRequest = u64{1} << 63U;

    /// Requests go to `service` in `session`: the one binding the editor's requests also use, so
    /// they reach the same navigation contexts.
    NavigationDriver(abi::EditorServiceBackend& service, CyServiceSession session) noexcept
        : service_(&service), session_(session) {}

    /// Compares the bound world's navigation state with the last one seen and queues the requests
    /// that bring the service up to date: a restore when a recorded bake identity changed, an
    /// overlay set when recorded flags changed, a clear when a world's recorded bake went away, and
    /// an update per dirtied world.
    [[nodiscard]] Status document_changed(AuthoredNavigationSource& source);
    /// Starts a frame: a request the service answered `navigation.busy` may be retried again.
    void begin_frame() noexcept { backoff_ = false; }
    /// Submits the next queued request when none of the runtime's is in flight. After a
    /// `navigation.busy` answer it waits for the next frame, so a long editor bake is not polled
    /// with one refused request per event.
    void pump() noexcept;
    /// Every event polled from the service passes here first. Answers true for the runtime's own
    /// requests (consumed); for the editor's it records path and flow-field answers for the overlay
    /// and answers false, so the caller forwards the event.
    [[nodiscard]] bool observe(const CyServiceEvent& event);
    /// Every editor request passes here before it is submitted.
    void editor_request(u64 id, std::string_view operation, Span<const u8> payload);

    /// No request of the runtime's is queued or in flight.
    [[nodiscard]] bool idle() const noexcept { return queue_.empty() && in_flight_ == 0; }
    [[nodiscard]] const NavUpdateResult& last_update() const noexcept { return last_update_; }
    [[nodiscard]] u32 updates_completed() const noexcept { return updates_completed_; }
    [[nodiscard]] u32 restores_completed() const noexcept { return restores_completed_; }
    [[nodiscard]] u32 clears_completed() const noexcept { return clears_completed_; }
    [[nodiscard]] const std::string& last_failure() const noexcept { return last_failure_; }

    /// The worlds the frame draws: those whose document records an accepted bake, with the mesh and
    /// overlay flags `navigation_session` (the NavigationService child session) holds.
    void overlay_worlds(CyServiceSession navigation_session,
                        std::vector<NavOverlayWorld>& out) const;

private:
    enum class JobKind : u8 { Restore, Overlay, Update, Clear };
    struct Job {
        JobKind kind = JobKind::Update;
        u32 world = 0;
        std::vector<u8> payload;
    };
    struct Query {
        bool flow = false;
        u32 world = 0;
        Aabb region;
    };
    struct WorldOverlay {
        std::vector<Vec3> path;
        NavFlowOverlay flow;
        bool has_flow = false;
    };

    void queue_update(u32 world, const Aabb& region);
    void queue_world_changes(const std::vector<NavWorldRecord>& next);
    void queue_clear(u32 world);
    void finish_internal(const CyServiceEvent& event);
    void record_query(const Query& query, Span<const u8> payload);

    abi::EditorServiceBackend* service_;
    CyServiceSession session_;
    std::vector<NavNodeState> nodes_;
    std::vector<NavWorldRecord> worlds_;
    std::deque<Job> queue_;
    Job current_;
    u64 in_flight_ = 0;
    bool backoff_ = false;
    u64 next_request_ = 1;
    std::map<u64, Query> queries_;
    std::map<u32, WorldOverlay> overlays_;
    NavUpdateResult last_update_;
    u32 updates_completed_ = 0;
    u32 restores_completed_ = 0;
    u32 clears_completed_ = 0;
    std::string last_failure_;
};

/// The navigation half of an editor frame's overlays: every world whose document records a bake,
/// drawn with its recorded overlay flags through the frame's own view and eye. `main.cpp`'s
/// `draw_frame_overlays` calls exactly this for an editor camera's frame; the image test over the
/// known test map (`test_nav_runtime.cpp`) drives the same call. `worlds` is the caller's scratch.
void draw_editor_navigation(const NavigationDriver& driver, CyServiceSession navigation_session,
                            const NavOverlayView& view, const Canvas& canvas,
                            std::vector<NavOverlayWorld>& worlds);

/// Receives an editor event the runtime forwards, for `drain_service_events`.
using ServiceEventSink = void (*)(void* user, const CyServiceEvent& event);

/// One frame of service work: lets `driver` submit its next request, then polls at most `limit`
/// events. The runtime's own events are consumed by the driver; every other event is handed to
/// `forward`. The limit spreads a bake (one tile per poll) over frames. Returns the events polled.
u32 drain_service_events(abi::EditorServiceBackend& service, CyServiceSession session,
                         NavigationDriver& driver, u32 limit, ServiceEventSink forward, void* user);

}  // namespace cy::sample::editor_window
