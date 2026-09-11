#pragma once
// Baselines, deltas, change masks, spawning and late join. M9 task 4.3.
//
// ================================================================================================
// A DELTA IS AGAINST WHAT THE PEER ACKNOWLEDGED, NOT AGAINST THE LAST THING SENT
// ================================================================================================
//
// `networking-and-replication` — "Snapshots, baselines and delta encoding": "each peer has an
// acknowledged **baseline**, and subsequent updates encode differences against it", and the server
// keeps "the last acknowledged snapshot, a bounded history of unacknowledged snapshots, and
// per-entity change masks".
//
// The tempting shortcut is to delta against the previous *send*, which is one array instead of two.
// It is wrong the first time a packet is lost: the client decodes a delta against a baseline it
// never received and every field it touches is silently garbage, with no error anywhere. So a
// `PeerBaseline` holds two things per (entity, component) — the bytes the peer has
// **acknowledged**, and the bytes sent since, tagged with the snapshot that carried them. An
// acknowledgement promotes everything at or before that snapshot; a loss simply leaves it
// unpromoted, and the next delta is computed against the older state, which is what the client
// actually has.
//
// ================================================================================================
// AN ACKNOWLEDGEMENT OLDER THAN THE HISTORY IS A FRESH BASELINE, NOT A GUESS
// ================================================================================================
//
// "When a peer's acknowledgement falls outside the retained history, the server SHALL send a fresh
// baseline rather than an undecodable delta." The history is bounded — `kSnapshotHistory` — and a
// peer whose newest acknowledgement is older than the oldest snapshot still retained is marked
// `needs_baseline()`, which makes its next update a full one. Late join is the same mechanism from
// the other end: a peer that has acknowledged nothing needs a baseline by definition.
//
// ================================================================================================
// A SNAPSHOT IS ONE TICK'S WORTH, AND THE WRITER REFUSES TO MIX
// ================================================================================================
//
// "Snapshots SHALL be self-consistent: a delta SHALL never mix state from different simulation
// ticks for the same entity." `SnapshotWriter::open()` takes the tick and every `add_*` is against
// it; there is no way to add an instance sampled at another tick, because the writer never asks for
// one. The tick travels in the header so the client can assert the same thing from its side.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/entity.h>
#include <cy/networking/authority.h>
#include <cy/networking/schema.h>

namespace cy::net {

/// A snapshot's number. Monotonic within a session, and what an acknowledgement names.
using SnapshotId = u32;

/// How many unacknowledged snapshots a peer's history retains before its next update must be a
/// fresh baseline. Sixty-four at 20 Hz is three seconds of loss, which is past any connection worth
/// keeping — and bounded, because an unbounded history is a per-peer memory leak with a network on
/// the other end of it.
inline constexpr u32 kSnapshotHistory = 64;

/// A replicated entity as it arrives on a peer that has never seen it.
struct SpawnEvent {
    NetworkId id;
    /// The networked prefab's stable id. `networking-and-replication`: "the authority spawns from a
    /// networked prefab identified by a stable id, and interested peers instantiate it".
    u32 prefab = 0;
    PeerId owner;
};

/// One peer's acknowledged state, and what has been sent since.
///
/// Keyed by (network id, schema index) folded into one `u64`, because a peer's baseline is asked
/// about once per entity per component per tick and a two-level lookup would be two probes.
class PeerBaseline {
public:
    PeerBaseline(Allocator& allocator, const SchemaSet& schemas) noexcept;

    PeerBaseline(const PeerBaseline&) = delete;
    PeerBaseline& operator=(const PeerBaseline&) = delete;

    /// The acknowledged bytes for one component of one entity, or null when the peer has never
    /// acknowledged any — which makes the next update a full one for that component.
    [[nodiscard]] const void* acknowledged(NetworkId id, u32 schema) const noexcept;

    /// Record what a snapshot carried, so an acknowledgement can promote it.
    [[nodiscard]] Status record_sent(NetworkId id, u32 schema, SnapshotId snapshot,
                                     const void* instance, u32 size) noexcept;

    /// Promote everything sent at or before `snapshot`. `networking-and-replication`'s
    /// "Delta against acknowledged state".
    void acknowledge(SnapshotId snapshot) noexcept;

    /// Drop everything about an entity the peer no longer knows.
    void forget(NetworkId id) noexcept;

    [[nodiscard]] SnapshotId last_acknowledged() const noexcept { return acknowledged_; }
    [[nodiscard]] SnapshotId last_sent() const noexcept { return sent_; }
    void note_sent(SnapshotId snapshot) noexcept { sent_ = snapshot; }

    /// True when the peer's acknowledgement has fallen outside the retained history, or when it has
    /// acknowledged nothing at all. Its next update is a full baseline.
    [[nodiscard]] bool needs_baseline() const noexcept {
        return acknowledged_ == 0 ||
               (sent_ > acknowledged_ && sent_ - acknowledged_ > kSnapshotHistory);
    }

    /// Entities this peer has been told about. Entering relevance adds; leaving removes.
    [[nodiscard]] bool knows(NetworkId id) const noexcept;
    [[nodiscard]] Status note_known(NetworkId id) noexcept;
    void note_unknown(NetworkId id) noexcept;

    [[nodiscard]] u32 tracked_components() const noexcept {
        return static_cast<u32>(entries_.size());
    }
    [[nodiscard]] u32 known_entities() const noexcept { return static_cast<u32>(known_.size()); }

    void clear() noexcept;

private:
    static constexpr u32 kNoEntry = 0xFFFF'FFFFU;

    struct Entry {
        u64 id_value = 0;
        u32 schema = 0;
        /// Where the acknowledged copy lives in `bytes_`, and where the not-yet-acknowledged one
        /// does. Two fixed slots per entry, allocated once: a baseline that reallocated per update
        /// would allocate once per entity per component per tick.
        u32 offset = 0;
        u32 pending_offset = 0;
        u32 size = 0;
        SnapshotId pending_snapshot = 0;
        bool has_acknowledged = false;
        /// The next entry for the same entity. A chain rather than a composite key, because a key
        /// folded from a 64-bit id and a schema index can collide and a collision here aliases two
        /// components' baselines — which decodes successfully and is wrong.
        u32 next = kNoEntry;
    };

    [[nodiscard]] Entry* locate(NetworkId id, u32 schema) noexcept;
    [[nodiscard]] const Entry* locate(NetworkId id, u32 schema) const noexcept;
    [[nodiscard]] Expected<u32, Error> obtain(NetworkId id, u32 schema, u32 size) noexcept;

    const SchemaSet* schemas_;
    Array<Entry> entries_;
    /// Network id to the head of its entry chain.
    HashMap<u64, u32> by_id_;
    /// Entries whose entity was forgotten, reusable by an entity of the same shape. Reuse rather
    /// than removal, because removing from `entries_` would move the indices the chains are made
    /// of.
    Array<u32> free_;
    Array<u8> bytes_;
    HashSet<u64> known_;
    SnapshotId acknowledged_ = 0;
    SnapshotId sent_ = 0;
};

/// What one snapshot cost, as it is built. `networking-and-replication` requires the achieved bits
/// per entity per component to be measurable; this is where the number comes from.
struct SnapshotCost {
    u32 entities = 0;
    u32 components = 0;
    u32 spawns = 0;
    u32 despawns = 0;
    u64 payload_bits = 0;
    /// What the same content would have cost with every field sent. The pair is what the schema
    /// tooling reports as "theoretical and observed".
    u64 theoretical_bits = 0;
};

/// Writes one tick's update for one peer.
class SnapshotWriter {
public:
    SnapshotWriter(Allocator& allocator, const SchemaSet& schemas) noexcept;

    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;

    /// Begin a snapshot. Everything added afterwards is state sampled at `tick`.
    [[nodiscard]] Status open(SnapshotId snapshot, u64 tick, bool full_baseline) noexcept;

    [[nodiscard]] Status add_spawn(const SpawnEvent& spawn) noexcept;
    [[nodiscard]] Status add_despawn(NetworkId id) noexcept;

    /// Add one component of one entity, delta-encoded against `baseline`. Skips the component
    /// entirely when nothing the peer may see has changed, which is "WHEN an entity's health is
    /// unchanged THEN no health data SHALL be transmitted for it".
    ///
    /// Returns the bits this component cost, or zero when it was skipped.
    [[nodiscard]] Expected<u32, Error> add_update(NetworkId id, u32 schema, const void* instance,
                                                  bool owner, PeerBaseline& baseline) noexcept;

    [[nodiscard]] Status close() noexcept;

    [[nodiscard]] Span<const u8> bytes() const noexcept { return payload_.span(); }
    [[nodiscard]] const SnapshotCost& cost() const noexcept { return cost_; }
    [[nodiscard]] u64 tick() const noexcept { return tick_; }

private:
    // ONE bit stream, written in order, with a two-bit tag per record and a terminator — rather
    // than three counted sections. Counted sections would need the counts before the records are
    // known, which for a bit-packed payload means either a second pass or byte alignment between
    // the sections. The tag costs two bits per record and keeps the payload one stream.
    const SchemaSet* schemas_;
    Array<u8> payload_;
    BitWriter writer_;
    SnapshotCost cost_{};
    SnapshotId snapshot_ = 0;
    u64 tick_ = 0;
    bool full_ = false;
    bool open_ = false;
};

/// The record tags of a snapshot payload. Two bits, and `End` is zero so a truncated payload reads
/// as ended rather than as whatever the next bits happen to be.
enum class SnapshotTag : u8 {
    End = 0,
    Spawn = 1,
    Despawn = 2,
    Update = 3,
};

/// What a client does with one decoded component. A function pointer rather than an interface, for
/// the reason `gameplay::CommandStream::RecordSink` gives: a virtual base declared here would be
/// this module's type in every consumer's inheritance list.
using ApplyFn = Status (*)(void* user, NetworkId id, u32 schema, Span<const u8> decoded,
                           ChangeMask mask) noexcept;

/// One snapshot, read back.
struct SnapshotHeader {
    SnapshotId snapshot = 0;
    u64 tick = 0;
    bool full_baseline = false;
    u32 spawns = 0;
    u32 despawns = 0;
    u32 updates = 0;
};

/// Reads a snapshot written by `SnapshotWriter`.
class SnapshotReader {
public:
    SnapshotReader(Allocator& allocator, const SchemaSet& schemas) noexcept;

    /// Decode. `scratch` is where each component is assembled before `apply` sees it — the reader
    /// owns no component storage, because the storage is the world's.
    [[nodiscard]] Status read(Span<const u8> payload, SnapshotHeader& header,
                              Array<SpawnEvent>& spawns, Array<NetworkId>& despawns, ApplyFn apply,
                              void* user) noexcept;

private:
    const SchemaSet* schemas_;
    Array<u8> scratch_;
};

/// Resolves a network id to the local entity that carries it, and remembers the references that
/// could not be resolved yet.
///
/// `networking-and-replication` — "Spawn ordering": "WHEN an entity references another that has not
/// yet been spawned on a peer THEN the reference SHALL resolve to null and re-resolve when the
/// target arrives, rather than failing." A dangling reference is the normal case in a system where
/// two entities' spawns are two datagrams.
class ReferenceResolver {
public:
    explicit ReferenceResolver(Allocator& allocator) noexcept
        : bindings_(allocator), pending_(allocator) {}

    [[nodiscard]] Status bind(NetworkId id, ecs::Entity local) noexcept;
    void unbind(NetworkId id) noexcept;

    /// The local entity, or an invalid one. A reference that could not be resolved is remembered so
    /// `resolve_pending()` can fix it when the target arrives.
    [[nodiscard]] ecs::Entity resolve(NetworkId id) noexcept;

    /// Record a reference that must be revisited: `holder`'s field at `field_offset` wants
    /// `target`.
    [[nodiscard]] Status defer(NetworkId holder, u32 field_offset, NetworkId target) noexcept;

    /// How many deferred references `id`'s arrival resolves, and drop them from the list.
    [[nodiscard]] u32 resolve_pending(NetworkId id, Array<u32>& offsets,
                                      Array<NetworkId>& holders) noexcept;

    [[nodiscard]] u32 bound() const noexcept { return static_cast<u32>(bindings_.size()); }
    [[nodiscard]] u32 deferred() const noexcept { return static_cast<u32>(pending_.size()); }
    [[nodiscard]] u64 unresolved_lookups() const noexcept { return unresolved_; }

private:
    struct Deferred {
        NetworkId holder;
        NetworkId target;
        u32 field_offset = 0;
    };

    HashMap<u64, u64> bindings_;
    Array<Deferred> pending_;
    u64 unresolved_ = 0;
};

}  // namespace cy::net
