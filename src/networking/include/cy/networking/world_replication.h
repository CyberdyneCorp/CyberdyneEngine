// SPDX-License-Identifier: MIT
#pragma once
// Replication over an `ecs::World`: the server's replicator and the client's replica. Issue #82.
//
// ================================================================================================
// WHAT CHANGED FROM M9: THERE IS A WORLD IN THE SIGNATURE
// ================================================================================================
//
// M9's replication addressed instances as bytes — `SnapshotWriter::add_update()` takes a pointer —
// and `src/networking/README.md` said plainly that nothing walked a world. `WorldReplicator` is
// that walk. It reads the world's own chunk columns, its own change versions and its own entities,
// and feeds them through the same `SchemaSet`, `PeerBaseline`, `SnapshotWriter`, `InterestSet` and
// `PriorityScheduler` M9 tested in isolation. `ReplicaWorld` is the other end: it decodes into a
// client world's components, creating and destroying the client's entities as spawns and despawns
// arrive.
//
// ================================================================================================
// WHAT MAKES AN ENTITY REPLICATED
// ================================================================================================
//
// One component, `NetworkIdentity` — the network id, the prefab, the owner — added by
// `WorldReplicator::replicate()`. Nothing else about the entity changes, so every query written
// without it still matches the entity, which is `networking-and-replication`'s "making an entity
// replicated adds components without changing existing queries". Which of its components travel
// is the schema set's decision: a component whose reflected type has a compiled schema is sent,
// and one without is not.
//
// ================================================================================================
// THE WORLD'S CHANGE VERSIONS DECIDE WHAT IS EVEN LOOKED AT
// ================================================================================================
//
// A chunk stamps each component column with the world version of its last write (`get_mut`, a
// query's `write`, and any row added). For every peer and every known entity the replicator keeps
// the world version at which the peer's acknowledged baseline was taken. A chunk whose replicated
// columns are all older than that is skipped without comparing a byte: the peer already has
// exactly what the chunk holds. So an unchanged component sends no bytes **and costs no
// comparison**, and the count of chunks skipped is reported, which is how a test tells the two
// apart. Change detection is chunk-granular by `ecs-core`'s specification, so a chunk that does
// change is compared entity by entity against the baseline, and only what differs is written.
//
// ================================================================================================
// ACKNOWLEDGEMENT IS EXACT, NOT CUMULATIVE
// ================================================================================================
//
// Snapshots are unreliable, so a client that acknowledges snapshot 12 has not necessarily received
// snapshot 11. The client therefore acknowledges each snapshot it applied — the newest and a
// 32-bit mask of the ones before — and the replicator promotes a baseline, confirms a spawn or
// retires a despawn only for the snapshot that actually carried it
// (`PeerBaseline::acknowledge_exactly()`). Promoting everything sent at or before the newest
// acknowledgement would take a lost update for delivered, and the client would silently keep a
// value the server believes it replaced.
//
// Spawns and despawns are repeated in every snapshot until one that carries them is acknowledged,
// so they need no reliable channel and cannot arrive out of order with the state they describe.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>
#include <cy/ecs/world.h>
#include <cy/networking/authority.h>
#include <cy/networking/interest.h>
#include <cy/networking/replication.h>
#include <cy/networking/scheduler.h>
#include <cy/networking/schema.h>

namespace cy::net {

/// The one component that makes an entity replicated. Engine-structural, so it is a built-in
/// component rather than a reflected type: it never travels as state, it IS the addressing.
struct NetworkIdentity {
    u64 id = 0;
    u32 prefab = 0;
    u32 owner_slot = 0;
    u32 owner_generation = 0;
    u32 reserved = 0;

    [[nodiscard]] NetworkId network_id() const noexcept {
        return NetworkId::make(static_cast<u16>(id >> 48), id);
    }
    [[nodiscard]] PeerId owner() const noexcept {
        return PeerId::make(owner_slot, owner_generation);
    }
};

/// The built-in component's registered name, which is how a world that already has it finds it.
inline constexpr const char* kNetworkIdentityComponent = "cy.net.NetworkIdentity";

/// Register `NetworkIdentity` in `world`, or find it when it already is.
[[nodiscard]] Expected<ecs::ComponentTypeId, Error> network_identity_component(
    ecs::World& world) noexcept;

/// Which world component each compiled schema reads. Resolved by the schema's reflected type, so a
/// server and a client that register components in different orders still agree.
[[nodiscard]] Status bind_schemas(const ecs::World& world, const SchemaSet& schemas,
                                  Array<ecs::ComponentTypeId>& out) noexcept;

/// Derive a replication schema from a reflected type's `Replicated(encoder, parameters, condition)`
/// attributes: every field that declares one is replicated, keyed by its stable `FieldId`, and
/// every other field — `Transient` ones included — is not. The encoder names are `raw`,
/// `quantised`, `quantised-vector`, `quaternion`, `bitfield` and `dictionary`; parameters are
/// `bits=N`, and a quantised field takes its interval from its own `Range`. The condition is
/// `always`, `on-change`, `owner-only`, `except-owner` or `owner-and-observers` (everyone, on
/// change). `fields` is where the derived entries live; it must outlive the schema.
[[nodiscard]] Expected<ReplicationSchema, SchemaRejection> schema_from_reflection(
    const reflect::TypeInfo& type, Array<FieldSchema>& fields) noexcept;

/// Fills a relevance subject for one replicated entity: its position, cell, team and importance.
/// The replicator owns the network id and the owner; the callback owns the game's notion of where
/// the entity is.
using SubjectFn = void (*)(void* user, const ecs::World& world, ecs::Entity entity,
                           RelevanceSubject& subject) noexcept;

/// How the replicator decides relevance and priority. The defaults make everything relevant.
struct ReplicatorConfig {
    /// Null: every replicated entity is relevant to every peer.
    SubjectFn subject = nullptr;
    void* subject_user = nullptr;
    PriorityWeights weights{};
    BandTable bands{};
    i64 near_distance_squared = 2500;
    i64 far_distance_squared = 250000;
};

/// What one `build()` did for one peer.
struct ReplicationReport {
    SnapshotId snapshot = 0;
    u32 relevant = 0;
    u32 candidates = 0;
    u32 written = 0;
    u32 spawns = 0;
    u32 despawns = 0;
    u32 updates = 0;
    /// Selected by the scheduler and still left out because the bytes would not fit.
    u32 deferred = 0;
    /// Relevant entities never compared, because every replicated column of their chunk is older
    /// than the world version of the peer's acknowledged baseline: the world's change ticks said
    /// nothing could differ.
    u32 skipped_by_version = 0;
    /// Relevant entities that were not skipped: spawns, and entities in a chunk written since.
    u32 examined = 0;
    /// Of those, the known entities whose components were compared against a baseline.
    u32 entities_compared = 0;
    u32 bytes = 0;
    u32 budget = 0;
    SchedulerReport scheduler{};
};

/// The server's half.
class WorldReplicator {
public:
    WorldReplicator(Allocator& allocator, ecs::World& world, const SchemaSet& schemas,
                    const ReplicatorConfig& config = {}) noexcept;
    ~WorldReplicator();

    WorldReplicator(const WorldReplicator&) = delete;
    WorldReplicator& operator=(const WorldReplicator&) = delete;

    /// Register `NetworkIdentity` and resolve every schema's component.
    [[nodiscard]] Status initialize() noexcept;

    /// Make `entity` replicated. Mints its network id; the entity keeps every component it had.
    [[nodiscard]] Expected<NetworkId, Error> replicate(ecs::Entity entity, u32 prefab,
                                                       PeerId owner) noexcept;

    [[nodiscard]] Status add_peer(PeerId peer) noexcept;
    void remove_peer(PeerId peer) noexcept;
    [[nodiscard]] bool has_peer(PeerId peer) const noexcept;

    /// A peer's viewpoint and interest cells. Used only with a `SubjectFn`.
    [[nodiscard]] Status set_interest(PeerId peer, const PeerInterest& interest,
                                      Span<const CellId> cells) noexcept;

    /// Walk the world once for this tick: every replicated entity, its chunk and its columns.
    /// Every peer's `build()` for the tick reads what this found.
    [[nodiscard]] Status gather(u64 tick) noexcept;

    /// One snapshot for `peer`, within `budget`. The bytes are valid until the next `build()`.
    [[nodiscard]] Expected<Span<const u8>, Error> build(PeerId peer, u64 tick,
                                                        const BandwidthBudget& budget) noexcept;

    /// The client's acknowledgement: `newest` and, bit `i`, snapshot `newest - 1 - i`.
    void acknowledge(PeerId peer, SnapshotId newest, u32 previous) noexcept;

    [[nodiscard]] const ReplicationReport& last_report(PeerId peer) const noexcept;
    [[nodiscard]] u32 replicated_count() const noexcept {
        return static_cast<u32>(records_.size());
    }
    [[nodiscard]] ecs::ComponentTypeId identity_component() const noexcept { return identity_; }
    /// Entities in the relevance index. Equal to `replicated_count()` after a `gather()` with a
    /// `SubjectFn`: the dead are removed whether or not a peer ever knew them.
    [[nodiscard]] u32 relevance_subjects() const noexcept { return interest_.live_count(); }
    [[nodiscard]] const SchemaSet& schemas() const noexcept { return *schemas_; }
    [[nodiscard]] ecs::World& world() const noexcept { return *world_; }
    /// The local entity behind a network id, as of the last `gather()`.
    [[nodiscard]] ecs::Entity entity_of(NetworkId id) const noexcept;

private:
    struct Column;
    struct Record;
    struct Peer;
    struct Dirty;
    struct SnapshotRecord;

    [[nodiscard]] Peer* find(PeerId peer) noexcept;
    [[nodiscard]] const Peer* find(PeerId peer) const noexcept;
    [[nodiscard]] Status gather_chunk(void* query_chunk) noexcept;
    [[nodiscard]] Status place_subjects() noexcept;
    [[nodiscard]] Status collect_relevant(Peer& peer, Array<Candidate>& out) noexcept;
    [[nodiscard]] Status retire_unknown(Peer& peer, Span<const NetworkId> left) noexcept;
    [[nodiscard]] Status classify(Peer& peer, Span<const Candidate> relevant, u64 tick) noexcept;
    [[nodiscard]] Status classify_one(Peer& peer, const Candidate& candidate, u32 index,
                                      u64 tick) noexcept;
    /// Append one change mask per schema to the peer's pool; true when any is non-zero.
    [[nodiscard]] bool compute_masks(Peer& peer, const Record& record) noexcept;
    [[nodiscard]] bool chunk_is_clean(const Record& record, u64 synced_version) const noexcept;
    [[nodiscard]] Status write(Peer& peer, u64 tick, const BandwidthBudget& budget) noexcept;
    [[nodiscard]] Status write_despawns(Peer& peer, SnapshotRecord& record, u64 budget_bits,
                                        u64& used) noexcept;
    [[nodiscard]] Status write_entity(Peer& peer, SnapshotRecord& record, const Dirty& dirty,
                                      u64 tick) noexcept;
    [[nodiscard]] Status write_components(Peer& peer, const Dirty& dirty) noexcept;
    static void acknowledge_one(Peer& peer, SnapshotId id) noexcept;
    [[nodiscard]] u32 cost_bits(const Peer& peer, const Dirty& dirty) const noexcept;
    static u32 cost_of(void* user, NetworkId id, u32 precision_percent) noexcept;

    Allocator* allocator_;
    ecs::World* world_;
    const SchemaSet* schemas_;
    ReplicatorConfig config_;
    ecs::ComponentTypeId identity_ = ecs::kInvalidComponent;
    Array<ecs::ComponentTypeId> components_;
    NetworkIdMinter minter_{0};
    Array<Column> columns_;
    Array<Record> records_;
    HashMap<u64, u32> by_id_;
    Array<Peer*> peers_;
    InterestSet interest_;
    PriorityScheduler scheduler_;
    Array<Candidate> relevant_;
    /// The network ids placed in `interest_` by the last gather, so the dead can be removed.
    Array<u64> placed_;
    Array<NetworkId> left_;
    Array<ScheduledEntry> selected_;
    SnapshotWriter writer_;
    Array<u8> out_;
    Peer* costing_ = nullptr;
    u64 gathered_version_ = 0;
    bool initialized_ = false;
};

/// The client's half: snapshots decoded into a client world.
class ReplicaWorld {
public:
    ReplicaWorld(Allocator& allocator, ecs::World& world, const SchemaSet& schemas) noexcept;

    [[nodiscard]] Status initialize() noexcept;

    /// Apply one snapshot. A snapshot older than the newest applied is dropped and not
    /// acknowledged: it would overwrite newer state with older.
    [[nodiscard]] Expected<SnapshotHeader, Error> apply(Span<const u8> snapshot) noexcept;

    /// What to acknowledge: the newest applied snapshot and the 32 before it, bit `i` for
    /// `newest - 1 - i`.
    [[nodiscard]] SnapshotId newest() const noexcept { return newest_; }
    [[nodiscard]] u32 previous_mask() const noexcept { return previous_; }

    [[nodiscard]] ecs::Entity entity_of(NetworkId id) const noexcept;
    [[nodiscard]] u32 entity_count() const noexcept { return static_cast<u32>(bound_.size()); }
    [[nodiscard]] u64 stale_dropped() const noexcept { return stale_; }
    [[nodiscard]] u64 updates_applied() const noexcept { return updates_; }
    [[nodiscard]] ecs::ComponentTypeId identity_component() const noexcept { return identity_; }
    [[nodiscard]] ecs::ComponentTypeId component_of(u32 schema) const noexcept {
        return schema < components_.size() ? components_[schema] : ecs::kInvalidComponent;
    }

private:
    static Status apply_update(void* user, NetworkId id, u32 schema, Span<const u8> decoded,
                               ChangeMask mask) noexcept;
    [[nodiscard]] Expected<ecs::Entity, Error> obtain(NetworkId id) noexcept;
    [[nodiscard]] Status spawn(const SpawnEvent& spawn) noexcept;
    void despawn(NetworkId id) noexcept;
    void note_applied(SnapshotId snapshot) noexcept;

    ecs::World* world_;
    const SchemaSet* schemas_;
    SnapshotReader reader_;
    ecs::ComponentTypeId identity_ = ecs::kInvalidComponent;
    Array<ecs::ComponentTypeId> components_;
    HashMap<u64, u64> bound_;
    Array<SpawnEvent> spawns_;
    Array<NetworkId> despawns_;
    SnapshotId newest_ = 0;
    u32 previous_ = 0;
    u64 stale_ = 0;
    u64 updates_ = 0;
};

/// A deterministic digest of the replicated state of a world: every entity's network id and every
/// replicated component's fields, as `schemas` would put them on the wire — quantised fields
/// compared quantised. Two worlds that agree on what replication can carry have the same digest,
/// whatever order their entities were created in.
[[nodiscard]] u64 replicated_digest(const ecs::World& world, const SchemaSet& schemas) noexcept;

}  // namespace cy::net
