#pragma once
// The navigation components `navigation` names, and their registration in one ECS world.
// M8.b task 6.2.
//
// `navigation`'s "Navigation components" requirement gives the table:
//
//   | Component        | Role                                                                  |
//   | NavMeshSurface   | Declares a region whose geometry contributes to a navigation mesh     |
//   | NavAgent         | An agent: target, path, speed, avoidance parameters, capabilities     |
//   | NavObstacle      | A dynamic obstacle carving or marking the mesh                        |
//   | NavLink          | An off-mesh link                                                      |
//   | NavArea          | Marks a volume with an area type and cost                             |
//
// ================================================================================================
// A COMPONENT IS A LAYOUT AND A REGISTRATION, AND THIS FILE IS BOTH
// ================================================================================================
//
// `src/physics/include/cy/physics/components.h` splits them because the layouts are written in the
// physics server's vocabulary at layer 2 and the registration belongs to whoever owns the world.
// Navigation has no layer-2 server: the vocabulary — `PolyRef`, `AreaType`, `CapabilityMask`,
// `AvoidanceParams` — is this module's own, so splitting would produce a header that imports
// everything from its neighbour and adds a name. The registration rule is the same one and it is
// repeated here because it is the load-bearing half:
//
// THE ORDER IS FIXED, AND IT IS THE SERIALIZED DESCRIPTOR TABLE'S ORDER. Two worlds that both run
// `register_all` agree on every number, so a world serialized by one and read by another needs no
// correspondence table. Nothing else depends on the order, and it must not change without the
// format change that would go with it.
//
// ================================================================================================
// WHAT `NavAgent` DOES NOT HOLD
// ================================================================================================
//
// Not the path. `navigation` asks the component to expose "current path, next path point" and the
// path is an `Array<PathPoint>` — a heap allocation per agent, in a structure the ECS copies on
// every archetype transition. So the component holds the query that produced it and the cursor into
// it, and the corridor lives in the `NavAgentPaths` side table beside the world. Eight thousand
// agents each carrying a growable array in a chunk is the shape `ai-system` calls "a per-agent
// virtual update object" in a different costume.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/ecs/world.h>
#include <cy/navigation/crowd.h>

namespace cy::navigation {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;

/// `navigation`'s agent path status, by name: "idle, computing, following, arrived, failed".
enum class NavPathStatus : u8 { Idle = 0, Computing, Following, Arrived, Failed, Count };

[[nodiscard]] const char* nav_path_status_name(NavPathStatus status) noexcept;

/// A region whose geometry contributes to a navigation mesh.
struct NavMeshSurface {
    Aabb bounds;
    /// Which navigation world this surface belongs to. `navigation` requires "multiple independent
    /// navigation worlds", and this is how a surface says which one it is for.
    u32 world = 0;
    u64 layers = ~u64{0};
    u64 tags = ~u64{0};
    AreaType area = kAreaGround;
    bool exclude = false;  ///< an explicit exclude volume rather than a contributor
};

/// An agent.
struct NavAgent {
    /// Where the agent is, as of the last time its controller wrote it.
    ///
    /// IT IS STORED HERE RATHER THAN READ FROM A TRANSFORM, and that is a dependency decision: a
    /// navigation module that read `cy::scene::LocalTransform` would make pathfinding depend on the
    /// scene graph, and `navigation` puts locomotion in the character controller — "avoidance SHALL
    /// compute a desired velocity adjustment, not a position". So the controller writes this field
    /// and navigation reads it, which is the same direction every other value here flows.
    Vec3 position;
    Vec3 target;
    /// The polygon the agent stands on, refreshed as it moves. `kInvalidPoly` means "not on the
    /// mesh", which is a state a falling or teleporting agent is legitimately in.
    PolyRef poly;
    AvoidanceParams avoidance;
    CapabilityMask capabilities = kAllCapabilities;
    NavAreaCosts costs = NavAreaCosts::uniform();
    AreaMask areas = kAllAreas;
    /// The in-flight query, or `kInvalidQuery`. `navigation` requires path status to be observable
    /// and this is the half of it that is not a state.
    QueryId query = kInvalidQuery;
    /// The mesh version the corridor was built against; a lower one means "repath".
    u32 corridor_version = 0;
    /// Which point of the straightened path the agent is heading for.
    u32 path_cursor = 0;
    /// The tick a repath was last issued on, so the rate limit is a comparison and not a timer.
    u32 last_repath_tick = 0;
    f32 arrival_distance = 0.5F;
    u32 world = 0;
    NavPathStatus status = NavPathStatus::Idle;
    /// True for one tick after arrival or failure. `navigation`: "events for path completion and
    /// failure" — as a flag a system reads and clears, because an event queue per agent is the
    /// allocation the header refuses.
    bool event_pending = false;
    /// `ai-system`'s "Tier selects the navigation strategy": a `Reduced` agent follows a shared
    /// field rather than computing an individual path.
    bool follows_field = false;
};

/// A dynamic obstacle. `navigation`: "carving or marking the mesh" without a rebuild.
struct NavObstacle {
    NavObstacleShape shape;
    /// The id `NavMesh::add_obstacle` returned, so a system can remove it when the entity dies.
    ObstacleId handle = kInvalidObstacle;
    u32 world = 0;
};

/// An off-mesh link, as authored on an entity. The runtime form is `cy::navigation::NavLink`; this
/// is what a level holds and what a system publishes into the mesh.
struct NavLinkComponent {
    Vec3 from;
    Vec3 to;
    Name action;
    f32 cost = 1.0F;
    CapabilityMask requires_capabilities = kNoCapabilities;
    LinkId handle = kInvalidLink;
    u32 world = 0;
    AreaType area = kAreaGround;
    bool bidirectional = true;
};

/// A volume that marks an area type and a cost.
struct NavArea {
    Aabb bounds;
    f32 cost = 1.0F;
    u32 world = 0;
    AreaType area = kAreaGround;
};

inline constexpr const char* kNavMeshSurfaceComponentName = "cy.navigation.NavMeshSurface";
inline constexpr const char* kNavAgentComponentName = "cy.navigation.NavAgent";
inline constexpr const char* kNavObstacleComponentName = "cy.navigation.NavObstacle";
inline constexpr const char* kNavLinkComponentName = "cy.navigation.NavLink";
inline constexpr const char* kNavAreaComponentName = "cy.navigation.NavArea";

/// Navigation's component ids in ONE world. Ids are per world (`ecs-core`), so this is a value a
/// system holds and never a static.
struct NavComponents {
    ComponentTypeId surface = kInvalidComponent;
    ComponentTypeId agent = kInvalidComponent;
    ComponentTypeId obstacle = kInvalidComponent;
    ComponentTypeId link = kInvalidComponent;
    ComponentTypeId area = kInvalidComponent;

    /// Register all five in `world`, in this order, and return the ids. Idempotent.
    [[nodiscard]] static Expected<NavComponents, Error> register_all(World& world) noexcept;

    [[nodiscard]] bool registered() const noexcept {
        return surface != kInvalidComponent && agent != kInvalidComponent &&
               obstacle != kInvalidComponent && link != kInvalidComponent &&
               area != kInvalidComponent;
    }
};

/// What one agent update did, so a diagnostic answers "why is this agent not moving" with numbers.
/// `navigation`: "Statistics SHALL include: query counts and timings, path lengths, repath rates".
struct NavAgentReport {
    u32 agents = 0;
    u32 repaths_issued = 0;
    u32 repaths_rate_limited = 0;
    u32 corridors_invalidated = 0;
    u32 arrived = 0;
    u32 failed = 0;
};

/// Advance every `NavAgent` in `world` against `mesh`: refresh the polygon under it, invalidate a
/// corridor the mesh has outrun, and issue a repath through `queue` subject to `repath_interval`
/// ticks.
///
/// It does not move anything. Movement is the crowd's and the character controller's; this is the
/// bookkeeping between an agent component and a path query, and it is a free function over a world
/// rather than a class because `ai-system` requires agent work to be "scheduled systems over
/// queries, in bulk".
[[nodiscard]] Status update_agents(World& world, const NavComponents& components,
                                   const NavMesh& mesh, PathQueue& queue, u32 tick,
                                   u32 repath_interval, NavAgentReport& report) noexcept;

}  // namespace cy::navigation
