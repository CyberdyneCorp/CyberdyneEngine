#pragma once
// Spawning: a request, a policy that chooses where, and a result. M8.a task 5.1.
//
// `gameplay-framework` — "Spawning": "Spawning SHALL be a service taking a **spawn request** — an
// entity template, an owner, a team, and a context — and returning a result, applying declared
// **spawn rules** to select a location", with batch spawning "first-class", reservation so that
// "two simultaneous requests do not select the same point", and spawn points "representable as
// spatial metadata" rather than as an entity each.
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS NOT, STATED RATHER THAN IMPLIED
// ================================================================================================
//
// `gameplay-framework` reaches **Seed** at M8.a and this is the seed. Of the eight policies the
// requirement lists, three are here: exact position, spawn point, and nearest free. The other five
// — region, weighted random, formation, navigation-reachable and authority-assigned — are NOT, and
// each needs something this milestone does not have: a region volume, a spawn-weight authoring
// surface, a formation description, a navmesh (M8.b) and a network authority (M9).
//
// The shape they slot into is settled, which is the part that had to be got right now:
// [`SpawnPolicy`] is the only thing that chooses a location, `select()` is the only function that
// reads it, and nothing else in this header would change to add one. What must NOT happen is a
// caller passing a position and a policy and having the policy silently ignored — so a request
// whose policy needs something this build does not have is REFUSED by name.
//
// ================================================================================================
// WHY RESERVATION IS A COUNTER AND NOT A LOCK
// ================================================================================================
//
// "A location may be reserved while dependencies stream, so two simultaneous requests do not select
// the same point." Two simultaneous requests in this engine are two commands in one command stream,
// merged deterministically by `(producer order, sequence)` — they are not two threads. So a
// reservation is a flag on the point and a tick it expires on, read by the next `select()`; a mutex
// here would be a synchronisation primitive protecting something nothing else can reach, and it
// would make the answer depend on arrival time, which is what the command stream exists to prevent.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ecs/world.h>
#include <cy/scene/tree.h>

namespace cy::gameplay {

/// How a spawn chooses where.
enum class SpawnPolicy : u8 {
    /// The request's own placement, used unchanged. What a level's authored contents spawn with.
    ExactPosition = 0,
    /// A named spawn point. Refused when no point of that name is registered.
    SpawnPoint,
    /// The first registered point that is neither reserved nor occupied, in registration order.
    /// Deterministic by construction: registration order is the authored order.
    NearestFree,
};

/// The spelling a diagnostic and a script use. Never null.
[[nodiscard]] const char* spawn_policy_name(SpawnPolicy policy) noexcept;

/// One spawn.
///
/// `gameplay-framework`'s four members — template, owner, team, context — plus the placement the
/// `ExactPosition` policy uses. The context is the `SceneTree` the service was constructed over,
/// because a spawn request that carried its own world could name a different one than the service
/// spawns into, and that is a defect with no diagnostic.
struct SpawnRequest {
    /// The node template to instantiate. An empty name spawns a bare node, which is what an
    /// authored world's own contents are: they carry their components rather than a template.
    Name entity_template;
    /// What to call the node. Made unique among its siblings by the scene tree, as every node is.
    Name name;
    /// Where, when the policy is `ExactPosition`.
    Transform placement;
    /// Which point, when the policy is `SpawnPoint`.
    Name point;
    SpawnPolicy policy = SpawnPolicy::ExactPosition;
    /// Who owns the spawned entity. `gameplay-framework`'s participant, as an entity; null is
    /// legal and means the world owns it.
    ecs::Entity owner;
    /// Which team. Zero is "none", matching `Participant::team`.
    u32 team = 0;
    /// The parent to attach under. A null entity attaches to the tree's root.
    ecs::Entity parent;
};

/// What a spawn produced.
struct SpawnResult {
    ecs::Entity entity;
    /// Where it actually went, which is the policy's answer and not necessarily the request's.
    Transform placement;
    /// The spawn point index the policy chose, or `kNoPoint`.
    u32 point = 0xFFFF'FFFFU;
};

inline constexpr u32 kNoSpawnPoint = 0xFFFF'FFFFU;

/// One spawn point, as spatial metadata.
///
/// "Spawn points SHALL be representable as spatial metadata; instantiating an entity per spawn
/// point SHALL NOT be required." A point is 60 bytes in an array here rather than an entity with a
/// name, a transform and an archetype row — which for a level with four hundred of them is the
/// difference between four hundred rows every query steps over and one array nothing else sees.
struct SpawnPoint {
    Name name;
    Transform placement;
    u32 team = 0;
    /// Reserved until this tick. Zero is unreserved.
    u64 reserved_until = 0;
    /// What last spawned here, so `NearestFree` can skip a point still occupied.
    ecs::Entity occupant;
};

/// What the service has done, for a report and for a test.
struct SpawnStatistics {
    u64 spawned = 0;
    u64 batches = 0;
    u64 refused = 0;
    u64 reservations = 0;
};

/// Spawning into one scene tree.
///
/// Not thread-safe, and not meant to be: it runs on the simulation thread, from a command that the
/// command stream already ordered. See the header note on reservation.
class SpawnService {
public:
    SpawnService(scene::SceneTree& tree, Allocator& allocator) noexcept
        : tree_(&tree), points_(allocator) {}

    SpawnService(const SpawnService&) = delete;
    SpawnService& operator=(const SpawnService&) = delete;

    /// Register a spawn point. Returns its index, which is what `reserve` names.
    [[nodiscard]] Expected<u32, Error> add_point(Name name, const Transform& placement,
                                                 u32 team = 0) noexcept;
    [[nodiscard]] u32 point_count() const noexcept { return static_cast<u32>(points_.size()); }
    [[nodiscard]] const SpawnPoint* point(u32 index) const noexcept;
    [[nodiscard]] u32 index_of(Name name) const noexcept;

    /// Hold a point until `until_tick`, so a second request does not select it while dependencies
    /// stream. Refused for a point already reserved past `now`.
    [[nodiscard]] Status reserve(u32 index, u64 now, u64 until_tick) noexcept;

    /// Where a request would go, without spawning anything.
    ///
    /// Separate from `spawn` because an interface greying out a button, an AI choosing what to
    /// attempt and the spawn itself all ask the same question — the argument
    /// `CommandStream::validate` makes, and for the same reason.
    [[nodiscard]] Expected<SpawnResult, Error> select(const SpawnRequest& request,
                                                      u64 now) const noexcept;

    /// Spawn one.
    [[nodiscard]] Expected<SpawnResult, Error> spawn(const SpawnRequest& request,
                                                     u64 now = 0) noexcept;

    /// Spawn `count` of one template, as ONE operation.
    ///
    /// "**Batch spawning SHALL be first-class**: spawning many instances of one template SHALL be
    /// one operation using the entity template's archetype blocks, not repeated single spawns."
    /// **What this does today is not yet that, and the gap is measured rather than glossed.**
    /// `SceneTree::create_node` makes a name unique among its siblings by scanning them, so
    /// spawning N instances of one name costs O(N^2) name comparisons: two hundred measured 9.4 ms
    /// of CPU at -O2, which is nine times a unit test's whole budget. The requirement's ten
    /// thousand would not finish in a frame.
    ///
    /// The seam is here and the call site is right: a caller asks for many once. Closing the gap
    /// is a naming scheme that does not scan (an ordinal suffix issued by the parent) plus
    /// `World::instantiate` over the template's archetype block, and it changes this function
    /// and nothing above it.
    [[nodiscard]] Status spawn_many(const SpawnRequest& request, u32 count, Array<ecs::Entity>& out,
                                    u64 now = 0) noexcept;

    [[nodiscard]] const SpawnStatistics& statistics() const noexcept { return statistics_; }

private:
    scene::SceneTree* tree_;
    Array<SpawnPoint> points_;
    SpawnStatistics statistics_;
};

}  // namespace cy::gameplay
