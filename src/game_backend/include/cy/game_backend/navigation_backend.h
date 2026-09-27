// SPDX-License-Identifier: MIT
// cy/game_backend/navigation_backend.h — the `navigation` adapter behind ABI 1.3.
// `add-swift-game-api`.
//
// OWNER: implementer B. The contract is cy/abi/game/navigation.h and design.md; this is the half
// that knows `cy::navigation`.
//
// TWO JOBS, AND THE SECOND IS THE LARGER.
//
//   1. PATH QUERIES. `nav_find_path` is `find_path` plus `straighten` over the mesh bound to the
//      request's navigation world, with per-call scratch so it is safe from a job worker. The
//      asynchronous pair goes through that world's `PathQueue`, whose delivery tick is a function
//      of the submission tick only — `ai-system`'s "async completion is deterministic". A query id
//      packs the navigation world (high 32 bits) and the queue's own id plus one (low 32 bits), so
//      ids are issued in submission order and zero is never one. A READY path is taken out of the
//      queue on the first poll and held here until a poll has room for it, which is how
//      `nav_poll_path` can refuse a short buffer without losing the path.
//
//   2. AGENTS. `cy::navigation` has the pieces — the `NavAgent` component, `PathQueue`, `Crowd`,
//      `follow_path` — and no system that joins them into "walk there". `update()` is that system,
//      and it owns every agent `nav_agent_configure` made:
//
//        a. drop agents whose entity died or lost `NavAgent`; clear last tick's arrival event;
//        b. read each agent's position from `AgentMotion` when one is bound;
//        c. apply this tick's orders — `move_to` submits a path query (COMPUTING), or arrives at
//           once when already within the arrival distance; `stop` is IDLE — in entity order;
//        d. advance every navigation world's queue for the current tick;
//        e. take each COMPUTING agent's READY path (FOLLOWING, or FAILED with the event);
//        f. steer FOLLOWING agents along their path through the world's crowd (avoidance), and mark
//           those within the arrival distance of their last point ARRIVED with the event;
//        g. move them: `AgentMotion::drive` when bound, the agent's own position otherwise.
//
//      Orders take effect in (c), never at the call, so the order in which one tick's scripts issue
//      them cannot change the outcome; every loop runs in entity order and the crowd's own solver
//      is deterministic. The event flag set in (e) or (f) is cleared by the next update's (a), so
//      it is visible for exactly one tick.
//
//      The adapter is THE agent system for the entities it configured: a host that also runs
//      `NavWorlds::update` over them would re-path idle agents behind its back. It does call
//      `PathQueue::update` for the worlds it has used; a host that runs other queries through the
//      same queues gets them delivered by that call too.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/navigation.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/components.h>
#include <cy/navigation/crowd.h>
#include <cy/navigation/query.h>

namespace cy::game_backend {

/// Where an agent's motion comes from and goes to: a character controller, or a transform. With
/// none bound, the adapter moves `NavAgent::position` itself and the host reads it from there.
class AgentMotion {
public:
    virtual ~AgentMotion() = default;
    /// The entity's current position. False when it has none (the agent's stored one is kept).
    [[nodiscard]] virtual bool position(CyEntity entity, Vec3& out) const noexcept = 0;
    /// Move the entity with `velocity` for `dt` seconds. Returns where it ended up — which a
    /// controller that collides may not be where the velocity pointed.
    [[nodiscard]] virtual Vec3 drive(CyEntity entity, Vec3 from, Vec3 velocity,
                                     f32 dt) noexcept = 0;
};

/// Implements `cy::abi::game::NavigationBackend`, and is the agent system for configured agents.
class NavigationAdapter final : public abi::game::NavigationBackend {
public:
    /// Tunables that are the host's rather than the ABI's.
    struct Config {
        /// Half-extents a path endpoint is snapped to the mesh within, when a request says zero.
        Vec3 default_extents{2.0F, 4.0F, 2.0F};
        /// The crowd's neighbour grid, in metres.
        f32 crowd_cell_size = 4.0F;
    };

    /// `components` must be registered in `world`; `worlds` pairs each navigation world id with its
    /// mesh and queue; `clock` is the host's (`host.game.clock`), read for the tick a query is
    /// submitted and delivered on. All are borrowed and must outlive the adapter.
    NavigationAdapter(Allocator& allocator, ecs::World& world,
                      const navigation::NavComponents& components, navigation::NavWorlds& worlds,
                      const abi::game::GameClock& clock, Config config) noexcept;
    NavigationAdapter(Allocator& allocator, ecs::World& world,
                      const navigation::NavComponents& components, navigation::NavWorlds& worlds,
                      const abi::game::GameClock& clock) noexcept;
    ~NavigationAdapter() override = default;

    NavigationAdapter(const NavigationAdapter&) = delete;
    NavigationAdapter& operator=(const NavigationAdapter&) = delete;
    NavigationAdapter(NavigationAdapter&&) = delete;
    NavigationAdapter& operator=(NavigationAdapter&&) = delete;

    /// Bind (or, with null, unbind) where agents' motion comes from. Borrowed.
    void set_motion(AgentMotion* motion) noexcept { motion_ = motion; }

    /// The fixed step's navigation update, for `clock.tick`, over `dt` seconds. Once per tick,
    /// after the tick's scripts and before physics, on the game thread. See the header for its
    /// steps.
    [[nodiscard]] Status update(f32 dt) noexcept;

    /// Agents this adapter is driving.
    [[nodiscard]] u32 agent_count() const noexcept { return static_cast<u32>(agents_.size()); }

    CyResult find_path(const CyNavPathRequest& request, Span<f32> out_points_xyz,
                       CyNavPathResult& out_result) noexcept override;
    CyResult request_path(const CyNavPathRequest& request, CyNavQuery& out_query) noexcept override;
    CyResult poll_path(CyNavQuery query, Span<f32> out_points_xyz,
                       CyNavPathResult& out_result) noexcept override;
    CyResult cancel_path(CyNavQuery query) noexcept override;
    CyResult agent_configure(CyEntity entity, const CyNavAgentParams& params,
                             bool& out_structural) noexcept override;
    CyResult agent_move_to(CyEntity entity, const f32* target_xyz) noexcept override;
    CyResult agent_stop(CyEntity entity) noexcept override;
    CyResult agent_state(CyEntity entity, CyNavAgentState& out_state) const noexcept override;

private:
    enum class Order : u8 { None, Move, Stop };

    /// One configured agent. The path lives here, not in the component — see
    /// cy/navigation/components.h on why `NavAgent` holds no array.
    struct Agent {
        explicit Agent(Allocator& allocator) noexcept : path(allocator) {}
        u64 entity = 0;
        u32 world = 0;
        navigation::CrowdAgentId crowd = navigation::kInvalidCrowdAgent;
        Order order = Order::None;
        Vec3 order_target;
        Array<navigation::PathPoint> path;
    };

    /// A script's asynchronous query, and its path once taken out of the queue.
    struct ScriptQuery {
        explicit ScriptQuery(Allocator& allocator) noexcept : points(allocator) {}
        CyNavQuery id = CY_NAV_QUERY_NULL;
        /// The request's endpoints, which `straighten` needs and the queue does not hand back.
        Vec3 start;
        Vec3 end;
        bool cancelled = false;
        bool taken = false;
        CyNavPathResult result{};
        Array<f32> points;
    };

    struct WorldCrowd {
        u32 world = 0;
        navigation::Crowd crowd;
    };

    [[nodiscard]] Agent* find_agent(u64 entity) noexcept;
    [[nodiscard]] const Agent* find_agent(u64 entity) const noexcept;
    [[nodiscard]] ScriptQuery* find_query(CyNavQuery id) noexcept;
    /// The world's crowd, created on first use; null only when out of memory.
    [[nodiscard]] navigation::Crowd* crowd_for(u32 world) noexcept;
    /// The world's crowd if it has one. Never creates, so never moves the others.
    [[nodiscard]] navigation::Crowd* find_crowd(u32 world) noexcept;
    [[nodiscard]] const navigation::Crowd* find_crowd(u32 world) const noexcept;
    [[nodiscard]] Status note_world(u32 world) noexcept;
    [[nodiscard]] Vec3 extents_of(const CyNavPathRequest& request) const noexcept;
    [[nodiscard]] CyResult take_ready(ScriptQuery& query) noexcept;
    void remove_agent(usize index) noexcept;

    // agent_configure()'s steps.
    void leave_world(Agent& agent, navigation::NavAgent& value) noexcept;
    [[nodiscard]] CyResult store_component(ecs::Entity entity, const navigation::NavAgent& value,
                                           bool& out_structural) noexcept;
    [[nodiscard]] Agent* add_record(u64 entity) noexcept;
    [[nodiscard]] static CyResult join_crowd(Agent& agent, navigation::Crowd& crowd,
                                             const navigation::NavAgent& value) noexcept;

    // update()'s steps.
    void prune_and_refresh() noexcept;
    void apply_orders(u32 tick) noexcept;
    void advance_queues(u32 tick) noexcept;
    void take_paths() noexcept;
    void steer(f32 dt) noexcept;
    void move(f32 dt) noexcept;

    Allocator* allocator_;
    ecs::World* world_;
    navigation::NavComponents components_;
    navigation::NavWorlds* worlds_;
    const abi::game::GameClock* clock_;
    Config config_;
    AgentMotion* motion_ = nullptr;
    /// Sorted by entity bits, so every pass over agents is in entity order.
    Array<Agent> agents_;
    /// Sorted by id, which is submission order within a world.
    Array<ScriptQuery> queries_;
    /// Both sorted by world id, so worlds are always visited in the same order.
    Array<u32> used_worlds_;
    Array<WorldCrowd> crowds_;
};

/// Bind `adapter` as `host`'s navigation backend (`host.game.navigation`), or unbind with null. The
/// one place an embedder wires the navigation service; the adapter must outlive the binding.
void bind(cy::abi::Host& host, NavigationAdapter* adapter) noexcept;

}  // namespace cy::game_backend
